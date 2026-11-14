#ifndef PAGE_MANAGER_H
#define PAGE_MANAGER_H

#include "ipage.h"
#include "framebuffer.h"
#include "input_handler.h"
#include <vector>
#include <memory>
#include <string>

/**
 * PageManager - Manages application pages and handles page transitions
 *
 * Responsibilities:
 * - Register and store pages
 * - Switch between pages
 * - Route input events to active page
 * - Handle global shortcuts (page switching)
 */
class PageManager {
public:
    PageManager(Framebuffer& fb, InputHandler& input);
    ~PageManager();

    /**
     * Register a new page
     * @param page Unique pointer to the page
     */
    void registerPage(std::unique_ptr<IPage> page);

    /**
     * Switch to a specific page by name
     * @param page_name Name of the page to switch to
     * @return true if switch was successful, false if page not found
     */
    bool switchToPage(const std::string& page_name);

    /**
     * Switch to the next page in the list (circular)
     */
    void nextPage();

    /**
     * Switch to the previous page in the list (circular)
     */
    void previousPage();

    /**
     * Get the currently active page
     * @return Pointer to current page, or nullptr if no pages registered
     */
    IPage* getCurrentPage() const;

    /**
     * Get the name of the currently active page
     * @return Page name, or empty string if no active page
     */
    const char* getCurrentPageName() const;

    /**
     * Main loop - runs until stop() is called
     * @return Exit code (0 for success)
     */
    int run();

    /**
     * Stop the main loop
     */
    void stop();

    /**
     * Check if a key is a global page-switching shortcut
     * @param key The key code to check
     * @return true if it's a page switch key
     */
    bool isPageSwitchKey(AppKeyCode key) const;

private:
    Framebuffer& fb_;
    InputHandler& input_;
    std::vector<std::unique_ptr<IPage>> pages_;
    size_t current_page_index_;
    bool running_;

    // Timing
    struct timeval last_update_time_;

    // Hold-to-exit: ESC must be held for three seconds, with an on-screen
    // semi-transparent prompt.
    bool exit_hold_active_ = false;
    struct timeval exit_hold_start_ {};
    std::vector<uint16_t> exit_snapshot_;

    /**
     * Perform page transition
     * @param new_index Index of the page to switch to
     */
    void transitionToPage(size_t new_index);

    /**
     * Calculate delta time since last update
     * @return Delta time in seconds
     */
    float calculateDeltaTime();

    // Centered translucent exit prompt with a progress bar.
    void drawExitPrompt(float progress);
    void drawRoundedPanel(int x, int y, int w, int h, int radius,
                          uint16_t color, float alpha);
    void drawText6x8(int x, int y, const char* text, uint16_t color);
};

#endif // PAGE_MANAGER_H
