/**
 * murm-286 - i386 PC Emulator for RP2350
 *
 * VGA OSD (On-Screen Display) - text overlay for disk manager and settings UI.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 *
 * orange386 (Linux) variant of drivers/vga/vga_osd.c: the 80x25 cell buffer
 * is a dedicated array instead of a borrowed, SD-backed slice of gfx_buffer
 * (RP2350 has no spare SRAM for it), and the overlay is rendered by the
 * scanout thread (vga_hw_linux.c) as XRGB8888 instead of VGA/HDMI DMA lines.
 */
#include "vga_osd.h"
#include "font8x16.h"
#include <string.h>
#include <stdio.h>

static uint8_t osd_buffer[OSD_BUFFER_SIZE];

// OSD visibility flag
static volatile bool osd_visible = false;

// Box drawing characters (CP437)
#define BOX_TL  0xDA  // Top-left corner
#define BOX_TR  0xBF  // Top-right corner
#define BOX_BL  0xC0  // Bottom-left corner
#define BOX_BR  0xD9  // Bottom-right corner
#define BOX_H   0xC4  // Horizontal line
#define BOX_V   0xB3  // Vertical line

// CGA colors (same as vga_hw.c), 6-bit RRGGBB
static const uint8_t cga_colors[16] = {
    0x00,  // 0: Black
    0x02,  // 1: Blue
    0x08,  // 2: Green
    0x0A,  // 3: Cyan
    0x20,  // 4: Red
    0x22,  // 5: Magenta
    0x28,  // 6: Brown
    0x2A,  // 7: Light Gray
    0x15,  // 8: Dark Gray
    0x17,  // 9: Light Blue
    0x1D,  // 10: Light Green
    0x1F,  // 11: Light Cyan
    0x35,  // 12: Light Red
    0x37,  // 13: Light Magenta
    0x3D,  // 14: Yellow
    0x3F,  // 15: White
};

// Convert CGA color index (RRGGBB, 2 bits per channel) to XRGB8888
static uint32_t color_to_output(uint8_t color_idx) {
    uint8_t c = cga_colors[color_idx & 0x0F];
    uint32_t r = ((c >> 4) & 3) * 0x55u;
    uint32_t g = ((c >> 2) & 3) * 0x55u;
    uint32_t b = (c & 3) * 0x55u;
    return (r << 16) | (g << 8) | b;
}

void osd_init(void) {
    osd_visible = false;
}

void osd_show(void) {
    osd_visible = true;
}

void osd_hide(void) {
    osd_visible = false;
}

bool osd_is_visible(void) {
    return osd_visible;
}

void osd_clear(void) {
    // Fill with spaces and default attribute
    for (int i = 0; i < OSD_COLS * OSD_ROWS; i++) {
        osd_buffer[i * 2] = ' ';
        osd_buffer[i * 2 + 1] = OSD_ATTR_NORMAL;
    }
}

void osd_putchar(int x, int y, char ch, uint8_t attr) {
    if (x < 0 || x >= OSD_COLS || y < 0 || y >= OSD_ROWS) return;

    int idx = (y * OSD_COLS + x) * 2;
    osd_buffer[idx] = (uint8_t)ch;
    osd_buffer[idx + 1] = attr;
}

void osd_print(int x, int y, const char *str, uint8_t attr) {
    while (*str && x < OSD_COLS) {
        osd_putchar(x++, y, *str++, attr);
    }
}

void osd_print_center(int y, const char *str, uint8_t attr) {
    int len = strlen(str);
    int x = (OSD_COLS - len) / 2;
    if (x < 0) x = 0;
    osd_print(x, y, str, attr);
}

void osd_draw_box(int x, int y, int w, int h, uint8_t attr) {
    if (w < 2 || h < 2) return;

    // Corners
    osd_putchar(x, y, BOX_TL, attr);
    osd_putchar(x + w - 1, y, BOX_TR, attr);
    osd_putchar(x, y + h - 1, BOX_BL, attr);
    osd_putchar(x + w - 1, y + h - 1, BOX_BR, attr);

    // Top and bottom edges
    for (int i = 1; i < w - 1; i++) {
        osd_putchar(x + i, y, BOX_H, attr);
        osd_putchar(x + i, y + h - 1, BOX_H, attr);
    }

    // Left and right edges
    for (int i = 1; i < h - 1; i++) {
        osd_putchar(x, y + i, BOX_V, attr);
        osd_putchar(x + w - 1, y + i, BOX_V, attr);
    }
}

void osd_draw_box_titled(int x, int y, int w, int h, const char *title, uint8_t attr) {
    osd_draw_box(x, y, w, h, attr);

    // Draw title centered in top border
    int title_len = strlen(title);
    int title_x = x + (w - title_len - 4) / 2;
    if (title_x < x + 1) title_x = x + 1;

    osd_putchar(title_x, y, ' ', attr);
    osd_print(title_x + 1, y, title, attr);
    osd_putchar(title_x + 1 + title_len, y, ' ', attr);
}

void osd_fill(int x, int y, int w, int h, char ch, uint8_t attr) {
    for (int row = y; row < y + h && row < OSD_ROWS; row++) {
        for (int col = x; col < x + w && col < OSD_COLS; col++) {
            osd_putchar(col, row, ch, attr);
        }
    }
}

uint8_t *osd_get_buffer(void) {
    return osd_buffer;
}

// Fast integer sine approximation for plasma effect (returns -127 to 127)
static int8_t fast_sin(int angle) {
    static const int8_t sin_table[65] = {
        0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
        49, 51, 54, 57, 60, 62, 65, 67, 70, 72, 75, 77, 79, 81, 83, 85,
        87, 89, 91, 93, 94, 96, 97, 99, 100, 101, 103, 104, 105, 106, 107, 108,
        108, 109, 110, 110, 111, 111, 112, 112, 112, 112, 113, 113, 113, 113, 113, 113, 113
    };
    angle = angle & 255;
    int quadrant = angle >> 6;
    int idx = angle & 63;

    switch (quadrant) {
        case 0: return sin_table[idx];
        case 1: return sin_table[64 - idx];
        case 2: return -sin_table[idx];
        default: return -sin_table[64 - idx];
    }
}

void osd_draw_plasma_background(int seed, int wx, int wy, int ww, int wh) {
    static const char shade_chars[] = {' ', '\xB0', '\xB1', '\xB2', '\xDB'};

    for (int y = 0; y < OSD_ROWS; y++) {
        for (int x = 0; x < OSD_COLS; x++) {
            // Skip the window area
            if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) {
                continue;
            }

            // Plasma formula with multiple sine waves
            int v1 = fast_sin(x * 8 + seed);
            int v2 = fast_sin(y * 14 + seed * 2);
            int v3 = fast_sin((x + y) * 5 + seed);
            int v4 = fast_sin((x - y) * 7 - seed * 2);

            // Normalize to 0-4 range for shade index
            int value = (v1 + v2 + v3 + v4 + 512) >> 8;
            if (value > 4) value = 4;
            if (value < 0) value = 0;

            char ch = shade_chars[value];
            osd_putchar(x, y, ch, OSD_ATTR(OSD_DARKGRAY, OSD_BLACK));
        }
    }
}

// Render one of the 400 OSD scanlines (80x25 cells of 8x16) as 640 pixels
void osd_render_line_rgb(uint32_t line, uint32_t *out) {
    uint32_t char_row = line / 16;
    uint32_t glyph_line = line & 15;

    if (char_row >= OSD_ROWS) {
        memset(out, 0, OSD_COLS * 8 * sizeof(uint32_t));
        return;
    }

    const uint8_t *row_data = &osd_buffer[char_row * OSD_COLS * 2];
    for (int col = 0; col < OSD_COLS; col++) {
        uint8_t ch = row_data[col * 2];
        uint8_t attr = row_data[col * 2 + 1];
        uint8_t glyph = font_8x16[ch * 16 + glyph_line];
        uint32_t fg = color_to_output(attr & 0x0F);
        uint32_t bg = color_to_output((attr >> 4) & 0x0F);

        // font_8x16: bit 0 is the leftmost pixel
        for (int b = 0; b < 8; b++)
            *out++ = (glyph >> b) & 1 ? fg : bg;
    }
}
