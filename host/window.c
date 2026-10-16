/* The host window (SDL2) and input: runs on the process's main thread, as macOS requires.
 * Keyboard events become DirectInput scan codes (g_dik), Windows virtual-key state (g_keys) and
 * WM_KEYDOWN/WM_KEYUP messages for the game's window procedure. */
#include "win32.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>
#include <objc/message.h>
#include <objc/runtime.h>

/* Opt out of App Nap: when the window is hidden, macOS otherwise stretches our 16 ms frame sleeps
   to ~100 ms and the game slows to a crawl. */
static void disable_app_nap(void) {
    id info = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSProcessInfo"), sel_registerName("processInfo"));
    id reason = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
                                                                  sel_registerName("stringWithUTF8String:"), "game");
    /* NSActivityUserInitiated | NSActivityLatencyCritical */
    id act = ((id (*)(id, SEL, uint64_t, id))objc_msgSend)(info, sel_registerName("beginActivityWithOptions:reason:"),
                                                           0x00FFFFFFULL | 0xFF00000000ULL, reason);
    ((id (*)(id, SEL))objc_msgSend)(act, sel_registerName("retain"));
}

void *g_metal_layer;           /* CAMetalLayer the renderer draws into */
int g_window_w = 1024, g_window_h = 768;   /* drawable size in pixels */
static SDL_Window *g_window;

void input_key_hid(int usage, int down);   /* input.c */
void input_mouse_button(int b, int down);
void dinput_mouse_move(int dx, int dy);



int window_create(const char *title) {
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_window_w, g_window_h,
                                SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                (getenv("RC_FULLSCREEN") ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!g_window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 0; }
    SDL_RaiseWindow(g_window);
    disable_app_nap();
    SDL_MetalView view = SDL_Metal_CreateView(g_window);
    g_metal_layer = SDL_Metal_GetLayer(view);
    SDL_Metal_GetDrawableSize(g_window, &g_window_w, &g_window_h);
    /* fullscreen: relative mouse mode keeps the (hidden) system cursor from stopping at the edges */
    if (getenv("RC_FULLSCREEN")) SDL_SetRelativeMouseMode(SDL_TRUE);
    fprintf(stderr, "window: %dx%d drawable\n", g_window_w, g_window_h);
    return 1;
}

/* Runs forever on the main thread. */
void window_event_loop(void) {
    int scripted = getenv("RC_INPUT") != NULL;   /* scripted runs ignore the real mouse and keyboard */
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 10)) continue;
        if (scripted && (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_MOUSEMOTION ||
                         e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP))
            continue;
        switch (e.type) {
        case SDL_QUIT:
            fprintf(stderr, "window closed\n");
            exit(0);
        case SDL_KEYDOWN: if (!e.key.repeat) input_key_hid((int)e.key.keysym.scancode, 1); break;
        case SDL_KEYUP: input_key_hid((int)e.key.keysym.scancode, 0); break;
        case SDL_MOUSEMOTION: dinput_mouse_move(e.motion.xrel, e.motion.yrel); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            input_mouse_button(b, e.type == SDL_MOUSEBUTTONDOWN);
            break;
        }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) SDL_Metal_GetDrawableSize(g_window, &g_window_w, &g_window_h);
            break;
        }
    }
}
