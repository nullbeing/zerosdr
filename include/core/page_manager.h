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

    // Tab opens a translucent page menu. The page frame is frozen while the
    // menu is up; arrows/Tab move the highlight, Enter switches, Esc closes.
    struct PageMenuEntry {
        int page_index;
        bool radar_shortcut;
        int row;
    };
    bool page_menu_open_ = false;
    std::vector<PageMenuEntry> page_menu_entries_;
    size_t page_menu_selection_ = 0;
    std::vector<uint16_t> page_menu_snapshot_;

    // fn+D / fn+S: give the launcher's own binding a moment to take effect;
    // if the sink volume did not move, apply the step ourselves.
    bool volume_key_pending_ = false;
    int volume_key_delta_ = 0;
    int volume_key_before_ = -1;
    struct timeval volume_key_deadline_ {};

    // Fn+P is a global shortcut: the frame is saved after the page (or the
    // menu/exit overlay) has finished drawing, so the PNG matches the screen.
    bool screenshot_requested_ = false;

    // Brief "SCREENSHOT SAVED" confirmation drawn after the page frame.
    bool toast_active_ = false;
    std::vector<uint16_t> toast_snapshot_;
    struct timeval toast_until_ {};

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
    void drawScreenshotToast();
    void drawCircleOutline(int cx, int cy, int radius, uint16_t color);

    // Tab page menu.
    void openPageMenu();
    void closePageMenu(bool restore);
    void handlePageMenuInput(AppKeyCode key);
    void drawPageMenu();
    void drawPageIcon(int page_index, int x, int y, uint16_t color);
    void drawRadarIcon(int cx, int cy, uint16_t color);
    void restoreSnapshot(const std::vector<uint16_t>& snapshot);
};

#endif // PAGE_MANAGER_H
