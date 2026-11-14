#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <linux/input.h>
#include <sys/time.h>

enum AppKeyCode {
    APPKEY_NONE = 0,
    APPKEY_UP,
    APPKEY_DOWN,
    APPKEY_LEFT,
    APPKEY_RIGHT,
    APPKEY_PAGEUP,
    APPKEY_PAGEDOWN,
    APPKEY_PLUS,
    APPKEY_MINUS,
    APPKEY_M,
    APPKEY_A,
    APPKEY_G,      // Toggle hardware AGC
    APPKEY_D,      // Toggle demod mode
    APPKEY_V,      // Volume up
    APPKEY_C,      // Volume down
    APPKEY_S,      // Adjust squelch
    APPKEY_E,      // Cycle FM deemphasis (50/75us)
    APPKEY_T,      // Cycle tuning step (Auto / 100Hz / 1k / 5k / 10k / 100k / 1M)
    APPKEY_SPACE,  // Toggle audio
    APPKEY_LEFT_COARSE,   // Shift+Left  = current step × 10 (coarse tune)
    APPKEY_RIGHT_COARSE,  // Shift+Right = current step × 10 (coarse tune)
    APPKEY_Q,
    APPKEY_ESC,
    APPKEY_H,
    APPKEY_L,      // Location settings (ADSB)
    APPKEY_R,    // Retry (for device detection)
    APPKEY_TAB,  // Page switching
    APPKEY_P,    // Page switching (alternative)
    APPKEY_RES,  // Shift+R: cycle FFT resolution
    APPKEY_BW,   // Shift+B: cycle demodulation bandwidth
    APPKEY_ZOOM, // Shift+Z: cycle display zoom
    APPKEY_SCREENSHOT,  // Shift+S: take screenshot
    APPKEY_ENTER,
    APPKEY_BACKSPACE,
    APPKEY_DOT,
    APPKEY_0, APPKEY_1, APPKEY_2, APPKEY_3, APPKEY_4,
    APPKEY_5, APPKEY_6, APPKEY_7, APPKEY_8, APPKEY_9
};

class InputHandler {
public:
    InputHandler();
    ~InputHandler();

    bool init(const char* device_path = nullptr);
    void restore();
    bool isReady() const { return initialized; }
    AppKeyCode pollKey();

    // Get hold duration in milliseconds for navigation keys (0 if not held)
    int getHoldDuration(AppKeyCode key);

    // Get the hold duration of the last released key
    int getLastReleasedHoldDuration(AppKeyCode key);

    // True while the ESC key is physically down (used for hold-to-exit).
    bool escHeld() const { return esc_held_; }

private:
    int kbd_fd;
    bool initialized;
    bool cardputer_zero_;
    bool esc_held_ = false;

    AppKeyCode translateKeyCode(int linux_keycode, bool shift_pressed);
    AppKeyCode translateCardputerZero(int linux_keycode, bool shift_pressed);

    // Track key hold state
    AppKeyCode last_nav_key;
    struct timeval key_press_time;
    bool key_is_held;

    // Track last released key hold duration
    AppKeyCode last_released_key;
    int last_released_hold_ms;
};

#endif // INPUT_HANDLER_H
