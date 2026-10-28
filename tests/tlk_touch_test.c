/* The touchscreen's SNES pad: where its buttons are and what fingers on it hold. */
#include "tlk_touch.h"

#include <stdio.h>

static int s_failed;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            s_failed++;                                                       \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond);        \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)

/* The Fold 7's outer and inner screens in landscape, and a small tablet. */
static const int kScreens[][2] = { { 2520, 1080 }, { 2184, 1968 }, { 1280, 800 } };
enum { kScreenCount = sizeof kScreens / sizeof kScreens[0] };

static TlkTouchLayout Layout(int s) {
    TlkTouchLayout l;
    int w = kScreens[s][0], h = kScreens[s][1];
    tlk_touch_layout(&l, w, h, (float)(w < h ? w : h));
    return l;
}

static uint16_t Held1(const TlkTouchLayout *l, float x, float y, int on_dpad) {
    TlkTouchFinger f = { x, y, on_dpad };
    return tlk_touch_buttons(l, &f, 1);
}

static void touch_finger_on_each_button_gives_its_bit(void) {
    for (int s = 0; s < kScreenCount; s++) {
        TlkTouchLayout l = Layout(s);
        uint16_t all = 0;
        for (int i = 0; i < TLK_TOUCH_BUTTONS; i++) {
            const TlkTouchButton *b = &l.buttons[i];
            CHECK(Held1(&l, b->x, b->y, 0) == b->bit, "%s on %dx%d", b->label, kScreens[s][0], kScreens[s][1]);
            all |= b->bit;
        }
        CHECK(all == (TLK_PAD_B | TLK_PAD_Y | TLK_PAD_SELECT | TLK_PAD_START | TLK_PAD_A | TLK_PAD_X |
                      TLK_PAD_L | TLK_PAD_R), "buttons %04x", all);
        CHECK(Held1(&l, kScreens[s][0] / 2.0f, kScreens[s][1] / 2.0f, 0) == 0, "the middle of the screen");
    }
}

static void touch_dpad_directions_and_dead_centre(void) {
    TlkTouchLayout l = Layout(0);
    float r = l.dpad_r, x = l.dpad_x, y = l.dpad_y;
    CHECK(Held1(&l, x, y, 1) == 0, "centre");
    CHECK(Held1(&l, x, y - r, 1) == TLK_PAD_UP, "up");
    CHECK(Held1(&l, x + r, y, 1) == TLK_PAD_RIGHT, "right");
    CHECK(Held1(&l, x + 0.7f * r, y - 0.7f * r, 1) == (TLK_PAD_UP | TLK_PAD_RIGHT), "up-right");
    CHECK(Held1(&l, x - 3 * r, y + 3 * r, 1) == (TLK_PAD_DOWN | TLK_PAD_LEFT), "a finger slid off the d-pad keeps it");
}

static void touch_two_fingers_combine_and_lifting_one_clears_it(void) {
    TlkTouchLayout l = Layout(0);
    const TlkTouchButton *b = &l.buttons[2];
    TlkTouchFinger both[2] = { { b->x, b->y, 0 }, { l.dpad_x + l.dpad_r, l.dpad_y, 1 } };
    CHECK(tlk_touch_buttons(&l, both, 2) == (TLK_PAD_B | TLK_PAD_RIGHT), "B and right");
    CHECK(tlk_touch_buttons(&l, both + 1, 1) == TLK_PAD_RIGHT, "right alone");
}

static void touch_layout_fits_outer_and_inner_fold_screens(void) {
    for (int s = 0; s < kScreenCount; s++) {
        TlkTouchLayout l = Layout(s);
        int w = kScreens[s][0], h = kScreens[s][1];
        for (int i = 0; i < TLK_TOUCH_BUTTONS; i++) {
            const TlkTouchButton *a = &l.buttons[i];
            float rw = a->shape == TLK_TOUCH_ROUND ? a->w : a->w / 2, rh = a->shape == TLK_TOUCH_ROUND ? a->w : a->h / 2;
            CHECK(a->x - rw >= 0 && a->x + rw <= w && a->y - rh >= 0 && a->y + rh <= h, "%s on screen", a->label);
            CHECK(!tlk_touch_on_dpad(&l, a->x, a->y), "%s clear of the d-pad", a->label);
            CHECK(!tlk_touch_on_menu(&l, a->x, a->y), "%s clear of the menu button", a->label);
            for (int j = 0; j < TLK_TOUCH_BUTTONS; j++)
                if (i != j) CHECK(tlk_touch_distance(a, l.buttons[j].x, l.buttons[j].y) > 0, "%s apart from %s", a->label, l.buttons[j].label);
        }
    }
}

int main(void) {
    touch_finger_on_each_button_gives_its_bit();
    touch_dpad_directions_and_dead_centre();
    touch_two_fingers_combine_and_lifting_one_clears_it();
    touch_layout_fits_outer_and_inner_fold_screens();
    printf("%s (%d failed)\n", s_failed ? "FAIL" : "PASS", s_failed);
    return s_failed != 0;
}
