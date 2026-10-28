/* tlk_touch.c -- the touchscreen pad's layout and hit test (tlk_touch.h). */
#include "tlk_touch.h"

#include <math.h>

static float MinF(float a, float b) { return a < b ? a : b; }
static float MaxF(float a, float b) { return a > b ? a : b; }

void tlk_touch_layout(TlkTouchLayout *l, int w, int h, float unit) {
    float u = MinF(unit, MinF(w / 1.1f, h / 0.56f)), m = 0.05f * u;
    l->dpad_r = 0.17f * u;
    l->dpad_x = m + l->dpad_r;
    l->dpad_y = h - m - l->dpad_r;
    l->menu_r = 0.045f * u;
    l->menu_x = w / 2.0f;
    l->menu_y = m + l->menu_r;

    float r = 0.068f * u, spread = 1.45f * r;   /* each face button's radius and its distance from the diamond's centre */
    float cx = w - m - r - spread, cy = h - m - r - spread;
    TlkTouchButton *b = l->buttons;
    *b++ = (TlkTouchButton){ "X", TLK_PAD_X, TLK_TOUCH_ROUND, cx, cy - spread, r, r };
    *b++ = (TlkTouchButton){ "A", TLK_PAD_A, TLK_TOUCH_ROUND, cx + spread, cy, r, r };
    *b++ = (TlkTouchButton){ "B", TLK_PAD_B, TLK_TOUCH_ROUND, cx, cy + spread, r, r };
    *b++ = (TlkTouchButton){ "Y", TLK_PAD_Y, TLK_TOUCH_ROUND, cx - spread, cy, r, r };

    float pw = 2.2f * r, ph = 0.8f * r, py = h - m - ph / 2;
    *b++ = (TlkTouchButton){ "SELECT", TLK_PAD_SELECT, TLK_TOUCH_PILL, w / 2.0f - 0.65f * pw, py, pw, ph };
    *b++ = (TlkTouchButton){ "START", TLK_PAD_START, TLK_TOUCH_PILL, w / 2.0f + 0.65f * pw, py, pw, ph };

    float sw = 0.3f * u, sh = 0.1f * u, above = 0.045f * u;
    *b++ = (TlkTouchButton){ "L", TLK_PAD_L, TLK_TOUCH_PILL, l->dpad_x, l->dpad_y - l->dpad_r - above - sh / 2, sw, sh };
    float top = cy - spread - r;
    *b++ = (TlkTouchButton){ "R", TLK_PAD_R, TLK_TOUCH_PILL, w - m - sw / 2, top - above - sh / 2, sw, sh };
}

int tlk_touch_on_dpad(const TlkTouchLayout *l, float x, float y) {
    return hypotf(x - l->dpad_x, y - l->dpad_y) < 1.2f * l->dpad_r;
}

int tlk_touch_on_menu(const TlkTouchLayout *l, float x, float y) {
    return hypotf(x - l->menu_x, y - l->menu_y) < 1.5f * l->menu_r;
}

float tlk_touch_distance(const TlkTouchButton *b, float x, float y) {
    if (b->shape == TLK_TOUCH_ROUND) return hypotf(x - b->x, y - b->y) - b->w;
    float dx = MaxF(fabsf(x - b->x) - b->w / 2, 0.0f), dy = MaxF(fabsf(y - b->y) - b->h / 2, 0.0f);
    return hypotf(dx, dy);
}

uint16_t tlk_touch_buttons(const TlkTouchLayout *l, const TlkTouchFinger *fingers, int n) {
    uint16_t held = 0;
    for (int i = 0; i < n; i++) {
        const TlkTouchFinger *f = &fingers[i];
        if (f->on_dpad) {
            float dx = f->x - l->dpad_x, dy = f->y - l->dpad_y, d = hypotf(dx, dy);
            if (d < 0.2f * l->dpad_r) continue;
            const float lean = 0.38f;   /* sin 22.5 degrees: eight directions of 45 degrees each */
            if (dx / d > lean) held |= TLK_PAD_RIGHT;
            if (dx / d < -lean) held |= TLK_PAD_LEFT;
            if (dy / d > lean) held |= TLK_PAD_DOWN;
            if (dy / d < -lean) held |= TLK_PAD_UP;
            continue;
        }
        const TlkTouchButton *best = 0;
        float best_d = 0;
        for (int j = 0; j < TLK_TOUCH_BUTTONS; j++) {
            const TlkTouchButton *b = &l->buttons[j];
            float d = tlk_touch_distance(b, f->x, f->y), slack = 0.5f * MinF(b->w, b->h);
            if (d < slack && (!best || d < best_d)) best = b, best_d = d;
        }
        if (best) held |= best->bit;
    }
    return held;
}
