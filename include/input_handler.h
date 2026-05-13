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
    APPKEY_Q,
    APPKEY_ESC,
    APPKEY_H,
    APPKEY_RES,  // Shift+R: cycle FFT resolution
    APPKEY_BW,   // Shift+B: cycle display bandwidth
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
    AppKeyCode pollKey();

    // Get hold duration in milliseconds for navigation keys (0 if not held)
    int getHoldDuration(AppKeyCode key);

private:
    int kbd_fd;
    bool initialized;

    AppKeyCode translateKeyCode(int linux_keycode, bool shift_pressed);

    // Track key hold state
    AppKeyCode last_nav_key;
    struct timeval key_press_time;
    bool key_is_held;
};

#endif // INPUT_HANDLER_H
