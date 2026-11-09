#ifndef TTF_TEXT_H
#define TTF_TEXT_H

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Framebuffer;

/**
 * TtfText - minimal anti-aliased TrueType text renderer.
 *
 * Glyph bitmaps are rasterised once by FreeType and cached, then blended
 * into the framebuffer. Used for the "professional" look of the radar
 * callsign; the rest of the UI keeps its built-in bitmap fonts.
 */
class TtfText {
public:
    TtfText() = default;
    ~TtfText();

    TtfText(const TtfText&) = delete;
    TtfText& operator=(const TtfText&) = delete;

    /** Load a font file at the given pixel height. */
    bool load(const char* path, int pixel_size);

    bool valid() const { return face_ != nullptr; }
    int pixelSize() const { return pixel_size_; }
    int lineHeight() const { return height_; }

    int textWidth(const char* text) const;

    /** Draw text with its baseline at `baseline`. */
    void draw(Framebuffer& fb, int x, int baseline, const char* text,
              uint16_t color, uint8_t alpha = 255) const;

private:
    struct Glyph {
        int width = 0;
        int height = 0;
        int bearing_x = 0;
        int bearing_y = 0;
        int advance = 0;
        std::vector<uint8_t> coverage;
    };

    const Glyph* glyphFor(unsigned char c) const;

    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
    int pixel_size_ = 0;
    int height_ = 0;
    mutable std::unordered_map<unsigned char, Glyph> cache_;
};

/** Load the first available UI font (DejaVu Mono Bold, Liberation Mono
 *  Bold, Noto Sans Mono) at the given pixel size. */
bool loadDefaultUiFont(TtfText& font, int pixel_size);

#endif // TTF_TEXT_H
