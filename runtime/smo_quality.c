/* smo_quality.c - dynamic_resolution = off (graphics.ini): the game's 3D at the full size it asks for.
 *
 * al::GraphicsQualityInfo::calcRenderResolution [main 0xa58394] (out, requested size, view info) gives a view's render
 * size: a fixed 680 x 680 for one kind of view (view info +4 set), otherwise the requested size capped by the quality
 * info's own size, which al::GraphicsQualityController lowers when frames run long. Here frames run long because of the
 * CPU, so the cap only made the 3D coarse (800 x 450 measured on a Galaxy S24 Ultra) without helping the frame rate.
 * With dynamic_resolution = off the cap is not applied. libsmo.so is linked with -Wl,--wrap=f_4000a58394
 * (tools/android/build_runtime.py); the one caller is in another object. Android port (2026-11-02). */
#include <stdio.h>
#include <stdlib.h>
#include "plat.h"
#include "runtime.h"
#include "gfx_options.h"

void __real_f_4000a58394(Ctx *c);
static int g_full = -1;
static volatile uint32_t g_rs_in_w, g_rs_w, g_rs_h;  /* 0.3: smo_render_size */

/* 0.1.8: with real GPU timers (0.1.7) the game's controller moved its cap about 5 times a second on a phone (S24 Ultra, 60 fps
 * mode: 300 changes in a minute), between 800 and 1600 wide, and the game makes its render targets again at each new size:
 * Cascade 44 fps with that, 54-57 without. Here the main view (the largest size asked for) follows the cap slowly and in steps:
 * a cap under the size for RES_DOWN_MS takes it one step down, a cap over it for g_up_ms one step up. When a step up is taken
 * back soon (RES_FAILED_MS) the wait for the next one doubles (up to RES_UP_MAX_MS), so the size settles where the GPU keeps up
 * and tries more only now and then. Views that ask for less go through untouched.
 * SMO_RES_HOLD=0: the game's cap as it is. SMO_RES_FIX=<width>: that width all the time (a test). */
#define RES_DOWN_MS    1000
#define RES_UP_MS      8000
#define RES_UP_MAX_MS  128000
#define RES_FAILED_MS  16000
#define RES_STEP       96
static void res_hold(uint64_t out, uint64_t in) {
    static int on = -1, fix, g_up_ms = RES_UP_MS, want_dir;      /* want_dir: the cap's side of the size, and since when */
    static uint32_t main_w, held_w;
    static int64_t want_at, up_at, ticks_per_ms;
    static long long changes;
    if (on < 0) {
        const char *e = getenv("SMO_RES_HOLD");
        int parsed_fix;
        on = !e || atoi(e) != 0;
        e = getenv("SMO_RES_FIX");
        parsed_fix = e ? atoi(e) : 0;
        fix = parsed_fix > 0 ? parsed_fix : 0;
        printf("smo_quality: render size %s\n", fix ? "fixed (SMO_RES_FIX)" : on ? "follows the game's cap slowly, in steps" : "as the game's cap (SMO_RES_HOLD=0)");
    }
    if (!on) return;
    uint32_t iw = rd32(in), ih = rd32(in + 4), w = rd32(out);
    if (!iw) return;
    if (iw > main_w) { main_w = iw; held_w = 0; }  /* a larger main view: start from its cap */
    if (iw != main_w) return;
    if (fix) { w = (uint32_t)fix < iw ? (uint32_t)fix : iw; wr32(out, w); wr32(out + 4, (uint32_t)((uint64_t)w * ih / iw)); return; }
    if (!held_w) { held_w = w; want_dir = 0; return; }
    int dir = w < held_w ? -1 : w > held_w ? 1 : 0;
    int64_t now = 0;
    if (dir) {
        now = plat_qpc();
        if (dir != want_dir) {
            want_dir = dir;
            want_at = now;
        }
        if (!ticks_per_ms) {
            ticks_per_ms = plat_qpf() / 1000;
            if (!ticks_per_ms) ticks_per_ms = 1;
        }
        if (now - want_at >= (int64_t)(dir < 0 ? RES_DOWN_MS : g_up_ms) * ticks_per_ms) {
            uint32_t next = dir < 0 ? (held_w > RES_STEP ? held_w - RES_STEP : held_w) : held_w + RES_STEP;
            if (dir < 0 && next < w) next = w;
            if (dir > 0 && next > w) next = w;
            if (dir < 0 && up_at && now - up_at < (int64_t)RES_FAILED_MS * ticks_per_ms) g_up_ms = g_up_ms * 2 < RES_UP_MAX_MS ? g_up_ms * 2 : RES_UP_MAX_MS;      /* the last step up did not hold */
            if (dir > 0) up_at = now;
            held_w = next; want_dir = 0; want_at = now;
            if (++changes <= 300) { printf("smo_quality: render size now %u wide (change %lld, next step up after %d s of room)\n", held_w, changes, g_up_ms / 1000); fflush(stdout); }
        }
    } else {
        want_dir = 0;
    }
    wr32(out, held_w); wr32(out + 4, (uint32_t)((uint64_t)held_w * ih / iw));
}

void __wrap_f_4000a58394(Ctx *c) {
    uint64_t out = c->x[1], in = c->x[2], view = c->x[3];
    if (g_full < 0) {
        const char *e = getenv("SMO_DYNRES");
        g_full = e ? !atoi(e) : !gfx_options()->dynres;
        printf("smo_quality: dynamic_resolution = %s\n", g_full ? "off: the 3D is drawn at the full size the game asks for" : "on: the game caps its 3D size by its quality setting");
    }
    __real_f_4000a58394(c);
    uint32_t subview = rd32(view + 4);
    uint32_t in_w = rd32(in), in_h = rd32(in + 4);
    uint32_t out_w = rd32(out), out_h = rd32(out + 4);
    static int nlog; static uint32_t last_w, last_h;
    if (nlog < 300 && !subview && (out_w != last_w || out_h != last_h)) {      /* 0.1.7: when it changes */
        nlog++; last_w = out_w; last_h = out_h;
        printf("smo_quality: render size asked %ux%u, the game's cap gives %ux%u%s\n", in_w, in_h, out_w, out_h, g_full ? " (cap not applied)" : "");
        fflush(stdout);
    }
    if (!subview) {
        if (g_full) {
            wr32(out, in_w);
            wr32(out + 4, in_h);
        } else {
            res_hold(out, in);
        }
        if (in_w >= g_rs_in_w) {
            g_rs_in_w = in_w;
            g_rs_w = rd32(out);
            g_rs_h = rd32(out + 4);
        }
    }
}

/* 0.3: the main view's render size (the largest asked for since the last call) for android_perf.c's perf line; 0 when the game
 * asked for none. The next window starts afresh, so a change of mode (docked / handheld) shows within one line. */
void smo_render_size(unsigned *w, unsigned *h, unsigned *asked_w) {
    *w = g_rs_w; *h = g_rs_h; *asked_w = g_rs_in_w;
    g_rs_in_w = 0;
}
