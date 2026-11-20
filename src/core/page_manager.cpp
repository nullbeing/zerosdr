#include "core/page_manager.h"
#include "system_volume.h"
#include "font.h"
#include <iostream>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>

PageManager::PageManager(Framebuffer& fb, InputHandler& input)
    : fb_(fb), input_(input), current_page_index_(0), running_(false) {
    gettimeofday(&last_update_time_, nullptr);
}

PageManager::~PageManager() {
    // Cleanup all pages
    if (!pages_.empty() && current_page_index_ < pages_.size()) {
        pages_[current_page_index_]->onExit();
        pages_[current_page_index_]->cleanup();
    }
}

void PageManager::registerPage(std::unique_ptr<IPage> page) {
    if (!page) {
        std::cerr << "PageManager: Cannot register null page" << std::endl;
        return;
    }

    std::cout << "PageManager: Registering page '" << page->getName() << "'" << std::endl;

    // Initialize the page
    if (!page->init()) {
        std::cerr << "PageManager: Failed to initialize page '" << page->getName() << "'" << std::endl;
        return;
    }

    pages_.push_back(std::move(page));

    // If this is the first page, activate it
    if (pages_.size() == 1) {
        current_page_index_ = 0;
        pages_[0]->onEnter();
        std::cout << "PageManager: Activated first page '" << pages_[0]->getName() << "'" << std::endl;
    }
}

bool PageManager::switchToPage(const std::string& page_name) {
    // Find page by name
    for (size_t i = 0; i < pages_.size(); i++) {
        if (page_name == pages_[i]->getName()) {
            if (i != current_page_index_) {
                transitionToPage(i);
            }
            return true;
        }
    }

    std::cerr << "PageManager: Page '" << page_name << "' not found" << std::endl;
    return false;
}

void PageManager::nextPage() {
    if (pages_.empty()) return;

    size_t next_index = (current_page_index_ + 1) % pages_.size();
    transitionToPage(next_index);
}

void PageManager::previousPage() {
    if (pages_.empty()) return;

    size_t prev_index = (current_page_index_ + pages_.size() - 1) % pages_.size();
    transitionToPage(prev_index);
}

IPage* PageManager::getCurrentPage() const {
    if (pages_.empty() || current_page_index_ >= pages_.size()) {
        return nullptr;
    }
    return pages_[current_page_index_].get();
}

const char* PageManager::getCurrentPageName() const {
    IPage* page = getCurrentPage();
    return page ? page->getName() : "";
}

int PageManager::run() {
    if (pages_.empty()) {
        std::cerr << "PageManager: No pages registered" << std::endl;
        return 1;
    }

    std::cout << "PageManager: Starting main loop with " << pages_.size() << " page(s)" << std::endl;
    running_ = true;
    gettimeofday(&last_update_time_, nullptr);

    while (running_) {
        float delta_time = calculateDeltaTime();

        // Handle input
        AppKeyCode key = input_.pollKey();

        // ESC requires a two second hold so a stray tap cannot close the
        // viewer; there is no single-key quit.
        if (key == APPKEY_ESC && !exit_hold_active_) {
            exit_hold_active_ = true;
            gettimeofday(&exit_hold_start_, nullptr);
            // Freeze the page frame so the translucent prompt does not
            // flicker between page redraws, and so repeated blends do not
            // accumulate on the back buffer.
            exit_snapshot_.resize(fb_.pixelCount());
            std::memcpy(exit_snapshot_.data(), fb_.pixels(),
                        exit_snapshot_.size() * sizeof(uint16_t));
        }

        // Global volume keys stay live even while the page menu is open.
        if (key == APPKEY_V || key == APPKEY_C) {
            if (!volume_key_pending_) {
                // Read the live value so a change made from the launcher does
                // not look like a reaction to this key press.
                const int current = systemVolumeRefresh();
                volume_key_before_ =
                    current >= 0 ? current : systemVolumePercent();
                volume_key_pending_ = true;
            }
            volume_key_delta_ += (key == APPKEY_V) ? 10 : -10;
            gettimeofday(&volume_key_deadline_, nullptr);
            volume_key_deadline_.tv_usec += 300000;
            if (volume_key_deadline_.tv_usec >= 1000000) {
                volume_key_deadline_.tv_usec -= 1000000;
                volume_key_deadline_.tv_sec += 1;
            }
        }

        if (page_menu_open_) {
            // The menu owns the key while it is up; the page below is frozen.
            handlePageMenuInput(key);
        } else {
            if (key == APPKEY_ESC && !exit_hold_active_) {
                exit_hold_active_ = true;
                gettimeofday(&exit_hold_start_, nullptr);
                // Freeze the page frame so the translucent prompt does not
                // flicker between page redraws, and so repeated blends do not
                // accumulate on the back buffer.
                exit_snapshot_.resize(fb_.pixelCount());
                std::memcpy(exit_snapshot_.data(), fb_.pixels(),
                            exit_snapshot_.size() * sizeof(uint16_t));
            }

            // Check for global page-switching keys
            if (key == APPKEY_ESC) {
                // Swallow the press; the hold is handled after the page update.
            } else if (!exit_hold_active_ && isPageSwitchKey(key)) {
                openPageMenu();
            } else if (key != APPKEY_NONE) {
                // Route input to current page
                IPage* current_page = getCurrentPage();
                if (current_page) {
                    current_page->handleInput(key);
                }
            }
        }

        // Update current page (frozen while an overlay is up).
        IPage* current_page = getCurrentPage();
        if (current_page && !exit_hold_active_ && !page_menu_open_) {
            current_page->update(delta_time);
        }

        // Resolve a pending volume key once the launcher has had its chance.
        if (volume_key_pending_) {
            struct timeval now;
            gettimeofday(&now, nullptr);
            const bool due =
                now.tv_sec > volume_key_deadline_.tv_sec ||
                (now.tv_sec == volume_key_deadline_.tv_sec &&
                 now.tv_usec >= volume_key_deadline_.tv_usec);
            if (due) {
                volume_key_pending_ = false;
                int percent = systemVolumeRefresh();
                if (percent >= 0 && percent == volume_key_before_) {
                    // The launcher did not react: step the sink ourselves.
                    percent = systemVolumeStep(volume_key_delta_);
                }
                volume_key_delta_ = 0;
                if (percent >= 0) {
                    std::cout << "Volume: " << percent << "% (system)"
                              << std::endl;
                } else {
                    std::cerr << "Volume: system volume unavailable"
                              << std::endl;
                }
                if (current_page) current_page->onVolumeChanged(percent);
            }
        }

        // Hold-to-exit overlay and timing.
        if (exit_hold_active_) {
            if (!input_.escHeld()) {
                exit_hold_active_ = false;
                // Put the frozen page back before resuming updates.
                if (!exit_snapshot_.empty()) {
                    std::memcpy(fb_.pixels(), exit_snapshot_.data(),
                                exit_snapshot_.size() * sizeof(uint16_t));
                }
                // A short tap still reaches the page (cancel frequency input,
                // leave the aircraft detail view, ...).
                if (current_page) {
                    current_page->handleInput(APPKEY_ESC);
                    current_page->update(0.05f);
                }
            } else {
                struct timeval now;
                gettimeofday(&now, nullptr);
                const float held =
                    (now.tv_sec - exit_hold_start_.tv_sec) +
                    (now.tv_usec - exit_hold_start_.tv_usec) / 1000000.0f;
                if (held >= 2.0f) {
                    std::cout << "PageManager: Exit confirmed, stopping..."
                              << std::endl;
                    running_ = false;
                } else {
                    exit_snapshot_.resize(fb_.pixelCount());
                    std::memcpy(fb_.pixels(), exit_snapshot_.data(),
                                exit_snapshot_.size() * sizeof(uint16_t));
                    drawExitPrompt(held / 2.0f);
                    fb_.update();
                }
            }
        }

        // Page menu: redraw the frozen frame plus the translucent bar every
        // loop so repeated blends never pile up.
        if (page_menu_open_) {
            restoreSnapshot(page_menu_snapshot_);
            drawPageMenu();
            fb_.update();
        }

        // Frame rate control (~33 FPS)
        usleep(30000);
    }

    std::cout << "PageManager: Main loop stopped" << std::endl;
    return 0;
}

void PageManager::stop() {
    std::cout << "PageManager: Stopping main loop" << std::endl;
    running_ = false;
}

bool PageManager::isPageSwitchKey(AppKeyCode key) const {
    return (key == APPKEY_TAB || key == APPKEY_P);
}

void PageManager::transitionToPage(size_t new_index) {
    if (new_index >= pages_.size()) {
        std::cerr << "PageManager: Invalid page index " << new_index << std::endl;
        return;
    }

    if (new_index == current_page_index_) {
        return;  // Already on this page
    }

    std::cout << "PageManager: Switching from '" << pages_[current_page_index_]->getName()
              << "' to '" << pages_[new_index]->getName() << "'" << std::endl;

    // Exit current page
    pages_[current_page_index_]->onExit();

    // Clear screen for transition
    fb_.clear(COLOR_BG_DARK);
    fb_.update();

    // Switch to new page
    current_page_index_ = new_index;

    // Enter new page
    pages_[current_page_index_]->onEnter();
}

void PageManager::drawText6x8(int x, int y, const char* text,
                              uint16_t color) {
    for (const char* p = text; *p != '\0'; ++p) {
        const int index = static_cast<int>(*p) - 32;
        if (index < 0 || index >= 95) {
            x += FONT_6X8_WIDTH;
            continue;
        }
        for (int row = 0; row < FONT_6X8_HEIGHT; ++row) {
            const uint8_t bits = font6x8[index][row];
            for (int col = 0; col < FONT_6X8_WIDTH; ++col) {
                if (bits & (0x80 >> col)) {
                    fb_.drawPixel(x + col, y + row, color);
                }
            }
        }
        x += FONT_6X8_WIDTH;
    }
}

void PageManager::drawRoundedPanel(int x, int y, int w, int h, int radius,
                                   uint16_t color, float alpha) {
    if (w <= 0 || h <= 0) return;
    radius = std::max(0, std::min(radius, std::min(w / 2, h / 2)));
    for (int row = 0; row < h; ++row) {
        int inset = 0;
        if (row < radius) {
            const float dy = radius - (row + 0.5f);
            inset = radius - static_cast<int>(std::lround(
                                 std::sqrt(radius * radius - dy * dy)));
        } else if (row >= h - radius) {
            const float dy = (row + 0.5f) - (h - radius);
            inset = radius - static_cast<int>(std::lround(
                                 std::sqrt(radius * radius - dy * dy)));
        }
        for (int col = 0; col < w - 2 * inset; ++col) {
            fb_.blendPixel(x + inset + col, y + row, color, alpha);
        }
    }
}

void PageManager::drawExitPrompt(float progress) {
    const int w = fb_.getWidth();
    const int h = fb_.getHeight();
    if (w <= 0 || h <= 0) return;

    progress = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);

    // Dim the page, then lay a translucent panel on top of it.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            fb_.blendPixel(x, y, COLOR_BLACK, 0.45f);
        }
    }

    const int panel_w = 224;
    const int panel_h = 46;
    const int px = (w - panel_w) / 2;
    const int py = (h - panel_h) / 2;
    drawRoundedPanel(px, py, panel_w, panel_h, 9, COLOR_DIALOG_BG, 0.82f);

    const char* title = "HOLD ESC 2S TO EXIT";
    const int title_w = static_cast<int>(std::strlen(title)) * FONT_6X8_WIDTH;
    drawText6x8(px + (panel_w - title_w) / 2, py + 10, title,
                COLOR_TEXT_BRIGHT);

    const int bar_x = px + 16;
    const int bar_y = py + 30;
    const int bar_w = panel_w - 32;
    fb_.fillRect(bar_x, bar_y, bar_w, 4, COLOR_GRID);
    const int filled = static_cast<int>(bar_w * progress);
    if (filled > 0) {
        fb_.fillRect(bar_x, bar_y, filled, 4, COLOR_SPECTRUM);
    }
}

void PageManager::drawCircleOutline(int cx, int cy, int radius,
                                    uint16_t color) {
    if (radius <= 0) return;
    int x = radius;
    int y = 0;
    int err = 1 - radius;
    while (x >= y) {
        fb_.drawPixel(cx + x, cy + y, color);
        fb_.drawPixel(cx + y, cy + x, color);
        fb_.drawPixel(cx - y, cy + x, color);
        fb_.drawPixel(cx - x, cy + y, color);
        fb_.drawPixel(cx - x, cy - y, color);
        fb_.drawPixel(cx - y, cy - x, color);
        fb_.drawPixel(cx + y, cy - x, color);
        fb_.drawPixel(cx + x, cy - y, color);
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

void PageManager::restoreSnapshot(const std::vector<uint16_t>& snapshot) {
    if (snapshot.empty()) return;
    if (snapshot.size() != fb_.pixelCount()) return;
    std::memcpy(fb_.pixels(), snapshot.data(),
                snapshot.size() * sizeof(uint16_t));
}

void PageManager::openPageMenu() {
    if (pages_.empty()) return;

    page_menu_entries_.clear();
    for (int i = 0; i < (int)pages_.size(); ++i) {
        page_menu_entries_.push_back({i, false});
    }

    // Second row: a direct radar shortcut when the ADSB page exists.
    int adsb_index = -1;
    for (int i = 0; i < (int)pages_.size(); ++i) {
        if (std::strcmp(pages_[i]->getName(), "ADSB") == 0) {
            adsb_index = i;
            break;
        }
    }
    if (adsb_index >= 0) {
        page_menu_entries_.push_back({adsb_index, true});
    }

    page_menu_selection_ = 0;
    for (size_t i = 0; i < page_menu_entries_.size(); ++i) {
        if (!page_menu_entries_[i].radar_shortcut &&
            page_menu_entries_[i].page_index == (int)current_page_index_) {
            page_menu_selection_ = i;
            break;
        }
    }

    page_menu_snapshot_.resize(fb_.pixelCount());
    std::memcpy(page_menu_snapshot_.data(), fb_.pixels(),
                page_menu_snapshot_.size() * sizeof(uint16_t));
    page_menu_open_ = true;
    std::cout << "PageManager: Page menu open" << std::endl;
}

void PageManager::closePageMenu(bool restore) {
    page_menu_open_ = false;
    if (restore) {
        restoreSnapshot(page_menu_snapshot_);
        fb_.update();
    }
    page_menu_snapshot_.clear();
}

void PageManager::handlePageMenuInput(AppKeyCode key) {
    const size_t count = page_menu_entries_.size();
    if (count == 0) {
        closePageMenu(false);
        return;
    }
    const size_t page_count = pages_.size();
    const bool has_shortcut = (count > page_count);

    switch (key) {
        case APPKEY_TAB:
        case APPKEY_P:
        case APPKEY_RIGHT:
            page_menu_selection_ = (page_menu_selection_ + 1) % count;
            break;

        case APPKEY_LEFT:
            page_menu_selection_ = (page_menu_selection_ + count - 1) % count;
            break;

        case APPKEY_DOWN:
            // Row 0 (pages) -> row 1 (radar shortcut).
            if (has_shortcut && page_menu_selection_ < page_count) {
                page_menu_selection_ = page_count;
            }
            break;

        case APPKEY_UP:
            // Row 1 -> the page above the shortcut.
            if (has_shortcut && page_menu_selection_ >= page_count) {
                page_menu_selection_ = page_count - 1;
            }
            break;

        case APPKEY_ENTER: {
            const PageMenuEntry entry = page_menu_entries_[page_menu_selection_];
            if (!entry.radar_shortcut &&
                entry.page_index == (int)current_page_index_) {
                closePageMenu(true);
                break;
            }

            page_menu_open_ = false;
            page_menu_snapshot_.clear();
            if (entry.page_index != (int)current_page_index_) {
                transitionToPage((size_t)entry.page_index);
            }
            if (entry.radar_shortcut) {
                pages_[(size_t)entry.page_index]->onShortcut("radar");
            }
            break;
        }

        case APPKEY_ESC:
            closePageMenu(true);
            break;

        default:
            break;
    }
}

void PageManager::drawPageIcon(int page_index, int x, int y,
                               uint16_t color) {
    if (page_index < 0 || page_index >= (int)pages_.size()) return;
    const char* name = pages_[page_index]->getName();

    if (std::strcmp(name, "SDR") == 0) {
        // Mini spectrum with a waterfall underneath.
        static const uint8_t bars[9] = {2, 4, 6, 9, 5, 8, 3, 7, 4};
        const int base = y + 10;
        for (int i = 0; i < 9; ++i) {
            fb_.fillRect(x + 1 + i * 2, base - bars[i], 1, bars[i], color);
        }
        fb_.fillRect(x, y + 13, 18, 1, color);
        fb_.fillRect(x, y + 16, 18, 1, color);
    } else if (std::strcmp(name, "Spectrum") == 0) {
        // Wideband trace with a baseline.
        static const int points[][2] = {
            {0, 11}, {2, 9}, {4, 12}, {6, 7}, {8, 10},
            {10, 4}, {12, 8}, {14, 2}, {17, 6},
        };
        constexpr int count = (int)(sizeof(points) / sizeof(points[0]));
        for (int i = 0; i + 1 < count; ++i) {
            fb_.drawLine(x + points[i][0], y + points[i][1],
                         x + points[i + 1][0], y + points[i + 1][1], color);
        }
        fb_.fillRect(x, y + 14, 18, 1, color);
        fb_.fillRect(x + 14, y + 1, 2, 2, color);
    } else if (std::strcmp(name, "ADSB") == 0) {
        // Radar ring with a small aircraft.
        drawCircleOutline(x + 9, y + 9, 8, color);
        fb_.fillRect(x + 9, y + 4, 1, 8, color);
        fb_.fillRect(x + 4, y + 7, 11, 1, color);
        fb_.fillRect(x + 6, y + 12, 7, 1, color);
    } else {
        fb_.drawRect(x + 1, y + 1, 16, 16, color);
    }
}

void PageManager::drawRadarIcon(int cx, int cy, uint16_t color) {
    drawCircleOutline(cx, cy, 8, color);
    // Sweep line and a couple of blips.
    fb_.drawLine(cx, cy, cx + 6, cy - 6, color);
    fb_.drawLine(cx, cy, cx + 3, cy - 7, color);
    fb_.drawPixel(cx - 4, cy + 2, color);
    fb_.drawPixel(cx + 2, cy + 4, color);
    fb_.drawPixel(cx, cy, color);
}

void PageManager::drawPageMenu() {
    const int w = fb_.getWidth();
    const int h = fb_.getHeight();
    if (w <= 0 || h <= 0 || pages_.empty()) return;

    // Dim the frozen page, then lay the translucent bar on top.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            fb_.blendPixel(x, y, COLOR_BLACK, 0.45f);
        }
    }

    const int page_count = (int)pages_.size();
    const bool has_shortcut =
        page_menu_entries_.size() > (size_t)page_count;
    const int tile_w = 70;
    const int tile_h = 46;
    const int gap = 6;
    const int panel_w = page_count * tile_w + (page_count - 1) * gap + 18;
    const int panel_h = has_shortcut ? 122 : 74;
    const int px = (w - panel_w) / 2;
    const int py = (h - panel_h) / 2;
    drawRoundedPanel(px, py, panel_w, panel_h, 10, COLOR_DIALOG_BG, 0.86f);

    int x = px + 9;
    const int tile_y = py + 6;
    for (int i = 0; i < page_count; ++i) {
        const bool selected =
            (page_menu_entries_.size() > (size_t)i &&
             page_menu_selection_ == (size_t)i);
        const bool current = (i == (int)current_page_index_);

        if (selected) {
            drawRoundedPanel(x, tile_y, tile_w, tile_h, 7,
                             COLOR_SELECTED, 0.95f);
            fb_.fillRect(x + 6, tile_y, tile_w - 12, 1, COLOR_SPECTRUM);
        }

        const uint16_t icon_color =
            selected ? COLOR_SPECTRUM : COLOR_TEXT_DIM;
        drawPageIcon(i, x + (tile_w - 18) / 2, tile_y + 5, icon_color);

        const char* name = pages_[i]->getName();
        const int label_w = (int)std::strlen(name) * FONT_6X8_WIDTH;
        drawText6x8(x + (tile_w - label_w) / 2, tile_y + 30, name,
                    selected ? COLOR_WHITE : COLOR_TEXT_DIM);

        if (current) {
            fb_.fillRect(x + tile_w / 2 - 1, tile_y + tile_h - 5, 2, 2,
                         selected ? COLOR_SPECTRUM : COLOR_TEXT_BRIGHT);
        }
        x += tile_w + gap;
    }

    // Second row: radar shortcut.
    if (has_shortcut) {
        const bool selected = (page_menu_selection_ >= (size_t)page_count);
        const int rx = px + (panel_w - tile_w) / 2;
        const int ry = tile_y + tile_h + 6;

        if (selected) {
            drawRoundedPanel(rx, ry, tile_w, tile_h, 7,
                             COLOR_SELECTED, 0.95f);
            fb_.fillRect(rx + 6, ry, tile_w - 12, 1, COLOR_SPECTRUM);
        }

        const uint16_t icon_color =
            selected ? COLOR_SPECTRUM : COLOR_TEXT_DIM;
        drawRadarIcon(rx + tile_w / 2, ry + 14, icon_color);

        const char* label = "RADAR";
        const int label_w = (int)std::strlen(label) * FONT_6X8_WIDTH;
        drawText6x8(rx + (tile_w - label_w) / 2, ry + 30, label,
                    selected ? COLOR_WHITE : COLOR_TEXT_DIM);
    }

    const char* hint = "<> SELECT  UP/DOWN ROW  ENTER  ESC";
    const int hint_w = (int)std::strlen(hint) * FONT_6X8_WIDTH;
    drawText6x8((w - hint_w) / 2, py + panel_h - 14, hint,
                COLOR_TEXT_DIM);
}

float PageManager::calculateDeltaTime() {
    struct timeval now;
    gettimeofday(&now, nullptr);

    float delta = (now.tv_sec - last_update_time_.tv_sec) +
                  (now.tv_usec - last_update_time_.tv_usec) / 1000000.0f;

    last_update_time_ = now;
    return delta;
}
