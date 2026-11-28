#include "ui/scanner_view.h"

#include "font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Amber phosphor palette (RGB565).
constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kHi = 0xFE8C;      // #FFD060 brightest readouts
constexpr uint16_t kAmber = 0xFD80;   // #FFB000 primary
constexpr uint16_t kMid = 0xC420;     // #C08400 secondary
constexpr uint16_t kDim = 0x7A80;     // #7A5200 labels, inactive
constexpr uint16_t kFaint = 0x3920;   // #3A2400 grid, rules
constexpr uint16_t kGhost = 0x1880;   // #1C1000 empty segments
constexpr uint16_t kAlert = 0xFAC3;   // #FF5A18 hold / lockout / mute
constexpr uint16_t kAlertDim = 0x7940;  // #782800

constexpr int kW = 320;
constexpr int kH = 170;
constexpr int kAdvance = 6;  // 6x8 font, no extra spacing

// Signal list geometry.
constexpr int kListTop = 22;
constexpr int kRowH = 11;
constexpr int kIdX = 2;
constexpr int kStripX = 24;
constexpr int kSlotW = 4;
constexpr int kSlots = 20;            // seconds of history shown
constexpr int kStripR = kStripX + kSlots * kSlotW;  // exclusive
constexpr int kScaleX = 112;
constexpr int kTagX = 134;
constexpr int kTagW = 56;
constexpr int kInfoX = 196;

// Lower panels.
constexpr int kRule1 = 11;
constexpr int kRule2 = 100;
constexpr int kRule3 = 144;
constexpr int kScopeX = 108;
constexpr int kScopeY = 102;
constexpr int kScopeW = 210;
constexpr int kScopeH = 40;

std::string mhz(uint32_t hz) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.3f", hz / 1e6);
    return buf;
}

const char* stateLabel(ScannerState s) {
    switch (s) {
        case ScannerState::Scan: return "SCAN";
        case ScannerState::Lock: return "LOCK";
        case ScannerState::Hang: return "HANG";
        case ScannerState::Hold: return "HOLD";
        case ScannerState::Error: return "FAULT";
    }
    return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// Primitives

void ScannerView::text(int x, int y, const char* s, uint16_t color) {
    for (; *s; ++s, x += kAdvance) {
        const char c = *s;
        if (c < 0x20 || c > 0x7E) continue;
        const uint8_t* glyph = font6x8[c - 0x20];
        for (int row = 0; row < FONT_6X8_HEIGHT; ++row) {
            const uint8_t line = glyph[row];
            if (!line) continue;
            for (int col = 0; col < FONT_6X8_WIDTH; ++col) {
                if (line & (0x80 >> col)) fb_.drawPixel(x + col, y + row, color);
            }
        }
    }
}

void ScannerView::textBig(int x, int y, const char* s, uint16_t color) {
    for (; *s; ++s, x += FONT_8X16_WIDTH) {
        const char c = *s;
        if (c < 0x20 || c > 0x7E) continue;
        const uint8_t* glyph = font8x16[c - 0x20];
        for (int row = 0; row < FONT_8X16_HEIGHT; ++row) {
            const uint8_t line = glyph[row];
            if (!line) continue;
            for (int col = 0; col < FONT_8X16_WIDTH; ++col) {
                if (line & (0x80 >> col)) fb_.drawPixel(x + col, y + row, color);
            }
        }
    }
}

int ScannerView::textWidth(const char* s) const {
    return static_cast<int>(std::strlen(s)) * kAdvance;
}

void ScannerView::rule(int y) {
    fb_.fillRect(0, y, kW, 1, kDim);
    // Small end notches, a nod to tape-deck panel seams.
    fb_.fillRect(0, y - 2, 1, 5, kDim);
    fb_.fillRect(kW - 1, y - 2, 1, 5, kDim);
    fb_.fillRect(kW / 2, y + 1, 1, 2, kFaint);
}

void ScannerView::hatch(int x, int y, int w, int h, uint16_t color) {
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            if (((xx + yy) & 3) < 2) fb_.drawPixel(xx, yy, color);
        }
    }
}

// Price-tag label pointing left, 10 px tall (cy-4 .. cy+5) so the 7 px
// digits drawn at cy-3 keep a pixel of air above and below.
void ScannerView::tag(int x, int cy, int w, bool filled, uint16_t color) {
    const int top = cy - 4;
    constexpr int h = 10;
    for (int r = 0; r < h; ++r) {
        const int inset = std::abs(2 * r - (h - 1)) / 2;
        const int from = x + inset;
        if (filled) {
            fb_.fillRect(from, top + r, x + w - from, 1, color);
        } else {
            fb_.drawPixel(from, top + r, color);
        }
    }
    if (!filled) {
        fb_.fillRect(x + 4, top, w - 4, 1, color);
        fb_.fillRect(x + 4, top + h - 1, w - 4, 1, color);
        fb_.fillRect(x + w - 1, top, 1, h, color);
    }
}

void ScannerView::segmentBar(int x, int y, int w, int h, float fraction,
                             uint16_t on, uint16_t off) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    const int segments = w / 3;
    const int lit = static_cast<int>(std::lround(fraction * segments));
    for (int i = 0; i < segments; ++i) {
        fb_.fillRect(x + i * 3, y, 2, h, i < lit ? on : off);
    }
}

void ScannerView::brackets(int x, int y, int w, int h, uint16_t color) {
    const int l = 5;
    fb_.fillRect(x, y, l, 1, color);
    fb_.fillRect(x, y, 1, l, color);
    fb_.fillRect(x + w - l, y, l, 1, color);
    fb_.fillRect(x + w - 1, y, 1, l, color);
    fb_.fillRect(x, y + h - 1, l, 1, color);
    fb_.fillRect(x, y + h - l, 1, l, color);
    fb_.fillRect(x + w - l, y + h - 1, l, 1, color);
    fb_.fillRect(x + w - 1, y + h - l, 1, l, color);
}

// ---------------------------------------------------------------------------
// Main screen

void ScannerView::draw(const ScannerSnapshot& snap) {
    fb_.clear(kBg);
    drawTopBar(snap);
    rule(kRule1);
    if (snap.state == ScannerState::Error) {
        const char* msg = snap.error.empty() ? "SDR FAULT" : snap.error.c_str();
        textBig((kW - static_cast<int>(std::strlen(msg)) * 8) / 2, 60, msg, kAlert);
        text((kW - textWidth("TAB: OTHER PAGE   HOLD ESC: EXIT")) / 2, 84,
             "TAB: OTHER PAGE   HOLD ESC: EXIT", kDim);
        return;
    }
    drawSignals(snap);
    rule(kRule2);
    drawScope(snap);
    rule(kRule3);
    drawParams(snap);
}

void ScannerView::drawTopBar(const ScannerSnapshot& snap) {
    // State chip.
    const char* label = stateLabel(snap.state);
    uint16_t chip = kAmber;
    if (snap.state == ScannerState::Lock) chip = kHi;
    if (snap.state == ScannerState::Hang) chip = kMid;
    if (snap.state == ScannerState::Hold || snap.state == ScannerState::Error) chip = kAlert;
    const int chip_w = textWidth(label) + 6;
    fb_.fillRect(1, 1, chip_w, 9, chip);
    text(4, 2, label, kBg);

    int x = chip_w + 6;
    int live = 0;
    for (const ScannerRow& row : snap.rows) {
        if (row.age_s < 5.0 && !row.locked_out) ++live;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d SIG", live);
    text(x, 2, buf, kAmber);
    x += textWidth(buf) + 8;

    if (!snap.toast.empty()) {
        const int tw = textWidth(snap.toast.c_str());
        const int tx = std::max(x, 184 - tw / 2 - 30);
        fb_.fillRect(tx - 3, 1, tw + 6, 9, kGhost);
        text(tx, 2, snap.toast.c_str(), kHi);
    }

    // Right side: battery then clock.
    int rx = kW - 2 - textWidth("00:00:00");
    text(rx, 2, snap.clock.c_str(), kAmber);
    if (snap.battery_percent >= 0) {
        std::snprintf(buf, sizeof(buf), "%d%%", snap.battery_percent);
        const int bw = textWidth(buf);
        const int bx = rx - 8 - bw;
        text(bx, 2, buf, snap.battery_percent <= 15 ? kAlert : kMid);
        // Battery glyph.
        const int gx = bx - 15;
        fb_.drawRect(gx, 2, 11, 7, kMid);
        fb_.fillRect(gx + 11, 4, 1, 3, kMid);
        const int fill = std::clamp(snap.battery_percent * 9 / 100, 0, 9);
        fb_.fillRect(gx + 1, 3, fill, 5, snap.battery_percent <= 15 ? kAlert : kMid);
    }
}

void ScannerView::drawSignals(const ScannerSnapshot& snap) {
    // Header: section name and the history time axis.
    text(kIdX, 13, "ID", kDim);
    fb_.fillRect(kStripX, 20, kStripR - kStripX, 1, kFaint);
    for (int s = 0; s <= kSlots; s += 5) {
        const int tx = kStripR - s * kSlotW - 1;
        fb_.fillRect(tx, 18, 1, 2, kDim);
    }
    text(kStripX + 1, 12, "20S", kDim);
    text(kStripR - 10 * kSlotW - 10, 12, "10S", kDim);
    text(kStripR - 12, 12, "0S", kDim);
    text(kTagX + 8, 13, "MHZ", kDim);
    text(kInfoX, 13, "LEVEL / AGE", kDim);

    // Frequency scale for the whole scan range (top = highest).
    const int scale_top = kListTop;
    const int scale_bot = kListTop + ScannerView::kVisibleRows * kRowH - 2;
    fb_.fillRect(kScaleX, scale_top, 1, scale_bot - scale_top + 1, kDim);
    for (int q = 0; q <= 4; ++q) {
        const int y = scale_top + q * (scale_bot - scale_top) / 4;
        fb_.fillRect(kScaleX - 3, y, 3, 1, q == 0 || q == 4 ? kMid : kDim);
    }
    const double span = std::max<double>(1.0, double(snap.stop_hz) - double(snap.start_hz));
    const auto scale_y = [&](uint32_t hz) {
        const double t = std::clamp((double(snap.stop_hz) - hz) / span, 0.0, 1.0);
        return scale_top + static_cast<int>(std::lround(t * (scale_bot - scale_top)));
    };

    // Sweep cursor on the scale while scanning.
    if (snap.state == ScannerState::Scan && snap.sweep_hz) {
        const int y = scale_y(snap.sweep_hz);
        fb_.fillRect(kScaleX - 4, y - 1, 4, 3, kHi);
    }

    const int n = static_cast<int>(snap.rows.size());
    if (n == 0) {
        const char* a = snap.state == ScannerState::Scan ? "LISTENING..." : "NO SIGNALS";
        text(kTagX + 4, 50, a, kMid);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "THRESHOLD %+.0f DB", snap.threshold_db);
        text(kTagX + 4, 62, buf, kDim);
        // Idle history lanes.
        for (int r = 0; r < kVisibleRows; ++r) {
            const int cy = kListTop + r * kRowH + 4;
            for (int x = kStripX; x < kStripR; x += 3) fb_.drawPixel(x, cy, kGhost);
        }
        return;
    }

    // Keep the tuned row on screen.
    int first = 0;
    int tuned_index = -1;
    for (int i = 0; i < n; ++i) {
        if (snap.rows[i].tuned) tuned_index = i;
    }
    if (n > kVisibleRows) {
        if (tuned_index >= 0) first = tuned_index - kVisibleRows / 2;
        first = std::clamp(first, 0, n - kVisibleRows);
    }
    const int last = std::min(n, first + kVisibleRows);
    if (first > 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "+%d", first);
        text(kScaleX - 3 - textWidth(buf) - 2, kListTop - 1, buf, kMid);
    }
    if (last < n) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "+%d", n - last);
        text(kScaleX - 3 - textWidth(buf) - 2, scale_bot - 7, buf, kMid);
    }

    // Faint ticks for every signal, including the ones scrolled off.
    for (const ScannerRow& row : snap.rows) {
        fb_.fillRect(kScaleX + 1, scale_y(row.hz), 2, 1, kDim);
    }

    for (int i = first; i < last; ++i) {
        const ScannerRow& row = snap.rows[i];
        const int cy = kListTop + (i - first) * kRowH + 4;
        const bool fresh = row.age_s < 2.0;
        uint16_t ink = row.tuned ? kHi : (fresh ? kAmber : kDim);
        if (row.locked_out) ink = kAlertDim;

        // ID.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u", row.id % 1000);
        text(kIdX, cy - 3, buf, row.tuned ? kAmber : kDim);

        // History lane: dotted baseline, hatched where the signal was heard.
        for (int x = kStripX; x < kStripR; x += 3) fb_.drawPixel(x, cy, kFaint);
        const uint16_t lane = row.locked_out ? kAlertDim : (row.tuned ? kAmber : kMid);
        // Each run of heard seconds becomes one framed, hatched block.
        for (int s = 0; s < kSlots;) {
            if (!((row.heard >> s) & 1u)) {
                ++s;
                continue;
            }
            int e = s;
            while (e + 1 < kSlots && ((row.heard >> (e + 1)) & 1u)) ++e;
            const int x0 = kStripR - (e + 1) * kSlotW;
            const int w = (e - s + 1) * kSlotW;
            fb_.fillRect(x0, cy - 3, w, 7, kBg);
            hatch(x0 + 1, cy - 2, w - 2, 5, lane);
            fb_.drawRect(x0, cy - 3, w, 7, lane);
            s = e + 1;
        }
        if (row.tuned && snap.active) {
            // Live end-cap knob, as on the reference panel.
            fb_.fillRect(kStripR, cy - 2, 3, 5, kHi);
            fb_.fillRect(kStripR + 3, cy, kScaleX - kStripR - 6, 1, kAmber);
        }

        // Leader from the scale position to the tag.
        // Leader: out from the scale, a diagonal, then into the tag tip.
        const int py = scale_y(row.hz);
        const uint16_t lead = row.tuned ? kAmber : kDim;
        const int bend0 = kScaleX + 4;
        const int bend1 = kTagX - 5;
        fb_.fillRect(kScaleX + 1, py, bend0 - kScaleX, 1, lead);
        fb_.drawLine(bend0, py, bend1, cy, lead);
        fb_.fillRect(bend1, cy, kTagX - bend1, 1, lead);

        // Frequency tag.
        const std::string f = mhz(row.hz);
        if (row.tuned) {
            tag(kTagX, cy, kTagW, true, snap.state == ScannerState::Hold ? kAlert : kHi);
            text(kTagX + 7, cy - 3, f.c_str(), kBg);
        } else {
            tag(kTagX, cy, kTagW, false, ink);
            text(kTagX + 7, cy - 3, f.c_str(), ink);
            if (row.locked_out) fb_.fillRect(kTagX + 6, cy + 1, kTagW - 9, 1, kAlert);
        }

        // Right column.
        if (row.locked_out) {
            text(kInfoX, cy - 3, "LOCKED OUT", kAlertDim);
        } else if (row.tuned) {
            text(kInfoX, cy - 3, "RF", kMid);
            segmentBar(kInfoX + 13, cy - 3, 48, 7, snap.snr_db / 40.0f, kHi, kGhost);
            text(kInfoX + 64, cy - 3, "AF", kMid);
            segmentBar(kInfoX + 77, cy - 3, 45, 7, snap.af_level, kHi, kGhost);
        } else {
            std::snprintf(buf, sizeof(buf), "SNR%3.0f", row.snr_db);
            text(kInfoX, cy - 3, buf, ink);
            if (fresh) {
                std::snprintf(buf, sizeof(buf), "DUR%5.1fS", row.duration_s);
            } else if (row.age_s < 100.0) {
                std::snprintf(buf, sizeof(buf), "%3.0fS AGO", row.age_s);
            } else {
                std::snprintf(buf, sizeof(buf), "%3.0fM AGO", row.age_s / 60.0);
            }
            text(kInfoX + 48, cy - 3, buf, fresh ? kMid : kDim);
        }
    }
}

void ScannerView::drawScope(const ScannerSnapshot& snap) {
    // Tuned frequency readout.
    char buf[40];
    const bool sweeping = snap.state == ScannerState::Scan;
    const uint32_t hz = sweeping ? snap.sweep_hz : snap.tuned_hz;
    uint16_t big = kHi;
    if (sweeping) big = kDim;
    else if (snap.state == ScannerState::Hang) big = kMid;
    else if (snap.state == ScannerState::Hold) big = snap.active ? kHi : kAmber;
    const std::string f = hz ? mhz(hz) : std::string("---.---");
    textBig(3, 103, f.c_str(), big);
    text(3 + static_cast<int>(f.size()) * 8 + 2, 111, "MHZ", kDim);

    // Mode chip + id.
    const char* mode = scanner::modeName(snap.mode);
    const int mw = textWidth(mode) + 4;
    fb_.drawRect(3, 121, mw, 10, kMid);
    text(5, 122, mode, kAmber);
    if (!sweeping && snap.tuned_id) {
        std::snprintf(buf, sizeof(buf), "SIG %u", snap.tuned_id);
        text(3 + mw + 5, 122, buf, kAmber);
    }

    // Status line.
    switch (snap.state) {
        case ScannerState::Scan:
            if (snap.all_mode && !snap.band.empty()) {
                std::snprintf(buf, sizeof(buf), "%.10s %3d%%", snap.band.c_str(),
                              snap.sweep_progress);
            } else {
                std::snprintf(buf, sizeof(buf), "SWEEP %3d%%", snap.sweep_progress);
            }
            text(3, 134, buf, kMid);
            break;
        case ScannerState::Hang:
            std::snprintf(buf, sizeof(buf), "NEXT IN %.1fS", std::max(0.0, snap.hang_left_s));
            text(3, 134, buf, kMid);
            break;
        default:
            std::snprintf(buf, sizeof(buf), "SNR %2.0f  %.1fS", std::max(0.0f, snap.snr_db),
                          snap.duration_s);
            text(3, 134, buf, snap.active ? kAmber : kDim);
            break;
    }

    // Oscilloscope.
    const int cx0 = kScopeX + 2;
    const int cw = kScopeW - 4;
    const int mid = kScopeY + kScopeH / 2;
    brackets(kScopeX, kScopeY, kScopeW, kScopeH, kDim);
    text(kScopeX + 4, kScopeY + 2, "OSC", kDim);
    for (int x = cx0; x < cx0 + cw; x += 4) fb_.drawPixel(x, mid, kFaint);
    for (int d = 1; d <= 3; ++d) {
        const int x = cx0 + d * cw / 4;
        for (int y = kScopeY + 3; y < kScopeY + kScopeH - 3; y += 4) fb_.drawPixel(x, y, kGhost);
    }

    const bool live = !sweeping && snap.active && !snap.scope.empty();
    if (live) {
        const int amp = kScopeH / 2 - 3;
        const size_t count = snap.scope.size();
        int px = cx0;
        int py = mid;
        for (int x = 0; x < cw; ++x) {
            const size_t idx = static_cast<size_t>(x) * count / static_cast<size_t>(cw);
            const float v = std::clamp(snap.scope[idx], -1.0f, 1.0f);
            const int y = mid - static_cast<int>(std::lround(v * amp));
            if (x > 0) fb_.drawLine(px, py, cx0 + x, y, snap.muted ? kDim : kAmber);
            px = cx0 + x;
            py = y;
        }
    } else {
        fb_.fillRect(cx0, mid, cw, 1, kDim);
        if (sweeping) {
            const int x = cx0 + snap.sweep_progress * (cw - 1) / 100;
            fb_.fillRect(x - 1, mid - 3, 3, 7, kHi);
            for (int t = 1; t < 10; ++t) {
                const int tx = x - t * 3;
                if (tx >= cx0) fb_.drawPixel(tx, mid - 1, t < 5 ? kMid : kFaint);
            }
        }
    }
    if (snap.muted) text(kScopeX + kScopeW - 4 - textWidth("MUTE"), kScopeY + 2, "MUTE", kAlert);
}

void ScannerView::drawParams(const ScannerSnapshot& snap) {
    char buf[48];
    const int y1 = 147;
    const int y2 = 158;

    text(2, y1, "RNG", kDim);
    if (snap.all_mode) {
        std::snprintf(buf, sizeof(buf), "ALL %d PRESETS", snap.band_count);
    } else {
        std::snprintf(buf, sizeof(buf), "%s-%s", mhz(snap.start_hz).c_str(),
                      mhz(snap.stop_hz).c_str());
    }
    text(24, y1, buf, kAmber);
    int x = 24 + textWidth(buf) + 6;
    const std::string& label = snap.all_mode ? snap.band : snap.preset;
    if (!label.empty()) {
        std::snprintf(buf, sizeof(buf), "[%s]", label.c_str());
        text(x, y1, buf, kHi);
    }
    text(232, y1, "LOCKOUT", kDim);
    std::snprintf(buf, sizeof(buf), "%d", snap.lockout_count);
    text(278, y1, buf, snap.lockout_count ? kAlert : kAmber);

    text(2, y2, "GAIN", kDim);
    if (snap.gain_tenths < 0) {
        std::snprintf(buf, sizeof(buf), "AUTO");
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", snap.gain_tenths / 10.0);
    }
    text(31, y2, buf, kAmber);
    text(64, y2, "THR", kDim);
    std::snprintf(buf, sizeof(buf), "%.0fDB", snap.threshold_db);
    text(87, y2, buf, kAmber);
    text(118, y2, "HANG", kDim);
    std::snprintf(buf, sizeof(buf), "%.0fS", snap.hang_s);
    text(146, y2, buf, kAmber);
    text(170, y2, "FLR", kDim);
    std::snprintf(buf, sizeof(buf), "%.0f", snap.floor_db);
    text(193, y2, buf, kAmber);
    text(kW - 2 - textWidth("H:HELP"), y2, "H:HELP", kMid);
}

// ---------------------------------------------------------------------------
// Overlays

void ScannerView::drawHelp() {
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) fb_.blendPixel(x, y, kBg, 0.94f);
    }
    fb_.drawRect(4, 3, kW - 8, kH - 6, kDim);
    brackets(4, 3, kW - 8, kH - 6, kAmber);
    const char* title = "SCANNER KEYS";
    fb_.fillRect((kW - textWidth(title)) / 2 - 4, 3, textWidth(title) + 8, 1, kBg);
    text((kW - textWidth(title)) / 2, 0 + 7, title, kHi);

    static const char* const rows[][2] = {
        {"SPACE", "HOLD / RESUME SCAN"},
        {"ENTER", "SKIP THIS SIGNAL"},
        {"UP/DOWN", "PICK SIGNAL (HOLDS IT)"},
        {"LEFT/RIGHT", "THRESHOLD -/+ 1 DB"},
        {"L", "LOCK OUT / RESTORE FREQ"},
        {"BACKSPACE", "CLEAR ALL LOCKOUTS"},
        {"D", "MODE AM / NFM"},
        {"G", "GAIN AUTO / MANUAL STEPS"},
        {"T", "HANG TIME BEFORE MOVING ON"},
        {"M", "MUTE"},
        {"R", "PRESETS / ALL / ADD / RANGE"},
        {"TAB  FN+D/S", "PAGE MENU   VOLUME"},
        {"FN+P  HOLD ESC", "SCREENSHOT  EXIT"},
        {"H", "CLOSE"},
    };
    int y = 19;
    for (const auto& row : rows) {
        text(14, y, row[0], kHi);
        text(110, y, row[1], kMid);
        y += 10;
    }
}

void ScannerView::drawEditor(const ScannerEditorState& ed) {
    fb_.clear(kBg);
    brackets(2, 2, kW - 4, kH - 4, kAmber);
    const char* title = "SCAN RANGE";
    textBig((kW - static_cast<int>(std::strlen(title)) * 8) / 2, 8, title, kHi);
    rule(28);

    const auto bigRow = [&](int index, int y, const char* label, const std::string& value,
                            const char* unit) {
        const bool active = ed.field == index;
        if (active) {
            fb_.fillRect(10, y - 3, kW - 20, 20, kGhost);
            fb_.fillRect(10, y - 3, 2, 20, kHi);
        }
        text(20, y + 4, label, active ? kHi : kDim);
        textBig(96, y, value.c_str(), active ? kHi : kAmber);
        if (unit) text(96 + static_cast<int>(value.size()) * 8 + 6, y + 6, unit, kDim);
    };
    const auto smallRow = [&](int index, int y, const char* label, const std::string& value,
                              const char* unit) {
        const bool active = ed.field == index;
        if (active) {
            fb_.fillRect(10, y - 2, kW - 20, 11, kGhost);
            fb_.fillRect(10, y - 2, 2, 11, kHi);
        }
        text(20, y, label, active ? kHi : kDim);
        const std::string shown = active ? "< " + value + " >" : value;
        text(96, y, shown.c_str(), active ? kHi : kAmber);
        if (unit) text(96 + textWidth(shown.c_str()) + 6, y, unit, kDim);
    };

    bigRow(0, 36, "PRESET", "< " + ed.preset + " >", nullptr);

    const char* help1;
    const char* help2;
    if (ed.all) {
        text(20, 62, "SWEEPS EVERY PRESET IN TURN, EACH", kDim);
        text(20, 72, "WITH ITS OWN MODE AND STEP:", kDim);
        int x = 20;
        int y = 86;
        for (const auto& name : ed.all_names) {
            const int w = textWidth(name.c_str());
            if (x + w > kW - 14) {
                x = 20;
                y += 10;
            }
            if (y > 116) break;
            text(x, y, name.c_str(), kAmber);
            x += w + 12;
        }
        help1 = "LEFT/RIGHT PRESET";
        help2 = "ENTER SCAN ALL   ESC CANCEL";
    } else {
        bigRow(1, 60, "START", ed.start, "MHZ");
        bigRow(2, 82, "STOP", ed.stop, "MHZ");
        smallRow(3, 104, "MODE", scanner::modeName(ed.mode), nullptr);
        smallRow(4, 116, "STEP", ed.step, "KHZ");
        help1 = "UP/DOWN FIELD  LEFT/RIGHT CHANGE";
        help2 = ed.adding ? "DIGITS . BKSP  ENTER SAVE+SCAN  ESC"
                          : "DIGITS . BKSP  ENTER SCAN  ESC";
    }

    text(12, 134, help1, kDim);
    text(12, 144, help2, kDim);
    if (!ed.error.empty()) {
        text(12, 156, ed.error.c_str(), kAlert);
    } else if (!ed.info.empty()) {
        text(12, 156, ed.info.c_str(), ed.info_alert ? kAlert : kMid);
    }
}
