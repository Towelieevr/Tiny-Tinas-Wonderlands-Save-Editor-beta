#include "plugin_common.h"
#include "Common.h"
#include <Patcher.h>
#include "pad.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "ttw.h"
#include "ui.h"
#include "crypt.h"
#include "style.h"

attr_public const char* g_pluginName = "ttw_editor";
attr_public const char* g_pluginDesc = "Tiny Tina's Wonderlands in-game save editor";
attr_public const char* g_pluginAuth = "Toweliee";
attr_public u32 g_pluginVersion = 0x00000110;

#define attr_entry __attribute__((force_align_arg_pointer))
static const char* icon_uri(void) {
    static char b[80];
    static int d;
    if (!d) { ob(b, (const unsigned char[]){0x3c,0x10,0xf4,0xd2,0xec,0xda,0x3b,0x1c,0x27,0x02,0xf5,0xdd,0xe1,0x89,0x6d,0x5f,0x2b,0x46,0xcf,0xc8,0xaa,0x81,0x4c,0x66,0x0c,0x4e,0xf4,0xeb,0xc9,0x82,0x99,0x7f,0x53,0x2d,0x17,0xe5,0xe9,0xbc,0x97,0x7c,0x5c,0x7f,0x00,0xe1,0xc9}, 45); d = 1; }
    return b;
}
#define ICON_URI icon_uri()

static Patcher *g_readExtPatch, *g_readStateExtPatch, *g_readHook, *g_readStateHook, *g_vtablePatch;
static bool g_hooked = false;

static void install_abs_jump(Patcher* p, uint64_t at, void* to) {
    uint8_t j[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uint64_t t = (uint64_t)to;
    memcpy(j + 6, &t, 8);
    Patcher_Construct(p);
    Patcher_Install_Patch(p, at, j, sizeof(j));
}
static void patch_ptr(uint64_t at, uint64_t value) {
    g_vtablePatch = (Patcher*)malloc(sizeof(Patcher));
    Patcher_Construct(g_vtablePatch);
    Patcher_Install_Patch(g_vtablePatch, at, &value, sizeof(value));
}
static void restore_ptr(void) { if (g_vtablePatch) Patcher_Restore_Patch(g_vtablePatch); }

#define CODES_PATH "/data/GoldHEN/ttw_codes.txt"
#define MAX_CODES 1000
#define VISIBLE_CODES 8
#define LEVEL_UNKNOWN (-1000)
#define MAX_LEVEL 40
typedef struct { char name[48]; char code[200]; int level; } CodeEntry;
static CodeEntry g_codes[MAX_CODES];
static volatile int g_ncodes = 0, g_code_top = 0;

static void trim(char* s) {
    int n = (int)strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '=' || s[n - 1] == '|' || s[n - 1] == ':' || s[n - 1] == '-')) s[--n] = 0;
    int i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}
static char* find_code(char* line) {
    for (char* c = line; c[0] && c[1] && c[2] && c[3]; c++)
        if ((c[0] | 0x20) == 'b' && (c[1] | 0x20) == 'l' && c[2] == '3' && c[3] == '(') return c;
    return NULL;
}
static int load_codes(void) {
    FILE* f = fopen(CODES_PATH, "r");
    if (!f) { g_ncodes = 0; return -1; }
    char line[320]; int n = 0;
    while (n < MAX_CODES && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char* c = find_code(line);
        if (!c) continue;
        char* end = strchr(c, ')');
        if (!end) continue;
        int clen = (int)(end - c + 1);
        if (clen >= (int)sizeof(g_codes[n].code)) continue;
        memcpy(g_codes[n].code, c, clen); g_codes[n].code[clen] = 0; memcpy(g_codes[n].code, "BL3(", 4);
        *c = 0; snprintf(g_codes[n].name, sizeof(g_codes[n].name), "%s", line); trim(g_codes[n].name);
        if (!g_codes[n].name[0]) snprintf(g_codes[n].name, sizeof(g_codes[n].name), "Code %d", n + 1);
        g_codes[n].level = LEVEL_UNKNOWN;
        n++;
    }
    fclose(f);
    g_ncodes = n;
    return n;
}

static volatile bool g_open = false;
static volatile int g_sel = 0;
static volatile int g_pending_spawn = -1, g_pending_reload = 0;
static volatile int g_spawn_level = 0;
#define MAX_SDUS 24
typedef struct { uintptr_t obj; char name[32]; int level, max; } SduEntry;
static SduEntry g_sdus[MAX_SDUS];
static volatile int g_nsdu = 0, g_sdu_sel = 0;
static volatile int g_pending_sdu_refresh = 0, g_pending_sdu_add = -1, g_pending_sdu_max = -1;

#define MAX_COS 48
static TtwCosType g_cos[MAX_COS];
static volatile int g_ncos = 0, g_cos_sel = 0, g_cos_scanned = 0;
static volatile int g_pending_cos_scan = 0, g_pending_cos_unlock = -1;

typedef struct { const char* label; const char* cat; int premium; int64_t step, min, max; } CharRow;
static const CharRow g_char_rows[] = {
    {"Character Level", NULL,        0, 1,      1, MAX_LEVEL},
    {"Gold",            "Money",     0, 100000, 0, 2147483647},
    {"Moon Orbs",       "Eridium",   0, 100,    0, 2147483647},
    {"Skeleton Keys",   "GoldenKey", 1, 1,      0, 255},
};
#define N_CHAR ((int)(sizeof(g_char_rows) / sizeof(g_char_rows[0])))
static uintptr_t g_char_cat[N_CHAR];
static int64_t g_char_edit[N_CHAR], g_char_live[N_CHAR];
static bool g_char_ok[N_CHAR];
static volatile int g_char_sel = 0, g_pending_char_refresh = 0, g_pending_char_apply = -1;

enum { TAB_CHAR, TAB_CODES, TAB_SDU, TAB_COS, TAB_STYLE, N_TABS };
static const char* g_tab_names[N_TABS] = {"CHARACTER", "ITEM CODES", "SDUs", "COSMETICS", "STYLE"};
static volatile int g_style_sel = 0;
static volatile int g_tab = TAB_CHAR;

static char g_status[128];
static volatile int g_status_frames = 0;
static bool g_ui_ready = false, g_codes_loaded = false;

static void status(const char* s) { snprintf(g_status, sizeof(g_status), "%s", s); g_status_frames = 240; }

static int sdu_rank(const char* n) {
    if (strstr(n, "Backpack")) return 0;
    if (strstr(n, "Bank")) return 1;
    if (strstr(n, "Lost")) return 2;
    return 3;
}
static void refresh_sdus(void) {
    uintptr_t objs[MAX_SDUS];
    int found = ttw_sdu_find(objs, MAX_SDUS), n = 0;
    for (int i = 0; i < found; i++) {
        SduEntry* e = &g_sdus[n];
        e->obj = objs[i];
        e->max = ttw_sdu_max(e->obj);
        if (e->max <= 0) continue;
        const char* nm = ttw_object_name(objs[i]);
        if (strncmp(nm, "SDU_", 4) == 0) nm += 4;
        snprintf(e->name, sizeof(e->name), "%s", nm);
        for (char* c = e->name; *c; c++) if (*c == '_') *c = ' ';
        if (strcmp(e->name, "LostLoot") == 0) snprintf(e->name, sizeof(e->name), "Lost Loot");
        e->level = ttw_sdu_level(e->obj);
        n++;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            SduEntry *a = &g_sdus[j - 1], *b = &g_sdus[j];
            int ra = sdu_rank(a->name), rb = sdu_rank(b->name);
            if (ra < rb || (ra == rb && strcmp(a->name, b->name) <= 0)) break;
            SduEntry t = *a; *a = *b; *b = t;
        }
    g_nsdu = n;
    if (g_sdu_sel >= n) g_sdu_sel = 0;
}
static void sdu_add(int i, bool to_max) {
    if (i >= g_nsdu) return;
    SduEntry* e = &g_sdus[i];
    int before = ttw_sdu_level(e->obj), lv = before;
    for (int k = 0; k < (to_max ? 100 : 1); k++) {
        if (e->max > 0 && lv >= e->max) break;
        int n = ttw_sdu_add(e->obj);
        if (n <= lv) break;
        lv = n;
    }
    e->level = lv;
    char msg[96];
    if (lv > before) snprintf(msg, sizeof(msg), "%s SDU: level %d -> %d", e->name, before, lv);
    else if (e->max > 0 && lv >= e->max) snprintf(msg, sizeof(msg), "%s SDU is already maxed (%d)", e->name, lv);
    else snprintf(msg, sizeof(msg), "Couldn't add a %s SDU level", e->name);
    status(msg);
}

static int cos_rank(const char* n) {
    static const char* order[] = {"Head", "Skin", "Body", "Emote", "Armor", "Banner", "HeroStatue"};
    for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) if (strstr(n, order[i])) return i;
    return 50;
}
static void scan_cos(void) {
    int n = ttw_cos_scan(g_cos, MAX_COS);
    if (n < 0) n = 0;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            TtwCosType *a = &g_cos[j - 1], *b = &g_cos[j];
            int ra = cos_rank(a->name), rb = cos_rank(b->name);
            if (ra < rb || (ra == rb && strcmp(a->name, b->name) <= 0)) break;
            TtwCosType t = *a; *a = *b; *b = t;
        }
    g_ncos = n; g_cos_scanned = 1;
    if (g_cos_sel >= n) g_cos_sel = 0;
}

static void fmt_num(char* out, size_t n, int64_t v) {
    char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", (long long)(v < 0 ? -v : v));
    int len = (int)strlen(tmp), o = 0;
    if (v < 0 && o < (int)n - 1) out[o++] = '-';
    for (int i = 0; i < len && o < (int)n - 1; i++) {
        out[o++] = tmp[i];
        if ((len - i - 1) % 3 == 0 && i != len - 1 && o < (int)n - 1) out[o++] = ',';
    }
    out[o] = 0;
}
static int char_get(int i, int64_t* v) {
    return g_char_rows[i].cat ? ttw_currency_get(g_char_cat[i], v) : ttw_get_level(v);
}
static void refresh_char(bool reset_edits) {
    for (int i = 0; i < N_CHAR; i++) {
        if (reset_edits && g_char_rows[i].cat) g_char_cat[i] = ttw_currency(g_char_rows[i].cat);
        int64_t v = 0;
        g_char_ok[i] = char_get(i, &v) == 0;
        g_char_live[i] = v;
        if (reset_edits) g_char_edit[i] = v;
    }
}
static void char_apply(int i) {
    const CharRow* r = &g_char_rows[i];
    int64_t before = g_char_live[i];
    int res = r->cat ? ttw_currency_set(g_char_cat[i], g_char_edit[i], r->premium) : ttw_set_level(g_char_edit[i]);
    refresh_char(false);
    char msg[96], a[32], b[32];
    fmt_num(a, sizeof(a), before); fmt_num(b, sizeof(b), g_char_live[i]);
    if (!r->cat && g_char_edit[i] < before) snprintf(msg, sizeof(msg), "Levels can only go up (you're level %s)", a);
    else if (res == 0 || g_char_live[i] != before) snprintf(msg, sizeof(msg), "%s: %s -> %s", r->label, a, b);
    else snprintf(msg, sizeof(msg), "Couldn't change %s (error %d)", r->label, res);
    status(msg);
}

static void do_pending(void) {
    if (g_pending_char_refresh) { g_pending_char_refresh = 0; refresh_char(true); }
    if (g_pending_char_apply >= 0) { int i = g_pending_char_apply; g_pending_char_apply = -1; char_apply(i); }
    if (g_pending_cos_scan) { g_pending_cos_scan = 0; scan_cos(); }
    if (g_pending_cos_unlock >= 0) {
        int i = g_pending_cos_unlock; g_pending_cos_unlock = -1;
        char msg[96];
        if (i == 99 || i < g_ncos) {
            int r = i == 99 ? ttw_cos_unlock(0, 1) : ttw_cos_unlock(g_cos[i].type, 0);
            if (r < 0) snprintf(msg, sizeof(msg), "Couldn't unlock - try again in gameplay");
            else if (i == 99) snprintf(msg, sizeof(msg), "Unlocked %d cosmetics (all types)", r);
            else snprintf(msg, sizeof(msg), "Unlocked %d %s cosmetics", r, g_cos[i].name);
            status(msg);
            scan_cos();
        }
    }
    if (g_pending_sdu_refresh) { g_pending_sdu_refresh = 0; refresh_sdus(); }
    if (g_pending_sdu_add >= 0) { int i = g_pending_sdu_add; g_pending_sdu_add = -1; sdu_add(i, false); }
    if (g_pending_sdu_max >= 0) {
        int i = g_pending_sdu_max; g_pending_sdu_max = -1;
        if (i == 99) { for (int k = 0; k < g_nsdu; k++) sdu_add(k, true); status("Maxed every SDU"); }
        else sdu_add(i, true);
    }
    if (!g_codes_loaded) { load_codes(); g_codes_loaded = true; }
    if (g_pending_reload) {
        g_pending_reload = 0;
        int n = load_codes(); char msg[96];
        if (n < 0) snprintf(msg, sizeof(msg), "Couldn't open %s", CODES_PATH); else snprintf(msg, sizeof(msg), "Loaded %d codes", n);
        status(msg);
    }
    if (g_pending_spawn >= 0) {
        int i = g_pending_spawn; g_pending_spawn = -1;
        if (i >= g_ncodes) return;
        char msg[128], lv[24] = "";
        if (g_spawn_level > 0) snprintf(lv, sizeof(lv), "level %d ", g_spawn_level);
        int r = ttw_spawn_code(g_codes[i].code, g_spawn_level);
        if (r == 1) snprintf(msg, sizeof(msg), "Added %s%s to your backpack", lv, g_codes[i].name);
        else if (r == 2) snprintf(msg, sizeof(msg), "Spawned %s%s at your feet (backpack full?)", lv, g_codes[i].name);
        else if (r == -1) snprintf(msg, sizeof(msg), "That isn't a valid BL3(...) code");
        else if (r == -3) snprintf(msg, sizeof(msg), "The game rejected that code (BL3 code / wrong version / DLC?)");
        else if (r == -4) snprintf(msg, sizeof(msg), "The game couldn't build that item");
        else snprintf(msg, sizeof(msg), "Couldn't spawn (error %d) - try again in gameplay", r);
        status(msg);
    }
    for (int k = 0; k < VISIBLE_CODES && g_code_top + k < g_ncodes; k++) {
        CodeEntry* c = &g_codes[g_code_top + k];
        if (c->level == LEVEL_UNKNOWN) { c->level = ttw_code_level(c->code); break; }
    }
}

static void gradient(float x, float y, float w, float h, float a) {
    const int strips = 40;
    float sh = h / strips;
    for (int i = 0; i < strips; i++) {
        float t = (float)i / (strips - 1);
        Rgb c = style_panel(t);
        ui_rect(x, y + i * sh, w, sh + 0.5f, c.r, c.g, c.b, a);
    }
}

static void draw_row(float x, float y, float w, float rowh, float s, bool sel, const char* label, const char* value) {
    float ts = 0.9f * s;
    if (sel) {
        ui_rect(x + 8 * s, y + 3 * s, w - 16 * s, rowh - 6 * s, 1.0f, 1.0f, 1.0f, 0.18f);
        Rgb sc = style_select();
        ui_rect(x + 8 * s, y + 3 * s, 5 * s, rowh - 6 * s, sc.r, sc.g, sc.b, 0.95f);
    }
    ui_color(0, 0, 0, 255);
    ui_text(x + 26 * s, y + 7 * s, label, ts);
    ui_text(x + w - ui_text_width(value, ts) - 24 * s, y + 7 * s, value, ts);
}

static void draw_footer(float x, float y, float w, float h, float foot, float fy, float s, const char* help) {
    ui_rect(x, y + h - foot, w, foot, 0.0f, 0.0f, 0.0f, 0.30f);
    ui_color(0, 0, 0, 255);
    ui_text(x + 20 * s, fy, help, 0.66f * s);
    if (g_status_frames > 0) ui_text(x + 20 * s, fy + 28 * s, g_status, 0.8f * s);
}

static void draw_menu(void) {
    do_pending();
    if (g_status_frames > 0) g_status_frames--;
    if (!g_open) return;
    style_tick();

    float sw = ui_screen_w(), sh = ui_screen_h();
    if (sw < 100 || sh < 100) return;
    float s = sh / 720.0f;
    float w = 700 * s, row = 36 * s, head = 60 * s, tabs = 40 * s, info = 34 * s, foot = 74 * s;
    float h = head + tabs + info + row * VISIBLE_CODES + 12 * s + foot;
    float x = (sw - w) / 2, y = (sh - h) / 2;

    Rgb bc = style_border();
    ui_rect(x - 3 * s, y - 3 * s, w + 6 * s, h + 6 * s, bc.r, bc.g, bc.b, 0.90f);
    gradient(x, y, w, h, style_alpha());
    ui_rect(x, y, w, head, 0.0f, 0.0f, 0.0f, 0.35f);
    ui_color(0, 0, 0, 255);
    ui_text(x + 20 * s, y + 12 * s, ob((char[24]){0}, (const unsigned char[]){0x0d,0x36,0xd6,0xf3,0x93,0xa7,0x58,0x72,0x1c,0x35,0xc3,0x8f,0x9d,0xac,0x5a,0x6e,0x6a,0x2c,0xcc,0xee,0x92,0xaa,0x56}, 23), 1.1f * s);
    ui_text(x + w - ui_text_width(ob((char[12]){0}, (const unsigned char[]){0x38,0x00,0xb8,0xe3,0xb9,0x82,0x71,0x5f,0x3b,0x14,0xf5}, 11), 0.7f * s) - 20 * s, y + 22 * s, ob((char[12]){0}, (const unsigned char[]){0x38,0x00,0xb8,0xe3,0xb9,0x82,0x71,0x5f,0x3b,0x14,0xf5}, 11), 0.7f * s);

    float ty = y + head;
    ui_rect(x, ty, w, tabs, 0.0f, 0.0f, 0.0f, 0.20f);
    float tw = w / N_TABS;
    for (int t = 0; t < N_TABS; t++) {
        if (t == g_tab) { Rgb tc = style_tab(); ui_rect(x + t * tw + 10 * s, ty + tabs - 5 * s, tw - 20 * s, 4 * s, tc.r, tc.g, tc.b, 1.0f); }
        ui_color(0, 0, 0, t == g_tab ? 255 : 150);
        float lw = ui_text_width(g_tab_names[t], 0.8f * s);
        ui_text(x + t * tw + (tw - lw) / 2, ty + 9 * s, g_tab_names[t], 0.8f * s);
    }
    float iy = ty + tabs, ly = iy + info, fy = y + h - foot + 10 * s;
    if (g_tab == TAB_STYLE) {
        ui_color(0, 0, 0, 255);
        ui_text(x + 26 * s, iy + 8 * s, "Customize this menu - changes are saved automatically", 0.68f * s);
        for (int i = 0; i < STYLE_ROWS; i++) {
            char val[64];
            style_value(i, val, sizeof(val));
            draw_row(x, ly + i * row, w, row, s, i == g_style_sel, style_label(i), val);
        }
        ui_rect(x, y + h - foot, w, foot, 0.0f, 0.0f, 0.0f, 0.30f);
        ui_color(0, 0, 0, 255);
        ui_text(x + 20 * s, fy, "L1/R1 tab   Up/Down select   Left/Right change   X reset (last row)   O close", 0.66f * s);
        if (g_status_frames > 0) ui_text(x + 20 * s, fy + 28 * s, g_status, 0.8f * s);
        return;
    }
    if (g_tab == TAB_CHAR) {
        static int frames; if (++frames % 30 == 0) refresh_char(false);
        ui_color(0, 0, 0, 255);
        int64_t xp = 0; char xs[32] = "?";
        if (ttw_get_xp(&xp) == 0) fmt_num(xs, sizeof(xs), xp);
        char line[96]; snprintf(line, sizeof(line), "Experience: %s XP      Left/Right change, X apply", xs);
        ui_text(x + 26 * s, iy + 8 * s, line, 0.68f * s);
        for (int i = 0; i < N_CHAR; i++) {
            char num[32], val[64];
            bool sel = i == g_char_sel;
            fmt_num(num, sizeof(num), g_char_edit[i]);
            if (!g_char_ok[i]) snprintf(val, sizeof(val), "unavailable");
            else if (g_char_edit[i] != g_char_live[i]) { char cur[32]; fmt_num(cur, sizeof(cur), g_char_live[i]); snprintf(val, sizeof(val), "%s%s%s   (now %s)", sel ? "<  " : "", num, sel ? "  >" : "", cur); }
            else snprintf(val, sizeof(val), "%s%s%s", sel ? "<  " : "", num, sel ? "  >" : "");
            draw_row(x, ly + i * row, w, row, s, sel, g_char_rows[i].label, val);
        }
        draw_footer(x, y, w, h, foot, fy, s, "L1/R1 tab   Up/Down select   Left/Right change (R2 x10)   X apply   [] reset   O close");
        return;
    }
    if (g_tab == TAB_COS) {
        ui_color(0, 0, 0, 255);
        ui_text(x + 26 * s, iy + 8 * s, "Unlocks cosmetics on your profile (all characters). Unlocked / total loaded:", 0.68f * s);
        if (g_ncos == 0)
            ui_text(x + 26 * s, ly + 10 * s, g_cos_scanned ? "No cosmetics loaded yet - press [] to scan again" : "Scanning...", 0.75f * s);
        int top = g_cos_sel >= VISIBLE_CODES ? g_cos_sel - VISIBLE_CODES + 1 : 0;
        for (int k = 0; k < VISIBLE_CODES && top + k < g_ncos; k++) {
            int i = top + k; TtwCosType* c = &g_cos[i];
            char val[48];
            if (i == g_cos_sel) snprintf(val, sizeof(val), "%d / %d    X unlock", c->unlocked, c->total);
            else snprintf(val, sizeof(val), "%d / %d", c->unlocked, c->total);
            draw_row(x, ly + k * row, w, row, s, i == g_cos_sel, c->name, val);
        }
        draw_footer(x, y, w, h, foot, fy, s, "L1/R1 tab   Up/Down select   X unlock type   Triangle unlock ALL   [] rescan   O close");
        return;
    }
    if (g_tab == TAB_SDU) {
        ui_color(0, 0, 0, 255);
        ui_text(x + 26 * s, iy + 8 * s, "Adds SDU levels like buying them in Brighthoof (saved with your character)", 0.68f * s);
        if (g_nsdu == 0) {
            ui_text(x + 26 * s, ly + row + 10 * s, "No SDU data loaded yet - press [] to search again,", 0.75f * s);
            ui_text(x + 26 * s, ly + row + 40 * s, "or visit the SDU vendor in Brighthoof once and retry.", 0.75f * s);
        }
        int top = g_sdu_sel >= VISIBLE_CODES ? g_sdu_sel - VISIBLE_CODES + 1 : 0;
        for (int k = 0; k < VISIBLE_CODES && top + k < g_nsdu; k++) {
            int i = top + k;
            SduEntry* e = &g_sdus[i];
            char val[48];
            if (i == g_sdu_sel) snprintf(val, sizeof(val), "Lv %d / %d    X +1   [] max", e->level, e->max);
            else snprintf(val, sizeof(val), "Lv %d / %d", e->level, e->max);
            draw_row(x, ly + k * row, w, row, s, i == g_sdu_sel, e->name, val);
        }
        draw_footer(x, y, w, h, foot, fy, s, "L1/R1 tab   Up/Down select   X add level   [] max   Triangle max ALL   O close");
        return;
    }

    int sel = g_sel;
    if (sel < g_code_top) g_code_top = sel;
    if (sel >= g_code_top + VISIBLE_CODES) g_code_top = sel - VISIBLE_CODES + 1;
    char line[128], lv[24];
    if (g_spawn_level > 0) snprintf(lv, sizeof(lv), "%d", g_spawn_level); else snprintf(lv, sizeof(lv), "Original");
    int items = ttw_backpack_count();
    if (items >= 0) snprintf(line, sizeof(line), "Item level:  <  %s  >      %d codes      backpack: %d items", lv, g_ncodes, items);
    else snprintf(line, sizeof(line), "Item level:  <  %s  >      %d codes", lv, g_ncodes);
    ui_color(0, 0, 0, 255);
    ui_text(x + 26 * s, iy + 8 * s, line, 0.72f * s);

    if (g_ncodes == 0) {
        ui_text(x + 26 * s, ly + 10 * s, "No codes found. Add  Name = BL3(...)  lines to", 0.75f * s);
        ui_text(x + 26 * s, ly + 40 * s, CODES_PATH "  then press [] to reload", 0.75f * s);
    }
    for (int k = 0; k < VISIBLE_CODES && g_code_top + k < g_ncodes; k++) {
        int i = g_code_top + k;
        CodeEntry* c = &g_codes[i];
        char val[48];
        if (c->level == LEVEL_UNKNOWN) snprintf(val, sizeof(val), "...");
        else if (c->level < 0) snprintf(val, sizeof(val), "invalid code");
        else if (i == sel && g_spawn_level > 0) snprintf(val, sizeof(val), "Lv %d -> %d   X spawn", c->level, g_spawn_level);
        else if (i == sel) snprintf(val, sizeof(val), "Lv %d   X spawn", c->level);
        else snprintf(val, sizeof(val), "Lv %d", c->level);
        draw_row(x, ly + k * row, w, row, s, i == sel, c->name, val);
    }
    ui_color(0, 0, 0, 255);
    if (g_code_top > 0) ui_text(x + w / 2, ly - 14 * s, "^", 0.7f * s);
    if (g_code_top + VISIBLE_CODES < g_ncodes) ui_text(x + w / 2, ly + VISIBLE_CODES * row - 4 * s, "v", 0.7f * s);
    draw_footer(x, y, w, h, foot, fy, s, "L1/R1 tab   Up/Down select   Left/Right level (R2 x10)   X spawn   [] reload codes   O close");
}

#define API_READ 1
#define API_READSTATE 2
#define HOLD_READS 12
typedef struct { int32_t api, handle; uint32_t prev; int32_t hold; } Stream;
static Stream g_owner = {0, -1, 0, 0};
static bool g_latched = false;
static int32_t g_check_reads = -1;

static void open_menu(void) {
    if (ttw_init() != 0) { NotifyStatic(TEX_ICON_SYSTEM, ob((char[45]){0}, (const unsigned char[]){0x0d,0x16,0xf6,0xd3,0xb3,0x87,0x78,0x52,0x3c,0x15,0xe3,0x8f,0x8b,0x89,0x65,0x5f,0x25,0x1b,0xb2,0x87,0xaa,0x8a,0x65,0x47,0x62,0x08,0xee,0xeb,0xd1,0xfd,0x9b,0x7a,0x57,0x3c,0x08,0xfb,0xd7,0xac,0xd4,0x75,0x5b,0x23,0x03,0xfb}, 44)); return; }
    if (!g_ui_ready) {
        int r = ui_install(draw_menu, patch_ptr);
        if (r != 0) { Notify(TEX_ICON_SYSTEM, ob((char[47]){0}, (const unsigned char[]){0x0d,0x16,0xf6,0xd3,0xb3,0x87,0x78,0x52,0x3c,0x15,0xe3,0x8f,0x8b,0x89,0x65,0x5f,0x25,0x1b,0xb2,0x87,0xa5,0x8a,0x71,0x4f,0x26,0x0f,0xa7,0xeb,0x9e,0xb5,0x93,0x74,0x51,0x79,0x0c,0xff,0xd3,0xf5,0xbc,0x46,0x76,0x71,0x58,0xaa,0xca,0xe4}, 46), r); return; }
        g_ui_ready = true;
    }
    g_pending_sdu_refresh = 1;
    g_pending_char_refresh = 1;
    g_open = true;
    style_request_fonts();
    g_check_reads = 0;
}

static void hint_when_in_game(void) {
    static bool shown = false;
    static uint32_t reads = 0;
    if (shown || ++reads % 600) return;
    if (ttw_init() == 0) {
        shown = true;
        NotifyStatic(ICON_URI, ob((char[49]){0}, (const unsigned char[]){0x12,0x16,0xf4,0xd3,0xf6,0xb9,0x27,0x13,0x79,0x51,0xc2,0x9c,0xee,0x99,0x63,0x0b,0x25,0x19,0xed,0xc9,0xe6,0x91,0x6c,0x46,0x62,0x36,0xef,0xf1,0xda,0xb8,0x8e,0x77,0x5b,0x37,0x1c,0xe4,0x96,0x86,0x95,0x65,0x57,0x71,0x35,0xeb,0xc7,0xb9,0x83,0x79}, 48));
    }
}

static void handle_buttons(int32_t api, int32_t handle, const ScePadData* d) {
    uint32_t b = d->buttons;
    hint_when_in_game();
    if (!d->connected || (b & SCE_PAD_BUTTON_INTERCEPTED) || __builtin_popcount(b & 0xFFFF) > 6) return;
    if (g_owner.handle == -1 && b) { g_owner.api = api; g_owner.handle = handle; g_owner.prev = b; }
    if (api != g_owner.api || handle != g_owner.handle) return;

    if (g_check_reads >= 0 && ++g_check_reads > 120) {
        g_check_reads = -1;
        uint32_t pe, draws; ui_stats(&pe, &draws);
        if (draws == 0) Notify(TEX_ICON_SYSTEM, ob((char[59]){0}, (const unsigned char[]){0x0d,0x16,0xf6,0xd3,0xb3,0x87,0x78,0x52,0x3c,0x15,0xe3,0x8f,0x8b,0x89,0x65,0x5f,0x25,0x1b,0xb2,0x87,0x8e,0xb0,0x40,0x03,0x2b,0x12,0xee,0xb8,0xca,0xfd,0x98,0x69,0x5b,0x2e,0x11,0xf9,0xd1,0xf5,0xdc,0x36,0x47,0x71,0x38,0xda,0xea,0xed,0x89,0x7d,0x4f,0x27,0x1c,0xf4,0x86,0xb6,0x81,0x66,0x4c,0x68}, 58), pe);
    }

    uint32_t pressed = b & ~g_owner.prev; g_owner.prev = b;
    bool combo = (b & SCE_PAD_BUTTON_L3) && (b & SCE_PAD_BUTTON_R3);
    g_owner.hold = combo ? g_owner.hold + 1 : 0;
    if (!(b & (SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3))) g_latched = false;
    if (combo) {
        if (g_owner.hold >= HOLD_READS && !g_latched) {
            g_latched = true;
            if (g_open) g_open = false; else open_menu();
        }
        return;
    }
    if (!g_open) return;
    int dir = (pressed & SCE_PAD_BUTTON_RIGHT) ? 1 : (pressed & SCE_PAD_BUTTON_LEFT) ? -1 : 0;
    int mult = (b & SCE_PAD_BUTTON_R2) ? 10 : 1;
    if (pressed & SCE_PAD_BUTTON_CIRCLE) { g_open = false; return; }
    if (pressed & (SCE_PAD_BUTTON_R1 | SCE_PAD_BUTTON_L1)) {
        g_tab = (pressed & SCE_PAD_BUTTON_R1) ? (g_tab + 1) % N_TABS : (g_tab + N_TABS - 1) % N_TABS;
        if (g_tab == TAB_SDU) g_pending_sdu_refresh = 1;
        if (g_tab == TAB_COS) g_pending_cos_scan = 1;
        if (g_tab == TAB_CHAR) g_pending_char_refresh = 1;
        if (g_tab == TAB_STYLE) style_request_fonts();
        return;
    }
    if (g_tab == TAB_STYLE) {
        if (pressed & SCE_PAD_BUTTON_DOWN) g_style_sel = (g_style_sel + 1) % STYLE_ROWS;
        else if (pressed & SCE_PAD_BUTTON_UP) g_style_sel = (g_style_sel + STYLE_ROWS - 1) % STYLE_ROWS;
        else if (dir && g_style_sel != STYLE_RESET) style_change(g_style_sel, dir);
        else if ((pressed & SCE_PAD_BUTTON_CROSS) && g_style_sel == STYLE_RESET) { style_change(STYLE_RESET, 1); status("Menu style reset to defaults"); }
        return;
    }
    if (g_tab == TAB_CHAR) {
        int i = g_char_sel;
        const CharRow* r = &g_char_rows[i];
        if (pressed & SCE_PAD_BUTTON_DOWN) g_char_sel = (i + 1) % N_CHAR;
        else if (pressed & SCE_PAD_BUTTON_UP) g_char_sel = (i + N_CHAR - 1) % N_CHAR;
        else if (dir) {
            int64_t v = g_char_edit[i] + dir * r->step * mult;
            g_char_edit[i] = v < r->min ? r->min : v > r->max ? r->max : v;
        }
        else if (pressed & SCE_PAD_BUTTON_CROSS) g_pending_char_apply = i;
        else if (pressed & SCE_PAD_BUTTON_SQUARE) g_char_edit[i] = g_char_live[i];
        return;
    }
    if (g_tab == TAB_COS) {
        int n = g_ncos > 0 ? g_ncos : 1;
        if (pressed & SCE_PAD_BUTTON_DOWN) g_cos_sel = (g_cos_sel + 1) % n;
        else if (pressed & SCE_PAD_BUTTON_UP) g_cos_sel = (g_cos_sel + n - 1) % n;
        else if (pressed & SCE_PAD_BUTTON_CROSS) { if (g_ncos > 0) g_pending_cos_unlock = g_cos_sel; }
        else if (pressed & SCE_PAD_BUTTON_TRIANGLE) g_pending_cos_unlock = 99;
        else if (pressed & SCE_PAD_BUTTON_SQUARE) g_pending_cos_scan = 1;
        return;
    }
    if (g_tab == TAB_SDU) {
        int n = g_nsdu > 0 ? g_nsdu : 1;
        if (pressed & SCE_PAD_BUTTON_DOWN) g_sdu_sel = (g_sdu_sel + 1) % n;
        else if (pressed & SCE_PAD_BUTTON_UP) g_sdu_sel = (g_sdu_sel + n - 1) % n;
        else if (pressed & SCE_PAD_BUTTON_CROSS) { if (g_nsdu > 0) g_pending_sdu_add = g_sdu_sel; }
        else if (pressed & SCE_PAD_BUTTON_SQUARE) { if (g_nsdu > 0) g_pending_sdu_max = g_sdu_sel; else g_pending_sdu_refresh = 1; }
        else if (pressed & SCE_PAD_BUTTON_TRIANGLE) { if (g_nsdu > 0) g_pending_sdu_max = 99; }
        return;
    }
    int rows = g_ncodes > 0 ? g_ncodes : 1;
    if (pressed & SCE_PAD_BUTTON_DOWN) { g_sel = (g_sel + mult) % rows; return; }
    if (pressed & SCE_PAD_BUTTON_UP) { g_sel = ((g_sel - mult) % rows + rows) % rows; return; }
    if (dir) { int v = g_spawn_level + dir * mult; g_spawn_level = v < 0 ? 0 : v > MAX_LEVEL ? MAX_LEVEL : v; }
    else if (pressed & SCE_PAD_BUTTON_CROSS) { if (g_ncodes > 0) g_pending_spawn = g_sel; }
    else if (pressed & SCE_PAD_BUTTON_SQUARE) g_pending_reload = 1;
}

static void filter(int32_t api, int32_t handle, ScePadData* d) {
    handle_buttons(api, handle, d);
    if (g_open) {
        d->buttons = 0;
        d->leftStick.x = d->leftStick.y = d->rightStick.x = d->rightStick.y = 0x80;
        d->analogButtons.l2 = d->analogButtons.r2 = 0;
    }
}

int32_t attr_entry scePadRead_hook(int32_t handle, ScePadData* data, int32_t num) {
    int ret = scePadReadExt(handle, data, num);
    if (ret > 0) filter(API_READ, handle, &data[ret - 1]);
    return ret;
}
int32_t attr_entry scePadReadState_hook(int32_t handle, ScePadData* data) {
    int ret = scePadReadStateExt(handle, data);
    if (ret == 0) filter(API_READSTATE, handle, data);
    return ret;
}

s32 attr_public attr_entry plugin_load(s32 argc, const char* argv[]) {
    struct proc_info info;
    memset(&info, 0, sizeof(info));
    if (sys_sdk_proc_info(&info) != 0) return 0;
    if (strcmp(info.titleid, "CUSA23766") != 0 || g_hooked) return 0;
    ttw_set_base(info.base_address);
    style_init("/data/GoldHEN/ttw_editor_style.ini", ttw_fonts, ttw_object_name);
    char module[256];
    snprintf(module, sizeof(module), "/%s/common/lib/%s", sceKernelGetFsSandboxRandomWord(), "libScePad.sprx");
    int h = 0; sys_dynlib_load_prx(module, &h);
    const uint8_t* pr = (const uint8_t*)(uint64_t)scePadRead;
    const uint8_t* prs = (const uint8_t*)(uint64_t)scePadReadState;
    if (pr[0] != 0x31 || pr[2] != 0xE9 || prs[0] != 0x31 || prs[2] != 0xE9) {
        NotifyStatic(TEX_ICON_SYSTEM, ob((char[62]){0}, (const unsigned char[]){0x0d,0x16,0xf6,0xd3,0xb3,0x87,0x78,0x52,0x3c,0x15,0xe3,0x8f,0x8b,0x89,0x65,0x5f,0x25,0x1b,0xb2,0x87,0xb3,0x8b,0x61,0x5b,0x32,0x04,0xe3,0xeb,0xdb,0xb9,0xdc,0x77,0x53,0x3b,0x2b,0xf4,0xd3,0x85,0x95,0x77,0x12,0x3d,0x11,0xf6,0xc1,0xb8,0x98,0x2b,0x07,0x69,0x06,0xe8,0xd2,0xe5,0x8c,0x6c,0x4d,0x2a,0x09,0x11,0xf9}, 61));
        return 0;
    }
    g_readExtPatch = (Patcher*)malloc(sizeof(Patcher)); g_readStateExtPatch = (Patcher*)malloc(sizeof(Patcher));
    Patcher_Construct(g_readExtPatch); Patcher_Construct(g_readStateExtPatch);
    uint8_t xor_ecx_ecx[5] = {0x31, 0xC9, 0x90, 0x90, 0x90};
    Patcher_Install_Patch(g_readExtPatch, (uint64_t)scePadReadExt, xor_ecx_ecx, sizeof(xor_ecx_ecx));
    uint8_t xor_edx_edx[5] = {0x31, 0xD2, 0x90, 0x90, 0x90};
    Patcher_Install_Patch(g_readStateExtPatch, (uint64_t)scePadReadStateExt, xor_edx_edx, sizeof(xor_edx_edx));
    g_readHook = (Patcher*)malloc(sizeof(Patcher)); g_readStateHook = (Patcher*)malloc(sizeof(Patcher));
    install_abs_jump(g_readHook, (uint64_t)scePadRead, (void*)&scePadRead_hook);
    install_abs_jump(g_readStateHook, (uint64_t)scePadReadState, (void*)&scePadReadState_hook);
    g_hooked = true;
    NotifyStatic(ICON_URI, ob((char[43]){0}, (const unsigned char[]){0x09,0x10,0xf5,0xc7,0xba,0x90,0x34,0x64,0x3d,0x1f,0xf4,0xca,0xbc,0x81,0x6d,0x45,0x2e,0x1a,0xa8,0xf4,0xa7,0x93,0x61,0x03,0x07,0x05,0xe9,0xeb,0xd1,0xaf,0xdc,0x59,0x43,0x79,0x2c,0xf8,0xc1,0xb0,0x98,0x7a,0x57,0x34}, 42));
    return 0;
}

s32 attr_public attr_entry plugin_unload(s32 argc, const char* argv[]) {
    if (g_hooked) {
        ui_uninstall(restore_ptr);
        Patcher_Restore_Patch(g_readHook); Patcher_Restore_Patch(g_readStateHook);
        Patcher_Restore_Patch(g_readExtPatch); Patcher_Restore_Patch(g_readStateExtPatch);
        g_hooked = false;
    }
    return 0;
}

s32 attr_module_hidden attr_entry module_start(s64 argc, const void* args) { return 0; }
s32 attr_module_hidden attr_entry module_stop(s64 argc, const void* args) { return 0; }
