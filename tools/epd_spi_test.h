/* Host model of the SSD1680 RAM windows and display activation. */
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/spi_master.h"
#include "epd.h"

enum {
    TEST_ROW_BYTES = PANEL_W / 8,
    TEST_FRAME_BYTES = TEST_ROW_BYTES * PANEL_H
};

struct epd_test_spi {
    uint8_t bw[TEST_FRAME_BYTES], old[TEST_FRAME_BYTES];
    uint8_t command, update;
    unsigned x_start, x_end, y_start, y_end;
    unsigned x, y, image_bytes, updates;
};

/* Returns true at a display activation. A 0xC0 power-up is only preparation. */
static inline bool epd_test_transfer(struct epd_test_spi *spi, bool dc, const spi_transaction_t *t)
{
    const uint8_t *p = t->tx_buffer;
    size_t count = t->length / 8;
    assert(t->length % 8 == 0 && count);
    if (!dc) {
        assert(count == 1);
        spi->command = p[0];
        if (spi->command == 0x24) {
            spi->image_bytes = 0;
        }
        if (spi->command != 0x20 || (spi->update != 0xC7 && spi->update != 0x0F)) {
            return false;
        }
        unsigned width = spi->x_end - spi->x_start + 1;
        unsigned height = spi->y_end - spi->y_start + 1;
        assert(spi->image_bytes == width * height);
        if (spi->update == 0xC7) {
            assert(spi->image_bytes == TEST_FRAME_BYTES);
        }
        spi->updates++;
        return true;
    }
    switch (spi->command) {
    case 0x22:
        spi->update = p[0];
        break;
    case 0x44:
        assert(count == 2 && p[0] <= p[1] && p[1] < TEST_ROW_BYTES);
        spi->x_start = p[0];
        spi->x_end = p[1];
        break;
    case 0x45:
        assert(count == 4);
        spi->y_start = p[0] | (p[1] << 8);
        spi->y_end = p[2] | (p[3] << 8);
        assert(spi->y_start <= spi->y_end && spi->y_end < PANEL_H);
        break;
    case 0x4E:
        spi->x = p[0];
        break;
    case 0x4F:
        assert(count == 2);
        spi->y = p[0] | (p[1] << 8);
        break;
    case 0x24:
    case 0x26: {
        uint8_t *ram = spi->command == 0x24 ? spi->bw : spi->old;
        if (spi->command == 0x24) {
            spi->image_bytes += count;
        }
        while (count) {
            assert(spi->x >= spi->x_start && spi->x <= spi->x_end);
            assert(spi->y >= spi->y_start && spi->y <= spi->y_end);
            size_t row = spi->x_end - spi->x + 1;
            size_t chunk = count < row ? count : row;
            memcpy(ram + spi->y * TEST_ROW_BYTES + spi->x, p, chunk);
            spi->x += chunk;
            if (spi->x == spi->x_end + 1) {
                spi->x = spi->x_start;
                spi->y = spi->y == spi->y_end ? spi->y_start : spi->y + 1;
            }
            p += chunk;
            count -= chunk;
        }
        break;
    }
    }
    return false;
}
