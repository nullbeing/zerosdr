#include "core/page_manager.h"
#include "screenshot.h"
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

        // Fn+P works on every page and over every overlay. Swallow the key
        // here and save once the frame below has been rendered.
        if (key == APPKEY_SCREENSHOT) {
            screenshot_requested_ = true;
            key = APPKEY_NONE;
        }

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

        // Update current page (frozen while an overlay or toast is up).
        IPage* current_page = getCurrentPage();
        if (current_page && !toast_active_ && !exit_hold_active_ &&
            !page_menu_open_) {
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

        if (screenshot_requested_) {
            screenshot_requested_ = false;
            const std::string path = makeScreenshotPath();
            if (fb_.saveScreenshot(path.c_str())) {
                std::cout << "Screenshot saved: " << path << std::endl;
                toast_snapshot_.assign(fb_.pixels(),
                                       fb_.pixels() + fb_.pixelCount());
                toast_active_ = true;
                gettimeofday(&toast_until_, nullptr);
                toast_until_.tv_usec += 1200000;
                if (toast_until_.tv_usec >= 1000000) {
                    toast_until_.tv_usec -= 1000000;
                    toast_until_.tv_sec += 1;
                }
            } else {
                std::cerr << "Screenshot failed" << std::endl;
            }
        }

        if (toast_active_) {
            struct timeval now;
            gettimeofday(&now, nullptr);
            if (now.tv_sec > toast_until_.tv_sec ||
                (now.tv_sec == toast_until_.tv_sec &&
                 now.tv_usec >= toast_until_.tv_usec)) {
                toast_active_ = false;
                toast_snapshot_.clear();
            } else {
                // Keep the frozen page under the toast; redrawing the toast
                // over the same frame would accumulate its alpha.
                if (toast_snapshot_.size() == fb_.pixelCount()) {
                    std::memcpy(fb_.pixels(), toast_snapshot_.data(),
                                toast_snapshot_.size() * sizeof(uint16_t));
                }
                drawScreenshotToast();
                fb_.update();
            }
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
    return key == APPKEY_TAB;
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

void PageManager::drawScreenshotToast() {
    const char* text = "SCREENSHOT SAVED";
    const int text_w = static_cast<int>(std::strlen(text)) * FONT_6X8_WIDTH;
    const int w = text_w + 10;
    const int h = FONT_6X8_HEIGHT + 6;
    const int x = (fb_.getWidth() - w) / 2;
    const int y = (fb_.getHeight() - h) / 2;
    // Rounded, translucent, no border, centered on the screen.
    drawRoundedPanel(x, y, w, h, 5, COLOR_DIALOG_BG, 0.78f);
    drawText6x8(x + 5, y + 3, text, COLOR_TEXT_BRIGHT);
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

    int adsb_index = -1;
    int ais_index = -1;
    for (int i = 0; i < (int)pages_.size(); ++i) {
        if (std::strcmp(pages_[i]->getName(), "ADSB") == 0) adsb_index = i;
        if (std::strcmp(pages_[i]->getName(), "AIS") == 0) ais_index = i;
    }

    // First row: SDR, Spectrum, RFI, ADSB, ACARS. AIS moves to the second
    // row, after the RADAR shortcut, so the first row keeps five tiles.
    for (int i = 0; i < (int)pages_.size(); ++i) {
        if (i == ais_index && adsb_index >= 0) continue;
        page_menu_entries_.push_back({i, false, 0});
    }
    if (adsb_index >= 0) {
        page_menu_entries_.push_back({adsb_index, true, 1});
        if (ais_index >= 0) {
            page_menu_entries_.push_back({ais_index, false, 1});
        }
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

    const auto row_entries = [&](int row) {
        std::vector<size_t> indices;
        for (size_t i = 0; i < count; ++i) {
            if (page_menu_entries_[i].row == row) indices.push_back(i);
        }
        return indices;
    };
    const auto position_in_row = [&](size_t selection) {
        const int row = page_menu_entries_[selection].row;
        const auto indices = row_entries(row);
        for (size_t i = 0; i < indices.size(); ++i) {
            if (indices[i] == selection) return i;
        }
        return size_t{0};
    };

    switch (key) {
        case APPKEY_TAB:
            // Tab alone walks through every entry, across both rows.
            page_menu_selection_ = (page_menu_selection_ + 1) % count;
            break;

        case APPKEY_RIGHT: {
            const int row = page_menu_entries_[page_menu_selection_].row;
            const auto indices = row_entries(row);
            size_t pos = position_in_row(page_menu_selection_);
            pos = (pos + 1) % indices.size();
            page_menu_selection_ = indices[pos];
            break;
        }

        case APPKEY_LEFT: {
            const int row = page_menu_entries_[page_menu_selection_].row;
            const auto indices = row_entries(row);
            size_t pos = position_in_row(page_menu_selection_);
            pos = (pos + indices.size() - 1) % indices.size();
            page_menu_selection_ = indices[pos];
            break;
        }

        case APPKEY_DOWN:
        case APPKEY_UP: {
            const int row = page_menu_entries_[page_menu_selection_].row;
            const int target_row = (row == 0) ? 1 : 0;
            const auto target = row_entries(target_row);
            if (!target.empty()) {
                size_t pos = position_in_row(page_menu_selection_);
                if (pos >= target.size()) pos = target.size() - 1;
                page_menu_selection_ = target[pos];
            }
            break;
        }

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
    } else if (std::strcmp(name, "RFI") == 0) {
        fb_.drawLine(x, y + 14, x + 17, y + 14, color);
        fb_.drawLine(x, y + 7, x + 17, y + 7, color);
        fb_.drawLine(x + 8, y + 2, x + 8, y + 17, color);
        fb_.drawLine(x + 5, y + 5, x + 8, y + 2, color);
        fb_.drawLine(x + 11, y + 5, x + 8, y + 2, color);
    } else if (std::strcmp(name, "AIS") == 0) {
        // Anchor: ring with a vertical shank and two curved flukes.
        drawCircleOutline(x + 9, y + 10, 7, color);
        fb_.drawLine(x + 9, y + 3, x + 9, y + 16, color);
        fb_.drawLine(x + 5, y + 6, x + 13, y + 6, color);
        fb_.drawLine(x + 5, y + 13, x + 3, y + 9, color);
        fb_.drawLine(x + 13, y + 13, x + 15, y + 9, color);
    } else if (std::strcmp(name, "ACARS") == 0) {
        // Radio mast with two side waves.
        fb_.drawLine(x + 9, y + 16, x + 9, y + 7, color);
        fb_.drawLine(x + 4, y + 16, x + 14, y + 16, color);
        fb_.drawLine(x + 9, y + 7, x + 5, y + 11, color);
        fb_.drawLine(x + 9, y + 7, x + 13, y + 11, color);
        fb_.drawLine(x + 2, y + 8, x + 2, y + 12, color);
        fb_.drawLine(x + 16, y + 8, x + 16, y + 12, color);
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

    // Dim the frozen page, then lay the translucent panel on top.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            fb_.blendPixel(x, y, COLOR_BLACK, 0.45f);
        }
    }

    int row0_count = 0;
    int row1_count = 0;
    for (const auto& entry : page_menu_entries_) {
        if (entry.row == 0) ++row0_count;
        else ++row1_count;
    }

    const int tile_h = 46;
    const int gap = 5;
    const int columns = std::max(row0_count, row1_count);
    int tile_w = 70;
    if (columns > 4) {
        // Five columns: 46 px tiles plus four 5 px gaps and 18 px padding
        // still fit inside 320 px (56 px each).
        tile_w = std::max(46, (w - 18 - (columns - 1) * gap) / columns);
    }
    const int panel_w = columns * tile_w + (columns - 1) * gap + 18;
    const int panel_h = (row1_count > 0) ? 124 : 74;
    const int px = (w - panel_w) / 2;
    const int py = (h - panel_h) / 2;
    drawRoundedPanel(px, py, panel_w, panel_h, 10, COLOR_DIALOG_BG, 0.86f);

    const auto draw_tile = [&](size_t index, int x, int y) {
        const PageMenuEntry& entry = page_menu_entries_[index];
        const bool selected = (page_menu_selection_ == index);
        const bool current = !entry.radar_shortcut &&
                             entry.page_index == (int)current_page_index_;

        if (selected) {
            drawRoundedPanel(x, y, tile_w, tile_h, 7,
                             COLOR_SELECTED, 0.95f);
            fb_.fillRect(x + 6, y, tile_w - 12, 1, COLOR_SPECTRUM);
        }

        const uint16_t icon_color =
            selected ? COLOR_SPECTRUM : COLOR_TEXT_DIM;
        if (entry.radar_shortcut) {
            drawRadarIcon(x + tile_w / 2, y + 14, icon_color);
        } else {
            drawPageIcon(entry.page_index, x + (tile_w - 18) / 2, y + 5,
                         icon_color);
        }

        const char* label = entry.radar_shortcut
                                ? "RADAR"
                                : pages_[(size_t)entry.page_index]->getName();
        const int label_w = (int)std::strlen(label) * FONT_6X8_WIDTH;
        drawText6x8(x + (tile_w - label_w) / 2, y + 30, label,
                    selected ? COLOR_WHITE : COLOR_TEXT_DIM);

        if (current) {
            fb_.fillRect(x + tile_w / 2 - 1, y + tile_h - 5, 2, 2,
                         selected ? COLOR_SPECTRUM : COLOR_TEXT_BRIGHT);
        }
    };

    // First row.
    int x = px + 9;
    const int row0_y = py + 6;
    for (size_t i = 0; i < page_menu_entries_.size(); ++i) {
        if (page_menu_entries_[i].row != 0) continue;
        draw_tile(i, x, row0_y);
        x += tile_w + gap;
    }

    // Second row, centered.
    if (row1_count > 0) {
        const int row1_w = row1_count * tile_w + (row1_count - 1) * gap;
        int rx = (w - row1_w) / 2;
        const int row1_y = row0_y + tile_h + 6;
        for (size_t i = 0; i < page_menu_entries_.size(); ++i) {
            if (page_menu_entries_[i].row != 1) continue;
            draw_tile(i, rx, row1_y);
            rx += tile_w + gap;
        }
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
