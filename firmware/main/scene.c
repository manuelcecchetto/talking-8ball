// The screen: dark liquid, drifting specks, and the blue triangle that rises with the answer.
// A port of reveal.html with its own compositor rather than LVGL's renderer, because the panel is
// 466x466 over a 40 MHz QSPI link: a full frame takes 22 ms just to send, so smoothness depends on
// sending few pixels. Every frame the render task works out which rectangles changed, composes only
// those (liquid, glows, specks, rings, triangle, bubbles, glass highlight) into two small DMA buffers
// in internal RAM, and sends one while it composes the next. What did not move costs nothing.
// Each item keeps the state it was last drawn with: composing always uses that state, so a rectangle
// that touches an item which has not been redrawn yet never shows it half moved.
// LVGL is only used off screen, to draw the triangle and the answer text once per answer.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cpu.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "mbedtls/base64.h"
#include "app.h"

static const char *TAG = "scene";

#define W 466
#define R (W / 2)
#define TRI_L 370.0f                // corners 214 px from the centre: the biggest that clears the rim
#define K (TRI_L / 300.0f)          // the layout below was tuned at 300 px
#define DEEP_S (90.0f / TRI_L)      // the die deep in the liquid is 90 px across
#define TRI_H (TRI_L * 0.8660254f)
#define TRI_TOP (-2.0f * TRI_H / 3.0f)
#define TRI_BOT (TRI_H / 3.0f)
#define TEXT_CY (TRI_H * 0.1f)
#define MARGIN 30
#define TB_W ((int)TRI_L + 2 * MARGIN)
#define TB_H ((int)TRI_H + 2 * MARGIN)
#define PIVOT_X (TB_W / 2)
#define PIVOT_Y (MARGIN + (int)(2.0f * TRI_H / 3.0f))
#define N_SPECKS 34
#define N_BLOBS 3
#define BLOB 200                    // blob glow size
#define GLOW 200                    // listening glow size (scaled with the voice)
#define N_RINGS 3
#define N_BUBBLES 24
#define HL_W 280                    // glass highlight
#define HL_H 200
#define HL_X ((int)(R * 0.72f) - HL_W / 2)
#define HL_Y ((int)(R * 0.5f) - HL_H / 2)
#define GLOW_LUT 2500               // glow alpha indexed by d^2/4 for a radius of 100 px
#define STRIP_PX (W * 24)           // pixels per DMA buffer
#define MAX_RECTS 384
#define FRAME_MS 16
#define BRIGHTNESS 80
#define UP_MS 8000
#define IDLE_DIM_MS 120000

extern const uint8_t font_ttf_start[] asm("_binary_BebasNeue_Regular_ttf_start");
extern const uint8_t font_ttf_end[] asm("_binary_BebasNeue_Regular_ttf_end");

typedef enum { TRI_HIDDEN, TRI_DEEP, TRI_RISE, TRI_UP, TRI_SINK } tri_mode_t;
typedef enum { M_IDLE, M_LISTENING, M_THINKING, M_REVEAL } mode_t_;

static float frand(float a, float b) { return a + (b - a) * (esp_random() / 4294967295.0f); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float ease_out(float t) { return 1 - powf(1 - t, 3); }
static float ease_in(float t) { return t * t * t; }
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// --- rectangles (inclusive; empty when x1 > x2) --------------------------------------------------
typedef struct { int16_t x1, y1, x2, y2; } rect_t;
static const rect_t NONE = {1, 1, 0, 0};
static bool empty(rect_t r) { return r.x1 > r.x2 || r.y1 > r.y2; }
static int area(rect_t r) { return empty(r) ? 0 : (r.x2 - r.x1 + 1) * (r.y2 - r.y1 + 1); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static rect_t rect_wh(int x, int y, int w, int h) { return (rect_t){x, y, x + w - 1, y + h - 1}; }
static rect_t rect_f(float x1, float y1, float x2, float y2)
{
    return (rect_t){(int16_t)floorf(x1), (int16_t)floorf(y1), (int16_t)ceilf(x2), (int16_t)ceilf(y2)};
}
static rect_t isect(rect_t a, rect_t b)
{
    return (rect_t){imax(a.x1, b.x1), imax(a.y1, b.y1), imin(a.x2, b.x2), imin(a.y2, b.y2)};
}
static rect_t unite(rect_t a, rect_t b)
{
    if (empty(a)) return b;
    if (empty(b)) return a;
    return (rect_t){imin(a.x1, b.x1), imin(a.y1, b.y1), imax(a.x2, b.x2), imax(a.y2, b.y2)};
}

// --- pixels: native RGB565, swapped to the panel's byte order just before sending -----------------
static uint16_t rgb565(int r, int g, int b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

static inline uint32_t spread(uint16_t c) { return (c | ((uint32_t)c << 16)) & 0x07E0F81F; }
static inline uint16_t pack(uint32_t s) { return (uint16_t)(s | (s >> 16)); }

// Normal blend, alpha 0..32.
static inline uint16_t mix565(uint16_t d, uint16_t s, uint32_t a5)
{
    uint32_t dd = spread(d);
    return pack(((((spread(s) - dd) * a5) >> 5) + dd) & 0x07E0F81F);
}

// Additive blend of a precomputed (spread) colour, saturating each channel.
static inline uint16_t add565(uint16_t d, uint32_t l)
{
    uint32_t s = spread(d) + l, o = s & 0x08010020;
    if (o) {
        if (o & 0x20) s |= 0x1F;
        if (o & 0x10000) s |= 0xF800;
        if (o & 0x08000000) s |= 0x07E00000;
    }
    return pack(s & 0x07E0F81F);
}

// lut[a] = colour * a/255 * opa/255, spread: additive glows keep 8-bit alpha precision this way.
static void make_lut(uint32_t *lut, int r, int g, int b, int opa)
{
    for (int a = 0; a < 256; a++) {
        float k = a * opa / (255.0f * 255.0f);
        uint32_t r5 = (uint32_t)(r * k * 31 / 255 + 0.5f), g6 = (uint32_t)(g * k * 63 / 255 + 0.5f),
                 b5 = (uint32_t)(b * k * 31 / 255 + 0.5f);
        lut[a] = (g6 << 21) | (r5 << 11) | b5;
    }
}

// --- state ---------------------------------------------------------------------------------------
// The triangle as drawn: body and blur with straight alpha, plus the text's coverage (one colour).
typedef struct { uint16_t blur, body; uint8_t blur_a, body_a, text_a, pad; } tri_px_t;

typedef struct { float x, y, z, r; float dx, dy; uint16_t col; uint8_t opa; rect_t box; } speck_t;
typedef struct { float a, r, s; int bx, by; int64_t t; rect_t box; } blob_t;
typedef struct { bool on; float r, w; int opa; } ring_t;
typedef struct { float x, y, r, vy, ph, life, age; bool on; bool drawn; float dx, dy; int opa; rect_t box; } bubble_t;
typedef struct {                      // the triangle as last drawn
    bool on, fast, flat, smooth;          // smooth: settled, so worth sampling bilinear
    float cx, cy, s, rot, ic, is;
    int32_t du, dv;
    int ix, iy;
    uint32_t ob, ot, ox;              // opacities 0..256: blur, body, text
    const tri_px_t *src;
    const uint16_t *fc;
    const uint8_t *fa;
    rect_t box;
} tri_draw_t;

static struct {
    // art
    uint16_t *bg;                                       // liquid, W x W
    uint8_t *hl_map;                                    // highlight alpha
    uint8_t blob_alut[GLOW_LUT], glow_alut[GLOW_LUT];   // glow alpha by squared distance
    uint32_t blob_lut[256], glow_lut[256], hl_lut[256];
    tri_px_t *tri_buf[2];                               // the triangle's layers, for rising and sinking
    uint16_t *flat_c[2];                                // and flattened as it rests: colour + alpha
    uint8_t *flat_a[2];
    int front, want_front;
    lv_draw_buf_t *buf_t, *buf_x;                       // LVGL renders the triangle into these
    uint8_t *half[2];                                   // its blur, at half resolution
    lv_obj_t *canvas;
    lv_font_t *fonts[96];
    // panel
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;
    uint16_t *dma[2];
    int cur;
    rect_t dirty[MAX_RECTS];
    int n_dirty;
    bool dirty_all, touch_down;
    int bright, want_bright;
    SemaphoreHandle_t mu;                               // guards the animation state below
    // animation
    speck_t specks[N_SPECKS];
    blob_t blobs[N_BLOBS];
    int blob_opa;
    ring_t rings[N_RINGS];
    int glow_size, glow_opa;
    int64_t glow_t;
    rect_t glow_box;
    bubble_t bubbles[N_BUBBLES];
    tri_draw_t T;
    mode_t_ mode;
    tri_mode_t tri, tri_next;
    int64_t tri_t, last, last_activity;
    float rot, rot0, offx0, offy0, churn, level, tiltx, tilty, tx, ty, agitation, upx, upy;
    float up_target, up;                                // which way is up on the screen (0 = top), radians
    bool burst, dimmed;
} S;

// Frame monitor (console: fps on|off).
static struct { bool on; uint32_t frames, rects; uint64_t px; int64_t draw_us, draw_max, upd_us, comp_us, win; uint64_t cyc[9]; } F;
#define LAYER(i) do { uint32_t c_ = esp_cpu_get_cycle_count(); if (xPortGetCoreID()) F.cyc[i] += c_ - c0; c0 = c_; } while (0)  // core 1's share

// --- damage ----------------------------------------------------------------------------------------
static void damage(rect_t r)
{
    r = isect(r, (rect_t){0, 0, W - 1, W - 1});
    if (empty(r)) return;
    r.x1 &= ~1; r.y1 &= ~1; r.x2 |= 1; r.y2 |= 1;       // CO5300 windows start even and end odd
    if (S.n_dirty == MAX_RECTS) { S.dirty_all = true; return; }
    S.dirty[S.n_dirty++] = r;
}

// The pixels of a ring between radii rin and rout, as the boxes of short arcs (not its bounding
// box). A multiple of 4 arcs puts the axes on arc ends, so each arc's box is that of its corners.
static void damage_annulus(float rin, float rout)
{
    rin = fmaxf(rin - 1, 0); rout += 1;
    int n = 4 * imax(1, imin(12, (int)(rout / 24)));
    for (int k = 0; k < n; k++) {
        float c0 = cosf(6.2831853f * k / n), s0 = sinf(6.2831853f * k / n);
        float c1 = cosf(6.2831853f * (k + 1) / n), s1 = sinf(6.2831853f * (k + 1) / n);
        float xs[4] = {c0 * rin, c0 * rout, c1 * rin, c1 * rout}, ys[4] = {s0 * rin, s0 * rout, s1 * rin, s1 * rout};
        float x1 = xs[0], x2 = xs[0], y1 = ys[0], y2 = ys[0];
        for (int i = 1; i < 4; i++) { x1 = fminf(x1, xs[i]); x2 = fmaxf(x2, xs[i]); y1 = fminf(y1, ys[i]); y2 = fmaxf(y2, ys[i]); }
        damage(rect_f(R + x1, R + y1, R + x2, R + y2));
    }
}

// Joins rectangles whose union wastes little: at most the smaller one's area, capped at about
// what a separate transfer costs. (A fixed allowance let long arcs grow back into big boxes.)
static void merge_dirty(void)
{
    if (S.dirty_all) { S.dirty[0] = (rect_t){0, 0, W - 1, W - 1}; S.n_dirty = 1; S.dirty_all = false; return; }
    for (bool again = true; again;) {
        again = false;
        for (int i = 0; i < S.n_dirty; i++)
            for (int j = i + 1; j < S.n_dirty; j++) {
                rect_t u = unite(S.dirty[i], S.dirty[j]);
                int ai = area(S.dirty[i]), aj = area(S.dirty[j]);
                if (area(u) <= ai + aj + imin(512, imin(ai, aj))) {
                    S.dirty[i] = u;
                    S.dirty[j--] = S.dirty[--S.n_dirty];
                    again = true;
                }
            }
    }
}

// --- static art ----------------------------------------------------------------------------------
static void *psram(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    assert(p);
    return p;
}

static void make_liquid(void)
{
    // Deep indigo in the middle, near black at the rim, and a vignette like the glass edge.
    S.bg = psram(W * W * 2);
    for (int y = 0; y < W; y++) {
        if (y % 64 == 0) vTaskDelay(1);
        for (int x = 0; x < W; x++) {
            float d = sqrtf((x - R) * (x - R) + (y - R) * (y - R)) / (R * 1.05f);
            float r, g, b;
            if (d < 0.55f) { float t = d / 0.55f; r = lerpf(11, 5, t); g = lerpf(18, 8, t); b = lerpf(64, 36, t); }
            else { float t = clampf((d - 0.55f) / 0.45f, 0, 1); r = lerpf(5, 1, t); g = lerpf(8, 1, t); b = lerpf(36, 7, t); }
            float vig = d < 0.52f ? 1.0f : clampf(1 - (d - 0.52f) / 0.48f * 0.85f, 0, 1);
            S.bg[y * W + x] = ((uint16_t)(r * vig * 31 / 255) << 11) | ((uint16_t)(g * vig * 63 / 255) << 5) |
                              (uint16_t)(b * vig * 31 / 255);
        }
    }
}

// A round glow's alpha by squared distance (in internal RAM: no image to fetch from PSRAM).
static void make_glow(uint8_t *lut, float power)
{
    for (int i = 0; i < GLOW_LUT; i++) {
        float d = sqrtf(i * 4.0f) / 100;
        lut[i] = d >= 1 ? 0 : (uint8_t)(powf(1 - d, power) * 255);
    }
}

static void make_highlight(void)
{
    // The reflection on the glass: a soft ellipse, tilted, top left.
    S.hl_map = psram(HL_W * HL_H);
    float ca = cosf(-0.6f), sa = sinf(-0.6f);
    for (int y = 0; y < HL_H; y++)
        for (int x = 0; x < HL_W; x++) {
            float dx = x - HL_W / 2.0f, dy = y - HL_H / 2.0f;
            float u = (dx * ca + dy * sa) / 130.0f, v = (-dx * sa + dy * ca) / 46.0f;
            float d = sqrtf(u * u + v * v);
            S.hl_map[y * HL_W + x] = d >= 1 ? 0 : (uint8_t)((1 - d) * (1 - d) * 0.10f * 255);
        }
    make_lut(S.hl_lut, 255, 255, 255, 255);
}

// --- compositing ---------------------------------------------------------------------------------
// Every draw_* composes one item into dst, which holds the rectangle c (w pixels per row).

static void draw_a8(rect_t c, uint16_t *dst, int w, const uint8_t *map, int mw, int mh, int ox, int oy, const uint32_t *lut)
{
    rect_t b = isect(c, rect_wh(ox, oy, mw, mh));
    for (int y = b.y1; y <= b.y2; y++) {
        const uint8_t *m = map + (y - oy) * mw + (b.x1 - ox);
        uint16_t *d = dst + (y - c.y1) * w + (b.x1 - c.x1);
        for (int i = 0, n = b.x2 - b.x1 + 1; i < n; i++)
            if (m[i]) d[i] = add565(d[i], lut[m[i]]);
    }
}

// An additive round glow of radius r centred on (cx, cy).
static void draw_glow(rect_t c, uint16_t *dst, int w, int cx, int cy, int r, const uint8_t *alut, const uint32_t *lut)
{
    rect_t b = isect(c, (rect_t){cx - r, cy - r, cx + r - 1, cy + r - 1});
    if (empty(b)) return;
    uint32_t k = (10000u << 10) / (uint32_t)(r * r);   // d^2 scaled to the 100 px table: (d^2 * k) >> 12
    for (int y = b.y1; y <= b.y2; y++) {
        uint32_t dy2 = (y - cy) * (y - cy);
        uint16_t *d = dst + (y - c.y1) * w - c.x1;
        for (int x = b.x1; x <= b.x2; x++) {
            uint32_t idx = (((x - cx) * (x - cx) + dy2) * k) >> 12;
            if (idx < GLOW_LUT && alut[idx]) d[x] = add565(d[x], lut[alut[idx]]);
        }
    }
}

static rect_t dot_box(float cx, float cy, float r) { return rect_f(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1); }

static void draw_dot(rect_t c, uint16_t *dst, int w, float cx, float cy, float r, uint16_t col, int opa)
{
    rect_t b = isect(c, dot_box(cx, cy, r));
    for (int y = b.y1; y <= b.y2; y++) {
        float dy = y + 0.5f - cy;
        uint16_t *d = dst + (y - c.y1) * w - c.x1;
        for (int x = b.x1; x <= b.x2; x++) {
            float dx = x + 0.5f - cx, cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0) continue;
            d[x] = mix565(d[x], col, (uint32_t)(fminf(cov, 1) * opa * (32.0f / 255) + 0.5f));
        }
    }
}

// An anti-aliased ring of radius r and width wd centred on (cx, cy); only the spans it covers.
// Near the ring the distance from its centre line is |d^2 - r^2| / 2r closely enough: no root per pixel.
static void draw_ring(rect_t c, uint16_t *dst, int w, float cx, float cy, float r, float wd, uint16_t col, int opa)
{
    float rin = r - wd / 2, rout = r + wd / 2;
    rect_t b = isect(c, rect_f(cx - rout - 1, cy - rout - 1, cx + rout + 1, cy + rout + 1));
    float o2 = (rout + 1) * (rout + 1), i2 = rin > 1 ? (rin - 1) * (rin - 1) : 0;
    float r2 = r * r, inv2r = 0.5f / r, half = wd / 2 + 0.5f;
    for (int y = b.y1; y <= b.y2; y++) {
        float dy = y + 0.5f - cy, dy2 = dy * dy;
        if (dy2 >= o2) continue;
        float ox = sqrtf(o2 - dy2), ix = i2 > dy2 ? sqrtf(i2 - dy2) : 0;
        int spans[2][2] = {{(int)floorf(cx - ox), (int)ceilf(cx - ix)}, {(int)floorf(cx + ix), (int)ceilf(cx + ox)}};
        int ns = 2;
        if (ix == 0) { spans[0][1] = spans[1][1]; ns = 1; }
        uint16_t *d = dst + (y - c.y1) * w - c.x1;
        for (int k = 0; k < ns; k++)
            for (int x = imax(spans[k][0], b.x1), xe = imin(spans[k][1], b.x2); x <= xe; x++) {
                float dx = x + 0.5f - cx, cov = half - fabsf(dx * dx + dy2 - r2) * inv2r;
                if (cov <= 0) continue;
                d[x] = mix565(d[x], col, (uint32_t)(fminf(cov, 1) * opa * (32.0f / 255) + 0.5f));
            }
    }
}

static const uint16_t TEXT565 = (238 >> 3) << 11 | (244 >> 2) << 5 | (255 >> 3);

static inline void blend_tri(uint16_t *d, const tri_px_t *p, uint32_t ob, uint32_t ot, uint32_t ox)
{
    if (!(p->blur_a | p->body_a | p->text_a)) return;
    uint16_t px = *d;
    if (p->blur_a) px = mix565(px, p->blur, (p->blur_a * ob + 1024) >> 11);
    if (p->body_a) px = mix565(px, p->body, (p->body_a * ot + 1024) >> 11);
    if (p->text_a) px = mix565(px, TEXT565, (p->text_a * ox + 1024) >> 11);
    *d = px;
}

// The flattened layer at (u, v) in 16.16 source pixels, bilinear: the resting triangle is turned to
// stay level, and its text would look jagged sampled at single pixels. Colours are weighted by alpha.
static inline void sample_flat(const tri_draw_t *T, int32_t u, int32_t v, uint16_t *d)
{
    int ui = u >> 16, vi = v >> 16, fu = (u >> 12) & 15, fv = (v >> 12) & 15;
    if (ui < -1 || vi < -1 || ui >= TB_W || vi >= TB_H) return;
    int wts[4] = {(16 - fu) * (16 - fv), fu * (16 - fv), (16 - fu) * fv, fu * fv};
    int r = 0, g = 0, b = 0, A = 0;
    for (int k = 0; k < 4; k++) {
        int x = ui + (k & 1), y = vi + (k >> 1);
        if (!wts[k] || (unsigned)x >= TB_W || (unsigned)y >= TB_H) continue;
        int j = y * TB_W + x, wa = wts[k] * T->fa[j];
        if (!wa) continue;
        uint16_t c = T->fc[j];
        r += (c >> 11) * wa; g += ((c >> 5) & 63) * wa; b += (c & 31) * wa; A += wa;
    }
    if (A < 256) return;
    // r, g, b are premultiplied with A at 256 * 255 for opaque: out = colour + background * (1 - alpha).
    uint16_t px = *d;
    int ia = 65536 - A;
    int R5 = (r + (px >> 11) * ia) >> 16, G6 = (g + ((px >> 5) & 63) * ia) >> 16, B5 = (b + (px & 31) * ia) >> 16;
    *d = (uint16_t)((imin(R5, 31) << 11) | (imin(G6, 63) << 5) | imin(B5, 31));
}

static void draw_tri(rect_t c, uint16_t *dst, int w)
{
    const tri_draw_t *T = &S.T;
    rect_t b = isect(c, T->box);
    if (!T->on || empty(b)) return;
    for (int y = b.y1; y <= b.y2; y++) {
        uint16_t *d = dst + (y - c.y1) * w + (b.x1 - c.x1);
        int n = b.x2 - b.x1 + 1;
        if (T->flat && T->fast) {                        // resting upright: flattened layer, offset copy
            int vi = y - T->iy;
            if ((unsigned)vi >= TB_H) continue;
            const uint16_t *cr = T->fc + vi * TB_W;
            const uint8_t *ar = T->fa + vi * TB_W;
            for (int i = 0; i < n; i++) {
                int ui = b.x1 + i - T->ix;
                if ((unsigned)ui < TB_W && ar[ui]) d[i] = mix565(d[i], cr[ui], (ar[ui] + 4) >> 3);
            }
        } else if (T->fast) {                            // upright, full size: offset copy of the layers
            int vi = y - T->iy;
            if ((unsigned)vi >= TB_H) continue;
            const tri_px_t *row = T->src + vi * TB_W;
            for (int i = 0; i < n; i++) {
                int ui = b.x1 + i - T->ix;
                if ((unsigned)ui < TB_W) blend_tri(&d[i], &row[ui], T->ob, T->ot, T->ox);
            }
        } else if (T->flat) {                            // resting but turned: flattened layer
            float ry = y + 0.5f - T->cy, rx = b.x1 + 0.5f - T->cx;
            int32_t u = (int32_t)((PIVOT_X + T->ic * rx + T->is * ry) * 65536.0f);
            int32_t v = (int32_t)((PIVOT_Y - T->is * rx + T->ic * ry) * 65536.0f);
            if (T->smooth) {                             // bilinear once it stops turning
                u -= 32768; v -= 32768;
                for (int i = 0; i < n; i++, u += T->du, v += T->dv) sample_flat(T, u, v, &d[i]);
            } else {                                     // one read per pixel while it turns
                for (int i = 0; i < n; i++, u += T->du, v += T->dv) {
                    int ui = u >> 16, vi = v >> 16;
                    if ((unsigned)ui >= TB_W || (unsigned)vi >= TB_H) continue;
                    int j = vi * TB_W + ui;
                    if (T->fa[j]) d[i] = mix565(d[i], T->fc[j], (T->fa[j] + 4) >> 3);
                }
            }
        } else {                                         // scaled and rotated: step through the source
            float ry = y + 0.5f - T->cy, rx = b.x1 + 0.5f - T->cx;
            int32_t u = (int32_t)((PIVOT_X + T->ic * rx + T->is * ry) * 65536.0f);
            int32_t v = (int32_t)((PIVOT_Y - T->is * rx + T->ic * ry) * 65536.0f);
            for (int i = 0; i < n; i++, u += T->du, v += T->dv) {
                int ui = u >> 16, vi = v >> 16;
                if ((unsigned)ui < TB_W && (unsigned)vi < TB_H)
                    blend_tri(&d[i], &T->src[vi * TB_W + ui], T->ob, T->ot, T->ox);
            }
        }
    }
}

static void compose(rect_t c, uint16_t *dst)
{
    uint32_t c0 = esp_cpu_get_cycle_count();
    int w = c.x2 - c.x1 + 1;
    for (int y = c.y1; y <= c.y2; y++) memcpy(dst + (y - c.y1) * w, S.bg + y * W + c.x1, w * 2);
    LAYER(0);
    for (int i = 0; i < N_BLOBS; i++)
        if (!empty(isect(c, S.blobs[i].box)))
            draw_glow(c, dst, w, S.blobs[i].bx + BLOB / 2, S.blobs[i].by + BLOB / 2, BLOB / 2, S.blob_alut, S.blob_lut);
    LAYER(1);
    for (int i = 0; i < N_SPECKS; i++) {
        speck_t *p = &S.specks[i];
        if (!empty(isect(c, p->box))) draw_dot(c, dst, w, p->dx, p->dy, p->r, p->col, p->opa);
    }
    LAYER(2);
    if (S.glow_size) draw_glow(c, dst, w, R, R, S.glow_size / 2, S.glow_alut, S.glow_lut);
    LAYER(3);
    for (int i = 0; i < N_RINGS; i++)
        if (S.rings[i].on) draw_ring(c, dst, w, R, R, S.rings[i].r, S.rings[i].w, rgb565(120, 190, 255), S.rings[i].opa);
    LAYER(4);
    draw_tri(c, dst, w);
    LAYER(5);
    for (int i = 0; i < N_BUBBLES; i++) {
        bubble_t *b = &S.bubbles[i];
        if (b->drawn && !empty(isect(c, b->box)))
            draw_ring(c, dst, w, b->dx, b->dy, (6 + i % 7) / 2.0f, 1, rgb565(170, 205, 255), b->opa);
    }
    LAYER(6);
    draw_a8(c, dst, w, S.hl_map, HL_W, HL_H, HL_X, HL_Y, S.hl_lut);
    LAYER(7);
}

static void swap_bytes(uint16_t *p, int n)
{
    uint32_t *q = (uint32_t *)p;
    for (int i = 0; i < n / 2; i++) q[i] = ((q[i] & 0x00FF00FF) << 8) | ((q[i] >> 8) & 0x00FF00FF);
}

static void compose_swap(rect_t s, uint16_t *buf)
{
    compose(s, buf);
    uint32_t c0 = esp_cpu_get_cycle_count();
    swap_bytes(buf, (s.x2 - s.x1 + 1) * (s.y2 - s.y1 + 1));
    LAYER(8);
}

// Core 0 composes the bottom half of each big strip while the render task (core 1) does the top.
// The halves are separate rows of the buffer, so they need no locking, only the two notifications.
static TaskHandle_t helper_h, render_h;
static rect_t job_rect;
static uint16_t *job_buf;

static void helper_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        compose_swap(job_rect, job_buf);
        xTaskNotifyGive(render_h);
    }
}

static void compose_strip(rect_t s, uint16_t *buf)
{
    int rows = s.y2 - s.y1 + 1, w = s.x2 - s.x1 + 1;
    if (!helper_h || rows < 4 || rows * w < 512) { compose_swap(s, buf); return; }
    int top = (rows / 2) & ~1;
    job_rect = (rect_t){s.x1, s.y1 + top, s.x2, s.y2};
    job_buf = buf + top * w;
    xTaskNotifyGive(helper_h);
    compose_swap((rect_t){s.x1, s.y1, s.x2, s.y1 + top - 1}, buf);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// Composes and sends every dirty rectangle in strips, alternating the two DMA buffers: drawing a
// strip waits for the previous transfer, so the buffer composed next is always free.
static uint32_t flush_dirty(void)
{
    merge_dirty();
    uint32_t px = 0;
    for (int k = 0; k < S.n_dirty; k++) {
        rect_t r = S.dirty[k];
        int w = r.x2 - r.x1 + 1, rows = imax(2, (STRIP_PX / w) & ~1);
        for (int y = r.y1; y <= r.y2; y += rows) {
            rect_t s = {r.x1, y, r.x2, imin(y + rows - 1, r.y2)};
            uint16_t *buf = S.dma[S.cur];
            S.cur ^= 1;
            int64_t tc = esp_timer_get_time();
            compose_strip(s, buf);
            int n = w * (s.y2 - s.y1 + 1);
            F.comp_us += esp_timer_get_time() - tc;
            esp_lcd_panel_draw_bitmap(S.panel, s.x1, s.y1, s.x2 + 1, s.y2 + 1, buf);
            px += n;
        }
    }
    F.rects += S.n_dirty;
    S.n_dirty = 0;
    return px;
}

// --- the triangle (rendered by LVGL off screen) -----------------------------------------------------
static lv_draw_buf_t *new_buf(int w, int h)
{
    lv_draw_buf_t *b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    assert(b);
    memset(b->data, 0, b->data_size);
    return b;
}

static lv_font_t *font_px(int px)
{
    if (px < 8 || px >= 96) return NULL;
    if (!S.fonts[px]) S.fonts[px] = lv_tiny_ttf_create_data(font_ttf_start, font_ttf_end - font_ttf_start, px);
    return S.fonts[px];
}

static void upper_utf8(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < n; i++) {
        unsigned char c = in[i];
        if (c >= 'a' && c <= 'z') out[j++] = c - 32;
        else if (c == 0xC3 && in[i + 1]) {       // Latin-1 lower case: U+00E0..U+00FE except U+00F7
            unsigned char d = in[++i];
            out[j++] = c;
            out[j++] = (d >= 0xA0 && d <= 0xBE && d != 0xB7) ? d - 0x20 : d;
        } else out[j++] = c;
    }
    out[j] = 0;
}

static int32_t text_width(const char *s, const lv_font_t *f)
{
    lv_point_t p;
    lv_text_get_size(&p, s, f, 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

static float width_at(float y) { return TRI_L * (y - TRI_TOP) / TRI_H; }

typedef struct { int px, n; char lines[5][96]; float y[5]; } layout_t;

// Fits the words into the triangle: the biggest size whose lines each fit the triangle's width at
// their height (same rules as reveal.html).
static bool layout_text(const char *text, layout_t *L)
{
    char words[24][40]; int nw = 0;
    const char *p = text;
    while (*p && nw < 24) {
        while (*p == ' ') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != ' ' && k < 39) words[nw][k++] = *p++;
        while (*p && *p != ' ') p++;
        words[nw++][k] = 0;
    }
    if (!nw) return false;
    for (int px = (int)(56 * K) & ~1; px >= 16; px -= 2) {
        lv_font_t *f = font_px(px);
        if (!f) continue;
        float lh = px * 0.98f;
        for (int n = 1; n <= 5; n++) {
            float top = TEXT_CY - lh * n / 2;
            if (top < TRI_TOP + 70 * K || top + lh * n > TRI_BOT - 12 * K) continue;
            int i = 0, k;
            for (k = 0; k < n && i < nw; k++) {
                float maxw = width_at(top + lh * k + px * 0.12f) * 0.8f;
                char line[96] = "";
                while (i < nw) {
                    char next[96];
                    snprintf(next, sizeof next, "%s%s%s", line, line[0] ? " " : "", words[i]);
                    if (text_width(next, f) <= maxw) { strcpy(line, next); i++; }
                    else break;
                }
                if (!line[0]) break;
                strcpy(L->lines[k], line);
                L->y[k] = top + lh * k + lh / 2;
            }
            if (i == nw && k == n) { L->px = px; L->n = n; return true; }
        }
    }
    return false;
}

// The blur (the die out of focus, and its glow) is done at half resolution: it has no detail to lose.
#define HW ((TB_W + 1) / 2)
#define HH ((TB_H + 1) / 2)

static int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

// One box-blur pass (radius 4) over premultiplied RGBA, src to dst, along x or y.
static void blur_pass(const uint8_t *src, uint8_t *dst, bool vertical)
{
    static int sum[HW * 4];
    const int r = 4, recip = 65536 / (2 * r + 1) + 1;
    if (vertical) {                                     // row by row, a running sum per column
        memset(sum, 0, sizeof sum);
        for (int y = -r; y <= r; y++) {
            const uint8_t *row = src + clampi(y, 0, HH - 1) * HW * 4;
            for (int i = 0; i < HW * 4; i++) sum[i] += row[i];
        }
        for (int y = 0; y < HH; y++) {
            uint8_t *o = dst + y * HW * 4;
            const uint8_t *add = src + clampi(y + r + 1, 0, HH - 1) * HW * 4, *sub = src + clampi(y - r, 0, HH - 1) * HW * 4;
            for (int i = 0; i < HW * 4; i++) { o[i] = (sum[i] * recip) >> 16; sum[i] += add[i] - sub[i]; }
        }
    } else {
        for (int y = 0; y < HH; y++) {
            const uint8_t *row = src + y * HW * 4;
            uint8_t *o = dst + y * HW * 4;
            int s4[4] = {0};
            for (int x = -r; x <= r; x++) for (int c = 0; c < 4; c++) s4[c] += row[clampi(x, 0, HW - 1) * 4 + c];
            for (int x = 0; x < HW; x++) {
                int xa = clampi(x + r + 1, 0, HW - 1) * 4, xs = clampi(x - r, 0, HW - 1) * 4;
                for (int c = 0; c < 4; c++) { o[x * 4 + c] = (s4[c] * recip) >> 16; s4[c] += row[xa + c] - row[xs + c]; }
            }
        }
    }
}

static void tri_point(float scale, int i, lv_point_precise_t *p)
{
    float cx = PIVOT_X, cy = PIVOT_Y;
    float xs[3] = {0, TRI_L / 2, -TRI_L / 2}, ys[3] = {TRI_TOP, TRI_BOT, TRI_BOT};
    p->x = (lv_value_precise_t)(cx + xs[i] * scale);
    p->y = (lv_value_precise_t)(cy + ys[i] * scale);
}

static void draw_line(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_color_t c, lv_opa_t opa, int w)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1 = a; d.p2 = b; d.color = c; d.opa = opa; d.width = w; d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

// Renders the body, the text and the blurred whole for this answer, and packs them into dst.
static void render_triangle(const char *answer, int k)
{
    int64_t t_start = esp_timer_get_time();
    char up[192];
    upper_utf8(answer, up, sizeof up);
    layout_t L = {0};
    bool fits = up[0] && layout_text(up, &L);

    lv_layer_t layer;
    lv_canvas_set_draw_buf(S.canvas, S.buf_t);
    lv_canvas_fill_bg(S.canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_canvas_init_layer(S.canvas, &layer);
    lv_draw_triangle_dsc_t td; lv_draw_triangle_dsc_init(&td);
    for (int i = 0; i < 3; i++) tri_point(1, i, &td.p[i]);
    td.opa = LV_OPA_COVER;
    td.grad.dir = LV_GRAD_DIR_VER;
    td.grad.stops_count = 2;
    td.grad.stops[0] = (lv_grad_stop_t){.color = lv_color_make(0x36, 0x53, 0xff), .opa = LV_OPA_COVER, .frac = 0};
    td.grad.stops[1] = (lv_grad_stop_t){.color = lv_color_make(0x14, 0x24, 0x9c), .opa = LV_OPA_COVER, .frac = 255};
    lv_draw_triangle(&layer, &td);
    lv_point_precise_t o[3], in[3];
    for (int i = 0; i < 3; i++) { tri_point(1, i, &o[i]); tri_point(0.9f, i, &in[i]); }
    for (int i = 0; i < 3; i++) {
        draw_line(&layer, o[i], o[(i + 1) % 3], lv_color_make(150, 180, 255), 150, 3);
        draw_line(&layer, in[i], in[(i + 1) % 3], lv_color_make(10, 20, 90), 150, 2);
    }
    lv_canvas_finish_layer(S.canvas, &layer);

    lv_canvas_set_draw_buf(S.canvas, S.buf_x);
    lv_canvas_fill_bg(S.canvas, lv_color_black(), LV_OPA_TRANSP);
    if (fits) {
        lv_canvas_init_layer(S.canvas, &layer);
        lv_font_t *f = font_px(L.px);
        for (int k = 0; k < L.n; k++) {
            lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
            ld.font = f; ld.color = lv_color_make(238, 244, 255); ld.opa = LV_OPA_COVER;
            ld.align = LV_TEXT_ALIGN_CENTER; ld.letter_space = 1; ld.text = L.lines[k];
            int yc = PIVOT_Y + (int)L.y[k];
            int lh = lv_font_get_line_height(f);
            lv_area_t a = {.x1 = 0, .x2 = TB_W - 1, .y1 = yc - lh / 2 + L.px / 14, .y2 = yc + lh / 2 + L.px / 14};
            lv_draw_label(&layer, &ld, &a);
        }
        lv_canvas_finish_layer(S.canvas, &layer);
    } else if (up[0]) {
        ESP_LOGW(TAG, "answer does not fit: %s", up);
    }

    int64_t t_canvas = esp_timer_get_time();
    // Blur: body + text, premultiplied, averaged down 2x2, blurred three times each way.
    const uint8_t *t = S.buf_t->data, *x = S.buf_x->data;
    int stride = S.buf_t->header.stride;
    uint8_t *h0 = S.half[0], *h1 = S.half[1];
    for (int hy = 0; hy < HH; hy++)
        for (int hx = 0; hx < HW; hx++) {
            int acc[4] = {0};
            for (int k2 = 0; k2 < 4; k2++) {
                int o2 = imin(2 * hy + (k2 >> 1), TB_H - 1) * stride + imin(2 * hx + (k2 & 1), TB_W - 1) * 4;
                int ax = x[o2 + 3], at = t[o2 + 3] * (255 - ax) / 255;
                for (int c = 0; c < 3; c++) acc[c] += (x[o2 + c] * ax + t[o2 + c] * at) / 255;
                acc[3] += ax + at;
            }
            for (int c = 0; c < 4; c++) h0[(hy * HW + hx) * 4 + c] = acc[c] / 4;
        }
    for (int pass = 0; pass < 3; pass++) { blur_pass(h0, h1, false); blur_pass(h1, h0, true); }

    int64_t t_blur = esp_timer_get_time();
    // Packed layers for rising and sinking (blur scaled back up), and the three flattened as they
    // rest, the blur at 0.6. Straight alpha out, premultiplied in between.
    tri_px_t *dst = S.tri_buf[k];
    for (int y = 0; y < TB_H; y++) {
        int ya = clampi((y - 1) >> 1, 0, HH - 1), yb = clampi((y + 1) >> 1, 0, HH - 1), wy = y & 1 ? 3 : 1;
        for (int i = 0; i < TB_W; i++) {
            int xa = clampi((i - 1) >> 1, 0, HW - 1), xb = clampi((i + 1) >> 1, 0, HW - 1), wx = i & 1 ? 3 : 1;
            // 2x bilinear: odd pixels weigh the lower neighbour 3/4, even ones 1/4
            const uint8_t *p00 = h0 + (ya * HW + xa) * 4, *p01 = h0 + (ya * HW + xb) * 4;
            const uint8_t *p10 = h0 + (yb * HW + xa) * 4, *p11 = h0 + (yb * HW + xb) * 4;
            int bp[4];
            for (int c = 0; c < 4; c++)
                bp[c] = ((p00[c] * wx + p01[c] * (4 - wx)) * wy + (p10[c] * wx + p11[c] * (4 - wx)) * (4 - wy)) / 16;
            int o2 = y * stride + i * 4, j = y * TB_W + i;
            int ba = bp[3];
            uint16_t blur = ba ? rgb565(imin(255, bp[2] * 255 / ba), imin(255, bp[1] * 255 / ba), imin(255, bp[0] * 255 / ba)) : 0;
            dst[j] = (tri_px_t){.blur = blur, .blur_a = ba, .body = rgb565(t[o2 + 2], t[o2 + 1], t[o2]), .body_a = t[o2 + 3],
                                .text_a = x[o2 + 3]};
            int A = ba * 153 / 255, P[3];
            for (int c = 0; c < 3; c++) P[c] = bp[c] * 153 / 255;
            const uint8_t *layers[2] = {t + o2, x + o2};
            for (int l = 0; l < 2; l++) {
                int al = layers[l][3];
                if (!al) continue;
                for (int c = 0; c < 3; c++) P[c] = (layers[l][c] * al + P[c] * (255 - al)) / 255;
                A = al + A * (255 - al) / 255;
            }
            S.flat_c[k][j] = A ? rgb565(imin(255, P[2] * 255 / A), imin(255, P[1] * 255 / A), imin(255, P[0] * 255 / A)) : 0;
            S.flat_a[k][j] = A;
        }
    }
    ESP_LOGI(TAG, "triangle: canvas %d ms, blur %d ms, pack %d ms", (int)((t_canvas - t_start) / 1000),
             (int)((t_blur - t_canvas) / 1000), (int)((esp_timer_get_time() - t_blur) / 1000));
}

// --- animation -----------------------------------------------------------------------------------
static void spawn_speck(speck_t *p)
{
    float a = frand(0, 6.283f), d = sqrtf(frand(0, 1)) * (R - 8);
    p->x = R + cosf(a) * d; p->y = R + sinf(a) * d; p->z = frand(0, 1);
}

static void sink(int64_t t) { S.tri = TRI_SINK; S.tri_t = t; S.tri_next = TRI_HIDDEN; }

static void burst(void)
{
    for (int i = 0; i < N_BUBBLES; i++) {
        bubble_t *b = &S.bubbles[i];
        float ox = frand(-140, 140) * K, oy = frand(-20, 110) * K, c = cosf(S.up), sn = sinf(S.up);   // below the centre
        b->x = R + c * ox - sn * oy; b->y = R + sn * ox + c * oy; b->r = frand(2, 7);
        b->vy = frand(0.04f, 0.15f); b->ph = frand(0, 6.28f); b->life = frand(1200, 2600); b->age = frand(-300, 0); b->on = true;
    }
}

static rect_t tri_box(float cx, float cy, float s, float rot)
{
    float c = cosf(rot) * s, sn = sinf(rot) * s, x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float rx = (k & 1 ? TB_W : 0) - PIVOT_X, ry = (k & 2 ? TB_H : 0) - PIVOT_Y;
        float x = cx + c * rx - sn * ry, y = cy + sn * rx + c * ry;
        x1 = fminf(x1, x); x2 = fmaxf(x2, x); y1 = fminf(y1, y); y2 = fmaxf(y2, y);
    }
    return rect_f(x1 - 1, y1 - 1, x2 + 1, y2 + 1);
}

// What the triangle covers on screen, glow included, as bands of 16 rows: turned, its bounding box
// is most of the screen, the triangle itself about half of that. Each band is the triangle's extent
// within the band grown by D on every side (the glow reaches MARGIN source pixels past the edges).
static void damage_tri(const tri_draw_t *T)
{
    if (!T->on) return;
    float c = cosf(T->rot) * T->s, sn = sinf(T->rot) * T->s, D = MARGIN * T->s + 2;
    float rx[3] = {0, TRI_L / 2, -TRI_L / 2}, ry[3] = {TRI_TOP, TRI_BOT, TRI_BOT}, vx[3], vy[3];
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < 3; i++) {
        vx[i] = T->cx + c * rx[i] - sn * ry[i]; vy[i] = T->cy + sn * rx[i] + c * ry[i];
        ymin = fminf(ymin, vy[i]); ymax = fmaxf(ymax, vy[i]);
    }
    for (float y0 = floorf(ymin - D); y0 <= ymax + D; y0 += 16) {
        float ya = y0 - D, yb = y0 + 16 + D, x1 = 1e9f, x2 = -1e9f;
        for (int i = 0; i < 3; i++) {                   // vertices in the slab, and edges crossing its sides
            int j = (i + 1) % 3;
            if (vy[i] >= ya && vy[i] <= yb) { x1 = fminf(x1, vx[i]); x2 = fmaxf(x2, vx[i]); }
            float dy = vy[j] - vy[i];
            if (dy == 0) continue;
            for (int k = 0; k < 2; k++) {
                float t = ((k ? yb : ya) - vy[i]) / dy;
                if (t < 0 || t > 1) continue;
                float x = vx[i] + t * (vx[j] - vx[i]);
                x1 = fminf(x1, x); x2 = fmaxf(x2, x);
            }
        }
        if (x1 <= x2) damage(rect_f(x1 - D, y0, x2 + D, y0 + 15));
    }
}

// Where the triangle should be now; redrawn only when that differs from what is on screen.
static void place_tri(float x, float y, float s, float rot, float ob, float ot, float ox)
{
    tri_draw_t *T = &S.T, n = *T;
    n.src = S.tri_buf[S.front];
    n.fc = S.flat_c[S.front]; n.fa = S.flat_a[S.front];
    n.ob = (uint32_t)(clampf(ob, 0, 1) * 256); n.ot = (uint32_t)(clampf(ot, 0, 1) * 256); n.ox = (uint32_t)(clampf(ox, 0, 1) * 256);
    n.on = n.ob > 2 || n.ot > 2 || n.ox > 2;
    n.fast = fabsf(s - 1) < 0.004f && fabsf(rot) < 0.0035f;
    n.cx = R + x; n.cy = R + y; n.s = s; n.rot = rot;
    if (n.fast) { n.cx = roundf(n.cx); n.cy = roundf(n.cy); n.s = 1; n.rot = 0; }
    n.flat = S.tri == TRI_UP;
    n.smooth = S.up == S.up_target;
    bool changed = n.on != T->on || (n.on && (n.src != T->src || n.ob != T->ob || n.ot != T->ot || n.ox != T->ox ||
                   n.fast != T->fast || n.flat != T->flat || n.smooth != T->smooth || fabsf(n.cx - T->cx) >= 0.25f || fabsf(n.cy - T->cy) >= 0.25f ||
                   fabsf(n.s - T->s) >= 0.002f || fabsf(n.rot - T->rot) >= 0.002f));
    if (!changed) return;
    damage_tri(T);
    n.ic = cosf(n.rot) / n.s; n.is = sinf(n.rot) / n.s;
    n.du = (int32_t)(n.ic * 65536); n.dv = (int32_t)(-n.is * 65536);
    n.ix = (int)n.cx - PIVOT_X; n.iy = (int)n.cy - PIVOT_Y;
    n.box = n.on ? tri_box(n.cx, n.cy, n.s, n.rot) : NONE;
    *T = n;
    damage_tri(T);
}

static void update(int64_t now)
{
    float dt = clampf((float)(now - S.last), 0, 60);
    S.last = now;

    S.tiltx += (S.tx - S.tiltx) * 0.08f; S.tilty += (S.ty - S.tilty) * 0.08f;
    float du = remainderf(S.up_target - S.up, 6.2831853f);            // turn toward up, the short way
    S.up = fabsf(du) < 0.003f ? S.up_target : remainderf(S.up + du * (1 - powf(0.75f, dt / 16)), 6.2831853f);
    float sa = sinf(S.up), ca = cosf(S.up);                            // up is (sa, -ca) on screen
    float target = S.mode == M_LISTENING ? 1.6f : S.mode == M_THINKING ? 4.0f : S.mode == M_REVEAL ? 1.4f : 1.0f;
    target += S.agitation;
    S.agitation *= 0.94f;
    S.churn += (target - S.churn) * (S.churn > target ? 0.02f : 0.05f);
    S.level += ((S.mode == M_LISTENING ? voice_level : 0) - S.level) * 0.3f;

    // Blobs: big soft glows, so they move in small steps and at most 20 times a second each.
    int bopa = (int)clampf(70 + 22 * (S.churn - 1), 0, 200) & ~7;
    bool bopa_changed = bopa != S.blob_opa;
    if (bopa_changed) { S.blob_opa = bopa; make_lut(S.blob_lut, 28, 46, 190, bopa); }
    for (int i = 0; i < N_BLOBS; i++) {
        blob_t *b = &S.blobs[i];
        b->a += b->s * dt * fminf(S.churn, 2.5f);
        int nx = (int)(R + cosf(b->a) * b->r + S.tiltx * 30) - BLOB / 2;
        int ny = (int)(R + sinf(b->a * 1.3f) * b->r * 0.8f + S.tilty * 30) - BLOB / 2;
        if (bopa_changed || ((abs(nx - b->bx) >= 3 || abs(ny - b->by) >= 3) && now - b->t >= 60)) {
            damage(b->box);
            b->bx = nx; b->by = ny; b->t = now;
            b->box = rect_wh(nx, ny, BLOB, BLOB);
            damage(b->box);
        }
    }

    for (int i = 0; i < N_SPECKS; i++) {
        speck_t *p = &S.specks[i];
        float k = 0.00045f * S.churn * dt, dx = p->x - R, dy = p->y - R;
        p->x += -dy * k + S.tiltx * 0.02f * dt * (1 - p->z);
        p->x += sa * 0.004f * dt;                                         // drift up
        p->y += dx * k - ca * 0.004f * dt + S.tilty * 0.02f * dt * (1 - p->z);
        if ((p->x - R) * (p->x - R) + (p->y - R) * (p->y - R) > (R - 6) * (R - 6)) spawn_speck(p);
        if (fabsf(p->x - p->dx) >= 0.25f || fabsf(p->y - p->dy) >= 0.25f) {
            damage(p->box);
            p->dx = p->x; p->dy = p->y;
            p->box = dot_box(p->dx, p->dy, p->r);
            damage(p->box);
        }
    }

    // Listening: a glow that swells with the voice, and rings travelling out.
    bool listening = S.mode == M_LISTENING;
    // The glow is redrawn whole when it changes, so it changes in steps, at most 8 times a second.
    int gsz = listening ? (int)((0.6f + S.level * 0.6f) * GLOW) & ~15 : 0;
    int gopa = listening ? (int)((0.35f + S.level * 0.5f) * 255) & ~31 : 0;
    if ((gsz != S.glow_size || gopa != S.glow_opa) && (!listening || now - S.glow_t >= 120)) {
        damage(S.glow_box);
        S.glow_size = gsz; S.glow_opa = gopa; S.glow_t = now;
        if (gsz) make_lut(S.glow_lut, 90, 170, 255, gopa);
        S.glow_box = gsz ? rect_wh(R - gsz / 2, R - gsz / 2, gsz, gsz) : NONE;
        damage(S.glow_box);
    }
    for (int i = 0; i < N_RINGS; i++) {
        ring_t *g = &S.rings[i];
        if (!listening) {
            if (g->on) { damage_annulus(g->r - g->w / 2, g->r + g->w / 2); g->on = false; }
            continue;
        }
        float p = fmodf(now / 1200.0f + i / (float)N_RINGS, 1.0f), rad = 20 + p * (R - 20);
        float wd = 2 + S.level * 4 * (1 - p);
        int opa = (int)((1 - p) * (0.18f + S.level * 0.5f) * 255);
        if (g->on && fabsf(rad - g->r) < 0.5f && fabsf(wd - g->w) < 0.5f && abs(opa - g->opa) < 4) continue;
        if (g->on && rad > g->r && rad - g->r < 40) {
            damage_annulus(fminf(g->r - g->w / 2, rad - wd / 2), fmaxf(g->r + g->w / 2, rad + wd / 2));
        } else {
            if (g->on) damage_annulus(g->r - g->w / 2, g->r + g->w / 2);
            damage_annulus(rad - wd / 2, rad + wd / 2);
        }
        *g = (ring_t){.on = true, .r = rad, .w = wd, .opa = opa};
    }

    // The triangle.
    if (S.front != S.want_front) S.front = S.want_front;
    float t = (float)(now - S.tri_t);
    float sx = cosf(now / 600.0f) * 22, sy = sinf(now / 520.0f) * 16;
    float x = 0, y = 0, s = 1, rot = 0, ob = 0, ot = 0, ox = 0;
    switch (S.tri) {
    case TRI_HIDDEN: break;
    case TRI_DEEP:
        S.rot += dt * 0.0035f * S.churn;
        x = sx; y = sy; s = DEEP_S; rot = S.rot; ob = 0.55f * clampf(t / 600, 0, 1);
        break;
    case TRI_RISE: {
        float p = clampf(t / 1700, 0, 1), e = ease_out(p);
        if (p > 0.42f && !S.burst) { S.burst = true; burst(); }
        S.upx = roundf(S.tiltx * 10); S.upy = roundf(S.tilty * 10);   // ends where it will rest: no hop
        x = lerpf(S.offx0, S.upx, e); y = lerpf(S.offy0, S.upy, e);
        s = lerpf(DEEP_S, 1, e) + 0.05f * sinf(p * 3.14159f * 2.2f) * (1 - p);
        rot = S.up + S.rot0 * (1 - e) + 0.16f * sinf(p * 17) * (1 - p) * (1 - p);
        float a = lerpf(0.55f, 1, e);
        ob = a * lerpf(1, 0.6f, e); ot = a * e * e; ox = clampf((p - 0.55f) / 0.45f, 0, 1);
        if (p >= 1) { S.tri = TRI_UP; S.tri_t = now; }
        break;
    }
    case TRI_UP:
        if (t > UP_MS && S.mode != M_THINKING) { sink(now); S.mode = M_IDLE; }
        // Follows the tilt, but only past a pixel and a bit: each move redraws the whole triangle.
        if (fabsf(S.tiltx * 10 - S.upx) > 1.2f) S.upx = roundf(S.tiltx * 10);
        if (fabsf(S.tilty * 10 - S.upy) > 1.2f) S.upy = roundf(S.tilty * 10);
        x = S.upx; y = S.upy; rot = S.up;
        ob = 0.6f; ot = 1; ox = 1;
        break;
    case TRI_SINK: {
        float q = clampf(t / 1000, 0, 1), e = ease_in(q);
        x = S.upx - sa * 25 * e; y = S.upy + ca * 25 * e;   // sinks down from where it rested
        s = lerpf(1, DEEP_S, e); rot = S.up + 0.4f * e;
        ob = lerpf(0.6f, 0.4f, q) * (1 - q * 0.6f); ot = 1 - q; ox = clampf(1 - 2 * q, 0, 1);
        if (q >= 1) { S.tri = S.tri_next; S.tri_t = now; if (S.mode == M_REVEAL) S.mode = M_IDLE; }
        break;
    }
    }
    place_tri(x, y, s, rot, ob, ot, ox);

    for (int i = 0; i < N_BUBBLES; i++) {
        bubble_t *b = &S.bubbles[i];
        float br = (6 + i % 7) / 2.0f;
        bool show = false;
        if (b->on) {
            b->age += dt;
            if (b->age > b->life) b->on = false;
            else if (b->age >= 0) {
                float wob = sinf(b->age / 160 + b->ph) * 0.25f;          // rise up, wobble across
                b->x += sa * b->vy * dt + ca * wob + S.tiltx * 0.03f * dt;
                b->y += -ca * b->vy * dt + sa * wob;
                show = true;
            }
        }
        int opa = show ? (int)(sinf(3.14159f * b->age / b->life) * 150) : 0;
        if (show == b->drawn && (!show || (fabsf(b->x - b->dx) < 0.5f && fabsf(b->y - b->dy) < 0.5f && abs(opa - b->opa) < 8)))
            continue;
        if (b->drawn) damage(b->box);
        b->drawn = show; b->dx = b->x; b->dy = b->y; b->opa = opa;
        b->box = show ? dot_box(b->dx, b->dy, br + 1) : NONE;
        damage(b->box);
    }

    if (S.mode == M_IDLE && S.tri == TRI_HIDDEN && now - S.last_activity > IDLE_DIM_MS) {
        S.dimmed = true;                    // saves the panel (and later the battery)
        S.want_bright = 0;
    }
}

// --- frame loop ----------------------------------------------------------------------------------
static uint16_t *snap_img;
static volatile bool snap_req, snap_printing;
static SemaphoreHandle_t snap_done;

static void poll_touch(void)
{
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    if (esp_lcd_touch_read_data(S.touch) != ESP_OK) return;
    esp_lcd_touch_get_data(S.touch, pt, &n, 1);
    static uint16_t tx, ty;
    if (n) { tx = pt[0].x; ty = pt[0].y; }
    if (!n && S.touch_down) {                           // a tap counts when the finger lifts
        ESP_LOGI(TAG, "tap at %u,%u", tx, ty);
        app_post_kind(EV_TAP);
    }
    S.touch_down = n > 0;
}

static void stats(int64_t t, uint32_t px, int64_t draw_us, int64_t upd_us)
{
    if (px) { F.frames++; F.px += px; F.draw_us += draw_us; if (draw_us > F.draw_max) F.draw_max = draw_us; }
    F.upd_us += upd_us;
    if (t - F.win < 2000000) return;
    if (F.on && !snap_printing) {             // a log line inside the snapshot would corrupt it
        float secs = (t - F.win) / 1e6f;
        unsigned n = F.frames ? F.frames : 1;
        ESP_LOGI(TAG, "fps %.1f | draw %.1f ms (max %.1f) | %u px, %u rects per frame | compose %.1f ms | update %.2f ms",
                 F.frames / secs, F.draw_us / 1000.0f / n, F.draw_max / 1000.0f, (unsigned)(F.px / n),
                 F.rects / n, F.comp_us / 1000.0f / n, F.upd_us / 1000.0f / n);
        uint64_t tot = F.px ? F.px : 1;
        ESP_LOGI(TAG, "cycles/px: bg %.1f blobs %.1f specks %.1f glow %.1f rings %.1f tri %.1f bubbles %.1f hl %.1f swap %.1f",
                 F.cyc[0] / (float)tot, F.cyc[1] / (float)tot, F.cyc[2] / (float)tot, F.cyc[3] / (float)tot, F.cyc[4] / (float)tot,
                 F.cyc[5] / (float)tot, F.cyc[6] / (float)tot, F.cyc[7] / (float)tot, F.cyc[8] / (float)tot);
    }
    F.win = t; F.frames = F.rects = 0; F.px = 0; F.draw_us = F.draw_max = F.upd_us = F.comp_us = 0; memset(F.cyc, 0, sizeof F.cyc);
}

static void render_task(void *arg)
{
    for (uint32_t frame = 0;; frame++) {
        int64_t t0 = esp_timer_get_time();
        xSemaphoreTake(S.mu, portMAX_DELAY);
        if (!S.dimmed) update(t0 / 1000);
        else S.last = t0 / 1000;
        xSemaphoreGive(S.mu);
        if (S.bright != S.want_bright) { S.bright = S.want_bright; bsp_display_brightness_set(S.bright); }
        int64_t t1 = esp_timer_get_time();
        uint32_t px = flush_dirty();
        int64_t t2 = esp_timer_get_time();
        if (frame % 2 == 0) poll_touch();
        if (snap_req) {
            snap_req = false;
            snap_img = heap_caps_malloc(W * W * 2, MALLOC_CAP_SPIRAM);
            if (snap_img)
                for (int y = 0; y < W; y += 32) compose((rect_t){0, y, W - 1, imin(y + 31, W - 1)}, snap_img + y * W);
            xSemaphoreGive(snap_done);
        }
        stats(t2, px, t2 - t1, t1 - t0);
        int el = (int)((esp_timer_get_time() - t0) / 1000);
        vTaskDelay(pdMS_TO_TICKS(el < FRAME_MS ? FRAME_MS - el : 1));
    }
}

// --- public --------------------------------------------------------------------------------------
static uint32_t tick_ms(void) { return (uint32_t)now_ms(); }

void scene_init(void)
{
    // Art and buffers.
    make_liquid();
    make_glow(S.blob_alut, 2.0f);
    make_glow(S.glow_alut, 1.6f);
    make_highlight();
    for (int i = 0; i < 2; i++) {
        S.tri_buf[i] = psram(TB_W * TB_H * sizeof(tri_px_t));
        S.flat_c[i] = psram(TB_W * TB_H * 2);
        S.flat_a[i] = psram(TB_W * TB_H);
        S.dma[i] = heap_caps_malloc(STRIP_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(S.dma[i]);
    }
    S.mu = xSemaphoreCreateMutex();
    snap_done = xSemaphoreCreateBinary();

    // Panel and touch, driven directly.
    const bsp_display_config_t dcfg = {.max_transfer_sz = STRIP_PX * 2};
    ESP_ERROR_CHECK(bsp_display_new(&dcfg, &S.panel, NULL));
    bsp_display_brightness_set(0);
    bsp_display_cfg_t tcfg = {.touch_flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1}};
    if (bsp_touch_new(&tcfg, &S.touch) != ESP_OK) ESP_LOGW(TAG, "no touch");

    // LVGL, off screen only: a display that is never refreshed, holding the canvas.
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(W, W);
    lv_display_enable_invalidation(disp, false);
    S.canvas = lv_canvas_create(lv_display_get_screen_active(disp));
    S.buf_t = new_buf(TB_W, TB_H);
    S.buf_x = new_buf(TB_W, TB_H);
    S.half[0] = psram(HW * HH * 4);
    S.half[1] = psram(HW * HH * 4);
    font_px(40);   // warm the font up
    render_triangle("", 0);                            // the blank die that tumbles while thinking

    // Scene.
    for (int i = 0; i < N_BLOBS; i++) {
        S.blobs[i] = (blob_t){.a = frand(0, 6.28f), .r = frand(40, 140), .s = frand(0.0001f, 0.0002f) * (i % 2 ? -1 : 1)};
        S.blobs[i].box = NONE;
    }
    S.blob_opa = -1;
    for (int i = 0; i < N_SPECKS; i++) {
        speck_t *p = &S.specks[i];
        spawn_speck(p);
        p->dx = p->x; p->dy = p->y;
        p->r = fmaxf(0.7f, frand(1.2f, 3.4f) / 2);
        p->box = dot_box(p->dx, p->dy, p->r);
        p->col = rgb565(150, 175, 255);
        p->opa = (uint8_t)(40 + (1 - p->z) * 90);
    }
    S.glow_box = S.T.box = NONE;
    S.churn = 1; S.last = S.last_activity = now_ms();
    update(S.last);
    S.dirty_all = true;
    flush_dirty();
    S.bright = S.want_bright = BRIGHTNESS;
    bsp_display_brightness_set(BRIGHTNESS);
    xTaskCreatePinnedToCore(render_task, "render", 6144, NULL, 5, &render_h, 1);
    xTaskCreatePinnedToCore(helper_task, "render2", 4096, NULL, 5, &helper_h, 0);
    ESP_LOGI(TAG, "scene ready, free PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void touch_activity(void)
{
    S.last_activity = now_ms();
    if (S.dimmed) { S.dimmed = false; S.want_bright = BRIGHTNESS; }
}

void scene_wake(void) { xSemaphoreTake(S.mu, portMAX_DELAY); touch_activity(); xSemaphoreGive(S.mu); }

void scene_listen(void)
{
    xSemaphoreTake(S.mu, portMAX_DELAY);
    touch_activity();
    if (S.tri == TRI_RISE || S.tri == TRI_UP || S.tri == TRI_DEEP) sink(now_ms());
    S.mode = M_LISTENING;
    xSemaphoreGive(S.mu);
}

void scene_think(void)
{
    xSemaphoreTake(S.mu, portMAX_DELAY);
    touch_activity();
    S.mode = M_THINKING;
    int64_t now = now_ms();
    if (S.tri != TRI_SINK) { S.tri = TRI_DEEP; S.tri_t = now; }
    else S.tri_next = TRI_DEEP;
    xSemaphoreGive(S.mu);
}

void scene_reveal(const char *answer)
{
    // Rendered into the buffer that is not on screen, while the die keeps tumbling.
    int64_t t0 = now_ms();
    int back = S.want_front ^ 1;
    render_triangle(answer, back);
    ESP_LOGI(TAG, "triangle rendered in %d ms", (int)(now_ms() - t0));

    xSemaphoreTake(S.mu, portMAX_DELAY);
    touch_activity();
    int64_t now = now_ms();
    const float third = 2.0943951f;
    float r = fmodf(S.rot - S.up, third);           // the blank die's corner nearest to up
    if (r < 0) r += third;
    if (r > third / 2) r -= third;
    S.rot0 = r;
    S.offx0 = cosf(now / 600.0f) * 22; S.offy0 = sinf(now / 520.0f) * 16;
    S.want_front = back;
    S.tri = TRI_RISE; S.tri_t = now; S.burst = false;
    S.mode = M_REVEAL;
    xSemaphoreGive(S.mu);
}

bool scene_busy(void) { return S.tri == TRI_RISE || S.tri == TRI_DEEP; }

void scene_set_tilt(float x, float y) { S.tx = clampf(x, -1, 1); S.ty = clampf(y, -1, 1); }

void scene_set_up(float x, float y)
{
    // Only when gravity lies enough across the screen (not lying face up), and past a few degrees:
    // a hand is never still, and each turn redraws the whole triangle.
    if (x * x + y * y < 400.0f * 400.0f) return;
    float a = atan2f(x, -y), d = remainderf(a - S.up_target, 6.2831853f);
    if (fabsf(d) > 0.09f) S.up_target = a;
}

void scene_agitate(float amount) { if (amount > S.agitation) S.agitation = clampf(amount, 0, 6); }

void scene_fps_log(bool on) { F.on = on; }

void scene_snapshot_to_console(void)
{
    snap_req = true;                                    // composed by the render task, between frames
    if (xSemaphoreTake(snap_done, pdMS_TO_TICKS(5000)) != pdTRUE || !snap_img) { printf("SNAP-ERROR\n"); return; }
    snap_printing = true;
    esp_log_level_set("*", ESP_LOG_NONE);              // other tasks' log lines would land inside it
    printf("\nSNAP-BEGIN %d %d %d\n", W, W, W * 2);
    const size_t chunk = 57 * 64;                  // multiple of 3: base64 lines join cleanly
    static unsigned char out[57 * 64 * 4 / 3 + 8];
    const uint8_t *data = (const uint8_t *)snap_img;
    for (size_t off = 0, size = W * W * 2; off < size; off += chunk) {
        size_t n = size - off < chunk ? size - off : chunk, olen = 0;
        mbedtls_base64_encode(out, sizeof out, &olen, data + off, n);
        fwrite(out, 1, olen, stdout);
        fputc('\n', stdout);
    }
    printf("SNAP-END\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
    snap_printing = false;
    free(snap_img);
    snap_img = NULL;
}
