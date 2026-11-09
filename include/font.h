#ifndef FONT_H
#define FONT_H

#include <stdint.h>

// 6x8 bitmap font (ASCII 0x20-0x7E, 95 characters)
// Each character is 6 pixels wide, 8 pixels tall
// Each byte represents one row, bit 7 is leftmost pixel
extern const uint8_t font6x8[95][8];

// Terminus 8x16 bitmap font (ASCII 0x20-0x7E, 95 characters)
// Each character is 8 pixels wide, 16 pixels tall
// Each byte represents one row, bit 7 is leftmost pixel
extern const uint8_t font8x16[95][16];

// Font dimensions
#define FONT_6X8_WIDTH  6
#define FONT_6X8_HEIGHT 8
#define FONT_8X16_WIDTH  8
#define FONT_8X16_HEIGHT 16

#endif // FONT_H
