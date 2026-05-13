#ifndef FONT_H
#define FONT_H

#include <stdint.h>

// 6x8 bitmap font (ASCII 0x20-0x7E, 95 characters)
// Each character is 6 pixels wide, 8 pixels tall
// Each byte represents one row, bit 7 is leftmost pixel
extern const uint8_t font6x8[95][8];

#endif // FONT_H
