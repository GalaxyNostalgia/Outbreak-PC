/* Controllers (Sony libpad2 + libdbc) for the PC port.
 *
 * Port 1 has a DualShock 2. It is played with the keyboard or any gamepad SDL knows (mapped the way a
 * PS2 pad's buttons sit on an Xbox-style pad), and for tests `--input` presses buttons on given
 * v-blanks. libpad2 hands the game the PS2's raw report: two bytes of buttons (active low), the
 * sticks, and the twelve pressure bytes.
 *
 * Keyboard: arrows = d-pad, Enter = START, Backspace = SELECT, Z = Cross, X = Circle, A = Square,
 * S = Triangle, Q/W = L1/R1, 1/2 = L2/R2. Numpad 8/4/2/6 = left stick.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SDL2
#include <SDL.h>
#endif
#include "libpad2.h"
#include "log.h"

/* PS2 pad button bits, as the 16-bit value (first byte << 8 | second byte) has them */
enum {
    BTN_SELECT = 0x0100, BTN_L3 = 0x0200, BTN_R3 = 0x0400, BTN_START = 0x0800,
    BTN_UP = 0x1000, BTN_RIGHT = 0x2000, BTN_DOWN = 0x4000, BTN_LEFT = 0x8000,
    BTN_L2 = 0x0001, BTN_R2 = 0x0002, BTN_L1 = 0x0004, BTN_R1 = 0x0008,
    BTN_TRIANGLE = 0x0010, BTN_CIRCLE = 0x0020, BTN_CROSS = 0x0040, BTN_SQUARE = 0x0080,
};

static const struct { const char *name; uint16_t bit; } g_names[] = {
    {"SELECT", BTN_SELECT}, {"L3", BTN_L3}, {"R3", BTN_R3}, {"START", BTN_START}, {"UP", BTN_UP},
    {"RIGHT", BTN_RIGHT}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"L2", BTN_L2}, {"R2", BTN_R2},
    {"L1", BTN_L1}, {"R1", BTN_R1}, {"TRIANGLE", BTN_TRIANGLE}, {"CIRCLE", BTN_CIRCLE},
    {"CROSS", BTN_CROSS}, {"SQUARE", BTN_SQUARE},
};

/* --input "START@1500,CROSS@1600+10": button presses on v-blanks (+frames held, default 6) */
#define MAX_SCRIPT 256
static struct { uint16_t bit; uint32_t from, to; } g_script[MAX_SCRIPT];
static int g_nscript;
extern uint32_t pc_vblank_count;

void pc_pad_script(const char *spec) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok && g_nscript < MAX_SCRIPT; tok = strtok(NULL, ",")) {
        char name[32];
        unsigned at = 0, len = 6;
        if (sscanf(tok, "%31[A-Z0-9]@%u+%u", name, &at, &len) < 2) {
            log_warn("pad: can't read input \"%s\"", tok);
            continue;
        }
        for (size_t i = 0; i < sizeof g_names / sizeof g_names[0]; i++)
            if (!strcmp(name, g_names[i].name)) {
                g_script[g_nscript].bit = g_names[i].bit;
                g_script[g_nscript].from = at;
                g_script[g_nscript].to = at + len;
                g_nscript++;
            }
    }
}

#ifdef HAVE_SDL2
static SDL_GameController *g_pad;

void pc_pad_sdl_event(const void *ev) {
    const SDL_Event *e = ev;
    if (e->type == SDL_CONTROLLERDEVICEADDED && !g_pad) {
        g_pad = SDL_GameControllerOpen(e->cdevice.which);
        if (g_pad) log_info("pad: %s", SDL_GameControllerName(g_pad));
    } else if (e->type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
               e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
        SDL_GameControllerClose(g_pad);
        g_pad = NULL;
    }
}

static uint8_t axis(SDL_GameControllerAxis a) {
    int v = SDL_GameControllerGetAxis(g_pad, a);
    return (uint8_t)((v + 32768) >> 8);
}
#else
void pc_pad_sdl_event(const void *ev) { (void)ev; }
#endif

/* The pad now: buttons (1 = pressed) and sticks rx, ry, lx, ly. */
static void pad_poll(uint16_t *buttons, uint8_t stick[4]) {
    uint16_t b = 0;
    stick[0] = stick[1] = stick[2] = stick[3] = 0x80;
    for (int i = 0; i < g_nscript; i++)
        if (pc_vblank_count >= g_script[i].from && pc_vblank_count < g_script[i].to) b |= g_script[i].bit;
#ifdef HAVE_SDL2
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        static const struct { SDL_Scancode key; uint16_t bit; } keys[] = {
            {SDL_SCANCODE_UP, BTN_UP}, {SDL_SCANCODE_DOWN, BTN_DOWN}, {SDL_SCANCODE_LEFT, BTN_LEFT},
            {SDL_SCANCODE_RIGHT, BTN_RIGHT}, {SDL_SCANCODE_RETURN, BTN_START}, {SDL_SCANCODE_BACKSPACE, BTN_SELECT},
            {SDL_SCANCODE_Z, BTN_CROSS}, {SDL_SCANCODE_X, BTN_CIRCLE}, {SDL_SCANCODE_A, BTN_SQUARE},
            {SDL_SCANCODE_S, BTN_TRIANGLE}, {SDL_SCANCODE_Q, BTN_L1}, {SDL_SCANCODE_W, BTN_R1},
            {SDL_SCANCODE_1, BTN_L2}, {SDL_SCANCODE_2, BTN_R2},
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (k[keys[i].key]) b |= keys[i].bit;
        if (k[SDL_SCANCODE_KP_4]) stick[2] = 0x00;
        if (k[SDL_SCANCODE_KP_6]) stick[2] = 0xFF;
        if (k[SDL_SCANCODE_KP_8]) stick[3] = 0x00;
        if (k[SDL_SCANCODE_KP_2]) stick[3] = 0xFF;
        if (g_pad) {
            static const struct { SDL_GameControllerButton btn; uint16_t bit; } pads[] = {
                {SDL_CONTROLLER_BUTTON_A, BTN_CROSS}, {SDL_CONTROLLER_BUTTON_B, BTN_CIRCLE},
                {SDL_CONTROLLER_BUTTON_X, BTN_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, BTN_TRIANGLE},
                {SDL_CONTROLLER_BUTTON_BACK, BTN_SELECT}, {SDL_CONTROLLER_BUTTON_START, BTN_START},
                {SDL_CONTROLLER_BUTTON_LEFTSTICK, BTN_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, BTN_R3},
                {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R1},
                {SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DOWN},
                {SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_RIGHT},
            };
            for (size_t i = 0; i < sizeof pads / sizeof pads[0]; i++)
                if (SDL_GameControllerGetButton(g_pad, pads[i].btn)) b |= pads[i].bit;
            if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= BTN_L2;
            if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= BTN_R2;
            stick[0] = axis(SDL_CONTROLLER_AXIS_RIGHTX);
            stick[1] = axis(SDL_CONTROLLER_AXIS_RIGHTY);
            stick[2] = axis(SDL_CONTROLLER_AXIS_LEFTX);
            stick[3] = axis(SDL_CONTROLLER_AXIS_LEFTY);
        }
    }
#endif
    *buttons = b;
}

s32 sceDbcInit(void) { return 1; }
s32 sceDbcEnd(void) { return 1; }
s32 scePad2Init(s32 mode) {
#ifdef HAVE_SDL2
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
#endif
    return 1;
}
s32 scePad2End(void) { return 1; }

static int g_sockets;
static int g_socket_port[8];

s32 scePad2CreateSocket(scePad2SocketParam *param, void *buffer) {
    log_info("pad: socket for port %d slot %d", param->port, param->slot);
    if (g_sockets >= 8) return -1;
    g_socket_port[g_sockets] = param->port;
    return g_sockets++;
}

s32 scePad2DeleteSocket(s32 socket) { return 1; }

/* 1: a pad is connected and stable (port 1 only) */
s32 scePad2GetState(s32 socket) {
    return socket >= 0 && socket < g_sockets && g_socket_port[socket] == 0 ? 1 : 0;
}

/* A DualShock 2: digital buttons, both sticks and pressure-sensitive buttons */
s32 scePad2GetButtonProfile(s32 socket, u8 *profile) {
    if (scePad2GetState(socket) != 1) return -1;
    profile[0] = profile[1] = profile[2] = profile[3] = 0xFF;
    return 4;
}

s32 scePad2Read(s32 socket, void *data) {
    u8 *d = data;
    uint16_t b;
    uint8_t stick[4];
    if (scePad2GetState(socket) != 1) return -1;
    pad_poll(&b, stick);
    memset(d, 0, 0x20);
    d[0] = (u8)~(b >> 8);
    d[1] = (u8)~b;
    memcpy(d + 2, stick, 4);
    /* pressure: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 */
    static const uint16_t order[12] = {BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN, BTN_TRIANGLE, BTN_CIRCLE,
                                       BTN_CROSS, BTN_SQUARE, BTN_L1, BTN_R1, BTN_L2, BTN_R2};
    for (int i = 0; i < 12; i++) d[6 + i] = (b & order[i]) ? 0xFF : 0;
    return 18;
}

/* libdbc: actuator (motor) info and data. No motors yet. */
s32 func_00189308(s32 socket, u8 *info) { return -1; }
s32 func_00189388(s32 socket, s32 n, u8 *mode, s32 len, u8 *data) { return 0; }
