#ifndef ORANGE386_VGA_TEXT_H
#define ORANGE386_VGA_TEXT_H
#include <stdint.h>
#include <stddef.h>
/* Static 720x400 VGA text raster, XRGB8888 destination. */
void vga_text_render(uint8_t *pixels, uint32_t pitch,
                     uint32_t width, uint32_t height,
                     const uint8_t *ram);
void vga_text_io_write(uint16_t port, uint8_t value);
uint8_t vga_text_io_read(uint16_t port);
#endif
