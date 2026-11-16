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

/* The Fold 7's outer and inner screens in landscape, a small tablet, and a small old phone, each with
 * its display scale (the Fold's 420 dpi is 2.625). */
static const struct { int w, h; float scale; } kScreens[] = {
    { 2520, 1080, 2.625f }, { 2184, 1968, 2.625f }, { 1280, 800, 1.5f }, { 800, 480, 1.5f } };
enum { kScreenCount = sizeof kScreens / sizeof kScreens[0] };
/* touch_size at its minimum, default, and maximum, in percent. */
static const int kSizes[] = { 50, 100, 150 };
enum { kSizeCount = sizeof kSizes / sizeof kSizes[0] };

static TlkTouchLayout LayoutAt(int s, int size) {
    TlkTouchLayout l;
    tlk_touch_layout(&l, 0, 0, (float)kScreens[s].w, (float)kScreens[s].h, tlk_touch_unit(kScreens[s].scale, size));
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
            CHECK(Held1(&l, b->x, b->y, 0) == b->bit, "%s on %dx%d", b->label, kScreens[s].w, kScreens[s].h);
            all |= b->bit;
        }
        CHECK(all == (TLK_PAD_B | TLK_PAD_Y | TLK_PAD_SELECT | TLK_PAD_START | TLK_PAD_A | TLK_PAD_X |
                      TLK_PAD_L | TLK_PAD_R), "buttons %04x", all);
        CHECK(Held1(&l, kScreens[s].w / 2.0f, kScreens[s].h / 2.0f, 0) == 0, "the middle of the screen");
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
    CHECK(tlk_touch_on_menu(&l, kScreens[0].w / 2.0f, l.menu_y + 1.4f * l.menu_r), "just below the menu button");
    CHECK(!tlk_touch_on_menu(&l, kScreens[0].w / 2.0f, kScreens[0].h / 2.0f), "the middle of the screen");
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
            int w = kScreens[s].w, h = kScreens[s].h;
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
    tlk_touch_layout(&l, 0, 0, 800, 480, 10000.0f);
    CHECK(l.dpad_x - l.dpad_r >= 0 && l.dpad_y + l.dpad_r <= 480, "the d-pad on screen");
    for (int i = 0; i < TLK_TOUCH_BUTTONS; i++) {
        const TlkTouchButton *b = &l.buttons[i];
        float rw = b->shape == TLK_TOUCH_ROUND ? b->w : b->w / 2;
        CHECK(b->x - rw >= 0 && b->x + rw <= 800, "%s on screen", b->label);
    }
    CHECK(l.menu_x == 400.0f && l.menu_y - l.menu_r >= 0 && l.menu_y < 100, "the menu button top centre");
}

static void touch_unit_is_480_density_independent_pixels_times_the_size(void) {
    CHECK(tlk_touch_unit(2.625f, 100) == 1260.0f, "%g", tlk_touch_unit(2.625f, 100));
    CHECK(tlk_touch_unit(2.625f, 50) == 630.0f, "%g", tlk_touch_unit(2.625f, 50));
    CHECK(tlk_touch_unit(1.0f, 150) == 720.0f, "%g", tlk_touch_unit(1.0f, 150));
    CHECK(tlk_touch_unit(0.0f, 100) == 480.0f, "an unknown scale counts as 1: %g", tlk_touch_unit(0.0f, 100));
}

static void touch_pad_is_the_same_size_on_both_fold_screens(void) {
    TlkTouchLayout outer = Layout(0), inner = Layout(1);
    CHECK(outer.dpad_r == inner.dpad_r, "d-pad %g outer, %g inner", outer.dpad_r, inner.dpad_r);
    CHECK(Find(&outer, TLK_PAD_A)->w == Find(&inner, TLK_PAD_A)->w, "A %g outer, %g inner",
          Find(&outer, TLK_PAD_A)->w, Find(&inner, TLK_PAD_A)->w);
}

/* The Fold's outer screen in landscape with the cutout on the left and the navigation bar on the right. */
static void touch_pad_stays_inside_the_safe_area(void) {
    const float x = 110, y = 40, w = 2520 - 110 - 130, h = 1080 - 40;
    TlkTouchLayout l;
    tlk_touch_layout(&l, x, y, w, h, tlk_touch_unit(2.625f, 150));
    CHECK(l.dpad_x - l.dpad_r >= x && l.dpad_y + l.dpad_r <= y + h, "the d-pad inside: %g %g", l.dpad_x, l.dpad_y);
    for (int i = 0; i < TLK_TOUCH_BUTTONS; i++) {
        const TlkTouchButton *b = &l.buttons[i];
        float rw = b->shape == TLK_TOUCH_ROUND ? b->w : b->w / 2, rh = b->shape == TLK_TOUCH_ROUND ? b->w : b->h / 2;
        CHECK(b->x - rw >= x && b->x + rw <= x + w && b->y - rh >= y && b->y + rh <= y + h, "%s inside: %g %g",
              b->label, b->x, b->y);
    }
    CHECK(l.menu_x == x + w / 2 && l.menu_y - l.menu_r >= y, "the menu button top centre of the safe area");
}

/* A HiDPI window: 1000x500 points drawn at 2000x1000 pixels, with 40 points of bar on the left. */
static void touch_area_is_the_safe_area_in_pixels_or_the_whole_output(void) {
    TlkTouchRect a = tlk_touch_area(2000, 1000, 1000, 500, 40, 10, 900, 480);
    CHECK(a.x == 80 && a.y == 20 && a.w == 1800 && a.h == 960, "%g %g %g %g", a.x, a.y, a.w, a.h);
    a = tlk_touch_area(2000, 1000, 0, 0, 40, 10, 900, 480);
    CHECK(a.x == 0 && a.y == 0 && a.w == 2000 && a.h == 1000, "no window size: %g %g %g %g", a.x, a.y, a.w, a.h);
    a = tlk_touch_area(2000, 1000, 1000, 500, 0, 0, 0, 0);
    CHECK(a.x == 0 && a.w == 2000 && a.h == 1000, "an empty safe area: %g %g %g %g", a.x, a.y, a.w, a.h);
}

static void touch_layout_at_an_origin_is_the_one_at_zero_shifted(void) {
    TlkTouchLayout o, l;
    tlk_touch_layout(&o, 0, 0, 2000, 900, 1260);
    tlk_touch_layout(&l, 110, 40, 2000, 900, 1260);
    CHECK(l.dpad_x == o.dpad_x + 110 && l.dpad_y == o.dpad_y + 40 && l.dpad_r == o.dpad_r, "the d-pad");
    CHECK(l.menu_x == o.menu_x + 110 && l.menu_y == o.menu_y + 40, "the menu button");
    for (int i = 0; i < TLK_TOUCH_BUTTONS; i++)
        CHECK(l.buttons[i].x == o.buttons[i].x + 110 && l.buttons[i].y == o.buttons[i].y + 40 &&
              l.buttons[i].w == o.buttons[i].w && l.buttons[i].h == o.buttons[i].h, "%s", l.buttons[i].label);
}

int main(void) {
    touch_area_is_the_safe_area_in_pixels_or_the_whole_output();
    touch_layout_at_an_origin_is_the_one_at_zero_shifted();
    touch_unit_is_480_density_independent_pixels_times_the_size();
    touch_pad_is_the_same_size_on_both_fold_screens();
    touch_pad_stays_inside_the_safe_area();
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
