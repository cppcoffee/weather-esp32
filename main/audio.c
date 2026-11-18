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

#define AUDIO_I2C_PORT            I2C_NUM_0
#define AUDIO_I2S_PORT            I2S_NUM_0
#define AUDIO_I2C_SPEED_HZ        400000
#define AUDIO_DEFAULT_VOLUME      60
#define AUDIO_DEFAULT_MIC_GAIN_DB 18.0f

static const char *TAG = "audio";

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static bool s_ready;

static esp_err_t check_codec_result(int result, const char *operation)
{
    if (result == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s failed: %d", operation, result);
    return ESP_FAIL;
}

static void cleanup_failed_init(void)
{
    if (s_codec) {
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_tx) {
        (void)i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx) {
        (void)i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_DMA_DESC_NUM;
    channel_config.dma_frame_num = AUDIO_DMA_FRAME_NUM;
    channel_config.auto_clear = true;

    esp_err_t err = i2s_new_channel(&channel_config, &s_tx, &s_rx);
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

    err = i2s_channel_init_std_mode(s_tx, &standard_config);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_init_std_mode(s_rx, &standard_config);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(s_rx);
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
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

static esp_err_t init_codec(void)
{
    audio_codec_i2c_cfg_t i2c_config = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = AUDIO_I2C_SPEED_HZ,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!s_ctrl_if) {
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2s_cfg_t i2s_config = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_config);
    if (!s_data_if) {
        return ESP_ERR_NO_MEM;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (!s_gpio_if) {
        return ESP_ERR_NO_MEM;
    }

    es8311_codec_cfg_t codec_config = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
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

    s_codec_if = codec_if;

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&device_config);
    if (!s_codec) {
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t sample_config = {
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel = AUDIO_CHANNEL_COUNT,
        .channel_mask = 0x03,
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
    };
    esp_err_t err = check_codec_result(esp_codec_dev_open(s_codec, &sample_config), "open ES8311");
    if (err != ESP_OK) {
        return err;
    }
    err = check_codec_result(esp_codec_dev_set_out_vol(s_codec, AUDIO_DEFAULT_VOLUME), "set speaker volume");
    if (err != ESP_OK) {
        return err;
    }
    err = check_codec_result(esp_codec_dev_set_in_gain(s_codec, AUDIO_DEFAULT_MIC_GAIN_DB), "set microphone gain");
    if (err != ESP_OK) {
        return err;
    }
    // esp_codec_dev_open() enables the codec and drives the PA enable pin high. Muting only
    // gates the DAC, so drop the PA enable as well: an indicator LED on that net would
    // otherwise stay lit while the device is idle.
    err = check_codec_result(esp_codec_dev_set_out_mute(s_codec, true), "mute speaker");
    if (err != ESP_OK) {
        return err;
    }
    return check_codec_result(s_gpio_if->set(AUDIO_PA_EN_GPIO, false), "disable speaker amp");
}

esp_err_t audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = init_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        cleanup_failed_init();
        return err;
    }
    err = init_i2c();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        cleanup_failed_init();
        return err;
    }
    err = init_codec();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(err));
        cleanup_failed_init();
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ES8311 audio ready: %d Hz, %d-bit stereo, PA GPIO %d", AUDIO_SAMPLE_RATE_HZ, AUDIO_BITS_PER_SAMPLE,
             AUDIO_PA_EN_GPIO);
    return ESP_OK;
}

bool audio_is_ready(void)
{
    return s_ready;
}

esp_err_t audio_read(void *buffer, size_t length, size_t *bytes_read)
{
    if (!s_ready || !buffer || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int err = esp_codec_dev_read(s_codec, buffer, (int)length);
    if (bytes_read) {
        *bytes_read = err == ESP_CODEC_DEV_OK ? length : 0;
    }
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_write(void *buffer, size_t length, size_t *bytes_written)
{
    if (!s_ready || !buffer || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int err = esp_codec_dev_write(s_codec, buffer, (int)length);
    if (bytes_written) {
        *bytes_written = err == ESP_CODEC_DEV_OK ? length : 0;
    }
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_set_mute(bool muted)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // The codec mute only gates the DAC. Drive the PA enable too so the amplifier (and any
    // LED sharing that net) is off whenever nothing is playing.
    esp_err_t err =
        check_codec_result(esp_codec_dev_set_out_mute(s_codec, muted), muted ? "mute speaker" : "unmute speaker");
    if (err != ESP_OK) {
        return err;
    }
    return check_codec_result(s_gpio_if->set(AUDIO_PA_EN_GPIO, !muted),
                              muted ? "disable speaker amp" : "enable speaker amp");
}
