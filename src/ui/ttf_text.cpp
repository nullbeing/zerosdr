#include "ui/ttf_text.h"

#include "framebuffer.h"

#include <algorithm>
#include <cstring>

TtfText::~TtfText() {
    if (face_ != nullptr) {
        FT_Done_Face(face_);
        face_ = nullptr;
    }
    if (library_ != nullptr) {
        FT_Done_FreeType(library_);
        library_ = nullptr;
    }
}

bool TtfText::load(const char* path, int pixel_size) {
    if (path == nullptr || pixel_size <= 0) return false;
    if (face_ != nullptr) return true;

    if (FT_Init_FreeType(&library_) != 0) {
        library_ = nullptr;
        return false;
    }
    if (FT_New_Face(library_, path, 0, &face_) != 0) {
        FT_Done_FreeType(library_);
        library_ = nullptr;
        face_ = nullptr;
        return false;
    }
    if (FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixel_size)) != 0) {
        FT_Done_Face(face_);
        FT_Done_FreeType(library_);
        face_ = nullptr;
        library_ = nullptr;
        return false;
    }

    pixel_size_ = pixel_size;
    height_ = static_cast<int>(face_->size->metrics.height >> 6);
    cache_.clear();
    return true;
}

const TtfText::Glyph* TtfText::glyphFor(unsigned char c) const {
    if (face_ == nullptr) return nullptr;

    auto it = cache_.find(c);
    if (it != cache_.end()) return &it->second;

    if (FT_Load_Char(face_, c, FT_LOAD_RENDER) != 0) return nullptr;

    const FT_GlyphSlot slot = face_->glyph;
    Glyph glyph;
    glyph.width = static_cast<int>(slot->bitmap.width);
    glyph.height = static_cast<int>(slot->bitmap.rows);
    glyph.bearing_x = slot->bitmap_left;
    glyph.bearing_y = slot->bitmap_top;
    glyph.advance = static_cast<int>(slot->advance.x >> 6);
    glyph.coverage.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);

    for (int row = 0; row < glyph.height; ++row) {
        const unsigned char* src = slot->bitmap.buffer + row * slot->bitmap.pitch;
        std::memcpy(&glyph.coverage[static_cast<size_t>(row) * glyph.width],
                    src, static_cast<size_t>(glyph.width));
    }

    auto inserted = cache_.emplace(c, std::move(glyph));
    return &inserted.first->second;
}

int TtfText::textWidth(const char* text) const {
    if (face_ == nullptr || text == nullptr) return 0;
    int width = 0;
    for (const char* p = text; *p; ++p) {
        const Glyph* g = glyphFor(static_cast<unsigned char>(*p));
        width += g ? g->advance : pixel_size_ / 2;
    }
    return width;
}

void TtfText::draw(Framebuffer& fb, int x, int baseline, const char* text,
                   uint16_t color, uint8_t alpha) const {
    if (face_ == nullptr || text == nullptr) return;

    int pen = x;
    for (const char* p = text; *p; ++p) {
        const Glyph* g = glyphFor(static_cast<unsigned char>(*p));
        if (g == nullptr) continue;

        const int gx = pen + g->bearing_x;
        const int gy = baseline - g->bearing_y;

        for (int row = 0; row < g->height; ++row) {
            for (int col = 0; col < g->width; ++col) {
                const uint8_t cov =
                    g->coverage[static_cast<size_t>(row) * g->width + col];
                if (cov == 0) continue;
                const int a = (cov * alpha) / 255;
                if (a == 0) continue;
                fb.blendPixel(gx + col, gy + row, color,
                              static_cast<float>(a) / 255.0f);
            }
        }
        pen += g->advance;
    }
}
