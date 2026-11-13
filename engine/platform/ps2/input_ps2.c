/* Port the engine's actual Key_Event input API to DualShock 2 / PS2SDK. */
#include "common.h"
#include "keydefs.h"
#include "input.h"
#include "client.h"
#include "platform/platform.h"

#include <loadfile.h>
#include <libpad.h>
#include <libkbd.h>
#include <libmouse.h>
#include <sbv_patches.h>
#include <string.h>
#include <stdio.h>

extern unsigned char cs_ps2_kbd_start[], cs_ps2_kbd_end[];
extern unsigned char cs_ps2_mouse_start[], cs_ps2_mouse_end[];
void PS2_LoadUsbd(void);

static int ps2_kbd_open, ps2_mouse_open, ps2_text_input;
static unsigned ps2_mouse_buttons;
static float ps2_mouse_dx, ps2_mouse_dy;

static char ps2_pad_area[256] __attribute__((aligned(64)));
static char ps2_pad2_area[256] __attribute__((aligned(64)));
static int ps2_pad2_open, ps2_pad2_analog_requested;
static unsigned ps2_old_buttons;
static int ps2_pad_open;
static int ps2_analog_mode_requested;
static int mouse_x = 160, mouse_y = 112;
static int ps2_left_key = K_LEFTARROW;
static int ps2_right_key = K_RIGHTARROW;
#if XASH_PS2
static int ps2_last_pad_state = -1;
static unsigned ps2_logged_keys;
static int ps2_first_read;
static int ps2_last_read_success = -1;
#endif

static const struct {
    unsigned short physical;
    int logical;
} keymap[] = {
    { PAD_UP, K_UPARROW }, { PAD_DOWN, K_DOWNARROW },
    { PAD_LEFT, K_LEFTARROW }, { PAD_RIGHT, K_RIGHTARROW },
    { PAD_START, K_START_BUTTON }, { PAD_SELECT, K_MODE_BUTTON },
    { PAD_CROSS, K_A_BUTTON }, { PAD_CIRCLE, K_B_BUTTON },
    { PAD_SQUARE, K_X_BUTTON }, { PAD_TRIANGLE, K_Y_BUTTON },
    { PAD_L1, K_L1_BUTTON }, { PAD_R1, K_R1_BUTTON },
    { PAD_L2, K_L2_BUTTON }, { PAD_R2, K_R2_BUTTON },
    { PAD_L3, K_LSTICK }, { PAD_R3, K_RSTICK }
};

static int PS2_ButtonKey(unsigned physical, int down, int default_key)
{
    int *held_key;
    const char *turn_binding, *strafe_binding;
    const char *binding;
    int auxiliary_key;

    if (physical == PAD_LEFT) {
        held_key = &ps2_left_key;
        turn_binding = "+left";
        strafe_binding = "+moveleft";
        auxiliary_key = K_AUX30;
    } else if (physical == PAD_RIGHT) {
        held_key = &ps2_right_key;
        turn_binding = "+right";
        strafe_binding = "+moveright";
        auxiliary_key = K_AUX31;
    } else {
        return default_key;
    }

    // Keep the original key on release, even if a menu opened while held.
    if (!down)
        return *held_key;

    *held_key = default_key;
    if (cls.key_dest != key_game)
        return *held_key;

    // Default arrow bindings turn the camera; unbindall can also erase them.
    // Use dedicated strafe keys in either case, preserving custom arrow binds.
    binding = Key_GetBinding(default_key);
    if (!binding || !binding[0] || strcmp(binding, turn_binding) == 0) {
        binding = Key_GetBinding(auxiliary_key);
        if (!binding || !binding[0])
            Key_SetBinding(auxiliary_key, strafe_binding);
        *held_key = auxiliary_key;
    }
    return *held_key;
}

static void PS2_RequestAnalogMode(int port, int *requested)
{
    int count, i;

    if (*requested)
        return;

    // PADMAN requires a stable connection before changing the controller mode.
    count = padInfoMode(port, 0, PAD_MODETABLE, -1);
    if (count <= 0)
        return; // Digital-only controller: preserve its buttons.

    for (i = 0; i < count; ++i) {
        if (padInfoMode(port, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK) {
            *requested = padSetMainMode(port, 0,
                PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK) != 0;
            printf("[XASH-PS2] PAD%d_ANALOG_REQUEST=%d\n", port + 1, *requested);
            return;
        }
    }

    *requested = 1; // No DualShock analog mode available.
}

// Second controller: raw state only, never Key_Event.
// Returns 0 when pad 2 is not ready; sticks stay centered (128) then.
static int PS2_Pad2_Read(unsigned *buttons, unsigned char sticks[4])
{
    struct padButtonStatus state;
    int connection;

    *buttons = 0;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 128;
    if (!ps2_pad2_open)
        return 0;
    connection = padGetState(1, 0);
    if (connection != PAD_STATE_STABLE && connection != PAD_STATE_FINDCTP1) {
        if (connection == PAD_STATE_DISCONN)
            ps2_pad2_analog_requested = 0;
        return 0;
    }
    PS2_RequestAnalogMode(1, &ps2_pad2_analog_requested);
    if (!padRead(1, 0, &state))
        return 0;
    *buttons = (~(unsigned)state.btns) & 0xffffu;
    if (padInfoMode(1, 0, PAD_MODECURID, 0) == PAD_TYPE_DUALSHOCK) {
        sticks[0] = state.ljoy_h; sticks[1] = state.ljoy_v;
        sticks[2] = state.rjoy_h; sticks[3] = state.rjoy_v;
    }
    return 1;
}

// Pad 2 feeds splitscreen player 2 with the same key numbers as pad 1.
// A missing pad reads as all released, so player 2 stops.
static void PS2_PollPad2(void)
{
    static unsigned old_buttons;
    unsigned buttons, changed, i;
    unsigned char sticks[4];

    PS2_Pad2_Read(&buttons, sticks);
    changed = buttons ^ old_buttons;
    for (i = 0; i < sizeof(keymap)/sizeof(keymap[0]); ++i)
        if (changed & keymap[i].physical)
            CL_SplitKeyEvent(keymap[i].logical, (buttons & keymap[i].physical) != 0);
    old_buttons = buttons;

    CL_SplitAxisEvent(0, ((int)sticks[0] - 128) * 256);
    CL_SplitAxisEvent(1, ((int)sticks[1] - 128) * 256);
    CL_SplitAxisEvent(2, ((int)sticks[3] - 128) * 256);
    CL_SplitAxisEvent(3, ((int)sticks[2] - 128) * 256);
}

// USB HID keyboard usage (page 7) to engine key, US layout. 0 = unmapped.
static int PS2_HidKey(unsigned usage)
{
    static const unsigned char keys[] = { // usages 0x28..0x64
        K_ENTER, K_ESCAPE, K_BACKSPACE, K_TAB, K_SPACE, '-', '=', '[', ']', '\\',
        '\\', ';', '\'', '`', ',', '.', '/', K_CAPSLOCK,
        K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
        0, K_SCROLLOCK, K_PAUSE, K_INS, K_HOME, K_PGUP, K_DEL, K_END, K_PGDN,
        K_RIGHTARROW, K_LEFTARROW, K_DOWNARROW, K_UPARROW, K_KP_NUMLOCK,
        K_KP_SLASH, '*', K_KP_MINUS, K_KP_PLUS, K_KP_ENTER,
        K_KP_END, K_KP_DOWNARROW, K_KP_PGDN, K_KP_LEFTARROW, K_KP_5, K_KP_RIGHTARROW,
        K_KP_HOME, K_KP_UPARROW, K_KP_PGUP, K_KP_INS, K_KP_DEL, '\\'
    };
    static const unsigned char modifiers[8] = { // usages 0xE0..0xE7
        K_CTRL, K_SHIFT, K_ALT, K_WIN, K_CTRL, K_SHIFT, K_ALT, K_WIN
    };
    typedef char keys_cover_usages[sizeof(keys) == 0x65 - 0x28 ? 1 : -1];
    (void)sizeof(keys_cover_usages);

    if (usage >= 0x04 && usage <= 0x1d) return 'a' + usage - 0x04;
    if (usage >= 0x1e && usage <= 0x26) return '1' + usage - 0x1e;
    if (usage == 0x27) return '0';
    if (usage >= 0x28 && usage < 0x65) return keys[usage - 0x28];
    if (usage >= 0xe0 && usage <= 0xe7) return modifiers[usage - 0xe0];
    return 0;
}

static int PS2_LoadInputModule(const char *name, unsigned char *start, unsigned char *end)
{
    int result = -1;
    int id = SifExecModuleBuffer(start, (unsigned)(end - start), 0, NULL, &result);
    printf("[XASH-PS2] IOP %s id=%d result=%d\n", name, id, result);
    return id >= 0 && result != 1;
}

// USB keyboard and mouse are optional: any failure leaves only the pads.
static void PS2_UsbInputInit(void)
{
    SifLoadFileInit();
    sbv_patch_enable_lmb(); // stock LOADFILE cannot load EE buffers without it
    PS2_LoadUsbd();
    ps2_kbd_open = PS2_LoadInputModule("ps2kbd", cs_ps2_kbd_start, cs_ps2_kbd_end)
        && PS2KbdInit() > 0;
    if (ps2_kbd_open) {
        PS2KbdSetReadmode(PS2KBD_READMODE_RAW);
        PS2KbdSetBlockingMode(PS2KBD_NONBLOCKING);
    }
    // PS2MouseInit spins until its RPC server exists, so only after a good load.
    ps2_mouse_open = PS2_LoadInputModule("ps2mouse", cs_ps2_mouse_start, cs_ps2_mouse_end)
        && PS2MouseInit() > 0;
    if (ps2_mouse_open)
        PS2MouseSetReadMode(PS2MOUSE_READMODE_DIFF);
    printf("[XASH-PS2] USB_KBD=%d USB_MOUSE=%d\n", ps2_kbd_open, ps2_mouse_open);
}

static void PS2_PollKeyboard(void)
{
    PS2KbdRawKey raw;
    int key, down;

    while (ps2_kbd_open && PS2KbdReadRaw(&raw) > 0) {
        key = PS2_HidKey(raw.key);
        if (!key) continue;
        down = raw.state == PS2KBD_RAWKEY_DOWN;
        Key_Event(key, down);
        if (down && key >= 32 && key < 127 && (ps2_text_input ||
            cls.key_dest == key_console || cls.key_dest == key_message))
            CL_CharEvent(Key_IsDown(K_SHIFT) ? Key_ToUpper(key) : key);
    }
}

static void PS2_PollMouse(void)
{
    static const int keys[3] = { K_MOUSE1, K_MOUSE2, K_MOUSE3 };
    PS2MouseData data;
    unsigned changed, i;
    int key;

    if (!ps2_mouse_open || PS2MouseRead(&data) <= 0) return;

    // Menu cursor is absolute; the game view takes the relative motion.
    mouse_x = bound(0, mouse_x + data.x, refState.width - 1);
    mouse_y = bound(0, mouse_y + data.y, refState.height - 1);
    if (cls.key_dest == key_game) {
        ps2_mouse_dx += data.x;
        ps2_mouse_dy += data.y;
    } else {
        ps2_mouse_dx = ps2_mouse_dy = 0; // no view jump when leaving a menu
    }

    changed = (data.buttons ^ ps2_mouse_buttons) & 7;
    for (i = 0; i < 3; ++i)
        if (changed & (1u << i))
            Key_Event(keys[i], (data.buttons >> i) & 1);
    ps2_mouse_buttons = data.buttons;

    if (data.wheel) {
        key = data.wheel > 0 ? K_MWHEELUP : K_MWHEELDOWN;
        Key_Event(key, 1);
        Key_Event(key, 0);
    }
}

int Platform_JoyInit(int index)
{
    int sio2, pad;
    (void)index;
    PS2_UsbInputInit();
    sio2 = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    pad = SifLoadModule("rom0:PADMAN", 0, NULL);
    if (sio2 < 0 || pad < 0 || !padInit(0)) {
        printf("[XASH-PS2] PADMAN init failed: %d/%d\n", sio2, pad);
        return 0;
    }
    ps2_pad_open = padPortOpen(0, 0, ps2_pad_area) != 0;
    printf("[XASH-PS2] STAGE=PAD_%s\n", ps2_pad_open ? "READY" : "FAILED");
    ps2_pad2_open = padPortOpen(1, 0, ps2_pad2_area) != 0;
    printf("[XASH-PS2] PAD2_OPEN=%d\n", ps2_pad2_open);
    return ps2_pad_open ? 1 : 0;
}

void Platform_RunEvents(void)
{
    struct padButtonStatus state;
    unsigned buttons, changed;
    unsigned i;
    int connection;
    PS2_PollKeyboard();
    PS2_PollMouse();
    if (!ps2_pad_open) return;
    PS2_PollPad2();
    connection = padGetState(0, 0);
    #if XASH_PS2
    if (connection != ps2_last_pad_state) {
        printf("[XASH-PS2] PAD_STATE=%d\n", connection);
        ps2_last_pad_state = connection;
    }
    #endif
    if (connection != PAD_STATE_STABLE && connection != PAD_STATE_FINDCTP1)
    {
        if (connection == PAD_STATE_DISCONN) {
            ps2_analog_mode_requested = 0;
            // A disconnected pad must not leave the player walking or firing.
            for (i = 0; i < sizeof(keymap)/sizeof(keymap[0]); ++i)
                if (ps2_old_buttons & keymap[i].physical)
                    Key_Event(PS2_ButtonKey(keymap[i].physical, 0,
                        keymap[i].logical), 0);
            ps2_old_buttons = 0;
            for (i = 0; i < 4; ++i)
                Joy_AxisMotionEvent(i, 0);
        }
        return;
    }

    PS2_RequestAnalogMode(0, &ps2_analog_mode_requested);
    {
        int read_ok = padRead(0, 0, &state);
        #if XASH_PS2
        if (read_ok != ps2_last_read_success) {
            printf("[XASH-PS2] PAD_READ_OK=%d\n", read_ok);
            ps2_last_read_success = read_ok;
        }
        #endif
        if (!read_ok) return;
    }
    #if XASH_PS2
    if (!ps2_first_read) {
        ps2_first_read = 1;
        printf("[XASH-PS2] PAD_FIRST_BUTTONS=0x%04x\n", state.btns);
    }
    #endif
    buttons = (~(unsigned)state.btns) & 0xffffu;
    changed = buttons ^ ps2_old_buttons;
    for (i = 0; i < sizeof(keymap)/sizeof(keymap[0]); ++i) {
        if (changed & keymap[i].physical) {
            int down = (buttons & keymap[i].physical) != 0;
            int logical = PS2_ButtonKey(keymap[i].physical, down,
                keymap[i].logical);
            #if XASH_PS2
            if (ps2_logged_keys < 30) {
                printf("[XASH-PS2] PAD_EVENT key=%d down=%u\n",
                    logical, (unsigned)down);
                ps2_logged_keys++;
            }
            #endif
            Key_Event(logical, down);
        }
    }
    ps2_old_buttons = buttons;

    // PS2SDK returns 0..255, centered at 128. The common joystick code
    // expects signed axes (left x, left y, right y, right x) in -32768..32767.
    /* Debug (camera turning by itself): raw sticks and view angles every 120 polls. */
    {
        static unsigned polls;
        if (cls.state == ca_active && ++polls % 120 == 0)
            printf("[XASH-PS2] VIEW yaw=%.4f pitch=%.4f lx=%d ly=%d rx=%d ry=%d mode=%d\n",
                cl.viewangles[YAW], cl.viewangles[PITCH], state.ljoy_h, state.ljoy_v,
                state.rjoy_h, state.rjoy_v, padInfoMode(0, 0, PAD_MODECURID, 0));
    }

    if (padInfoMode(0, 0, PAD_MODECURID, 0) == PAD_TYPE_DUALSHOCK) {
        Joy_AxisMotionEvent(0, ((int)state.ljoy_h - 128) * 256);
        Joy_AxisMotionEvent(1, ((int)state.ljoy_v - 128) * 256);
        Joy_AxisMotionEvent(2, ((int)state.rjoy_v - 128) * 256);
        Joy_AxisMotionEvent(3, ((int)state.rjoy_h - 128) * 256);
    } else {
        // Avoid stale stick movement if the controller switches to digital mode.
        for (i = 0; i < 4; ++i)
            Joy_AxisMotionEvent(i, 0);
    }
}

void Platform_EnableTextInput(qboolean enabled) { ps2_text_input = enabled; }
void Platform_GetMousePos(int *x, int *y)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}
void Platform_SetMousePos(int x, int y) { mouse_x = x; mouse_y = y; }
void Platform_MouseMove(float *x, float *y)
{
    // Engine look path applies only m_yaw/m_pitch; add CS's sensitivity.
    float scale = Cvar_VariableValue("sensitivity");
    if (scale <= 0) scale = 1;
    *x = ps2_mouse_dx * scale;
    *y = ps2_mouse_dy * scale;
    ps2_mouse_dx = ps2_mouse_dy = 0;
}
void Platform_PreCreateMove(void) {}
