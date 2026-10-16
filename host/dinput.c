/* DirectInput (DirectX 7 era): keyboard and mouse devices. Device state comes from g_keys / the
 * window backend; until there is a real window everything reads as "nothing pressed". */
#include "com.h"

#include <pthread.h>

#define W(name) static void name(CPU *c)

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2 };
typedef struct { int kind; uint32_t format_size; int acquired; } DIDev;

static const uint8_t GUID_SysMouse[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7,
                                           0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7,
                                              0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };

/* DirectInput keyboard state is indexed by DIK scan codes; the window backend fills this. */
uint8_t g_dik[256];
int32_t g_mouse_dx, g_mouse_dy, g_mouse_buttons;
/* A press counts as held for at least 50 ms: the game samples state once per frame, so a very quick
   tap (or an automated click) would otherwise fall between two samples. */
static double g_dik_down_at[256], g_button_down_at[4];
#define MIN_HOLD 0.05

/* Buffered input (GetDeviceData): DIDEVICEOBJECTDATA records per device kind. */
typedef struct { uint32_t ofs, data, time, seq; } DIEvent;
static DIEvent g_queue[3][256];
static int g_qhead[3], g_qtail[3];
static uint32_t g_seq;
static pthread_mutex_t g_di_lock = PTHREAD_MUTEX_INITIALIZER;

static void push(int kind, uint32_t ofs, uint32_t data) {
    pthread_mutex_lock(&g_di_lock);
    int next = (g_qtail[kind] + 1) % 256;
    if (next != g_qhead[kind]) {
        g_queue[kind][g_qtail[kind]] = (DIEvent){ ofs, data, (uint32_t)(host_seconds() * 1000), ++g_seq };
        g_qtail[kind] = next;
    }
    pthread_mutex_unlock(&g_di_lock);
}
void dinput_key(uint8_t dik, int down) {
    g_dik[dik] = (uint8_t)down;
    if (down) g_dik_down_at[dik] = host_seconds();
    push(1, dik, down ? 0x80 : 0);
}
void dinput_mouse_button(int b, int down) {
    if (down) { g_mouse_buttons |= 1 << b; if (b < 4) g_button_down_at[b] = host_seconds(); }
    else g_mouse_buttons &= ~(1 << b);
    push(2, 12u + (uint32_t)b, down ? 0x80 : 0);
}
void dinput_mouse_move(int dx, int dy) {
    g_mouse_dx += dx; g_mouse_dy += dy;
    if (dx) push(2, 0, (uint32_t)dx);
    if (dy) push(2, 4, (uint32_t)dy);
}

static ComInterface IDirectInputDevice_i;

/* ------------------------------------------------------------------ IDirectInputDevice */

W(dev_GetCapabilities) {
    DIDev *d = com_data(THIS);
    uint32_t p = ARG(1);
    uint32_t sz = R32(p);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    W32(p + 4, 1 /* DIDC_ATTACHED */);
    W32(p + 8, d && d->kind == DEV_KEYBOARD ? 0x0403 : 0x0202);  /* dev type */
    W32(p + 12, d && d->kind == DEV_KEYBOARD ? 0 : 3);            /* axes */
    W32(p + 16, d && d->kind == DEV_KEYBOARD ? 128 : 4);          /* buttons */
    RET(2, S_OK);
}
W(dev_EnumObjects) { RET(4, S_OK); }
W(dev_GetProperty) { RET(3, 0x80070057u /* E_INVALIDARG -> use defaults */); }
W(dev_SetProperty) { RET(3, S_OK); }
W(dev_Acquire) { DIDev *d = com_data(THIS); if (d) d->acquired = 1; LOG("[dinput] Acquire\n"); RET(1, S_OK); }
W(dev_Unacquire) { DIDev *d = com_data(THIS); if (d) d->acquired = 0; RET(1, S_OK); }
W(dev_GetDeviceState) {
    DIDev *d = com_data(THIS);
    uint32_t n = ARG(1), p = ARG(2);
    memset(HP(p), 0, n);
    if (d && d->kind == DEV_KEYBOARD) {
        double now = host_seconds();
        for (uint32_t i = 0; i < n && i < 256; i++)
            W8(p + i, g_dik[i] || now - g_dik_down_at[i] < MIN_HOLD ? 0x80 : 0);
    } else if (d && d->kind == DEV_MOUSE && n >= 16) {
        W32(p, (uint32_t)g_mouse_dx); W32(p + 4, (uint32_t)g_mouse_dy);
        g_mouse_dx = g_mouse_dy = 0;
        double now = host_seconds();
        for (int b = 0; b < 4; b++)
            W8(p + 12 + b, ((g_mouse_buttons >> b) & 1) || now - g_button_down_at[b] < MIN_HOLD ? 0x80 : 0);
    }
    RET(3, S_OK);
}
W(dev_GetDeviceData) {
    DIDev *d = com_data(THIS);
    uint32_t size = ARG(1), out = ARG(2), pn = ARG(3), peek = ARG(4) & 1;
    int kind = d ? d->kind : 0;
    uint32_t max = pn ? R32(pn) : 0, n = 0;
    pthread_mutex_lock(&g_di_lock);
    int h = g_qhead[kind];
    while (h != g_qtail[kind] && n < max) {
        if (out) {
            memset(HP(out + n * size), 0, size);
            memcpy(HP(out + n * size), &g_queue[kind][h], size < 16 ? size : 16);
        }
        h = (h + 1) % 256;
        n++;
    }
    if (!peek) g_qhead[kind] = h;
    pthread_mutex_unlock(&g_di_lock);
    if (pn) W32(pn, n);
    RET(5, S_OK);
}
W(dev_SetDataFormat) { DIDev *d = com_data(THIS); if (d) d->format_size = R32(ARG(1) + 12); RET(2, S_OK); }
W(dev_SetEventNotification) { RET(2, S_OK); }
W(dev_SetCooperativeLevel) { RET(3, S_OK); }
static void guid_axis(uint8_t *g, int n) {   /* GUID_XAxis/YAxis/ZAxis = {A36D02E0+n-C9F3-11CF-BFC7-444553540000} */
    static const uint8_t base[16] = { 0xE0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    memcpy(g, base, 16);
    g[0] = (uint8_t)(0xE0 + n);
}
W(dev_GetObjectInfo) {
    DIDev *d = com_data(THIS);
    uint32_t p = ARG(1), obj = ARG(2), how = ARG(3);
    uint32_t sz = R32(p);
    LOG("[dinput] GetObjectInfo(%s, obj %u, how %u)\n", d && d->kind == DEV_KEYBOARD ? "keyboard" : "mouse", obj, how);
    uint32_t ofs = obj;
    if (how != 1 /* DIPH_BYOFFSET */) RET(4, 0x80070002u /* DIERR_OBJECTNOTFOUND */);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    uint8_t guid[16];
    uint32_t type;
    char name[32];
    if (d && d->kind == DEV_MOUSE) {
        if (ofs <= 8 && ofs % 4 == 0) {
            guid_axis(guid, (int)(ofs / 4));
            type = 1u | ((ofs / 4) << 8);                   /* DIDFT_RELAXIS | instance */
            snprintf(name, sizeof name, "%c Axis", "XYZ"[ofs / 4]);
        } else if (ofs >= 12 && ofs < 16) {
            static const uint8_t button[16] = { 0xF0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
            memcpy(guid, button, 16);
            type = 4u | ((ofs - 12) << 8);                  /* DIDFT_PSHBUTTON | instance */
            snprintf(name, sizeof name, "Button %u", ofs - 12);
        } else RET(4, 0x80070002u);
    } else {
        if (ofs > 255) RET(4, 0x80070002u);
        static const uint8_t key[16] = { 0x20, 0x82, 0x72, 0x55, 0x3C, 0xD3, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
        memcpy(guid, key, 16);
        type = 0xC | (ofs << 8);                            /* DIDFT_BUTTON | instance */
        snprintf(name, sizeof name, "Key %u", ofs);
    }
    memcpy(HP(p + 4), guid, 16);
    W32(p + 20, ofs);
    W32(p + 24, type);
    snprintf((char *)HP(p + 32), 260, "%s", name);
    RET(4, S_OK);
}
W(dev_GetDeviceInfo) {
    DIDev *d = com_data(THIS);
    uint32_t p = ARG(1), sz = R32(p);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    int kb = d && d->kind == DEV_KEYBOARD;
    memcpy(HP(p + 4), kb ? GUID_SysKeyboard : GUID_SysMouse, 16);
    memcpy(HP(p + 20), kb ? GUID_SysKeyboard : GUID_SysMouse, 16);
    W32(p + 36, kb ? 0x0403 : 0x0202);
    strcpy((char *)HP(p + 40), kb ? "Keyboard" : "Mouse");
    strcpy((char *)HP(p + 300), kb ? "Keyboard" : "Mouse");
    RET(2, S_OK);
}
W(dev_RunControlPanel) { RET(3, S_OK); }
W(dev_Initialize) { RET(4, S_OK); }
W(dev_Poll) { RET(1, S_OK); }

static const ComMethod didev_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetCapabilities", 1, dev_GetCapabilities }, { "EnumObjects", 3, dev_EnumObjects },
    { "GetProperty", 2, dev_GetProperty }, { "SetProperty", 2, dev_SetProperty }, { "Acquire", 0, dev_Acquire },
    { "Unacquire", 0, dev_Unacquire }, { "GetDeviceState", 2, dev_GetDeviceState },
    { "GetDeviceData", 4, dev_GetDeviceData }, { "SetDataFormat", 1, dev_SetDataFormat },
    { "SetEventNotification", 1, dev_SetEventNotification }, { "SetCooperativeLevel", 2, dev_SetCooperativeLevel },
    { "GetObjectInfo", 3, dev_GetObjectInfo }, { "GetDeviceInfo", 1, dev_GetDeviceInfo },
    { "RunControlPanel", 2, dev_RunControlPanel }, { "Initialize", 3, dev_Initialize },
    /* IDirectInputDevice2 / 7 */
    { "CreateEffect", 4, NULL }, { "EnumEffects", 3, NULL }, { "GetEffectInfo", 2, NULL },
    { "GetForceFeedbackState", 1, NULL }, { "SendForceFeedbackCommand", 1, NULL },
    { "EnumCreatedEffectObjects", 3, NULL }, { "Escape", 1, NULL }, { "Poll", 0, dev_Poll },
    { "SendDeviceData", 4, NULL }, { "EnumEffectsInFile", 4, NULL }, { "WriteEffectToFile", 4, NULL },
};
static ComInterface IDirectInputDevice_i = { "IDirectInputDeviceA", didev_methods, 29, 0 };

/* ------------------------------------------------------------------ IDirectInput */

static uint32_t create_device(const uint8_t *guid) {
    DIDev *d = calloc(1, sizeof *d);
    d->kind = !memcmp(guid, GUID_SysKeyboard, 16) ? DEV_KEYBOARD : !memcmp(guid, GUID_SysMouse, 16) ? DEV_MOUSE : 0;
    if (!d->kind) { free(d); return 0; }
    LOG("[dinput] CreateDevice(%s)\n", d->kind == DEV_KEYBOARD ? "keyboard" : "mouse");
    return com_new(&IDirectInputDevice_i, 0, d);
}

W(di_CreateDevice) {
    uint32_t dev = create_device(HP(ARG(1)));
    W32(ARG(2), dev);
    RET(4, dev ? S_OK : 0x80040154u /* DIERR_DEVICENOTREG */);
}
W(di_CreateDeviceEx) {
    uint32_t dev = create_device(HP(ARG(1)));
    W32(ARG(3), dev);
    RET(5, dev ? S_OK : 0x80040154u);
}

static void enum_one(CPU *c, uint32_t cb, uint32_t ref, const uint8_t *guid, uint32_t type, const char *name) {
    uint32_t p = heap_alloc(580, 1);
    W32(p, 580);
    memcpy(HP(p + 4), guid, 16);
    memcpy(HP(p + 20), guid, 16);
    W32(p + 36, type);
    strcpy((char *)HP(p + 40), name);
    strcpy((char *)HP(p + 300), name);
    uint32_t a[2] = { p, ref };
    guest_stdcall(c, cb, 2, a);
    heap_free(p);
}
W(di_EnumDevices) {
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3);
    LOG("[dinput] EnumDevices(type %u)\n", type);
    /* same order as Windows: system mouse first, then keyboard (the game's controller slots follow it) */
    if (type == 0 || type == 2) enum_one(c, cb, ref, GUID_SysMouse, 0x0202, "Mouse");
    if (type == 0 || type == 3) enum_one(c, cb, ref, GUID_SysKeyboard, 0x0403, "Keyboard");
    RET(5, S_OK);
}
W(di_GetDeviceStatus) { RET(2, S_OK); }
W(di_RunControlPanel) { RET(3, S_OK); }
W(di_Initialize) { RET(3, S_OK); }
W(di_FindDevice) { RET(4, 0x80070002u); }

static const ComMethod di_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "CreateDevice", 3, di_CreateDevice }, { "EnumDevices", 4, di_EnumDevices },
    { "GetDeviceStatus", 1, di_GetDeviceStatus }, { "RunControlPanel", 2, di_RunControlPanel },
    { "Initialize", 2, di_Initialize }, { "FindDevice", 3, di_FindDevice }, { "CreateDeviceEx", 4, di_CreateDeviceEx },
};
static ComInterface IDirectInput_i = { "IDirectInputA", di_methods, 10, 0 };

void w_DirectInputCreateA(CPU *c) {
    LOG("[dinput] DirectInputCreateA(version %x)\n", ARG(1));
    W32(ARG(2), com_new(&IDirectInput_i, 0, NULL));
    RET(4, S_OK);
}
