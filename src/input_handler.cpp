#include "input_handler.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <glob.h>

InputHandler::InputHandler() : kbd_fd(-1), initialized(false),
                                last_nav_key(APPKEY_NONE), key_is_held(false) {
    key_press_time.tv_sec = 0;
    key_press_time.tv_usec = 0;
}

InputHandler::~InputHandler() {
    if (initialized) {
        restore();
    }
}

bool InputHandler::init(const char* device_path) {
    if (device_path == nullptr) {
        const char* try_paths[] = {
            "/dev/input/by-id/usb-XING_WEI_2.4G_USB_USB_Composite_Device-if02-event-kbd",
            "/dev/input/event0",
            "/dev/input/event1",
            "/dev/input/event2",
            nullptr
        };

        for (int i = 0; try_paths[i] != nullptr; i++) {
            kbd_fd = open(try_paths[i], O_RDONLY | O_NONBLOCK);
            if (kbd_fd >= 0) {
                std::cout << "Keyboard input: " << try_paths[i] << std::endl;
                break;
            }
        }
    } else {
        kbd_fd = open(device_path, O_RDONLY | O_NONBLOCK);
    }

    if (kbd_fd < 0) {
        std::cerr << "Failed to open keyboard device" << std::endl;
        return false;
    }

    initialized = true;
    return true;
}

void InputHandler::restore() {
    if (initialized && kbd_fd >= 0) {
        close(kbd_fd);
        kbd_fd = -1;
        initialized = false;
    }
}

AppKeyCode InputHandler::pollKey() {
    if (!initialized) return APPKEY_NONE;

    struct input_event ev;
    static bool shift_pressed = false;

    ssize_t n = read(kbd_fd, &ev, sizeof(ev));
    if (n != sizeof(ev)) {
        return APPKEY_NONE;
    }

    if (ev.type != EV_KEY) {
        return APPKEY_NONE;
    }

    // Track shift state (press and repeat)
    if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
        shift_pressed = (ev.value == 1 || ev.value == 2);
        return APPKEY_NONE;
    }

    AppKeyCode code = translateKeyCode(ev.code, shift_pressed);

    if (ev.value == 1) {
        // Fresh key press — start hold timer for nav keys
        bool is_nav = (code == APPKEY_LEFT || code == APPKEY_RIGHT ||
                       code == APPKEY_PAGEUP || code == APPKEY_PAGEDOWN);
        if (is_nav) {
            last_nav_key = code;
            key_is_held = true;
            gettimeofday(&key_press_time, nullptr);
        } else {
            last_nav_key = APPKEY_NONE;
            key_is_held = false;
        }
        return code;
    } else if (ev.value == 2) {
        // Kernel auto-repeat — fire nav keys only
        bool is_nav = (code == APPKEY_LEFT || code == APPKEY_RIGHT ||
                       code == APPKEY_PAGEUP || code == APPKEY_PAGEDOWN);
        if (is_nav) {
            last_nav_key = code;
            key_is_held = true;
            return code;
        }
        return APPKEY_NONE;
    } else {
        // Key release
        if (code == last_nav_key) {
            last_nav_key = APPKEY_NONE;
            key_is_held = false;
        }
        return APPKEY_NONE;
    }
}

int InputHandler::getHoldDuration(AppKeyCode key) {
    if (!key_is_held || last_nav_key != key) return 0;

    struct timeval now;
    gettimeofday(&now, nullptr);
    long ms = (now.tv_sec - key_press_time.tv_sec) * 1000
            + (now.tv_usec - key_press_time.tv_usec) / 1000;
    return (int)ms;
}

AppKeyCode InputHandler::translateKeyCode(int linux_keycode, bool shift_pressed) {
    switch (linux_keycode) {
        case KEY_UP:       return APPKEY_UP;
        case KEY_DOWN:     return APPKEY_DOWN;

        // Left/Right: Shift+Left/Right = PageUp/PageDown
        case KEY_LEFT:     return shift_pressed ? APPKEY_PAGEDOWN : APPKEY_LEFT;
        case KEY_RIGHT:    return shift_pressed ? APPKEY_PAGEUP : APPKEY_RIGHT;

        // Keep original Page Up/Down for compatibility
        case KEY_PAGEUP:   return APPKEY_PAGEUP;
        case KEY_PAGEDOWN: return APPKEY_PAGEDOWN;

        case KEY_EQUAL:    return shift_pressed ? APPKEY_PLUS : APPKEY_NONE;
        case KEY_KPPLUS:   return APPKEY_PLUS;
        case KEY_MINUS:    return APPKEY_MINUS;
        case KEY_KPMINUS:  return APPKEY_MINUS;

        case KEY_M:        return APPKEY_M;
        case KEY_A:        return APPKEY_A;
        case KEY_Q:        return APPKEY_Q;
        case KEY_ESC:      return APPKEY_ESC;
        case KEY_H:        return APPKEY_H;
        case KEY_R:        return shift_pressed ? APPKEY_RES        : APPKEY_NONE;
        case KEY_B:        return shift_pressed ? APPKEY_BW         : APPKEY_NONE;
        case KEY_S:        return shift_pressed ? APPKEY_SCREENSHOT : APPKEY_NONE;

        // Number keys (top row, not numpad)
        case KEY_0:        return APPKEY_0;
        case KEY_1:        return APPKEY_1;
        case KEY_2:        return APPKEY_2;
        case KEY_3:        return APPKEY_3;
        case KEY_4:        return APPKEY_4;
        case KEY_5:        return APPKEY_5;
        case KEY_6:        return APPKEY_6;
        case KEY_7:        return APPKEY_7;
        case KEY_8:        return APPKEY_8;
        case KEY_9:        return APPKEY_9;

        case KEY_DOT:      return APPKEY_DOT;
        case KEY_ENTER:    return APPKEY_ENTER;
        case KEY_BACKSPACE: return APPKEY_BACKSPACE;

        default:           return APPKEY_NONE;
    }
}
