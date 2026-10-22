/*
 * rumble_bridge.c: Lightweight force-feedback bridge for macOS Wine / SDL2.
 *
 * Why this is needed:
 * While SDL2 natively supports haptics for first-party gamepads (Xbox, PlayStation,
 * Nintendo Switch Pro), third-party Bluetooth gamepads (like 8BitDo) report through
 * Darwin's generic IOKit driver, which has no output vibration support in SDL2.
 *
 * This bridge intercepts SDL_JoystickRumble and dispatches to driver handlers
 * defined in s_drivers[] via IOHIDDeviceSetReport. All other gamepads fall
 * through to native SDL2 rumble.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <CoreFoundation/CoreFoundation.h>

#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct{ const void* replacement; const void* replacee; } _interpose_##_replacee \
            __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

#define MAX_SLOTS 4

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id; // Target verified PIDs to avoid interfering with native models
    const char* name;
    void (*send_rumble)(IOHIDDeviceRef dev, uint8_t left, uint8_t right, uint32_t duration_ms);
} RumbleDriver;

// --- Driver Handlers ---

// 8BitDo Bluetooth Gamepad Haptic Protocol (Report ID 0x05)
// Emulates Xbox One MCC Impulse Trigger haptics (matching community X1nput standard):
// Byte 1: Left Heavy Palm Motor (low-frequency concussive rumble, 0..255)
// Byte 2: Right Light Palm Motor (high-frequency weapon cycle buzz, 0..255)
// Byte 3: Left Trigger Motor (heavy impact / plasma charge impulse feedback, 50% scale)
// Byte 4: Right Trigger Motor (weapon fire impulse recoil, 50% scale)
static void rumble_8bitdo(IOHIDDeviceRef dev, uint8_t left, uint8_t right, uint32_t duration_ms) {
    (void)duration_ms;
    uint8_t pkt[5];
    pkt[0] = 0x05;
    pkt[1] = left;
    pkt[2] = right;
    pkt[3] = left >> 1;
    pkt[4] = right >> 1;

    IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x05, pkt, sizeof(pkt));
}

// Table of custom hardware drivers (tested third-party Bluetooth gamepads)
static const RumbleDriver s_drivers[] = {
    { 0x2DC8, 0x202F, "8BitDo Ultimate 3E Controller", rumble_8bitdo },
};
static const int s_driver_count = sizeof(s_drivers) / sizeof(s_drivers[0]);

// --- Core Bridge ---

typedef struct {
    void* joystick;
    IOHIDDeviceRef dev;
    uint8_t last_left;
    uint8_t last_right;
    uint32_t generation;
} DeviceSlot;

static pthread_mutex_t g_bridge_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static IOHIDManagerRef g_mgr = NULL;
static IOHIDDeviceRef g_active_devices[MAX_SLOTS] = { NULL };
static int g_active_count = 0;
static DeviceSlot g_slots[MAX_SLOTS] = {
    { NULL, NULL, 0xFF, 0xFF, 0 },
    { NULL, NULL, 0xFF, 0xFF, 0 },
    { NULL, NULL, 0xFF, 0xFF, 0 },
    { NULL, NULL, 0xFF, 0xFF, 0 },
};

static uint32_t get_device_num_prop(IOHIDDeviceRef dev, CFStringRef key) {
    uint32_t val = 0;
    CFTypeRef prop = IOHIDDeviceGetProperty(dev, key);
    if (prop && CFGetTypeID(prop) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)prop, kCFNumberIntType, &val);
    }
    return val;
}

static void get_device_serial(IOHIDDeviceRef dev, char* buf, size_t max_len) {
    buf[0] = '\0';
    CFTypeRef prop = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDSerialNumberKey));
    if (prop && CFGetTypeID(prop) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)prop, buf, max_len, kCFStringEncodingUTF8);
    }
}

static int compare_devices(const void* a, const void* b) {
    IOHIDDeviceRef dev_a = *(IOHIDDeviceRef*)a;
    IOHIDDeviceRef dev_b = *(IOHIDDeviceRef*)b;
    uint32_t loc_a = get_device_num_prop(dev_a, CFSTR(kIOHIDLocationIDKey));
    uint32_t loc_b = get_device_num_prop(dev_b, CFSTR(kIOHIDLocationIDKey));
    return (loc_a < loc_b) ? -1 : (loc_a > loc_b) ? 1 : 0;
}

static void on_device_removed(void* context, IOReturn result, void* sender, IOHIDDeviceRef dev) {
    (void)context; (void)result; (void)sender;
    pthread_mutex_lock(&g_bridge_lock);
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_slots[i].dev == dev) {
            g_slots[i].dev = NULL;
            g_slots[i].joystick = NULL;
            g_slots[i].last_left = 0xFF;
            g_slots[i].last_right = 0xFF;
            g_slots[i].generation++;
        }
    }
    pthread_mutex_unlock(&g_bridge_lock);
}

static void init_hid_manager(void) {
    if (g_mgr) return;
    g_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_mgr) return;

    // Filter strictly to registered driver vendors and PIDs
    CFMutableArrayRef matching_arr = CFArrayCreateMutable(kCFAllocatorDefault, s_driver_count, &kCFTypeArrayCallBacks);
    for (int i = 0; i < s_driver_count; i++) {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (dict) {
            int vid = s_drivers[i].vendor_id;
            CFNumberRef vid_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
            if (vid_num) {
                CFDictionarySetValue(dict, CFSTR(kIOHIDVendorIDKey), vid_num);
                CFRelease(vid_num);
            }
            if (s_drivers[i].product_id != 0) {
                int pid = s_drivers[i].product_id;
                CFNumberRef pid_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
                if (pid_num) {
                    CFDictionarySetValue(dict, CFSTR(kIOHIDProductIDKey), pid_num);
                    CFRelease(pid_num);
                }
            }
            CFArrayAppendValue(matching_arr, dict);
            CFRelease(dict);
        }
    }
    IOHIDManagerSetDeviceMatchingMultiple(g_mgr, matching_arr);
    CFRelease(matching_arr);

    IOHIDManagerRegisterDeviceRemovalCallback(g_mgr, on_device_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(g_mgr, kIOHIDOptionsTypeNone);
}

static void refresh_devices(void) {
    pthread_mutex_lock(&g_bridge_lock);
    init_hid_manager();
    if (!g_mgr) {
        pthread_mutex_unlock(&g_bridge_lock);
        return;
    }

    CFSetRef set = IOHIDManagerCopyDevices(g_mgr);

    for (int i = 0; i < g_active_count; i++) {
        if (g_active_devices[i]) {
            IOHIDDeviceClose(g_active_devices[i], kIOHIDOptionsTypeNone);
            CFRelease(g_active_devices[i]);
            g_active_devices[i] = NULL;
        }
    }
    g_active_count = 0;

    if (!set) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            g_slots[i].dev = NULL;
        }
        pthread_mutex_unlock(&g_bridge_lock);
        return;
    }

    CFIndex count = CFSetGetCount(set);
    IOHIDDeviceRef devs[count > 0 ? count : 1];
    CFSetGetValues(set, (const void**)devs);

    for (CFIndex i = 0; i < count && g_active_count < MAX_SLOTS; i++) {
        g_active_devices[g_active_count] = devs[i];
        CFRetain(g_active_devices[g_active_count]);
        IOHIDDeviceOpen(g_active_devices[g_active_count], kIOHIDOptionsTypeNone);

        // Prime the device report so IOHID element cache is populated with true centers (LX=128, LY=128, Hat=8)
        uint8_t prime_buf[64] = {0};
        CFIndex prime_len = sizeof(prime_buf);
        IOHIDDeviceGetReport(g_active_devices[g_active_count], kIOHIDReportTypeInput, 0x01, prime_buf, &prime_len);

        g_active_count++;
    }
    CFRelease(set);

    if (g_active_count > 1) {
        qsort(g_active_devices, g_active_count, sizeof(IOHIDDeviceRef), compare_devices);
    }

    // Clear any slot pairings whose devices are no longer present
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_slots[i].dev) {
            int found = 0;
            for (int j = 0; j < g_active_count; j++) {
                if (g_slots[i].dev == g_active_devices[j]) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                g_slots[i].dev = NULL;
                g_slots[i].joystick = NULL;
                g_slots[i].last_left = 0xFF;
                g_slots[i].last_right = 0xFF;
            }
        }
    }
    pthread_mutex_unlock(&g_bridge_lock);
}

extern uint16_t SDL_JoystickGetVendor(void* joystick);
extern uint16_t SDL_JoystickGetProduct(void* joystick);
extern const char* SDL_JoystickGetSerial(void* joystick);
extern void* SDL_GameControllerGetJoystick(void* controller);
extern void SDL_JoystickClose(void* joystick);
extern void SDL_GameControllerClose(void* controller);
extern void* SDL_JoystickOpen(int device_index);
extern void* SDL_GameControllerOpen(int device_index);
extern void SDL_PumpEvents(void);
extern int SDL_WaitEventTimeout(void* event, int timeout);
extern int SDL_JoystickRumble(void* joystick, uint16_t low, uint16_t high, uint32_t duration_ms);
extern int SDL_GameControllerRumble(void* controller, uint16_t low, uint16_t high, uint32_t duration_ms);

static const RumbleDriver* find_driver(uint16_t vid, uint16_t pid) {
    for (int i = 0; i < s_driver_count; i++) {
        if (s_drivers[i].vendor_id == vid) {
            if (s_drivers[i].product_id == 0 || s_drivers[i].product_id == pid) {
                return &s_drivers[i];
            }
        }
    }
    return NULL;
}

static int find_slot(void* joystick, uint16_t vid, uint16_t pid) {
    if (!joystick) return -1;

    // 1. Check existing pairing (fast path)
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_slots[i].joystick == joystick && g_slots[i].dev != NULL) {
            if (get_device_num_prop(g_slots[i].dev, CFSTR(kIOHIDVendorIDKey)) == vid) {
                return i;
            }
            // Stale dead handle on reconnect
            g_slots[i].dev = NULL;
            g_slots[i].joystick = NULL;
            g_slots[i].last_left = 0xFF;
            g_slots[i].last_right = 0xFF;
            g_slots[i].generation++;
        }
    }

    // 2. Cache miss: re-enumerate devices
    refresh_devices();
    if (g_active_count == 0) return -1;

    // 3. Match by serial number if available from SDL
    const char* sdl_serial = SDL_JoystickGetSerial(joystick);
    if (sdl_serial && sdl_serial[0] != '\0') {
        char dev_serial[64];
        for (int i = 0; i < g_active_count; i++) {
            IOHIDDeviceRef dev = g_active_devices[i];
            if (get_device_num_prop(dev, CFSTR(kIOHIDVendorIDKey)) == vid &&
                get_device_num_prop(dev, CFSTR(kIOHIDProductIDKey)) == pid) {
                get_device_serial(dev, dev_serial, sizeof(dev_serial));
                if (strcmp(sdl_serial, dev_serial) == 0) {
                    int slot_idx = -1;
                    for (int s = 0; s < MAX_SLOTS; s++) {
                        if (g_slots[s].joystick == joystick) { slot_idx = s; break; }
                        if (slot_idx < 0 && g_slots[s].joystick == NULL) { slot_idx = s; }
                    }
                    if (slot_idx >= 0) {
                        g_slots[slot_idx].joystick = joystick;
                        g_slots[slot_idx].dev = dev;
                        return slot_idx;
                    }
                }
            }
        }
    }

    // 4. Fallback: match first unassigned active device with matching VID/PID
    for (int i = 0; i < g_active_count; i++) {
        IOHIDDeviceRef dev = g_active_devices[i];
        if (get_device_num_prop(dev, CFSTR(kIOHIDVendorIDKey)) == vid &&
            get_device_num_prop(dev, CFSTR(kIOHIDProductIDKey)) == pid) {
            int already_assigned = 0;
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].dev == dev && g_slots[s].joystick != joystick) {
                    already_assigned = 1;
                    break;
                }
            }
            if (!already_assigned) {
                int slot_idx = -1;
                for (int s = 0; s < MAX_SLOTS; s++) {
                    if (g_slots[s].joystick == joystick) { slot_idx = s; break; }
                    if (slot_idx < 0 && g_slots[s].joystick == NULL) { slot_idx = s; }
                }
                if (slot_idx < 0) { slot_idx = 0; }
                g_slots[slot_idx].joystick = joystick;
                g_slots[slot_idx].dev = dev;
                return slot_idx;
            }
        }
    }

    return -1;
}

int my_SDL_JoystickRumble(void* joystick, uint16_t low, uint16_t high, uint32_t duration_ms) {
    uint16_t vid = joystick ? SDL_JoystickGetVendor(joystick) : 0;
    uint16_t pid = joystick ? SDL_JoystickGetProduct(joystick) : 0;

    const RumbleDriver* driver = find_driver(vid, pid);
    if (driver) {
        uint8_t left = (uint8_t)(low >> 8);
        uint8_t right = (uint8_t)(high >> 8);

        pthread_mutex_lock(&g_bridge_lock);
        int slot = find_slot(joystick, vid, pid);
        if (slot >= 0 && g_slots[slot].dev != NULL) {
            IOHIDDeviceRef dev = g_slots[slot].dev;

            // Invalidate any previously scheduled stop timer
            uint32_t current_gen = ++g_slots[slot].generation;

            // Explicit stop command from game / Wine: cut off motors immediately!
            if (left == 0 && right == 0) {
                if (g_slots[slot].last_left != 0 || g_slots[slot].last_right != 0) {
                    g_slots[slot].last_left = 0;
                    g_slots[slot].last_right = 0;
                    driver->send_rumble(dev, 0, 0, 0);
                }
                pthread_mutex_unlock(&g_bridge_lock);
                return 0;
            }

            // Deduplicate: drop identical repeat frames while holding continuous fire
            if (left == g_slots[slot].last_left && right == g_slots[slot].last_right) {
                pthread_mutex_unlock(&g_bridge_lock);
                return 0;
            }

            g_slots[slot].last_left = left;
            g_slots[slot].last_right = right;

            driver->send_rumble(dev, left, right, duration_ms);

            // Safety net: stop motors when duration expires if game does not send explicit stop
            if (duration_ms > 0) {
                CFRetain(dev);
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)duration_ms * NSEC_PER_MSEC),
                               dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
                    pthread_mutex_lock(&g_bridge_lock);
                    if (g_slots[slot].generation == current_gen && g_slots[slot].dev == dev) {
                        g_slots[slot].last_left = 0;
                        g_slots[slot].last_right = 0;
                        driver->send_rumble(dev, 0, 0, 0);
                    }
                    pthread_mutex_unlock(&g_bridge_lock);
                    CFRelease(dev);
                });
            }
            pthread_mutex_unlock(&g_bridge_lock);
            return 0;
        }
        pthread_mutex_unlock(&g_bridge_lock);

        // Return success for supported driver on startup probe
        return 0;
    }

    // For all other gamepads (Xbox, PlayStation, Switch Pro), use native SDL!
    return SDL_JoystickRumble(joystick, low, high, duration_ms);
}

int my_SDL_GameControllerRumble(void* controller, uint16_t low, uint16_t high, uint32_t duration_ms) {
    void* joy = controller ? SDL_GameControllerGetJoystick(controller) : NULL;
    return my_SDL_JoystickRumble(joy, low, high, duration_ms);
}

void my_SDL_JoystickClose(void* joystick) {
    if (joystick) {
        pthread_mutex_lock(&g_bridge_lock);
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (g_slots[i].joystick == joystick) {
                g_slots[i].joystick = NULL;
                g_slots[i].dev = NULL;
                g_slots[i].last_left = 0xFF;
                g_slots[i].last_right = 0xFF;
                g_slots[i].generation++;
            }
        }
        pthread_mutex_unlock(&g_bridge_lock);
    }
    SDL_JoystickClose(joystick);
}

void my_SDL_GameControllerClose(void* controller) {
    void* joy = controller ? SDL_GameControllerGetJoystick(controller) : NULL;
    if (joy) {
        my_SDL_JoystickClose(joy);
    }
    SDL_GameControllerClose(controller);
}

void* my_SDL_JoystickOpen(int device_index) {
    void* joy = SDL_JoystickOpen(device_index);
    if (joy) {
        // ponytail: pump initial hardware reports so Wine does not read uninitialized axis values
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
        SDL_PumpEvents();
    }
    return joy;
}

void* my_SDL_GameControllerOpen(int device_index) {
    void* gc = SDL_GameControllerOpen(device_index);
    if (gc) {
        // ponytail: pump initial hardware reports so Wine does not read uninitialized axis values
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
        SDL_PumpEvents();
    }
    return gc;
}

int my_SDL_WaitEventTimeout(void* event, int timeout) {
    int res = SDL_WaitEventTimeout(event, timeout);
    if (res && event) {
        uint32_t type = *(uint32_t*)event;
        // 0x605 = SDL_JOYDEVICEADDED, 0x606 = SDL_JOYDEVICEREMOVED
        if (type == 0x605 || type == 0x606) {
            refresh_devices();
        }
    }
    return res;
}

void* my_dlsym(void* handle, const char* symbol) {
    if (symbol) {
        if (strcmp(symbol, "SDL_JoystickRumble") == 0) return (void*)my_SDL_JoystickRumble;
        if (strcmp(symbol, "SDL_GameControllerRumble") == 0) return (void*)my_SDL_GameControllerRumble;
        if (strcmp(symbol, "SDL_JoystickClose") == 0) return (void*)my_SDL_JoystickClose;
        if (strcmp(symbol, "SDL_GameControllerClose") == 0) return (void*)my_SDL_GameControllerClose;
        if (strcmp(symbol, "SDL_JoystickOpen") == 0) return (void*)my_SDL_JoystickOpen;
        if (strcmp(symbol, "SDL_GameControllerOpen") == 0) return (void*)my_SDL_GameControllerOpen;
        if (strcmp(symbol, "SDL_WaitEventTimeout") == 0) return (void*)my_SDL_WaitEventTimeout;
    }
    return dlsym(handle, symbol);
}

DYLD_INTERPOSE(my_SDL_JoystickRumble, SDL_JoystickRumble)
DYLD_INTERPOSE(my_SDL_GameControllerRumble, SDL_GameControllerRumble)
DYLD_INTERPOSE(my_SDL_JoystickClose, SDL_JoystickClose)
DYLD_INTERPOSE(my_SDL_GameControllerClose, SDL_GameControllerClose)
DYLD_INTERPOSE(my_SDL_JoystickOpen, SDL_JoystickOpen)
DYLD_INTERPOSE(my_SDL_GameControllerOpen, SDL_GameControllerOpen)
DYLD_INTERPOSE(my_SDL_WaitEventTimeout, SDL_WaitEventTimeout)
DYLD_INTERPOSE(my_dlsym, dlsym)
