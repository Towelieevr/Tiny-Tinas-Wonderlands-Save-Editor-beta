#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "ttw.h"

#define GOBJECTS_OFF   0xBA700D0
#define GNAMES_PTR_OFF 0xB9689C0
#define O_FLAGS 0x08
#define O_CLASS 0x10
#define O_NAME  0x18
#define O_OUTER 0x20
#define S_SUPER 0x30
#define PE_SLOT (0x210 / 8)
#define PC_MYHUD 0x518
#define PC_PAWN  0x488
#define CHAR_INVENTORY 0xEE0
#define INV_CAPACITY_USED 0x230
#define RF_TEMPLATES 0x30

#define FN_SERIAL_FROM_BASE64 0x59347F0
#define FN_INITDATA_FROM_SERIAL 0x592DEC0
#define FN_FREE_TARRAY   0x8A2B0
#define FN_FREE_PARTS    0xB41630
#define FN_FREE_GENERIC  0xB41550
#define FN_FREE_CUSTOM   0xB41470

typedef struct { uintptr_t* chunks; void* pre; int32_t max, num, max_chunks, num_chunks; } ObjArray;
typedef struct { uintptr_t chunks[128]; int32_t num, num_chunks; } NameArray;
typedef struct { uint16_t* data; int32_t count, max; } FString;
typedef struct { uint8_t state; uint8_t pad[7]; uintptr_t data; int32_t count, max; int32_t extra, pad2; } GbxSerial;
typedef struct {
    int32_t game_stage, pad0;
    uintptr_t inventory_data, balance_data, manufacturer;
    uint8_t part_list[0x10], generic_parts[0x10], additional[0x10], customization[0x10];
    uint8_t reroll, pad1[3];
    int32_t overpower_level;
} InitData;

typedef void (*ProcessEvent_t)(void* self, void* fn, void* parms);
typedef void (*Fn2)(void* a, const void* b);
typedef void (*Fn1)(void* a);

static uint64_t g_base;
static uintptr_t g_pc, g_pawn, g_hud;

void ttw_set_base(uint64_t base) { g_base = base; }

static ObjArray* objarray(void) { return (ObjArray*)(g_base + GOBJECTS_OFF); }
static NameArray* names(void) { return *(NameArray**)(g_base + GNAMES_PTR_OFF); }

static const char* name_str(int32_t i) {
    NameArray* n = names();
    if (!n || i < 0 || i >= n->num) return "";
    uintptr_t chunk = n->chunks[i / 16384];
    if (!chunk) return "";
    uintptr_t e = ((uintptr_t*)chunk)[i % 16384];
    return e ? (const char*)(e + 0x10) : "";
}
static int32_t name_index(const char* s) {
    NameArray* n = names();
    if (!n) return -1;
    for (int32_t i = 0; i < n->num; i++) {
        uintptr_t chunk = n->chunks[i / 16384];
        if (!chunk) { i += 16383; continue; }
        uintptr_t e = ((uintptr_t*)chunk)[i % 16384];
        if (e && strcmp((const char*)(e + 0x10), s) == 0) return i;
    }
    return -1;
}
static uintptr_t obj_at(int32_t i) {
    ObjArray* a = objarray();
    uintptr_t chunk = a->chunks[i / 65536];
    return chunk ? *(uintptr_t*)(chunk + (i % 65536) * 0x18) : 0;
}
static inline uintptr_t obj_class(uintptr_t o) { return *(uintptr_t*)(o + O_CLASS); }
static inline int32_t obj_name(uintptr_t o) { return *(int32_t*)(o + O_NAME); }
static inline uintptr_t obj_outer(uintptr_t o) { return *(uintptr_t*)(o + O_OUTER); }

static bool is_a(uintptr_t o, int32_t cls_name) {
    for (uintptr_t c = obj_class(o); c; c = *(uintptr_t*)(c + S_SUPER))
        if (obj_name(c) == cls_name) return true;
    return false;
}

uintptr_t ttw_find_object(const char* name) {
    int32_t nn = name_index(name);
    if (nn < 0) return 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num; i++) {
        uintptr_t o = obj_at(i);
        if (o && obj_name(o) == nn) return o;
    }
    return 0;
}

uintptr_t ttw_find_function(const char* owner, const char* name) {
    int32_t no = name_index(owner), nn = name_index(name), nf = name_index("Function");
    if (no < 0 || nn < 0 || nf < 0) return 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num; i++) {
        uintptr_t o = obj_at(i);
        if (!o || obj_name(o) != nn || !obj_class(o) || obj_name(obj_class(o)) != nf) continue;
        uintptr_t out = obj_outer(o);
        if (out && obj_name(out) == no) return o;
    }
    return 0;
}

int ttw_fonts(uintptr_t* out, int max) {
    static int32_t n_font = -1;
    if (n_font < 0) n_font = name_index("Font");
    if (n_font < 0) return 0;
    ObjArray* a = objarray();
    int n = 0;
    for (int32_t i = 0; i < a->num && n < max; i++) {
        uintptr_t o = obj_at(i);
        if (!o || !obj_class(o) || (*(uint32_t*)(o + O_FLAGS) & RF_TEMPLATES)) continue;
        if (obj_name(obj_class(o)) == n_font) out[n++] = o;
    }
    return n;
}

void ttw_process_event(uintptr_t obj, uintptr_t fn, void* parms) {
    ProcessEvent_t f = (ProcessEvent_t)(*(void***)obj)[PE_SLOT];
    f((void*)obj, (void*)fn, parms);
}

int ttw_init(void) {
    if (!g_base || !names() || objarray()->num <= 0) return -1;
    static int32_t n_pc = -1;
    if (n_pc < 0) n_pc = name_index("PlayerController");
    if (n_pc < 0) return -1;
    if (g_pc && g_hud && *(uintptr_t*)(g_pc + PC_MYHUD) == g_hud && *(uintptr_t*)(g_pc + PC_PAWN)) {
        g_pawn = *(uintptr_t*)(g_pc + PC_PAWN);
        return 0;
    }
    g_pc = g_pawn = g_hud = 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num; i++) {
        uintptr_t o = obj_at(i);
        if (!o || !obj_class(o) || (*(uint32_t*)(o + O_FLAGS) & RF_TEMPLATES)) continue;
        if (!is_a(o, n_pc)) continue;
        uintptr_t hud = *(uintptr_t*)(o + PC_MYHUD), pawn = *(uintptr_t*)(o + PC_PAWN);
        if (hud && pawn) { g_pc = o; g_hud = hud; g_pawn = pawn; return 0; }
    }
    return -1;
}

uintptr_t ttw_hud(void) { return g_hud; }

static uintptr_t backpack(void) { return g_pawn ? *(uintptr_t*)(g_pawn + CHAR_INVENTORY) : 0; }

int ttw_backpack_count(void) {
    uintptr_t inv = backpack();
    return inv ? *(int32_t*)(inv + INV_CAPACITY_USED) : -1;
}

static uint16_t g_wcode[256];

static int decode(const char* code, InitData* out) {
    memset(out, 0, sizeof(*out));
    const char* p = strstr(code, "BL3(");
    if (!p) return -1;
    p += 4;
    int n = 0;
    while (p[n] && p[n] != ')' && n < 255) { g_wcode[n] = (uint8_t)p[n]; n++; }
    if (p[n] != ')' || n == 0 || (n % 4) != 0) return -1;
    g_wcode[n] = 0;
    FString b64 = {g_wcode, n + 1, n + 1};
    GbxSerial serial; memset(&serial, 0, sizeof(serial));
    ((Fn2)(g_base + FN_SERIAL_FROM_BASE64))(&serial, &b64);
    if (!serial.data || serial.count <= 0) { if (serial.data) ((Fn1)(g_base + FN_FREE_TARRAY))(&serial.data); return -2; }
    ((Fn2)(g_base + FN_INITDATA_FROM_SERIAL))(out, &serial);
    ((Fn1)(g_base + FN_FREE_TARRAY))(&serial.data);
    if (!out->balance_data || !out->inventory_data || out->game_stage <= 0) return -2;
    return 0;
}
static void free_init(InitData* d) {
    ((Fn1)(g_base + FN_FREE_PARTS))(d->part_list);
    ((Fn1)(g_base + FN_FREE_GENERIC))(d->generic_parts);
    ((Fn1)(g_base + FN_FREE_TARRAY))(d->additional);
    ((Fn1)(g_base + FN_FREE_CUSTOM))(d->customization);
}

int ttw_code_level(const char* code) {
    InitData d;
    int r = decode(code, &d);
    int lv = d.game_stage;
    free_init(&d);
    return r == 0 ? lv : r;
}

int ttw_spawn_code(const char* code, int level) {
    if (ttw_init() != 0) return -5;
    static uintptr_t fn_create, fn_add, fn_give, fn_loc, cdo_lib;
    if (!fn_create) {
        fn_create = ttw_find_function("InventoryBlueprintLibrary", "CreateInventory");
        fn_add    = ttw_find_function("InventoryListComponent", "AddItemFromPickup");
        fn_give   = ttw_find_function("InventoryItemPickup", "GiveInventoryToUser");
        fn_loc    = ttw_find_function("Actor", "K2_GetActorLocation");
        cdo_lib   = ttw_find_object("Default__InventoryBlueprintLibrary");
    }
    if (!fn_create || !fn_add || !fn_loc || !cdo_lib) return -6;

    InitData d;
    int r = decode(code, &d);
    if (r != 0) { free_init(&d); return r == -1 ? -1 : -3; }
    if (level > 0) d.game_stage = level;

    struct { float x, y, z; } loc = {0, 0, 0};
    ttw_process_event(g_pawn, fn_loc, &loc);

    struct {
        uintptr_t world_context; uint8_t create_pickup, pad[3];
        float x, y, z;
        InitData init;
        uintptr_t ret;
    } cp;
    _Static_assert(sizeof(InitData) == 0x68, "InitData size");
    _Static_assert(__builtin_offsetof(__typeof__(cp), init) == 0x18 && __builtin_offsetof(__typeof__(cp), ret) == 0x80, "CreateInventory layout");
    memset(&cp, 0, sizeof(cp));
    cp.world_context = g_pawn; cp.create_pickup = 1;
    cp.x = loc.x; cp.y = loc.y; cp.z = loc.z + 60.0f;
    memcpy(&cp.init, &d, sizeof(d));
    ttw_process_event(cdo_lib, fn_create, &cp);
    free_init(&d);
    uintptr_t pickup = cp.ret;
    if (!pickup) return -4;

    uintptr_t inv = backpack();
    int before = ttw_backpack_count();
    if (inv) {
        struct { uintptr_t pickup; uint32_t ret; uint32_t pad; } ap = {pickup, 0, 0};
        ttw_process_event(inv, fn_add, &ap);
    }
    if (ttw_backpack_count() > before) return 1;
    if (fn_give) {
        struct { uintptr_t other; uint8_t auto_equip, pad[3]; int32_t slot; } gp = {g_pawn, 0, {0}, 0};
        ttw_process_event(pickup, fn_give, &gp);
        if (ttw_backpack_count() > before) return 1;
    }
    return 2;
}

int ttw_sdu_find(uintptr_t* out, int max) {
    static int32_t n_sdu = -1;
    if (n_sdu < 0) n_sdu = name_index("OakSDUData");
    if (n_sdu < 0) return 0;
    int n = 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num && n < max; i++) {
        uintptr_t o = obj_at(i);
        if (!o || !obj_class(o) || (*(uint32_t*)(o + O_FLAGS) & RF_TEMPLATES)) continue;
        if (is_a(o, n_sdu)) out[n++] = o;
    }
    return n;
}

const char* ttw_object_name(uintptr_t o) { return o ? name_str(obj_name(o)) : ""; }

int ttw_sdu_level(uintptr_t sdu) {
    static uintptr_t fn;
    if (!fn) fn = ttw_find_function("OakCharacter_Player", "GetSDULevel");
    if (!fn || !sdu || ttw_init() != 0) return -1;
    struct { uintptr_t sdu; int32_t ret, pad; } p = {sdu, -1, 0};
    ttw_process_event(g_pawn, fn, &p);
    return p.ret;
}

int ttw_sdu_max(uintptr_t sdu) {
    static uintptr_t fn;
    if (!fn) fn = ttw_find_function("OakSDUData", "GetSDULevelInfo");
    if (!fn || !sdu) return -1;
    int lv = 0;
    for (int l = 1; l <= 100; l++) {
        uint8_t p[0x98]; memset(p, 0, sizeof(p));
        *(int32_t*)p = l;
        ttw_process_event(sdu, fn, p);
        if (!p[0x90]) break;
        lv = l;
    }
    return lv;
}

int ttw_sdu_add(uintptr_t sdu) {
    static uintptr_t fn;
    if (!fn) fn = ttw_find_function("OakCharacter_Player", "AddSDU");
    if (!fn || !sdu || ttw_init() != 0) return -1;
    struct { uintptr_t sdu; } p = {sdu};
    ttw_process_event(g_pawn, fn, &p);
    return ttw_sdu_level(sdu);
}

#define CUST_TYPE 0x110
static uintptr_t g_fn_cust_add, g_fn_cust_has, g_fn_part_add, g_fn_part_has;
static int32_t g_n_custdata = -1, g_n_partdata = -1;

static bool cos_init(void) {
    if (!g_fn_cust_add) {
        g_fn_cust_add = ttw_find_function("OakPlayerController", "AddUnlockedCustomization");
        g_fn_cust_has = ttw_find_function("OakPlayerController", "IsCustomizationUnlocked");
        g_fn_part_add = ttw_find_function("OakPlayerController", "AddUnlockedInventoryCustomizationPart");
        g_fn_part_has = ttw_find_function("OakPlayerController", "IsInventoryCustomizationPartUnlocked");
        g_n_custdata = name_index("GbxCustomizationData");
        g_n_partdata = name_index("OakInventoryCustomizationPartData");
    }
    return g_fn_cust_add && g_fn_cust_has && g_fn_part_add && g_fn_part_has && g_n_custdata >= 0 && ttw_init() == 0;
}
static bool cos_unlocked(uintptr_t o, bool part) {
    struct { uintptr_t obj; uint8_t ret, pad[7]; } p = {o, 0, {0}};
    ttw_process_event(g_pc, part ? g_fn_part_has : g_fn_cust_has, &p);
    return p.ret != 0;
}

int ttw_cos_scan(TtwCosType* out, int max) {
    if (!cos_init()) return -1;
    int n = 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num; i++) {
        uintptr_t o = obj_at(i);
        if (!o || !obj_class(o) || (*(uint32_t*)(o + O_FLAGS) & RF_TEMPLATES) || !is_a(o, g_n_custdata)) continue;
        uintptr_t type = *(uintptr_t*)(o + CUST_TYPE);
        bool part = g_n_partdata >= 0 && is_a(o, g_n_partdata);
        int k = 0;
        while (k < n && out[k].type != type) k++;
        if (k == n) {
            if (n == max) continue;
            out[n].type = type; out[n].total = out[n].unlocked = 0;
            const char* nm = type ? name_str(obj_name(type)) : "Other";
            if (strncmp(nm, "PlayerCustomizationType_", 24) == 0) nm += 24;
            else if (strncmp(nm, "CustomizationType_", 18) == 0) nm += 18;
            int j = 0;
            for (; nm[j] && j < (int)sizeof(out[n].name) - 1; j++) out[n].name[j] = nm[j] == '_' ? ' ' : nm[j];
            out[n].name[j] = 0;
            n++;
        }
        out[k].total++;
        if (cos_unlocked(o, part)) out[k].unlocked++;
    }
    return n;
}

int ttw_cos_unlock(uintptr_t type, int all) {
    if (!cos_init()) return -1;
    int done = 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num; i++) {
        uintptr_t o = obj_at(i);
        if (!o || !obj_class(o) || (*(uint32_t*)(o + O_FLAGS) & RF_TEMPLATES) || !is_a(o, g_n_custdata)) continue;
        if (!all && *(uintptr_t*)(o + CUST_TYPE) != type) continue;
        bool part = g_n_partdata >= 0 && is_a(o, g_n_partdata);
        if (cos_unlocked(o, part)) continue;
        struct { uintptr_t obj; } p = {o};
        ttw_process_event(g_pc, part ? g_fn_part_add : g_fn_cust_add, &p);
        done++;
    }
    return done;
}

#define PBS_EXPERIENCE 0x1C4
#define MAX_LEVEL 40
static const int32_t g_xp_table[MAX_LEVEL] = {0, 388, 1344, 3088, 5824, 9746, 15042, 21892, 30470, 40947, 53492, 68266, 85433, 105149, 127570, 152850, 181140, 212590, 247348, 285561, 327372, 372925, 422362, 475823, 533448, 595376, 661743, 732685, 808338, 888835, 974310, 1064895, 1160721, 1261920, 1368621, 1480953, 1599044, 1723023, 1853015, 1989148};
static uintptr_t g_pbs;

static uintptr_t player_balance(void) {
    if (ttw_init() != 0) return 0;
    if (g_pbs && obj_outer(g_pbs) == g_pawn) return g_pbs;
    static int32_t n_pbs = -1;
    if (n_pbs < 0) n_pbs = name_index("PlayerBalanceStateComponent");
    g_pbs = 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num && n_pbs >= 0; i++) {
        uintptr_t o = obj_at(i);
        if (!o || !obj_class(o) || (*(uint32_t*)(o + O_FLAGS) & RF_TEMPLATES) || obj_outer(o) != g_pawn) continue;
        if (is_a(o, n_pbs)) { g_pbs = o; break; }
    }
    return g_pbs;
}

int ttw_get_xp(int64_t* out) {
    uintptr_t c = player_balance();
    if (!c) return -1;
    *out = *(int32_t*)(c + PBS_EXPERIENCE);
    return 0;
}

int ttw_get_level(int64_t* out) {
    static uintptr_t fn;
    if (!fn) fn = ttw_find_function("BalanceStateComponent", "GetExperienceLevel");
    uintptr_t c = player_balance();
    if (!c || !fn) return -1;
    int32_t lv = 0;
    ttw_process_event(c, fn, &lv);
    *out = lv;
    return 0;
}

int ttw_set_level(int64_t v) {
    static uintptr_t fn;
    if (!fn) fn = ttw_find_function("PlayerBalanceStateComponent", "AddExperience");
    uintptr_t c = player_balance();
    if (!c || !fn || v < 1 || v > MAX_LEVEL) return -1;
    int32_t target = g_xp_table[v - 1];
    for (int tries = 0; tries < 8; tries++) {
        int32_t xp = *(int32_t*)(c + PBS_EXPERIENCE);
        int64_t lv = 0; ttw_get_level(&lv);
        if (lv >= v) return 0;
        struct { int32_t amount; uint8_t source, type, pad[2]; } p = {xp < target ? target - xp : 50, 0, 0, {0}};
        ttw_process_event(c, fn, &p);
        if (*(int32_t*)(c + PBS_EXPERIENCE) == xp) return -2;
    }
    int64_t lv = 0; ttw_get_level(&lv);
    return lv >= v ? 0 : -2;
}

uintptr_t ttw_currency(const char* name) {
    static int32_t n_cat = -1;
    if (n_cat < 0) n_cat = name_index("InventoryCategoryData");
    if (n_cat < 0) return 0;
    ObjArray* a = objarray();
    for (int32_t i = 0; i < a->num; i++) {
        uintptr_t o = obj_at(i);
        if (!o || !obj_class(o) || (*(uint32_t*)(o + O_FLAGS) & RF_TEMPLATES) || !is_a(o, n_cat)) continue;
        if (strstr(name_str(obj_name(o)), name)) return o;
    }
    return 0;
}

int ttw_currency_get(uintptr_t cat, int64_t* out) {
    static uintptr_t fn;
    if (!fn) fn = ttw_find_function("InventoryListComponent", "GetCategoryItemQuantity");
    uintptr_t inv = backpack();
    if (!cat || !inv || !fn) return -1;
    struct { uintptr_t cat; int32_t ret, pad; } p = {cat, 0, 0};
    ttw_process_event(inv, fn, &p);
    *out = p.ret;
    return 0;
}

int ttw_currency_set(uintptr_t cat, int64_t v, int premium) {
    static uintptr_t fn_upd, fn_prem;
    if (!fn_upd) {
        fn_upd = ttw_find_function("InventoryListComponent", "AddOrUpdateCategoryOnly");
        fn_prem = ttw_find_function("OakPlayerController", "ServerUpdatePremiumCurrency");
    }
    uintptr_t inv = backpack();
    int64_t cur;
    if (!cat || !inv || !fn_upd || ttw_currency_get(cat, &cur) != 0) return -1;
    if (v > 2147483647) v = 2147483647;
    if (v < 0) v = 0;
    if (cur == v) return 0;
    struct { uintptr_t cat; int32_t qty, pad; } p = {cat, (int32_t)(v - cur), 0};
    ttw_process_event(inv, fn_upd, &p);
    int64_t now = 0; ttw_currency_get(cat, &now);
    if (now != v) { p.qty = (int32_t)v; ttw_process_event(inv, fn_upd, &p); ttw_currency_get(cat, &now); }
    if (premium && fn_prem && g_pc) {
        struct { uintptr_t cat; int32_t qty, pad; } pp = {cat, (int32_t)v, 0};
        ttw_process_event(g_pc, fn_prem, &pp);
        ttw_currency_get(cat, &now);
    }
    return now == v ? 0 : -2;
}
