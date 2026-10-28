/* tlk_touch.h -- an SNES pad drawn on a touchscreen: where its buttons are and what fingers on it hold.
 *
 * The d-pad sits bottom left, B A Y X in the SNES diamond bottom right, Select and Start between them,
 * L above the d-pad and R above the diamond, and the menu button top centre. `unit` sets the pad's
 * size in pixels, shrunk where the screen is too small for it.
 *
 * Adapted from saturn-recomp's runtime/src/host/touch_pad.h (MIT). */
#pragma once

#include <stdint.h>

/* Bits of the runtime pad word, as sdl_main.c's bindings number them. */
enum {
    TLK_PAD_B = 1u << 0, TLK_PAD_Y = 1u << 1, TLK_PAD_SELECT = 1u << 2, TLK_PAD_START = 1u << 3,
    TLK_PAD_UP = 1u << 4, TLK_PAD_DOWN = 1u << 5, TLK_PAD_LEFT = 1u << 6, TLK_PAD_RIGHT = 1u << 7,
    TLK_PAD_A = 1u << 8, TLK_PAD_X = 1u << 9, TLK_PAD_L = 1u << 10, TLK_PAD_R = 1u << 11,
};

typedef enum { TLK_TOUCH_ROUND, TLK_TOUCH_PILL } TlkTouchShape;

typedef struct {
    const char *label;
    uint16_t bit;
    TlkTouchShape shape;
    float x, y;   /* the centre, in pixels */
    float w, h;   /* ROUND: w is the radius; PILL: the size */
} TlkTouchButton;

enum { TLK_TOUCH_BUTTONS = 8 };

typedef struct {
    float dpad_x, dpad_y, dpad_r;   /* the d-pad's centre and the reach of its arms */
    float menu_x, menu_y, menu_r;
    TlkTouchButton buttons[TLK_TOUCH_BUTTONS];
} TlkTouchLayout;

/* A finger that went down on the d-pad stays on it however far it slides. */
typedef struct {
    float x, y;
    int on_dpad;
} TlkTouchFinger;

void tlk_touch_layout(TlkTouchLayout *l, int w, int h, float unit);
int tlk_touch_on_dpad(const TlkTouchLayout *l, float x, float y);
int tlk_touch_on_menu(const TlkTouchLayout *l, float x, float y);
/* How far a point is outside the button, in pixels: 0 or less is on it. */
float tlk_touch_distance(const TlkTouchButton *b, float x, float y);
/* A finger on the d-pad holds the one or two directions it leans towards; any other presses the
 * nearest button within half a button's size of it. */
uint16_t tlk_touch_buttons(const TlkTouchLayout *l, const TlkTouchFinger *fingers, int n);
