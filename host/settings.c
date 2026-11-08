/* Remaster settings (native build only; Cricket 07: profiles/cricket07.h): the game's Display Options start at their highest and are
 * remembered between launches in <user data>/remaster.cfg.
 *
 * The PC game hardcodes 640x480 with shadows, Forced Detail and anti-aliasing off (FUN_00612f50), and never
 * stores what the player picks. Its display settings object is reached through the global at 0xa018c4; Apply
 * in Display Options writes it through small setters (and resets the device only when anti-aliasing or the
 * resolution changed). That global is only set after the device exists, but the boot code that fills the
 * device parameters (FUN_00612e10) copies them from [owner + 0xc] with the owner in a callee-saved register,
 * so:
 *   - at CreateDevice the object is found through the caller's registers (a candidate counts only if its
 *     resolution and anti-aliasing fields match the parameters being passed), and the saved anti-aliasing
 *     is written into it and into those parameters, so the device the game creates already uses it (the
 *     resolution is left alone: forced at start-up it breaks the front end);
 *   - the menu itself shows and edits the profile's options record in the game's data (0x9dc4f0: the profile
 *     name "Default", then option bytes): Forced Detail at 0x9dc514, Shadows at 0x9dc518, the anti-aliasing
 *     list position in the low bits of 0x9dc51c (0 Off, 1 2x, 2 4x, 3 8x). The game fills that record when it
 *     creates or loads a profile, so whenever the profile name changes the saved settings go (back) into it:
 *     display settings belong to this machine, not to a profile. The game takes shadows from the record
 *     when a match starts; the live shadow flag (settings object +0xc4) is left alone, since turning it on
 *     before the front end has loaded leaves Display Options with placeholder textures (and during device
 *     creation it crashes the game);
 *   - twice a second (settings_poll, from Present) the record is compared with the saved settings and the
 *     file rewritten when the player changed something in Display Options.
 * RC_GAME_DEFAULTS=1 leaves the game's own defaults alone (regression tests), and so does online play,
 * where both peers must run the same game. */
#include "win32.h"

#ifndef __EMSCRIPTEN__

#define GAME_SETTINGS_PTR 0xa018c4u   /* -> the game's display settings object */
enum { F_MSAA = 0x44, F_WIDTH = 0x4c, F_HEIGHT = 0x50 };   /* settings object fields */
#define OPT_DETAIL  0x9dc514u   /* profile options record: Forced Detail 0-3 */
#define OPT_SHADOWS 0x9dc518u   /* 0 or 1 */
#define OPT_AA      0x9dc51cu   /* low 2 bits: anti-aliasing list position (other bits: other options) */
#define OPT_NAME    0x9dc4f0u   /* the profile's name, at the start of the record */

typedef struct {
    uint32_t anti_aliasing;   /* D3DMULTISAMPLE_TYPE: 0, 2, 4 or 8 (the renderer draws any of 2-8 at 4x) */
    uint32_t shadows;         /* 0 or 1 */
    uint32_t detail;          /* Forced Detail: 0 Off, 1 Low, 2 Medium, 3 High */
    char window[32];          /* "" (a large window), "fullscreen", or "WxH" in points */
    uint32_t widescreen;      /* 1: the picture fills a wide window (widescreen.c) */
} Settings;

/* Highest. The game's resolution is left alone: forcing it at start-up breaks the front end (missing textures,
   picture in a corner), and the renderer draws at the window's size whatever it is. */
static Settings g_set = { 4, 1, 3, "", 1 };
static int g_loaded;
static uint32_t g_obj;   /* the game's display settings object, once found */
static char g_profile[16];   /* profile name when the record was last filled */

static int active(void) {   /* Cricket 07 only (profiles/cricket07.h): the patches use its addresses */
#ifndef PROFILE_CRICKET07
    return 0;
#endif
    extern int netplay_on(void);
    return !getenv("RC_GAME_DEFAULTS") && !netplay_on();
}

static void cfg_path(char *out, size_t n) { snprintf(out, n, "%s/remaster.cfg", g_user_root); }

static void settings_load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char path[1100];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;   /* first launch: the defaults above */
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[64];
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %63s", key, val) != 2) continue;
        uint32_t v = (uint32_t)strtoul(val, NULL, 10);
        if (!strcmp(key, "anti_aliasing")) g_set.anti_aliasing = v == 2 || v == 4 || v == 8 ? v : 0;
        else if (!strcmp(key, "shadows")) g_set.shadows = v != 0;
        else if (!strcmp(key, "detail")) g_set.detail = v > 3 ? 3 : v;
        else if (!strcmp(key, "window")) snprintf(g_set.window, sizeof g_set.window, "%s", val);
        else if (!strcmp(key, "widescreen")) g_set.widescreen = v != 0;
    }
    fclose(f);
}

static void settings_save(void) {
    char path[1100];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[settings] can't write %s\n", path); return; }
    fprintf(f, "# Cricket 07 remaster settings. Display Options in the game update this file when you apply them.\n"
               "# anti_aliasing: 0, 2, 4 or 8 (all render at 4x); shadows: 0 or 1; detail: 0 Off .. 3 High;\n"
               "# (the picture is drawn at the window's size, whatever the game's Resolution says);\n"
               "# window: empty for a large window, fullscreen, or WxH;\n"
               "# widescreen: 1 fills a wide window (a wider view; menus and HUD keep their 4:3 layout), 0 keeps 4:3.\n");
    fprintf(f, "anti_aliasing = %u\nshadows = %u\ndetail = %u\nwindow = %s\nwidescreen = %u\n", g_set.anti_aliasing,
            g_set.shadows, g_set.detail, g_set.window, g_set.widescreen);
    fclose(f);
}

const char *settings_window(void) {
    settings_load();
    return g_set.window;
}

int settings_widescreen(void) {
    settings_load();
    return g_set.widescreen && active();
}

static int is_settings_obj(uint32_t d, uint32_t pp) {   /* its fields match the parameters being passed */
    return d >= 0x10000 && d < 0xFFFF0000u && R32(d + F_WIDTH) == R32(pp) && R32(d + F_HEIGHT) == R32(pp + 4) &&
           R32(d + F_MSAA) == R32(pp + 16);
}

/* CreateDevice: put the saved settings into the game's settings object and into the parameters being passed
   (pp: D3DPRESENT_PARAMETERS in guest memory; regs: the caller's callee-saved registers). */
void settings_apply_to_game(uint32_t pp, const uint32_t regs[4]) {
    if (!active()) return;
    settings_load();
    uint32_t d = R32(GAME_SETTINGS_PTR);
    if (!is_settings_obj(d, pp)) {
        d = 0;
        for (int i = 0; i < 4 && !d; i++) {
            uint32_t owner = regs[i];
            if (owner >= 0x10000 && owner < 0xFFFF0000u && is_settings_obj(R32(owner + 0xc), pp)) d = R32(owner + 0xc);
        }
    }
    if (!d) { fprintf(stderr, "[settings] the game's display settings weren't found; using its defaults\n"); return; }
    W32(d + F_MSAA, g_set.anti_aliasing);
    if (pp) W32(pp + 16, g_set.anti_aliasing);
    g_obj = d;   /* shadows and detail follow once the game has a profile (settings_poll) */
    static int logged;
    if (!logged++)
        fprintf(stderr, "[settings] anti-aliasing %u, shadows %s, detail %u\n", g_set.anti_aliasing,
                g_set.shadows ? "on" : "off", g_set.detail);
}

/* Present, every 30 frames: remember what the player chose in Display Options. */
void settings_poll(uint32_t frame) {
    if (frame % 30 || !g_obj || !active()) return;
    uint32_t d = R32(GAME_SETTINGS_PTR) ? R32(GAME_SETTINGS_PTR) : g_obj;
    char name[16];
    memcpy(name, HP(OPT_NAME), sizeof name);
    if (memcmp(name, g_profile, sizeof name)) {   /* the game just created or loaded a profile: (re)apply */
        memcpy(g_profile, name, sizeof name);
        if (!name[0] || !R32(GAME_SETTINGS_PTR)) return;   /* still starting up */
        uint32_t pos = g_set.anti_aliasing == 2 ? 1 : g_set.anti_aliasing == 4 ? 2 : g_set.anti_aliasing == 8 ? 3 : 0;
        W32(OPT_DETAIL, g_set.detail);
        W32(OPT_SHADOWS, g_set.shadows);
        W32(OPT_AA, (R32(OPT_AA) & ~3u) | pos);
        return;
    }
    if (!name[0]) return;
    (void)d;
    static const uint32_t level[4] = { 0, 2, 4, 8 };
    Settings s = g_set;
    s.anti_aliasing = level[R32(OPT_AA) & 3];
    s.detail = R32(OPT_DETAIL) > 3 ? 3 : R32(OPT_DETAIL);
    s.shadows = R32(OPT_SHADOWS) != 0;
    if (!memcmp(&s, &g_set, sizeof s)) return;
    g_set = s;
    settings_save();
    fprintf(stderr, "[settings] saved: anti-aliasing %u, shadows %s, detail %u\n", s.anti_aliasing,
            s.shadows ? "on" : "off", s.detail);
}

#endif
