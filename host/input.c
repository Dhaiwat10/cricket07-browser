/* Platform-independent input: turns key and mouse events (from SDL natively, from the DOM in the
 * browser) into DirectInput scan codes (g_dik), Windows virtual-key state (g_keys) and WM_* messages
 * for the game's window procedure. Also scripted input and memory dumps for testing. Keys are
 * identified by USB HID usage IDs, which both SDL scancodes and the browser's KeyboardEvent.code map
 * onto directly. */
#include "win32.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE   /* called by the page */
#else
#define EXPORT
#endif

void dinput_key(uint8_t dik, int down);
void dinput_mouse_button(int b, int down);
void dinput_mouse_move(int dx, int dy);
/* Netplay (netplay.c): only pads are shared between peers, so keyboard and mouse don't reach the game
   (one peer's keys would desync the others); the page maps the keyboard onto the local pad instead. */
int netplay_on(void);
int netplay_local_pad(int idx, int connected, uint32_t buttons, const int32_t axis[4]);
EXPORT void input_mouse_move(int dx, int dy) { if (!netplay_on()) dinput_mouse_move(dx, dy); }

/* Touch (web/loader.js): a tap clicks where the finger is. The game moves its cursor by the mouse's relative motion,
   scaled per axis by its own rules (and frame timing), ignores a jump that is too large, and keeps the result as
   floats in its 640x480 screen at GAME_CURSOR (the profile's PROFILE_CURSOR). So a tap steers: each frame the cursor is read there and nudged
   toward the spot (at most TAP_STEP units, the units-to-pixels gain of each axis learnt from the last nudge),
   and once it is within a pixel and a half (or after TAP_FRAMES frames) the button goes down; letting go
   before then releases it the frame after the press. Moving the finger while it is down drags the cursor
   (input_tap_move, in screen pixels). Netplay: no mouse, as above. */
#ifdef PROFILE_CURSOR
#define GAME_CURSOR PROFILE_CURSOR   /* float x, y (profiles/cricket07.h) */
#endif
enum { TAP_STEP = 32, TAP_FRAMES = 45 };
static volatile struct { int stage, x, y, up; } g_tap;   /* stage: 0 idle, 1 steer, 3 press, 4 release */
static struct { int frames, sx, sy; float gx, gy, px, py; } g_steer;
EXPORT void input_tap(int x, int y) {
    if (netplay_on()) return;
    g_steer.frames = 0; g_steer.sx = g_steer.sy = 0;
    g_tap.x = x; g_tap.y = y; g_tap.up = 0; g_tap.stage = 1;
}
EXPORT void input_tap_move(int dx, int dy) {
    if (netplay_on()) return;
    if (g_tap.stage == 1) { g_tap.x += dx; g_tap.y += dy; }
    else dinput_mouse_move(dx, dy);
}
void input_mouse_button(int b, int down);
EXPORT void input_tap_release(void) {
    if (g_tap.stage) g_tap.up = 1;
    else input_mouse_button(0, 0);
}
static float cursor_at(int i) {   /* no cursor address in the profile: a tap clicks where the cursor is */
#ifdef GAME_CURSOR
    float v; memcpy(&v, HP(GAME_CURSOR + 4 * (uint32_t)i), 4); return v;
#else
    (void)i; return -1e9f;
#endif
}
/* the next nudge along one axis: err pixels to go, gain pixels per unit (learnt), at most TAP_STEP units */
static int nudge(float err, float gain) {
    float u = err / gain;
    int n = (int)(u > 0 ? u + 0.5f : u - 0.5f);
    if (!n && (err > 1.5f || err < -1.5f)) n = err > 0 ? 1 : -1;
    return n > TAP_STEP ? TAP_STEP : n < -TAP_STEP ? -TAP_STEP : n;
}
/* gain from the last nudge: pixels moved per unit sent, kept within sane bounds */
static float learn(float gain, int sent, float moved) {
    if (sent > -2 && sent < 2) return gain;
    float g = moved / (float)sent;
    return g > 0.2f && g < 5.f ? g : gain;
}
static void tap_tick(void) {
    switch (g_tap.stage) {
    case 1: {
        float cx = cursor_at(0), cy = cursor_at(1);
        if (!(cx >= -1.f && cx <= 641.f && cy >= -1.f && cy <= 481.f)) { g_tap.stage = 3; break; }   /* no cursor */
        if (g_steer.frames == 0) { g_steer.gx = g_steer.gy = 1.f; }
        else { g_steer.gx = learn(g_steer.gx, g_steer.sx, cx - g_steer.px); g_steer.gy = learn(g_steer.gy, g_steer.sy, cy - g_steer.py); }
        float ex = (float)g_tap.x - cx, ey = (float)g_tap.y - cy;
        if ((ex < 1.5f && ex > -1.5f && ey < 1.5f && ey > -1.5f) || ++g_steer.frames > TAP_FRAMES) { g_tap.stage = 3; break; }
        g_steer.sx = nudge(ex, g_steer.gx); g_steer.sy = nudge(ey, g_steer.gy);
        g_steer.px = cx; g_steer.py = cy;
        dinput_mouse_move(g_steer.sx, g_steer.sy);
        break;
    }
    case 3: input_mouse_button(0, 1); g_tap.stage = g_tap.up ? 4 : 0; break;
    case 4: input_mouse_button(0, 0); g_tap.stage = 0; break;
    }
}
/* Gamepads, in the W3C "standard gamepad" layout (both SDL's GameController and the browser's Gamepad API
   map onto it): buttons bit 0-16 = A B X Y, LB RB, LT RT (digital), Back Start, L3 R3, d-pad up down left
   right, Guide; axes -32768..32767 (left x/y, right x/y; +y = down). */
void dinput_pad(int idx, int connected, uint32_t std_buttons, const int32_t axis[4]);
EXPORT void input_gamepad(int idx, int connected, uint32_t buttons, int lx, int ly, int rx, int ry) {
    const int32_t axis[4] = { lx, ly, rx, ry };
    if (netplay_local_pad(idx, connected, buttons, axis)) return;
    dinput_pad(idx, connected, buttons, axis);
}

typedef struct { int usage; uint8_t dik, vk; } KeyMap;
static const KeyMap g_keymap[] = {
    {  41, 0x01, 0x1B },   /* escape */
    {  30, 0x02, '1' },   /* 1 */
    {  31, 0x03, '2' },   /* 2 */
    {  32, 0x04, '3' },   /* 3 */
    {  33, 0x05, '4' },   /* 4 */
    {  34, 0x06, '5' },   /* 5 */
    {  35, 0x07, '6' },   /* 6 */
    {  36, 0x08, '7' },   /* 7 */
    {  37, 0x09, '8' },   /* 8 */
    {  38, 0x0A, '9' },   /* 9 */
    {  39, 0x0B, '0' },   /* 0 */
    {  45, 0x0C, 0xBD },   /* minus */
    {  46, 0x0D, 0xBB },   /* equals */
    {  42, 0x0E, 0x08 },   /* backspace */
    {  43, 0x0F, 0x09 },   /* tab */
    {  20, 0x10, 'Q' },   /* q */
    {  26, 0x11, 'W' },   /* w */
    {   8, 0x12, 'E' },   /* e */
    {  21, 0x13, 'R' },   /* r */
    {  23, 0x14, 'T' },   /* t */
    {  28, 0x15, 'Y' },   /* y */
    {  24, 0x16, 'U' },   /* u */
    {  12, 0x17, 'I' },   /* i */
    {  18, 0x18, 'O' },   /* o */
    {  19, 0x19, 'P' },   /* p */
    {  47, 0x1A, 0xDB },   /* leftbracket */
    {  48, 0x1B, 0xDD },   /* rightbracket */
    {  40, 0x1C, 0x0D },   /* return */
    { 224, 0x1D, 0x11 },   /* lctrl */
    {   4, 0x1E, 'A' },   /* a */
    {  22, 0x1F, 'S' },   /* s */
    {   7, 0x20, 'D' },   /* d */
    {   9, 0x21, 'F' },   /* f */
    {  10, 0x22, 'G' },   /* g */
    {  11, 0x23, 'H' },   /* h */
    {  13, 0x24, 'J' },   /* j */
    {  14, 0x25, 'K' },   /* k */
    {  15, 0x26, 'L' },   /* l */
    {  51, 0x27, 0xBA },   /* semicolon */
    {  52, 0x28, 0xDE },   /* apostrophe */
    {  53, 0x29, 0xC0 },   /* grave */
    { 225, 0x2A, 0x10 },   /* lshift */
    {  49, 0x2B, 0xDC },   /* backslash */
    {  29, 0x2C, 'Z' },   /* z */
    {  27, 0x2D, 'X' },   /* x */
    {   6, 0x2E, 'C' },   /* c */
    {  25, 0x2F, 'V' },   /* v */
    {   5, 0x30, 'B' },   /* b */
    {  17, 0x31, 'N' },   /* n */
    {  16, 0x32, 'M' },   /* m */
    {  54, 0x33, 0xBC },   /* comma */
    {  55, 0x34, 0xBE },   /* period */
    {  56, 0x35, 0xBF },   /* slash */
    { 229, 0x36, 0x10 },   /* rshift */
    {  85, 0x37, 0x6A },   /* kp_multiply */
    { 226, 0x38, 0x12 },   /* lalt */
    {  44, 0x39, 0x20 },   /* space */
    {  57, 0x3A, 0x14 },   /* capslock */
    {  58, 0x3B, 0x70 },   /* f1 */
    {  59, 0x3C, 0x71 },   /* f2 */
    {  60, 0x3D, 0x72 },   /* f3 */
    {  61, 0x3E, 0x73 },   /* f4 */
    {  62, 0x3F, 0x74 },   /* f5 */
    {  63, 0x40, 0x75 },   /* f6 */
    {  64, 0x41, 0x76 },   /* f7 */
    {  65, 0x42, 0x77 },   /* f8 */
    {  66, 0x43, 0x78 },   /* f9 */
    {  67, 0x44, 0x79 },   /* f10 */
    {  95, 0x47, 0x67 },   /* kp_7 */
    {  96, 0x48, 0x68 },   /* kp_8 */
    {  97, 0x49, 0x69 },   /* kp_9 */
    {  86, 0x4A, 0x6D },   /* kp_minus */
    {  92, 0x4B, 0x64 },   /* kp_4 */
    {  93, 0x4C, 0x65 },   /* kp_5 */
    {  94, 0x4D, 0x66 },   /* kp_6 */
    {  87, 0x4E, 0x6B },   /* kp_plus */
    {  89, 0x4F, 0x61 },   /* kp_1 */
    {  90, 0x50, 0x62 },   /* kp_2 */
    {  91, 0x51, 0x63 },   /* kp_3 */
    {  98, 0x52, 0x60 },   /* kp_0 */
    {  99, 0x53, 0x6E },   /* kp_period */
    {  68, 0x57, 0x7A },   /* f11 */
    {  69, 0x58, 0x7B },   /* f12 */
    {  88, 0x9C, 0x0D },   /* kp_enter */
    { 228, 0x9D, 0x11 },   /* rctrl */
    {  84, 0xB5, 0x6F },   /* kp_divide */
    { 230, 0xB8, 0x12 },   /* ralt */
    {  74, 0xC7, 0x24 },   /* home */
    {  82, 0xC8, 0x26 },   /* up */
    {  75, 0xC9, 0x21 },   /* pageup */
    {  80, 0xCB, 0x25 },   /* left */
    {  79, 0xCD, 0x27 },   /* right */
    {  77, 0xCF, 0x23 },   /* end */
    {  81, 0xD0, 0x28 },   /* down */
    {  78, 0xD1, 0x22 },   /* pagedown */
    {  73, 0xD2, 0x2D },   /* insert */
    {  76, 0xD3, 0x2E },   /* delete */
};

static void key_event_dik(uint8_t dik, int down) {
    if (netplay_on()) return;
    for (size_t i = 0; i < sizeof g_keymap / sizeof g_keymap[0]; i++) {
        if (g_keymap[i].dik != dik) continue;
        dinput_key(dik, down);
        g_keys[g_keymap[i].vk] = (uint8_t)down;
        post_message(MAIN_HWND, down ? 0x100 : 0x101, g_keymap[i].vk, ((uint32_t)dik << 16) | 1u);
        return;
    }
}

/* A key the lockstep applies on every peer alike (netplay.c carries Esc with the pad inputs): not
   guarded like the physical keyboard below, which would desync. */
void input_key_usage_sync(int usage, int down) {
    for (size_t i = 0; i < sizeof g_keymap / sizeof g_keymap[0]; i++) {
        if (g_keymap[i].usage != usage) continue;
        dinput_key(g_keymap[i].dik, down);
        g_keys[g_keymap[i].vk] = (uint8_t)down;
        post_message(MAIN_HWND, down ? 0x100 : 0x101, g_keymap[i].vk, ((uint32_t)g_keymap[i].dik << 16) | 1u);
        return;
    }
}

EXPORT void input_key_hid(int usage, int down) {
    LOG("[key] usage %d %s\n", usage, down ? "down" : "up");
    if (netplay_on()) return;   /* online, the keyboard only drives the pad (web/loader.js) */
    input_key_usage_sync(usage, down);
}

EXPORT void input_mouse_button(int b, int down) {
    static const uint8_t vk[3] = { 0x01, 0x02, 0x04 };  /* VK_LBUTTON, VK_RBUTTON, VK_MBUTTON */
    if (netplay_on()) return;
    dinput_mouse_button(b, down);
    g_keys[vk[b]] = (uint8_t)down;
    static const uint32_t msg_down[3] = { 0x201, 0x204, 0x207 };
    post_message(MAIN_HWND, down ? msg_down[b] : msg_down[b] + 1, 0, 0);
}

/* Scripted input for automated testing, driven by the game's frame counter:
   RC_INPUT="500:click,700:key=1c,900:key=c8,950:move=-75:-180,990:pad=1,1200:stick=0:-32768"
   (DIK codes in hex; a key, click or pad press is held 4 frames, key=<hex>/<frames> longer; move is relative mouse motion; pad=<hex>[/frames]
   presses standard-gamepad buttons on pad 0, e.g. 1 = A, 2000 = d-pad down; stick=lx:ly sets the left
   stick until changed). Frame numbers and hold lengths are in 60 Hz frames whatever RC_REFRESH_HZ is, so a
   script means the same moments in time at 144 Hz. */
static struct { uint32_t frame; int kind; uint8_t code; } g_pending[64];
static int g_npending;
static uint32_t g_pad_buttons;
static int g_pad_lx, g_pad_ly;

static uint32_t script_to_frame(uint32_t s) { return (uint32_t)(((uint64_t)s * (uint64_t)refresh_hz() + 30) / 60); }
static uint32_t hold_frames(uint32_t h) {   /* a 60 Hz hold length at the current rate (at least one frame) */
    uint32_t n = (uint32_t)(((uint64_t)h * (uint64_t)refresh_hz() + 30) / 60);
    return n ? n : 1;
}

static void script_frame(const char *script, uint32_t sframe, uint32_t frame);

void input_script_tick(uint32_t frame) {
    tap_tick();
    static const char *script = (const char *)1;
    if (script == (const char *)1) script = getenv("RC_INPUT");
    if (!script) return;
    for (int i = 0; i < g_npending; i++)
        if (g_pending[i].frame == frame) {
            if (g_pending[i].kind == 0) input_mouse_button(0, 0);
            else if (g_pending[i].kind == 2) { g_pad_buttons = 0; input_gamepad(0, 1, 0, g_pad_lx, g_pad_ly, 0, 0); }
            else key_event_dik(g_pending[i].code, 0);
            g_pending[i] = g_pending[--g_npending]; i--;
        }
    /* the script frames that land on this frame */
    uint32_t hz = (uint32_t)refresh_hz();
    uint64_t lo = (uint64_t)frame * 60 / hz;
    for (uint64_t s = lo > 2 ? lo - 2 : 0; s <= lo + 2; s++)
        if (script_to_frame((uint32_t)s) == frame) script_frame(script, (uint32_t)s, frame);
}

static void script_frame(const char *script, uint32_t sframe, uint32_t frame) {
#define pending g_pending
#define npending g_npending
#define pad_buttons g_pad_buttons
#define pad_lx g_pad_lx
#define pad_ly g_pad_ly
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%u:", sframe);
    for (const char *p = strstr(script, tmp); p; p = strstr(p + 1, tmp)) {
        if (p != script && p[-1] != ',') continue;
        const char *a = p + strlen(tmp);
        if (!strncmp(a, "click", 5)) {
            fprintf(stderr, "[input] frame %u: click\n", frame);
            input_mouse_button(0, 1);
            if (npending < 64) pending[npending++] = (typeof(pending[0])){ frame + hold_frames(4), 0, 0 };
        } else if (!strncmp(a, "move=", 5)) {     /* move=dx:dy, relative mouse motion in pixels */
            int dx = (int)strtol(a + 5, (char **)&a, 10), dy = *a == ':' ? (int)strtol(a + 1, NULL, 10) : 0;
            fprintf(stderr, "[input] frame %u: move %d,%d\n", frame, dx, dy);
            dinput_mouse_move(dx, dy);
        } else if (!strncmp(a, "key=", 4)) {
            uint8_t code = (uint8_t)strtoul(a + 4, (char **)&a, 16);
            uint32_t hold = *a == '/' ? (uint32_t)strtoul(a + 1, NULL, 10) : 4;   /* key=<hex>/<frames> */
            if (!hold) hold = 1;
            fprintf(stderr, "[input] frame %u: key %02x for %u frames\n", frame, code, hold);
            key_event_dik(code, 1);
            if (npending < 64) pending[npending++] = (typeof(pending[0])){ frame + hold_frames(hold), 1, code };
        } else if (!strncmp(a, "pad=", 4)) {
            pad_buttons = (uint32_t)strtoul(a + 4, (char **)&a, 16);
            uint32_t hold = *a == '/' ? (uint32_t)strtoul(a + 1, NULL, 10) : 4;   /* pad=<hex>/<frames> */
            fprintf(stderr, "[input] frame %u: pad %x for %u frames\n", frame, pad_buttons, hold);
            input_gamepad(0, 1, pad_buttons, pad_lx, pad_ly, 0, 0);
            if (!hold) { pad_buttons = 0; input_gamepad(0, 1, 0, pad_lx, pad_ly, 0, 0); }   /* /0: released before the frame samples it */
            else if (npending < 64) pending[npending++] = (typeof(pending[0])){ frame + hold_frames(hold), 2, 0 };
        } else if (!strncmp(a, "stick=", 6)) {
            pad_lx = (int)strtol(a + 6, (char **)&a, 10); pad_ly = *a == ':' ? (int)strtol(a + 1, NULL, 10) : 0;
            fprintf(stderr, "[input] frame %u: stick %d,%d\n", frame, pad_lx, pad_ly);
            input_gamepad(0, 1, pad_buttons, pad_lx, pad_ly, 0, 0);
        }
    }
#undef pending
#undef npending
#undef pad_buttons
#undef pad_lx
#undef pad_ly
}

/* Debugging: RC_DUMP="frame:addr:len,..." hex-dumps guest memory at that frame (addr/len in hex). */
void dump_tick(uint32_t frame) {
    static const char *spec = (const char *)1;
    if (spec == (const char *)1) spec = getenv("RC_DUMP");
    if (!spec) return;
    for (const char *p = spec; *p;) {
        uint32_t f = (uint32_t)strtoul(p, (char **)&p, 10);
        if (*p == ':') p++;
        uint32_t a = (uint32_t)strtoul(p, (char **)&p, 16);
        if (*p == ':') p++;
        uint32_t n = (uint32_t)strtoul(p, (char **)&p, 16);
        if (f == frame) {
            fprintf(stderr, "[dump] frame %u, %08x (+%x):", frame, a, n);
            for (uint32_t i = 0; i < n; i++) fprintf(stderr, "%s%02x", i % 32 ? (i % 4 ? "" : " ") : "\n  ", R8(a + i));
            fprintf(stderr, "\n");
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}

