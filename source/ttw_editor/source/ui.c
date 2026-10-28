#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ttw.h"
#include "ui.h"
#include "style.h"

#define PE_SLOT (0x210 / 8)

typedef void (*ProcessEvent_t)(void* self, void* fn, void* parms);
typedef struct { uint16_t* data; int32_t count, max; } FString;
typedef struct { float r, g, b, a; } LinearColor;

static ProcessEvent_t g_orig_pe;
static void** g_hud_vtable;
static uintptr_t g_fn_drawhud, g_fn_rect, g_fn_text, g_fn_textsize;
static volatile uint32_t g_pe_calls, g_draw_calls;
static UiDraw g_draw_cb;
static uintptr_t g_cur_hud;
static float g_sw, g_sh;
static LinearColor g_color = {1, 1, 1, 1};
static bool g_in_draw;

static uint16_t g_wbuf[256];
static FString fstr(const char* s) {
    int n = 0;
    while (s[n] && n < 255) { g_wbuf[n] = (uint8_t)s[n]; n++; }
    g_wbuf[n] = 0;
    FString f = {g_wbuf, n + 1, n + 1};
    return f;
}

static void hud_call(uintptr_t fn, void* parms) { g_orig_pe((void*)g_cur_hud, (void*)fn, parms); }

void ui_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    g_color.r = r / 255.0f; g_color.g = g / 255.0f; g_color.b = b / 255.0f; g_color.a = a / 255.0f;
}
void ui_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    struct { LinearColor c; float x, y, w, h; } p = {{r, g, b, a}, x, y, w, h};
    hud_call(g_fn_rect, &p);
}
void ui_text(float x, float y, const char* s, float scale) {
    if (!s || !s[0]) return;
    struct { FString text; LinearColor c; float x, y; uintptr_t font; float scale; uint8_t scale_pos, pad[3]; } p;
    _Static_assert(sizeof(p) == 0x38, "DrawText parms");
    memset(&p, 0, sizeof(p));
    LinearColor c = g_color;
    if (c.r == 0 && c.g == 0 && c.b == 0) { Rgb t = style_text(); c.r = t.r; c.g = t.g; c.b = t.b; }
    p.text = fstr(s); p.c = c; p.x = x; p.y = y; p.font = style_font(); p.scale = scale * style_scale();
    hud_call(g_fn_text, &p);
}
float ui_text_width(const char* s, float scale) {
    if (!s || !s[0]) return 0;
    struct { FString text; float w, h; uintptr_t font; float scale, pad; } p;
    _Static_assert(sizeof(p) == 0x28, "GetTextSize parms");
    memset(&p, 0, sizeof(p));
    p.text = fstr(s); p.font = style_font(); p.scale = scale * style_scale();
    hud_call(g_fn_textsize, &p);
    return p.w;
}
float ui_screen_w(void) { return g_sw; }
float ui_screen_h(void) { return g_sh; }

static void __attribute__((force_align_arg_pointer)) hud_pe_hook(void* self, void* fn, void* parms) {
    g_orig_pe(self, fn, parms);
    g_pe_calls++;
    if ((uintptr_t)fn != g_fn_drawhud || !parms || !g_draw_cb || g_in_draw) return;
    g_draw_calls++;
    g_cur_hud = (uintptr_t)self;
    g_sw = (float)((int32_t*)parms)[0];
    g_sh = (float)((int32_t*)parms)[1];
    g_in_draw = true;
    g_draw_cb();
    g_in_draw = false;
}

void ui_stats(uint32_t* pe_calls, uint32_t* draw_calls) { *pe_calls = g_pe_calls; *draw_calls = g_draw_calls; }

int ui_install(UiDraw cb, void (*patch_ptr)(uint64_t at, uint64_t value)) {
    g_draw_cb = cb;
    if (g_hud_vtable) return 0;
    uintptr_t hud = ttw_hud();
    if (!hud) return -2;
    g_fn_drawhud  = ttw_find_function("HUD", "ReceiveDrawHUD");
    g_fn_rect     = ttw_find_function("HUD", "DrawRect");
    g_fn_text     = ttw_find_function("HUD", "DrawText");
    g_fn_textsize = ttw_find_function("HUD", "GetTextSize");
    if (!g_fn_drawhud || !g_fn_rect || !g_fn_text || !g_fn_textsize) return -3;
    void** vt = *(void***)hud;
    g_orig_pe = (ProcessEvent_t)vt[PE_SLOT];
    patch_ptr((uint64_t)&vt[PE_SLOT], (uint64_t)&hud_pe_hook);
    g_hud_vtable = vt;
    return 0;
}

void ui_uninstall(void (*restore)(void)) { if (g_hud_vtable && restore) restore(); g_hud_vtable = NULL; }
