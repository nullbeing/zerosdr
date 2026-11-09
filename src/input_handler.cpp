#include "input_handler.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <glob.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <dirent.h>

InputHandler::InputHandler() : kbd_fd(-1), initialized(false), cardputer_zero_(false),
                                last_nav_key(APPKEY_NONE), key_is_held(false),
                                last_released_key(APPKEY_NONE), last_released_hold_ms(0) {
    key_press_time.tv_sec = 0;
    key_press_time.tv_usec = 0;
}

InputHandler::~InputHandler() {
    if (initialized) {
        restore();
    }
}

static bool is_keyboard_device(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    // Check that this device supports EV_KEY events
    unsigned long ev_bits[EV_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
    bool is_kbd = false;

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0) {
        if (ev_bits[EV_KEY / (8 * sizeof(unsigned long))] & (1UL << (EV_KEY % (8 * sizeof(unsigned long))))) {
            // Check it has alphanumeric keys (KEY_A through KEY_Z)
            unsigned long key_bits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
                int key_count = 0;
                for (int k = KEY_1; k <= KEY_0; k++) {
                    if (key_bits[k / (8 * sizeof(unsigned long))] & (1UL << (k % (8 * sizeof(unsigned long)))))
                        key_count++;
                }
                for (int k = KEY_A; k <= KEY_Z; k++) {
                    if (key_bits[k / (8 * sizeof(unsigned long))] & (1UL << (k % (8 * sizeof(unsigned long)))))
                        key_count++;
                }
                // A real keyboard has many keys; mice/touchscreens have few
                is_kbd = (key_count > 10);
            }
        }
    }

    close(fd);
    return is_kbd;
}

static int scan_input_devices(char* out_path, size_t out_size) {
    // First try /dev/input/by-id/ for symbolic names
    const char* scan_dirs[] = {"/dev/input/by-id/", "/dev/input/by-path/", nullptr};
    char buf[512];

    for (int d = 0; scan_dirs[d]; d++) {
        DIR* dir = opendir(scan_dirs[d]);
        if (!dir) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            snprintf(buf, sizeof(buf), "%s%s", scan_dirs[d], entry->d_name);
            if (is_keyboard_device(buf)) {
                snprintf(out_path, out_size, "%s", buf);
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }

    // Fallback: scan event* in /dev/input/
    DIR* dir = opendir("/dev/input/");
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        snprintf(buf, sizeof(buf), "/dev/input/%s", entry->d_name);
        if (is_keyboard_device(buf)) {
            snprintf(out_path, out_size, "%s", buf);
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

bool InputHandler::init(const char* device_path) {
    if (device_path == nullptr) {
        char found_path[512];
        if (scan_input_devices(found_path, sizeof(found_path))) {
            kbd_fd = open(found_path, O_RDONLY | O_NONBLOCK);
            if (kbd_fd >= 0) {
                std::cout << "Keyboard input: " << found_path << std::endl;
            }
        }
    } else {
        kbd_fd = open(device_path, O_RDONLY | O_NONBLOCK);
    }

    if (kbd_fd < 0) {
        std::cerr << "Failed to open keyboard device" << std::endl;
        return false;
    }

    // Cardputer Zero uses a TCA8418 matrix keyboard with a non-PC layout:
    // Z/X/C/D are arrows, B/N/M emit Insert/End/PgDn, and the Shift layer
    // emits keypad/symbol codes. Detect it so we can use a dedicated map.
    char device_name[256] = {0};
    if (ioctl(kbd_fd, EVIOCGNAME(sizeof(device_name) - 1), device_name) >= 0) {
        device_name[sizeof(device_name) - 1] = '\0';
        if (strstr(device_name, "tca8418") != nullptr) {
            cardputer_zero_ = true;
        }
    }

    const char* keymap_override = getenv("ZEROSDR_KEYMAP");
    if (keymap_override && strcmp(keymap_override, "cardputer-zero") == 0) {
        cardputer_zero_ = true;
    }

    std::cout << "Keyboard map: "
              << (cardputer_zero_ ? "Cardputer Zero" : "standard")
              << " (" << (device_name[0] ? device_name : "unknown device") << ")"
              << std::endl;

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
    AppKeyCode result = APPKEY_NONE;
    bool found_event = false;

    // Read ALL available events, keep only the last relevant one
    // This prevents event queue buildup causing drift after key release
    while (true) {
        ssize_t n = read(kbd_fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) {
            break;  // No more events available
        }

        if (ev.type != EV_KEY) {
            continue;  // Skip non-key events
        }

        // Track shift state (press and repeat)
        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift_pressed = (ev.value == 1 || ev.value == 2);
            continue;
        }

        AppKeyCode code = translateKeyCode(ev.code, shift_pressed);

        static const bool debug_keys = (getenv("ZEROSDR_DEBUG_KEYS") != nullptr);
        if (debug_keys) {
            std::cout << "Key event: code=" << ev.code
                      << " value=" << ev.value
                      << " shift=" << shift_pressed
                      << " -> " << static_cast<int>(code) << std::endl;
        }

        if (ev.value == 1) {
            // Fresh key press — start hold timer for nav keys
            bool is_nav = (code == APPKEY_UP || code == APPKEY_DOWN ||
                           code == APPKEY_LEFT || code == APPKEY_RIGHT ||
                           code == APPKEY_LEFT_COARSE || code == APPKEY_RIGHT_COARSE ||
                           code == APPKEY_PAGEUP || code == APPKEY_PAGEDOWN);
            bool is_squelch = (code == APPKEY_S);

            if (is_nav || is_squelch) {
                last_nav_key = code;
                key_is_held = true;
                gettimeofday(&key_press_time, nullptr);
                // Clear previous release record
                last_released_key = APPKEY_NONE;
                last_released_hold_ms = 0;
            } else {
                last_nav_key = APPKEY_NONE;
                key_is_held = false;
            }

            // Don't return S key on press, only on release
            if (code != APPKEY_S) {
                result = code;
                found_event = true;
            }
        } else if (ev.value == 2) {
            // Kernel auto-repeat — fire nav keys only
            bool is_nav = (code == APPKEY_UP || code == APPKEY_DOWN ||
                           code == APPKEY_LEFT || code == APPKEY_RIGHT ||
                           code == APPKEY_LEFT_COARSE || code == APPKEY_RIGHT_COARSE ||
                           code == APPKEY_PAGEUP || code == APPKEY_PAGEDOWN);
            if (is_nav) {
                last_nav_key = code;
                key_is_held = true;
                result = code;
                found_event = true;
            }
            // Don't fire repeat for S key - we only care about hold duration
        } else {
            // Key release - HIGHEST PRIORITY
            if (code == last_nav_key) {
                // Calculate hold duration before resetting
                struct timeval now;
                gettimeofday(&now, nullptr);
                last_released_hold_ms = (now.tv_sec - key_press_time.tv_sec) * 1000
                                      + (now.tv_usec - key_press_time.tv_usec) / 1000;
                last_released_key = code;

                last_nav_key = APPKEY_NONE;
                key_is_held = false;

                // Return key code on release for S key (squelch needs release event)
                if (code == APPKEY_S) {
                    result = code;
                    found_event = true;
                }
            }
            // Don't return key code on release for other keys
            // This prevents double-triggering of toggle keys like G
            // Don't break - continue reading to clear any remaining events
        }
    }

    return found_event ? result : APPKEY_NONE;
}

int InputHandler::getHoldDuration(AppKeyCode key) {
    if (!key_is_held || last_nav_key != key) return 0;

    struct timeval now;
    gettimeofday(&now, nullptr);
    long ms = (now.tv_sec - key_press_time.tv_sec) * 1000
            + (now.tv_usec - key_press_time.tv_usec) / 1000;
    return (int)ms;
}

int InputHandler::getLastReleasedHoldDuration(AppKeyCode key) {
    if (last_released_key == key) {
        return last_released_hold_ms;
    }
    return 0;
}

// Cardputer Zero (TCA8418) key map.
//
// Follows the official key spec (cp0-keys):
//   * plain F / Z / X / C act as Up / Left / Down / Right without fn
//   * fn+<key> emits the standard special codes (Insert, PgUp, PgDn,
//     VolumeUp/Down, Mute, SysRq, Help, ...)
//   * plain letters/digits/Space/Tab/Esc fall through to the regular PC map
//     in translateKeyCode()
AppKeyCode InputHandler::translateCardputerZero(int linux_keycode, bool shift_pressed) {
    switch (linux_keycode) {
        // Arrows: official F/Z/X/C plus the real arrow codes from the fn layer.
        case KEY_UP:
        case KEY_F:
            return APPKEY_UP;
        case KEY_DOWN:
        case KEY_X:
            return APPKEY_DOWN;
        case KEY_LEFT:
        case KEY_Z:
            return shift_pressed ? APPKEY_LEFT_COARSE : APPKEY_LEFT;
        case KEY_RIGHT:
        case KEY_C:
            return shift_pressed ? APPKEY_RIGHT_COARSE : APPKEY_RIGHT;

        // fn layer codes (official combos).
        case KEY_PAGEUP:     return APPKEY_PAGEUP;      // fn+L
        case KEY_PAGEDOWN:   return APPKEY_PAGEDOWN;    // fn+M
        case KEY_VOLUMEUP:   return APPKEY_V;           // fn+D
        case KEY_VOLUMEDOWN: return APPKEY_C;           // fn+S
        case KEY_MUTE:       return APPKEY_SPACE;       // fn+A
        case KEY_PLAYPAUSE:  return APPKEY_SPACE;       // fn+Q
        case KEY_INSERT:     return APPKEY_BW;          // fn+B
        case KEY_SYSRQ:      return APPKEY_SCREENSHOT;  // fn+J (PrintScreen)
        case KEY_HELP:       return APPKEY_H;           // fn+H

        // Letter alias kept from the previous map (no fn needed).
        case KEY_Y:          return APPKEY_ZOOM;        // display zoom
        case KEY_DOT:
        case KEY_KPDOT:      return APPKEY_DOT;

        // Symbol-layer aliases. The TCA8418 sends these raw keycodes for the
        // blue sym layer instead of a real KEY_DOT / Shift+key combination.
        case KEY_HIRAGANA:            // sym + B -> "."
            return APPKEY_DOT;
        case KEY_KATAKANAHIRAGANA:    // sym + M
            return APPKEY_M;
        case KEY_KATAKANA:            // sym + ?
            return APPKEY_ZOOM;
        case KEY_KP4:                 // sym + 4 -> coarse left
            return APPKEY_LEFT_COARSE;
        case KEY_KP6:                 // sym + 6 -> coarse right
            return APPKEY_RIGHT_COARSE;

        default:
            return APPKEY_NONE;
    }
}

AppKeyCode InputHandler::translateKeyCode(int linux_keycode, bool shift_pressed) {
    if (cardputer_zero_) {
        // Volume is fn+D / fn+S only; a bare V or W must not change it.
        if (linux_keycode == KEY_V || linux_keycode == KEY_W) {
            return APPKEY_NONE;
        }
        AppKeyCode cardputer_key = translateCardputerZero(linux_keycode, shift_pressed);
        if (cardputer_key != APPKEY_NONE) {
            return cardputer_key;
        }
    }

    switch (linux_keycode) {
        case KEY_UP:       return APPKEY_UP;
        case KEY_DOWN:     return APPKEY_DOWN;

        // Left/Right: Shift+Left/Right = coarse tune (step × 10) in SDRPage
        case KEY_LEFT:     return shift_pressed ? APPKEY_LEFT_COARSE  : APPKEY_LEFT;
        case KEY_RIGHT:    return shift_pressed ? APPKEY_RIGHT_COARSE : APPKEY_RIGHT;

        // Page Up/Down: 1 MHz jump (or whatever the binding consumer uses)
        case KEY_PAGEUP:   return APPKEY_PAGEUP;
        case KEY_PAGEDOWN: return APPKEY_PAGEDOWN;

        case KEY_EQUAL:    return shift_pressed ? APPKEY_PLUS : APPKEY_NONE;
        case KEY_KPPLUS:   return APPKEY_PLUS;
        case KEY_MINUS:    return APPKEY_MINUS;
        case KEY_KPMINUS:  return APPKEY_MINUS;

        case KEY_M:        return APPKEY_M;
        case KEY_G:        return APPKEY_G;
        case KEY_D:        return APPKEY_D;
        case KEY_V:        return APPKEY_V;
        case KEY_C:        return APPKEY_C;
        case KEY_S:        return shift_pressed ? APPKEY_SCREENSHOT : APPKEY_S;
        case KEY_T:        return APPKEY_T;
        case KEY_SPACE:    return APPKEY_SPACE;
        case KEY_Q:        return APPKEY_Q;
        case KEY_ESC:      return APPKEY_ESC;
        case KEY_H:        return APPKEY_H;
        case KEY_L:        return APPKEY_L;
        case KEY_R:        return shift_pressed ? APPKEY_RES : APPKEY_R;
        case KEY_B:        return APPKEY_BW;
        case KEY_Z:        return shift_pressed ? APPKEY_ZOOM : APPKEY_NONE;

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

        // Page switching keys
        case KEY_TAB:      return APPKEY_TAB;
        case KEY_P:        return APPKEY_P;

        default:           return APPKEY_NONE;
    }
}
