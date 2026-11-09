#include "core/page_manager.h"
#include <iostream>
#include <sys/time.h>
#include <unistd.h>
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

        // Check for global exit keys (Q and ESC)
        if (key == APPKEY_Q || key == APPKEY_ESC) {
            std::cout << "PageManager: Exit key pressed, stopping..." << std::endl;
            running_ = false;
            continue;
        }

        // Check for global page-switching keys
        if (isPageSwitchKey(key)) {
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

        // Update current page
        IPage* current_page = getCurrentPage();
        if (current_page) {
            current_page->update(delta_time);
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

float PageManager::calculateDeltaTime() {
    struct timeval now;
    gettimeofday(&now, nullptr);

    float delta = (now.tv_sec - last_update_time_.tv_sec) +
                  (now.tv_usec - last_update_time_.tv_usec) / 1000000.0f;

    last_update_time_ = now;
    return delta;
}
