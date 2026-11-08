/* Cricket 07 widescreen, with profiles/cricket07.h (native: remaster.cfg `widescreen = 1`; browser: RC_WIDESCREEN=1, set by web/loader.js unless the page
 * has ?4x3): the picture fills a window (canvas) wider than 4:3.
 *
 *   - 3D: the game's cameras take their aspect ratio from the display mode object ([0xa003b8] + 0x24), which holds
 *     4:3 (16:9 when the console-only widescreen flag at options record +0x5c is set, see 0x5c9e40). Keeping it at
 *     the picture's aspect gives a wider view at the same vertical field of view ("Hor+"), and the game's own
 *     visibility culling, built from the camera, widens with it. The renderer stretches 3D across the window.
 *   - 2D keeps the game's 640x480 screen as a 4:3 area, and the HUD's side elements move to the window's sides:
 *     see draw_layout in render_rec.c and exec_draw in render_wgpu.c.
 *
 * Off with RC_GAME_DEFAULTS=1 (native) and in online play, where every peer must run the same game. */
#include "win32.h"

#define GAME_MODE_PTR 0xa003b8u   /* -> display mode object: in use at +0x18, set by Apply at +0x44 (11 dwords each) */
enum { F_ASPECT = 0x24, F_ASPECT_APPLIED = 0x50 };   /* aspect ratio (float) in each */

extern float render_aspect(void);

/* on for this run (the renderer asks too, from the page thread in the browser) */
int widescreen_on(void) {
#ifndef PROFILE_CRICKET07
    return 0;   /* Cricket 07 only (profiles/cricket07.h): the patch uses its addresses */
#endif
#ifdef __EMSCRIPTEN__
    extern int netplay_on(void);
    static int on = -1;
    if (on < 0) on = getenv("RC_WIDESCREEN") && atoi(getenv("RC_WIDESCREEN")) && !netplay_on();
    return on;
#else
    extern int settings_widescreen(void);
    return settings_widescreen();
#endif
}

/* Present: keep the cameras' aspect ratio at the picture's. */
void widescreen_frame(void) {
    if (!widescreen_on()) return;
    uint32_t m = R32(GAME_MODE_PTR);
    if (m < 0x10000 || m >= 0xFFFF0000u) return;
    float want = render_aspect(), cur;
    memcpy(&cur, HP(m + F_ASPECT), 4);
    if (!(cur > 1.f && cur < 4.f) || cur == want) return;   /* not set up yet, or already right */
    memcpy(HP(m + F_ASPECT), &want, 4);
    memcpy(HP(m + F_ASPECT_APPLIED), &want, 4);
    static float logged;
    if (logged != want) { logged = want; fprintf(stderr, "[widescreen] aspect ratio %.3f\n", want); }
}
