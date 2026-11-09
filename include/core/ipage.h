#ifndef IPAGE_H
#define IPAGE_H

#include "input_handler.h"

/**
 * IPage - Abstract interface for all application pages
 *
 * Each page (SDR, ADSB, Menu, etc.) implements this interface to provide
 * a consistent lifecycle and interaction model.
 */
class IPage {
public:
    virtual ~IPage() = default;

    /**
     * Initialize the page (called once at startup)
     * @return true if initialization suc false otherwise
     */
    virtual bool init() = 0;

    /**
     * Clean up page resources (called once at shutdown)
     */
    virtual void cleanup() = 0;

    /**
     * Called when this page becomes the active page
     * Use this to configure hardware, reset state, etc.
     */
    virtual void onEnter() = 0;

    /**
     * Called when this page is about to become inactive
     * Use this to save state, release hardware, etc.
     */
    virtual void onExit() = 0;

    /**
     * Main update loop (called every frame while page is active)
     * @param delta_time Time elapsed since lasin seconds)
     */
    virtual void update(float delta_time) = 0;

    /**
     * Handle input event
     * @param key The key code that was pressed
     * @return true if the key was handled, false otherwise
     */
    virtual bool handleInput(AppKeyCode key) = 0;

    /**
     * Get the page name (for display and identification)
     * @return Page name string
     */
    virtual const char* getName() const = 0;

    /**
     * Get a brief description of the page
     * @return Description string
     */
    virtual const char* getDescription() const = 0;
};

#endif // IPAGE_H
