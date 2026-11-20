#include "audio.h"

#include "sdkconfig.h"

#if CONFIG_SPIRAM_MODE_OCT
#define PSRAM_PIN_IN_USE(pin) ((pin) >= 33 && (pin) <= 37)
_Static_assert(!PSRAM_PIN_IN_USE(AUDIO_PA_EN_GPIO) && !PSRAM_PIN_IN_USE(AUDIO_I2S_DOUT_GPIO)
                   && !PSRAM_PIN_IN_USE(AUDIO_I2S_WS_GPIO) && !PSRAM_PIN_IN_USE(AUDIO_I2S_DIN_GPIO)
                   && !PSRAM_PIN_IN_USE(AUDIO_I2S_BCLK_GPIO) && !PSRAM_PIN_IN_USE(AUDIO_I2S_MCLK_GPIO)
                   && !PSRAM_PIN_IN_USE(AUDIO_I2C_SCL_GPIO) && !PSRAM_PIN_IN_USE(AUDIO_I2C_SDA_GPIO),
               "Audio wiring overlaps GPIO33-37 reserved for octal PSRAM");
#endif

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define AUDIO_I2C_PORT            I2C_NUM_0
#define AUDIO_I2S_PORT            I2S_NUM_0
#define AUDIO_I2C_SPEED_HZ        400000
#define AUDIO_DEFAULT_VOLUME      60
#define AUDIO_DEFAULT_MIC_GAIN_DB 18.0f

static const char *log_tag = "audio";

struct audio_device {
    i2c_master_bus_handle_t i2c_bus;
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    esp_codec_dev_handle_t codec;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_data_if_t *data_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_if_t *codec_if;
    SemaphoreHandle_t control_lock;
    int volume;
    bool ready;
};

static struct audio_device audio_device = {.volume = AUDIO_DEFAULT_VOLUME};

static esp_err_t check_codec_result(int result, const char *operation)
{
    if (result == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(log_tag, "%s failed: %d", operation, result);
    return ESP_FAIL;
}

static void cleanup_failed_init(void)
{
    if (audio_device.control_lock) {
        vSemaphoreDelete(audio_device.control_lock);
    }
    if (audio_device.codec) {
        esp_codec_dev_delete(audio_device.codec);
    }
    if (audio_device.codec_if) {
        audio_codec_delete_codec_if(audio_device.codec_if);
    }
    if (audio_device.data_if) {
        audio_codec_delete_data_if(audio_device.data_if);
    }
    if (audio_device.ctrl_if) {
        audio_codec_delete_ctrl_if(audio_device.ctrl_if);
    }
    if (audio_device.gpio_if) {
        audio_codec_delete_gpio_if(audio_device.gpio_if);
    }
    if (audio_device.tx) {
        (void)i2s_channel_disable(audio_device.tx);
        i2s_del_channel(audio_device.tx);
    }
    if (audio_device.rx) {
        (void)i2s_channel_disable(audio_device.rx);
        i2s_del_channel(audio_device.rx);
    }
    if (audio_device.i2c_bus) {
        i2c_del_master_bus(audio_device.i2c_bus);
    }
    audio_device = (struct audio_device){.volume = AUDIO_DEFAULT_VOLUME};
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_DMA_DESC_NUM;
    channel_config.dma_frame_num = AUDIO_DMA_FRAME_NUM;
    channel_config.auto_clear = true;

    esp_err_t err = i2s_new_channel(&channel_config, &audio_device.tx, &audio_device.rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = AUDIO_I2S_MCLK_GPIO,
                .bclk = AUDIO_I2S_BCLK_GPIO,
                .ws = AUDIO_I2S_WS_GPIO,
                .dout = AUDIO_I2S_DOUT_GPIO,
                .din = AUDIO_I2S_DIN_GPIO,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    // ES8311 supports this exact 16 kHz clock relationship and it avoids fractional MCLK.
    standard_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    err = i2s_channel_init_std_mode(audio_device.tx, &standard_config);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_init_std_mode(audio_device.rx, &standard_config);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(audio_device.tx);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(audio_device.rx);
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = AUDIO_I2C_PORT,
        .sda_io_num = AUDIO_I2C_SDA_GPIO,
        .scl_io_num = AUDIO_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_config, &audio_device.i2c_bus);
}

static int load_volume(void)
{
    int volume = AUDIO_DEFAULT_VOLUME;
    nvs_handle_t nvs;
    if (nvs_open("audio", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t saved;
        if (nvs_get_u8(nvs, "volume", &saved) == ESP_OK && saved <= 100) {
            volume = saved;
        }
        nvs_close(nvs);
    }
    return volume;
}

static esp_err_t init_codec(void)
{
    audio_codec_i2c_cfg_t i2c_config = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = audio_device.i2c_bus,
        .clock_speed_hz = AUDIO_I2C_SPEED_HZ,
    };
    audio_device.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!audio_device.ctrl_if) {
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2s_cfg_t i2s_config = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = audio_device.rx,
        .tx_handle = audio_device.tx,
    };
    audio_device.data_if = audio_codec_new_i2s_data(&i2s_config);
    if (!audio_device.data_if) {
        return ESP_ERR_NO_MEM;
    }

    audio_device.gpio_if = audio_codec_new_gpio();
    if (!audio_device.gpio_if) {
        return ESP_ERR_NO_MEM;
    }

    es8311_codec_cfg_t codec_config = {
        .ctrl_if = audio_device.ctrl_if,
        .gpio_if = audio_device.gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_PA_EN_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain =
            {
                // The NS415B supply and gain depend on the carrier board. Keep the codec's
                // standard 3.3 V/5 V assumptions; volume remains available for calibration.
                .pa_voltage = 5.0f,
                .codec_dac_voltage = 3.3f,
                .pa_gain = 0.0f,
            },
        .no_dac_ref = false,
        .mclk_div = I2S_MCLK_MULTIPLE_384,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_config);
    if (!codec_if) {
        return ESP_FAIL;
    }

    audio_device.codec_if = codec_if;

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = audio_device.data_if,
    };
    audio_device.codec = esp_codec_dev_new(&device_config);
    if (!audio_device.codec) {
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t sample_config = {
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel = AUDIO_CHANNEL_COUNT,
        .channel_mask = 0x03,
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    esp_err_t err = check_codec_result(esp_codec_dev_open(audio_device.codec, &sample_config), "open ES8311");
    if (err != ESP_OK) {
        return err;
    }
    audio_device.volume = load_volume();
    err = check_codec_result(esp_codec_dev_set_out_vol(audio_device.codec, audio_device.volume), "set speaker volume");
    if (err != ESP_OK) {
        return err;
    }
    err = check_codec_result(esp_codec_dev_set_in_gain(audio_device.codec, AUDIO_DEFAULT_MIC_GAIN_DB),
                             "set microphone gain");
    if (err != ESP_OK) {
        return err;
    }
    // esp_codec_dev_open() enables the codec and drives the PA enable pin high. Muting only
    // gates the DAC, so drop the PA enable as well: an indicator LED on that net would
    // otherwise stay lit while the device is idle.
    err = check_codec_result(esp_codec_dev_set_out_mute(audio_device.codec, true), "mute speaker");
    if (err != ESP_OK) {
        return err;
    }
    return check_codec_result(audio_device.gpio_if->set(AUDIO_PA_EN_GPIO, false), "disable speaker amp");
}

esp_err_t audio_init(void)
{
    if (audio_device.ready) {
        return ESP_OK;
    }

    esp_err_t err = init_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "I2S init failed: %s", esp_err_to_name(err));
        cleanup_failed_init();
        return err;
    }
    err = init_i2c();
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "I2C init failed: %s", esp_err_to_name(err));
        cleanup_failed_init();
        return err;
    }
    err = init_codec();
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "ES8311 init failed: %s", esp_err_to_name(err));
        cleanup_failed_init();
        return err;
    }

    audio_device.control_lock = xSemaphoreCreateMutex();
    if (!audio_device.control_lock) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }
    audio_device.ready = true;
    ESP_LOGI(log_tag, "ES8311 audio ready: %d Hz, %d-bit stereo, PA GPIO %d", AUDIO_SAMPLE_RATE_HZ,
             AUDIO_BITS_PER_SAMPLE, AUDIO_PA_EN_GPIO);
    return ESP_OK;
}

bool audio_is_ready(void)
{
    return audio_device.ready;
}

esp_err_t audio_read(void *buffer, size_t length, size_t *bytes_read)
{
    if (!audio_device.ready || !buffer || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int err = esp_codec_dev_read(audio_device.codec, buffer, (int)length);
    if (bytes_read) {
        *bytes_read = err == ESP_CODEC_DEV_OK ? length : 0;
    }
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_write(void *buffer, size_t length, size_t *bytes_written)
{
    if (!audio_device.ready || !buffer || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int err = esp_codec_dev_write(audio_device.codec, buffer, (int)length);
    if (bytes_written) {
        *bytes_written = err == ESP_CODEC_DEV_OK ? length : 0;
    }
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_set_mute(bool muted)
{
    if (!audio_device.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(audio_device.control_lock, portMAX_DELAY);
    // The codec mute only gates the DAC. Drive the PA enable too so the amplifier (and any
    // LED sharing that net) is off whenever nothing is playing.
    esp_err_t err = check_codec_result(esp_codec_dev_set_out_mute(audio_device.codec, muted),
                                       muted ? "mute speaker" : "unmute speaker");
    if (err == ESP_OK) {
        err = check_codec_result(audio_device.gpio_if->set(AUDIO_PA_EN_GPIO, !muted),
                                 muted ? "disable speaker amp" : "enable speaker amp");
    }
    xSemaphoreGive(audio_device.control_lock);
    return err;
}

esp_err_t audio_get_volume(int *volume)
{
    if (!volume) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_device.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(audio_device.control_lock, portMAX_DELAY);
    *volume = audio_device.volume;
    xSemaphoreGive(audio_device.control_lock);
    return ESP_OK;
}

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_device.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(audio_device.control_lock, portMAX_DELAY);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("audio", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = check_codec_result(esp_codec_dev_set_out_vol(audio_device.codec, volume), "set speaker volume");
        if (err == ESP_OK) {
            int previous = audio_device.volume;
            audio_device.volume = volume;
            err = nvs_set_u8(nvs, "volume", (uint8_t)volume);
            if (err == ESP_OK) {
                err = nvs_commit(nvs);
            }
            if (err != ESP_OK
                && check_codec_result(esp_codec_dev_set_out_vol(audio_device.codec, previous), "restore speaker volume")
                       == ESP_OK)
            {
                audio_device.volume = previous;
            }
        }
        nvs_close(nvs);
    }
    xSemaphoreGive(audio_device.control_lock);
    return err;
}
