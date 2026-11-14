#include "core/page_manager.h"
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

        // Q still exits immediately; ESC requires a three second hold so a
        // stray tap cannot close the viewer.
        if (key == APPKEY_Q) {
            std::cout << "PageManager: Exit key pressed, stopping..." << std::endl;
            running_ = false;
            continue;
        }
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
        } else if (isPageSwitchKey(key)) {
            if (key == APPKEY_TAB) {
                nextPage();
            } else if (key == APPKEY_P) {
                nextPage();  // P key also switches pages
            }
        } else if (key != APPKEY_NONE) {
            // Route input to current page
            IPage* current_page = getCurrentPage();
            if (current_page) {
                current_page->handleInput(key);
            }
        }

        // Update current page (frozen while the exit prompt is up).
        IPage* current_page = getCurrentPage();
        if (current_page && !exit_hold_active_) {
            current_page->update(delta_time);
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

float PageManager::calculateDeltaTime() {
    struct timeval now;
    gettimeofday(&now, nullptr);

    float delta = (now.tv_sec - last_update_time_.tv_sec) +
                  (now.tv_usec - last_update_time_.tv_usec) / 1000000.0f;

    last_update_time_ = now;
    return delta;
}
