/*
 * orange386 - Linux scanout for the emulated VGA.
 *
 * Port of murm386 drivers/vga/vga_hw.c (VGA output) / drivers/hdmi/hdmi.c
 * (HDMI output): the same per-frame latching, mode classification and
 * per-submode VRAM addressing, but rendering true-colour XRGB8888 pixels into
 * a 640x480 raster instead of 6-bit dithered / palette-indexed DMA lines.
 *
 * Differences from the RP2350 renderers, all toward real VGA behaviour:
 *  - colours come from the Attribute Controller + DAC for every mode
 *    (vga_get_palette16), so text and CGA palettes follow the guest instead
 *    of fixed CGA tables;
 *  - the text cursor is drawn in emulated character-cell scanlines (no
 *    rescaling to a 16-line font);
 *  - Line Compare is compared against the display scanline in every mode;
 *  - all VRAM fetches wrap at 256 KiB like the real address counter.
 */

#include "vga_hw_linux.h"
#include "video_drm.h"
#include "video_profile.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 640x480@59.94: 800 pixel clocks per line at 25.175 MHz, 525 lines. */
#define N_LINES_TOTAL   525
#define N_LINES_VISIBLE 480
#define LINE_NS         31778u
#define HACTIVE_NS      (LINE_NS * 640u / 800u)
#define FRAME_NS        ((uint64_t)LINE_NS * N_LINES_TOTAL)
#define LATCH_LINE      (N_LINES_TOTAL - 4)   /* vga_hw.c: line 521 */

/* Default active area for 400-line modes (text, CGA, EGA <=400, mode 13h) */
#define DEFAULT_ACTIVE_START  40
#define DEFAULT_ACTIVE_END    (DEFAULT_ACTIVE_START + 400)

#define VRAM_DWORD_MASK 0xFFFFu   /* 64 K dwords = 256 KiB */
#define VRAM_BYTE_MASK  0x3FFFFu

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/*
 * Port 3DAh status from the virtual scanline clock.
 * Bit 3 (V_RETRACE) covers the whole vertical blanking interval, lines
 * 480..524, as the RP2350 ISR reports it.  Bit 0 (DISP_ENABLE, active low
 * display) is set during horizontal and vertical blanking, as on real VGA.
 */
void vga_hw_update_st01(VGAState *s)
{
    uint32_t in_frame = (uint32_t)(mono_ns() % FRAME_NS);
    uint32_t line = in_frame / LINE_NS;
    uint32_t pos = in_frame % LINE_NS;
    uint8_t st = s->st01 & (uint8_t)~(ST01_V_RETRACE | ST01_DISP_ENABLE);
    if (line >= N_LINES_VISIBLE)
        st |= ST01_V_RETRACE | ST01_DISP_ENABLE;
    else if (pos >= HACTIVE_NS)
        st |= ST01_DISP_ENABLE;
    s->st01 = st;
}

/* ------------------------------------------------------------------------
 * Renderer state (owned by the render thread, like core1 on RP2350)
 * ---------------------------------------------------------------------- */

static VGAState *vga_state;
static const uint8_t *gfx_buffer;
static uint32_t *frame;

// Current video mode (0=blank, 1=text, 2=graphics)
static int current_mode = 1;
// Graphics sub-mode: 1=CGA4, 2=EGA, 3=VGA256, 4=CGA2, 5=ModeX, 6=EGA320,
// 7=VBE packed8, 8=mono640
static int gfx_submode = 3;
static int gfx_width = 320;
static int gfx_height = 200;
static int gfx_line_offset = 40;   // Words per line (CRTC Offset, CR13)

static int active_start = DEFAULT_ACTIVE_START;
static int active_end = DEFAULT_ACTIVE_END;

static int text_cols = 80;
static int text_stride_cells = 80;

static int cursor_x, cursor_y, cursor_start, cursor_end;
static int cursor_visible, cursor_blink_state = 1;

// Per-frame values latched late in vblank
static uint16_t frame_vram_offset;
static uint8_t frame_pixel_panning;
static int frame_line_compare = -1;

static uint32_t pal256[256];
static uint32_t pal16[16];
static uint32_t spread8_lut[256];

static inline uint32_t c6_to_8(uint32_t v)
{
    v &= 0x3f;
    return (v << 2) | (v >> 4);
}

static inline uint32_t rgb6(const uint8_t *p)
{
    return (c6_to_8(p[0]) << 16) | (c6_to_8(p[1]) << 8) | c6_to_8(p[2]);
}

static inline uint32_t vram32(uint32_t dword_index)
{
    return ((const uint32_t *)gfx_buffer)[dword_index & VRAM_DWORD_MASK];
}

static inline uint8_t vram8(uint32_t byte_index)
{
    return gfx_buffer[byte_index & VRAM_BYTE_MASK];
}

static inline void fill_black(uint32_t *out)
{
    memset(out, 0, VGA_HW_WIDTH * sizeof(uint32_t));
}

/* Spread 8 bits of a byte into positions 0,4,8,...28 */
static uint32_t spread8(uint32_t plane)
{
    plane = (plane | (plane << 12)) & 0x000F000Fu;
    plane = (plane | (plane <<  6)) & 0x03030303u;
    plane = (plane | (plane <<  3)) & 0x11111111u;
    return plane;
}

/* Merge 4 plane bytes [P3|P2|P1|P0] into 8 nibbles; bit 31..28 = leftmost. */
static inline uint32_t ega_pack8_from_planes(uint32_t ega_planes)
{
    return spread8_lut[ega_planes & 0xFFu]
         | spread8_lut[(ega_planes >> 8) & 0xFFu] << 1
         | spread8_lut[(ega_planes >> 16) & 0xFFu] << 2
         | spread8_lut[ega_planes >> 24] << 3;
}

/* Source line for a display line, and the Line Compare split.
 * scale_shift: 0 = 1:1, 1 = line doubling, 2 = line quadrupling. */
static inline uint32_t split_base(uint32_t line, uint32_t src_line,
                                  int scale_shift, uint32_t stride)
{
    if (frame_line_compare >= 0 && line >= (uint32_t)frame_line_compare) {
        uint32_t lc_src = (uint32_t)frame_line_compare >> scale_shift;
        return (src_line - lc_src) * stride;
    }
    return frame_vram_offset + src_line * stride;
}

/* Mode 13h: chain4 320x200x256, pixels in linear dwords */
static void render_gfx_line_vga256(uint32_t line, uint32_t *out)
{
    int shift = gfx_height > 200 ? 0 : 1;
    uint32_t src_line = line >> shift;
    if (gfx_height <= 0 || src_line >= (uint32_t)gfx_height) {
        fill_black(out);
        return;
    }
    uint32_t stride = gfx_line_offset > 0 ? (uint32_t)gfx_line_offset * 2u : 80u;
    uint32_t base = split_base(line, src_line, shift, stride) & VRAM_DWORD_MASK;
    for (int i = 0; i < 80; i++) {
        uint32_t px = vram32(base + (uint32_t)i);
        uint32_t c0 = pal256[px & 0xFF], c1 = pal256[(px >> 8) & 0xFF];
        uint32_t c2 = pal256[(px >> 16) & 0xFF], c3 = pal256[px >> 24];
        out[0] = c0; out[1] = c0; out[2] = c1; out[3] = c1;
        out[4] = c2; out[5] = c2; out[6] = c3; out[7] = c3;
        out += 8;
    }
}

/* Mode X: unchained 256-colour planar; each dword holds pixels x%4 = 0..3 */
static void render_gfx_line_vga_planar256(uint32_t line, uint32_t *out)
{
    int active_lines = active_end - active_start;
    int shift = (gfx_height * 2 <= active_lines) ? 1 : 0;
    uint32_t src_line = line >> shift;
    if (src_line >= (uint32_t)gfx_height) {
        fill_black(out);
        return;
    }
    uint32_t stride = gfx_line_offset > 0 ? (uint32_t)gfx_line_offset * 2u : 80u;
    uint32_t base = split_base(line, src_line, shift, stride) & VRAM_DWORD_MASK;
    for (int i = 0; i < 80; i++) {
        uint32_t px = vram32(base + (uint32_t)i);
        uint32_t c0 = pal256[px & 0xFF], c1 = pal256[(px >> 8) & 0xFF];
        uint32_t c2 = pal256[(px >> 16) & 0xFF], c3 = pal256[px >> 24];
        out[0] = c0; out[1] = c0; out[2] = c1; out[3] = c1;
        out[4] = c2; out[5] = c2; out[6] = c3; out[7] = c3;
        out += 8;
    }
}

/* VBE 100h: 640x400x8 packed pixels, one VRAM byte per pixel */
static void render_gfx_line_vbe8(uint32_t line, uint32_t *out)
{
    if (line >= (uint32_t)gfx_height || gfx_width != 640) {
        fill_black(out);
        return;
    }
    const uint8_t *src = gfx_buffer + line * 640u;
    for (int i = 0; i < 640; ++i)
        out[i] = pal256[src[i]];
}

/* CGA 320x200x4: odd/even addressing, even/odd scanlines at 0000h/2000h */
static void render_gfx_line_cga(uint32_t line, uint32_t *out)
{
    uint32_t src_line = line >> 1;
    if (src_line >= 200) {
        fill_black(out);
        return;
    }
    uint32_t cga_bank = (src_line & 1) ? 0x2000 : 0x0000;
    uint32_t cga_row = src_line >> 1;
    uint32_t cga_line_offset;
    if (frame_line_compare >= 0 && line >= (uint32_t)frame_line_compare)
        cga_line_offset = cga_bank + (src_line - ((uint32_t)frame_line_compare >> 1)) * 80;
    else
        cga_line_offset = frame_vram_offset + cga_bank + cga_row * 80;
    cga_line_offset &= 0xFFFF;

    for (int i = 0; i < 80; i++) {
        uint32_t cga_addr = cga_line_offset + (uint32_t)i;
        uint32_t vga_addr = ((cga_addr & ~1u) << 1) | (cga_addr & 1u);
        uint8_t byte = vram8(vga_addr);
        uint32_t p0 = pal16[(byte >> 6) & 3], p1 = pal16[(byte >> 4) & 3];
        uint32_t p2 = pal16[(byte >> 2) & 3], p3 = pal16[byte & 3];
        out[0] = p0; out[1] = p0; out[2] = p1; out[3] = p1;
        out[4] = p2; out[5] = p2; out[6] = p3; out[7] = p3;
        out += 8;
    }
}

/* CGA 640x200x2: plane 0 only, even/odd scanlines at 0000h/2000h */
static void render_gfx_line_cga2(uint32_t line, uint32_t *out)
{
    uint32_t src_line = line >> 1;
    if (src_line >= 200) {
        fill_black(out);
        return;
    }
    uint32_t bank_offset = (src_line & 1) ? 0x2000 : 0x0000;
    uint32_t row_in_bank = src_line >> 1;
    uint32_t offset;
    if (frame_line_compare >= 0 && line >= (uint32_t)frame_line_compare)
        offset = bank_offset + (src_line - ((uint32_t)frame_line_compare >> 1)) * 80;
    else
        offset = frame_vram_offset + bank_offset + row_in_bank * 80;
    offset &= 0xFFFF;
    uint32_t bg = pal16[0], fg = pal16[1];
    for (int i = 0; i < 80; i++) {
        uint8_t byte = vram8((offset + (uint32_t)i) * 4u);
        for (int b = 7; b >= 0; --b)
            *out++ = (byte >> b) & 1 ? fg : bg;
    }
}

/* Mode 11h: 640x480x2, one packed bit per pixel in plane 0 */
static void render_gfx_line_mono640(uint32_t line, uint32_t *out)
{
    if (line >= (uint32_t)gfx_height || gfx_width != 640) {
        fill_black(out);
        return;
    }
    uint32_t stride = gfx_line_offset > 0 ? (uint32_t)gfx_line_offset * 2u : 80u;
    uint32_t offset = split_base(line, line, 0, stride) & 0xFFFFu;
    uint32_t bg = pal16[0], fg = pal16[1];
    for (int i = 0; i < 80; ++i) {
        uint8_t byte = vram8((offset + (uint32_t)i) * 4u);
        for (int b = 7; b >= 0; --b)
            *out++ = (byte >> b) & 1 ? fg : bg;
    }
}

/* EGA planar 16-colour: 320-wide (pixel doubled) or 640-wide */
static void render_gfx_line_ega(uint32_t line, uint32_t *out)
{
    int double_pixels = (gfx_width <= 320);
    int height = gfx_height > 0 ? gfx_height : 200;
    uint32_t src_line;
    int shift = 0;
    if (height <= 100) {
        src_line = line >> 2;
        shift = 2;
    } else if (height <= 200) {
        src_line = line >> 1;
        shift = 1;
    } else if (height <= 350) {
        /* 350-line mode: spread over the 400-line active area */
        src_line = (line * (uint32_t)height) / (uint32_t)(active_end - active_start);
    } else {
        src_line = line;
    }
    if (src_line >= (uint32_t)height) {
        fill_black(out);
        return;
    }
    uint32_t stride = gfx_line_offset > 0 ? (uint32_t)gfx_line_offset * 2u
                                          : (uint32_t)gfx_width / 8u;
    uint32_t offset;
    if (frame_line_compare >= 0 && line >= (uint32_t)frame_line_compare) {
        uint32_t lc_src = shift ? (uint32_t)frame_line_compare >> shift
                                : (uint32_t)frame_line_compare * (uint32_t)height /
                                  (uint32_t)(active_end - active_start);
        offset = (src_line - lc_src) * stride;
    } else {
        offset = frame_vram_offset + src_line * stride;
    }
    offset &= VRAM_DWORD_MASK;

    int panning = frame_pixel_panning;
    int pshift = panning * 4;
    int words_to_render = gfx_width / 8;
    if (words_to_render > 80) words_to_render = 80;

    for (int i = 0; i < words_to_render; i++) {
        uint32_t eight = ega_pack8_from_planes(vram32(offset + (uint32_t)i));
        if (panning > 0) {
            uint32_t next = ega_pack8_from_planes(vram32(offset + (uint32_t)i + 1u));
            eight = (eight << pshift) | (next >> (32 - pshift));
        }
        for (int p = 7; p >= 0; --p) {
            uint32_t c = pal16[(eight >> (p * 4)) & 0xF];
            *out++ = c;
            if (double_pixels)
                *out++ = c;
        }
    }
    int done = words_to_render * (double_pixels ? 16 : 8);
    if (done < VGA_HW_WIDTH)
        memset(out, 0, (size_t)(VGA_HW_WIDTH - done) * sizeof(uint32_t));
}

/* Text mode from the interleaved plane 0/1 character/attribute cells */
static void render_text_line(uint32_t line, uint32_t *out)
{
    int char_height = vga_get_char_height(vga_state);
    if (char_height <= 0 || char_height > 32)
        char_height = 16;

    uint32_t char_row = line / (uint32_t)char_height;
    uint32_t glyph_line = line % (uint32_t)char_height;
    uint32_t text_rows = (uint32_t)(active_end - active_start) / (uint32_t)char_height;

    int cols = text_cols;
    int double_h = (cols == 40);

    if (char_row >= text_rows) {
        fill_black(out);
        return;
    }
    uint32_t row_base = (uint32_t)frame_vram_offset + char_row * (uint32_t)text_stride_cells;
    for (int col = 0; col < cols; col++) {
        uint16_t cell = (uint16_t)vram32(row_base + (uint32_t)col);
        uint8_t ch   = (uint8_t)(cell & 0xFF);
        uint8_t attr = (uint8_t)(cell >> 8);
        uint8_t glyph;
        if (cursor_visible && cursor_blink_state && col == cursor_x &&
            char_row == (uint32_t)cursor_y &&
            glyph_line >= (uint32_t)cursor_start &&
            glyph_line <= (uint32_t)cursor_end) {
            glyph = 0xFF;
        } else {
            const uint8_t *fp = vga_get_font_ptr(vga_state, ch, (attr >> 3) & 1);
            glyph = fp[glyph_line * 4];
        }
        uint32_t fg = pal16[attr & 0x0F];
        uint32_t bg = pal16[(attr >> 4) & 0x07];
        for (int b = 7; b >= 0; --b) {
            uint32_t c = (glyph >> b) & 1 ? fg : bg;
            *out++ = c;
            if (double_h)
                *out++ = c;
        }
    }
}

static void render_line(uint32_t line, uint32_t *out)
{
    if (line < (uint32_t)active_start || line >= (uint32_t)active_end) {
        fill_black(out);
        return;
    }
    line -= (uint32_t)active_start;
    if (current_mode == 1) {
        render_text_line(line, out);
        return;
    }
    if (current_mode == 2) {
        switch (gfx_submode) {
        case 1: render_gfx_line_cga(line, out); return;
        case 2: case 6: render_gfx_line_ega(line, out); return;
        case 4: render_gfx_line_cga2(line, out); return;
        case 8: render_gfx_line_mono640(line, out); return;
        case 5: render_gfx_line_vga_planar256(line, out); return;
        case 7: render_gfx_line_vbe8(line, out); return;
        default:
            if (video_profile_is_ega())
                render_gfx_line_ega(line, out);
            else
                render_gfx_line_vga256(line, out);
            return;
        }
    }
    fill_black(out);
}

/* ------------------------------------------------------------------------
 * Per-frame state (vga_hw.c: ISR latch + vga_hw_new_frame_deferred)
 * ---------------------------------------------------------------------- */

static void latch_frame(void)
{
    const volatile uint8_t *cr = vga_state->cr;
    // Guests write CR0C and CR0D with separate OUTs; re-read until stable.
    uint8_t hi, lo;
    do {
        hi = cr[0x0c];
        lo = cr[0x0d];
    } while (hi != cr[0x0c]);
    frame_vram_offset = (uint16_t)((hi << 8) | lo);
    frame_pixel_panning = vga_state->ar[0x13] & 0x07;
    int lc = (int)cr[0x18]
           | (((int)cr[0x07] & 0x10) << 4)
           | (((int)cr[0x09] & 0x40) << 3);
    frame_line_compare = (lc > 0 && lc < N_LINES_VISIBLE) ? lc : -1;

    // Text geometry: visible cols from CR01, stride in cells from CR13 * 2
    int cols = vga_get_text_cols(vga_state);
    int stride_cells = vga_get_line_offset(vga_state) * 2;
    if ((cols == 40 || cols == 80) && stride_cells > 0 && stride_cells <= 256) {
        text_cols = cols;
        text_stride_cells = stride_cells;
    }
}

static void set_mode(int mode)
{
    // Mode 0 (blank) is a transient state while the BIOS reprograms the
    // registers during a mode switch; keep showing the previous mode.
    if (mode == 0) return;
    if (mode == 1) {
        active_start = DEFAULT_ACTIVE_START;
        active_end = DEFAULT_ACTIVE_END;
    }
    current_mode = mode;
}

static void set_gfx_mode(int submode, int width, int height, int line_offset)
{
    gfx_submode = submode;
    gfx_width = width;
    gfx_height = height;
    gfx_line_offset = line_offset > 0 ? line_offset : (width / 8);

    // 480-line modes: mode 12h (height=480) or Mode X 320x240 (doubled)
    if (height > 400 || (submode == 5 && height > 200)) {
        active_start = 0;
        active_end = N_LINES_VISIBLE;
    } else {
        active_start = DEFAULT_ACTIVE_START;
        active_end = DEFAULT_ACTIVE_END;
    }
}

static void new_frame(void)
{
    static int last_vga_mode = -1;
    VGAState *s = vga_state;

    int cx, cy, cs, ce, cv;
    vga_get_cursor_info(s, &cx, &cy, &cs, &ce, &cv);
    cursor_visible = cv;
    if (cv) {
        cursor_x = cx;
        cursor_y = cy;
        cursor_start = cs;
        cursor_end = ce;
        cursor_blink_state = vga_get_cursor_blink_phase(s);
    }

    int vga_mode = vga_get_mode(s);
    if (vga_mode != last_vga_mode) {
        set_mode(vga_mode);
        last_vga_mode = vga_mode;
    }

    const uint8_t *dac = vga_get_palette(s);
    for (int i = 0; i < 256; i++)
        pal256[i] = rgb6(dac + i * 3);
    uint8_t p16[48];
    vga_get_palette16(s, p16);
    for (int i = 0; i < 16; i++)
        pal16[i] = rgb6(p16 + i * 3);

    if (vga_mode == 2) {
        int w, h;
        int submode = vga_get_graphics_mode(s, &w, &h);
        int line_offset = vga_get_line_offset(s);
        if (submode == 2 && w <= 320)
            submode = 6;
        set_gfx_mode(submode, w, h, line_offset);
    }
}

/* ------------------------------------------------------------------------
 * Render thread
 * ---------------------------------------------------------------------- */

static pthread_t render_thread;
static atomic_bool render_running;
static bool render_started;

static void sleep_until_ns(uint64_t target)
{
    struct timespec ts = {
        .tv_sec = (time_t)(target / 1000000000u),
        .tv_nsec = (long)(target % 1000000000u),
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

static void *render_main(void *arg)
{
    (void)arg;
    while (atomic_load(&render_running)) {
        uint64_t now = mono_ns();
        uint64_t target = now - now % FRAME_NS + (uint64_t)LATCH_LINE * LINE_NS;
        if (target <= now)
            target += FRAME_NS;
        sleep_until_ns(target);
        if (!atomic_load(&render_running))
            break;

        latch_frame();
        new_frame();
        for (uint32_t y = 0; y < VGA_HW_HEIGHT; ++y)
            render_line(y, frame + (size_t)y * VGA_HW_WIDTH);
        video_drm_present(frame, VGA_HW_WIDTH, VGA_HW_HEIGHT);
    }
    return NULL;
}

bool vga_hw_start(VGAState *s)
{
    if (render_started)
        return true;
    for (int i = 0; i < 256; i++)
        spread8_lut[i] = spread8((uint32_t)i);
    frame = calloc((size_t)VGA_HW_WIDTH * VGA_HW_HEIGHT, sizeof(uint32_t));
    if (!frame)
        return false;
    vga_state = s;
    gfx_buffer = s->vga_ram;
    atomic_store(&render_running, true);
    int err = pthread_create(&render_thread, NULL, render_main, NULL);
    if (err) {
        fprintf(stderr, "Cannot start VGA scanout thread: %s\n", strerror(err));
        atomic_store(&render_running, false);
        free(frame);
        frame = NULL;
        return false;
    }
    render_started = true;
    return true;
}

void vga_hw_stop(void)
{
    if (!render_started)
        return;
    atomic_store(&render_running, false);
    pthread_join(render_thread, NULL);
    render_started = false;
    free(frame);
    frame = NULL;
}
