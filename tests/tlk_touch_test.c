/* The touchscreen's SNES pad: where its buttons are and what fingers on it hold. */
#include "tlk_touch.h"

#include <math.h>
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

/* The Fold 7's outer and inner screens in landscape, a small tablet, and a small old phone. */
static const int kScreens[][2] = { { 2520, 1080 }, { 2184, 1968 }, { 1280, 800 }, { 800, 480 } };
enum { kScreenCount = sizeof kScreens / sizeof kScreens[0] };
/* touch_size at its minimum, default, and maximum, in percent. */
static const int kSizes[] = { 50, 100, 150 };
enum { kSizeCount = sizeof kSizes / sizeof kSizes[0] };

static TlkTouchLayout LayoutAt(int s, int size) {
    TlkTouchLayout l;
    int w = kScreens[s][0], h = kScreens[s][1];
    tlk_touch_layout(&l, w, h, (float)(w < h ? w : h) * (float)size / 100.0f);
    return l;
}
static TlkTouchLayout Layout(int s) { return LayoutAt(s, 100); }

static const TlkTouchButton *Find(const TlkTouchLayout *l, uint16_t bit) {
    for (int i = 0; i < TLK_TOUCH_BUTTONS; i++)
        if (l->buttons[i].bit == bit) return &l->buttons[i];
    return 0;
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
    CHECK(Held1(&l, x, y - 0.15f * r, 1) == 0, "inside the dead zone");
    CHECK(Held1(&l, x, y - 0.25f * r, 1) == TLK_PAD_UP, "just outside the dead zone");
    CHECK(Held1(&l, x, y - r, 1) == TLK_PAD_UP, "up");
    CHECK(Held1(&l, x + r, y, 1) == TLK_PAD_RIGHT, "right");
    CHECK(Held1(&l, x + 0.7f * r, y - 0.7f * r, 1) == (TLK_PAD_UP | TLK_PAD_RIGHT), "up-right");
    /* Eight directions of 45 degrees: 30 degrees off right is up-right, 15 degrees is right. */
    CHECK(Held1(&l, x + 0.866f * r, y - 0.5f * r, 1) == (TLK_PAD_UP | TLK_PAD_RIGHT), "30 degrees");
    CHECK(Held1(&l, x + 0.966f * r, y - 0.259f * r, 1) == TLK_PAD_RIGHT, "15 degrees");
    CHECK(Held1(&l, x - 3 * r, y + 3 * r, 1) == (TLK_PAD_DOWN | TLK_PAD_LEFT), "a finger slid off the d-pad keeps it");
}

static void touch_dpad_and_menu_take_fingers_near_them(void) {
    TlkTouchLayout l = Layout(0);
    CHECK(tlk_touch_on_dpad(&l, l.dpad_x, l.dpad_y), "the d-pad's centre");
    CHECK(tlk_touch_on_dpad(&l, l.dpad_x + 1.1f * l.dpad_r, l.dpad_y), "just past an arm");
    CHECK(!tlk_touch_on_dpad(&l, l.dpad_x + 1.3f * l.dpad_r, l.dpad_y), "well past an arm");
    CHECK(tlk_touch_on_menu(&l, l.menu_x, l.menu_y), "the menu button");
    CHECK(tlk_touch_on_menu(&l, kScreens[0][0] / 2.0f, l.menu_y + 1.4f * l.menu_r), "just below the menu button");
    CHECK(!tlk_touch_on_menu(&l, kScreens[0][0] / 2.0f, kScreens[0][1] / 2.0f), "the middle of the screen");
}

static void touch_finger_near_a_button_takes_the_nearest(void) {
    TlkTouchLayout l = Layout(0);
    const TlkTouchButton *x = Find(&l, TLK_PAD_X), *a = Find(&l, TLK_PAD_A);
    float r = a->w;
    CHECK(Held1(&l, a->x + 1.4f * r, a->y, 0) == TLK_PAD_A, "just outside A, within its margin");
    CHECK(Held1(&l, a->x + 1.6f * r, a->y, 0) == 0, "past A's margin");
    /* Between X and A, a little nearer A. */
    float px = x->x + 0.6f * (a->x - x->x), py = x->y + 0.6f * (a->y - x->y);
    CHECK(Held1(&l, px, py, 0) == TLK_PAD_A, "the nearer of two buttons");
}

static void touch_two_face_buttons_at_once(void) {
    TlkTouchLayout l = Layout(0);
    const TlkTouchButton *x = Find(&l, TLK_PAD_X), *a = Find(&l, TLK_PAD_A);
    TlkTouchFinger f[2] = { { x->x, x->y, 0 }, { a->x, a->y, 0 } };
    CHECK(tlk_touch_buttons(&l, f, 2) == (TLK_PAD_X | TLK_PAD_A), "X and A");
}

static void touch_two_fingers_combine_and_lifting_one_clears_it(void) {
    TlkTouchLayout l = Layout(0);
    const TlkTouchButton *b = &l.buttons[2];
    TlkTouchFinger both[2] = { { b->x, b->y, 0 }, { l.dpad_x + l.dpad_r, l.dpad_y, 1 } };
    CHECK(tlk_touch_buttons(&l, both, 2) == (TLK_PAD_B | TLK_PAD_RIGHT), "B and right");
    CHECK(tlk_touch_buttons(&l, both + 1, 1) == TLK_PAD_RIGHT, "right alone");
}

/* Points just inside a button's outline, all the way round: n around it, then a pill's four corners. */
static void EdgePoint(const TlkTouchButton *b, int k, int n, float *x, float *y) {
    if (k >= n) {
        *x = b->x + (k & 1 ? 0.495f : -0.495f) * b->w;
        *y = b->y + (k & 2 ? 0.495f : -0.495f) * b->h;
        return;
    }
    float t = 6.2831853f * (float)k / (float)n, c = cosf(t), s = sinf(t);
    if (b->shape == TLK_TOUCH_ROUND) { *x = b->x + 0.98f * b->w * c; *y = b->y + 0.98f * b->w * s; return; }
    float hw = 0.49f * b->w, hh = 0.49f * b->h, k2 = fabsf(c) * hh > fabsf(s) * hw ? hw / fabsf(c) : hh / fabsf(s);
    *x = b->x + k2 * c;
    *y = b->y + k2 * s;
}

static void touch_layout_fits_every_screen_and_size_without_overlap(void) {
    for (int s = 0; s < kScreenCount; s++)
        for (int z = 0; z < kSizeCount; z++) {
            TlkTouchLayout l = LayoutAt(s, kSizes[z]);
            int w = kScreens[s][0], h = kScreens[s][1];
            for (int i = 0; i < TLK_TOUCH_BUTTONS; i++) {
                const TlkTouchButton *a = &l.buttons[i];
                float rw = a->shape == TLK_TOUCH_ROUND ? a->w : a->w / 2, rh = a->shape == TLK_TOUCH_ROUND ? a->w : a->h / 2;
                CHECK(a->x - rw >= 0 && a->x + rw <= w && a->y - rh >= 0 && a->y + rh <= h,
                      "%s on screen at %dx%d size %d", a->label, w, h, kSizes[z]);
                for (int k = 0; k < (a->shape == TLK_TOUCH_PILL ? 36 : 32); k++) {
                    float x, y;
                    EdgePoint(a, k, 32, &x, &y);
                    CHECK(!tlk_touch_on_dpad(&l, x, y), "%s's edge clear of the d-pad at %dx%d size %d", a->label, w, h, kSizes[z]);
                    CHECK(!tlk_touch_on_menu(&l, x, y), "%s's edge clear of the menu button at %dx%d size %d", a->label, w, h, kSizes[z]);
                }
                for (int j = 0; j < TLK_TOUCH_BUTTONS; j++)
                    if (i != j) CHECK(tlk_touch_distance(a, l.buttons[j].x, l.buttons[j].y) > 0, "%s apart from %s", a->label, l.buttons[j].label);
            }
        }
}

static void touch_pad_too_big_for_the_screen_shrinks_to_fit(void) {
    TlkTouchLayout l;
    tlk_touch_layout(&l, 800, 480, 10000.0f);
    CHECK(l.dpad_x - l.dpad_r >= 0 && l.dpad_y + l.dpad_r <= 480, "the d-pad on screen");
    for (int i = 0; i < TLK_TOUCH_BUTTONS; i++) {
        const TlkTouchButton *b = &l.buttons[i];
        float rw = b->shape == TLK_TOUCH_ROUND ? b->w : b->w / 2;
        CHECK(b->x - rw >= 0 && b->x + rw <= 800, "%s on screen", b->label);
    }
    CHECK(l.menu_x == 400.0f && l.menu_y - l.menu_r >= 0 && l.menu_y < 100, "the menu button top centre");
}

int main(void) {
    touch_finger_on_each_button_gives_its_bit();
    touch_dpad_directions_and_dead_centre();
    touch_dpad_and_menu_take_fingers_near_them();
    touch_finger_near_a_button_takes_the_nearest();
    touch_two_face_buttons_at_once();
    touch_two_fingers_combine_and_lifting_one_clears_it();
    touch_layout_fits_every_screen_and_size_without_overlap();
    touch_pad_too_big_for_the_screen_shrinks_to_fit();
    printf("%s (%d failed)\n", s_failed ? "FAIL" : "PASS", s_failed);
    return s_failed != 0;
}
