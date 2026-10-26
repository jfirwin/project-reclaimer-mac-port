/*
 * rumble_bridge.c: Pure force-feedback rumble bridge for macOS Wine / SDL2.
 *
 * Why this is needed:
 * While SDL2 natively supports haptics for first-party gamepads (Xbox, PlayStation,
 * Nintendo Switch Pro), third-party Bluetooth gamepads report through Darwin's generic
 * IOKit driver, which has no output vibration support in SDL2.
 *
 * This bridge intercepts SDL_JoystickRumble and dispatches haptics directly to the
 * live IOHIDDeviceRef already opened and maintained by SDL2 (at joystick->hwdata->deviceRef),
 * delegating to controller-specific drivers defined in drivers/.
 * All inputs, axes, hats, events, and lifecycle are handled 100% natively by Wine and SDL2.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>

#include "drivers/registry.h"

#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct{ const void* replacement; const void* replacee; } _interpose_##_replacee \
            __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

// Forward declarations for SDL2 functions
extern uint16_t SDL_JoystickGetVendor(void* joystick);
extern uint16_t SDL_JoystickGetProduct(void* joystick);
extern void* SDL_GameControllerGetJoystick(void* controller);
extern void SDL_JoystickClose(void* joystick);
extern void SDL_GameControllerClose(void* controller);
extern int SDL_JoystickRumble(void* joystick, uint16_t low, uint16_t high, uint32_t duration_ms);
extern int SDL_GameControllerRumble(void* controller, uint16_t low, uint16_t high, uint32_t duration_ms);

// ponytail: in SDL2 on Darwin, joystick->hwdata is at offset 0xe0, and its first field is IOHIDDeviceRef.
// Pulling deviceRef directly from SDL guarantees we always use the live, connected handle SDL is reading from.
static inline IOHIDDeviceRef get_joystick_device(void* joystick) {
    if (!joystick) return NULL;
    void* hwdata = *(void**)((char*)joystick + 0xe0);
    return hwdata ? *(IOHIDDeviceRef*)hwdata : NULL;
}

static pthread_mutex_t g_rumble_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_rumble_generation = 0;
static uint8_t g_last_left = 0xFF;
static uint8_t g_last_right = 0xFF;

int my_SDL_JoystickRumble(void* joystick, uint16_t low, uint16_t high, uint32_t duration_ms) {
    if (!joystick) return -1;

    uint16_t vid = SDL_JoystickGetVendor(joystick);
    uint16_t pid = SDL_JoystickGetProduct(joystick);
    const ControllerDriver* driver = find_controller_driver(vid, pid);

    if (driver && driver->send_rumble) {
        IOHIDDeviceRef dev = get_joystick_device(joystick);
        if (!dev) return 0;

        uint8_t left = (uint8_t)(low >> 8);
        uint8_t right = (uint8_t)(high >> 8);

        pthread_mutex_lock(&g_rumble_lock);
        uint32_t current_gen = ++g_rumble_generation;

        if (left == 0 && right == 0) {
            int changed = (g_last_left != 0 || g_last_right != 0);
            g_last_left = 0;
            g_last_right = 0;
            pthread_mutex_unlock(&g_rumble_lock);
            if (changed) {
                driver->send_rumble(dev, 0, 0);
            }
            return 0;
        }

        if (left == g_last_left && right == g_last_right) {
            pthread_mutex_unlock(&g_rumble_lock);
            return 0;
        }

        g_last_left = left;
        g_last_right = right;
        pthread_mutex_unlock(&g_rumble_lock);

        driver->send_rumble(dev, left, right);

        if (duration_ms > 0) {
            CFRetain(dev);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)duration_ms * NSEC_PER_MSEC),
                           dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
                pthread_mutex_lock(&g_rumble_lock);
                if (g_rumble_generation == current_gen) {
                    g_last_left = 0;
                    g_last_right = 0;
                    pthread_mutex_unlock(&g_rumble_lock);
                    driver->send_rumble(dev, 0, 0);
                } else {
                    pthread_mutex_unlock(&g_rumble_lock);
                }
                CFRelease(dev);
            });
        }
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
    pthread_mutex_lock(&g_rumble_lock);
    g_last_left = 0xFF;
    g_last_right = 0xFF;
    pthread_mutex_unlock(&g_rumble_lock);
    SDL_JoystickClose(joystick);
}

void my_SDL_GameControllerClose(void* controller) {
    pthread_mutex_lock(&g_rumble_lock);
    g_last_left = 0xFF;
    g_last_right = 0xFF;
    pthread_mutex_unlock(&g_rumble_lock);
    SDL_GameControllerClose(controller);
}

typedef struct { uint8_t data[16]; } SDL_JoystickGUID;
extern const char* SDL_GameControllerMappingForGUID(SDL_JoystickGUID guid);

// shortcut: older Wine bundles SDL < 2.26 without wildcard CRC matching. Remove when Wine updates its bundled SDL2.
const char* my_SDL_GameControllerMappingForGUID(SDL_JoystickGUID guid) {
    const char* mapping = SDL_GameControllerMappingForGUID(guid);
    if (!mapping) {
        // Fallback: retry with wildcard CRC16 (0x0000) for cross-platform/bundled database entries
        guid.data[2] = 0;
        guid.data[3] = 0;
        mapping = SDL_GameControllerMappingForGUID(guid);
    }
    if (!mapping) {
        // Fallback: retry with wildcard version (0x0000)
        guid.data[12] = 0;
        guid.data[13] = 0;
        mapping = SDL_GameControllerMappingForGUID(guid);
    }
    return mapping;
}

void* my_dlsym(void* handle, const char* symbol) {
    if (symbol && handle != RTLD_NEXT && handle != RTLD_SELF && strncmp(symbol, "SDL_", 4) == 0) {
        if (strcmp(symbol, "SDL_JoystickRumble") == 0) return (void*)my_SDL_JoystickRumble;
        if (strcmp(symbol, "SDL_GameControllerRumble") == 0) return (void*)my_SDL_GameControllerRumble;
        if (strcmp(symbol, "SDL_JoystickClose") == 0) return (void*)my_SDL_JoystickClose;
        if (strcmp(symbol, "SDL_GameControllerClose") == 0) return (void*)my_SDL_GameControllerClose;
        if (strcmp(symbol, "SDL_GameControllerMappingForGUID") == 0) return (void*)my_SDL_GameControllerMappingForGUID;
    }
    return dlsym(handle, symbol);
}

DYLD_INTERPOSE(my_SDL_JoystickRumble, SDL_JoystickRumble)
DYLD_INTERPOSE(my_SDL_GameControllerRumble, SDL_GameControllerRumble)
DYLD_INTERPOSE(my_SDL_JoystickClose, SDL_JoystickClose)
DYLD_INTERPOSE(my_SDL_GameControllerClose, SDL_GameControllerClose)
DYLD_INTERPOSE(my_SDL_GameControllerMappingForGUID, SDL_GameControllerMappingForGUID)
DYLD_INTERPOSE(my_dlsym, dlsym)
