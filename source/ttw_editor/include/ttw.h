#pragma once
#include <stdint.h>

void      ttw_set_base(uint64_t base);
int       ttw_init(void);
uintptr_t ttw_hud(void);
uintptr_t ttw_find_object(const char* name);
uintptr_t ttw_find_function(const char* owner, const char* name);
int       ttw_fonts(uintptr_t* out, int max);
void      ttw_process_event(uintptr_t obj, uintptr_t fn, void* parms);
int       ttw_code_level(const char* code);
int       ttw_spawn_code(const char* code, int level);
int       ttw_backpack_count(void);
int         ttw_sdu_find(uintptr_t* out, int max);
const char* ttw_object_name(uintptr_t o);
int         ttw_sdu_level(uintptr_t sdu);
int         ttw_sdu_max(uintptr_t sdu);
int         ttw_sdu_add(uintptr_t sdu);
typedef struct { uintptr_t type; char name[40]; int total, unlocked; } TtwCosType;
int         ttw_cos_scan(TtwCosType* out, int max);
int         ttw_cos_unlock(uintptr_t type, int all);
int         ttw_get_level(int64_t* out);
int         ttw_get_xp(int64_t* out);
int         ttw_set_level(int64_t v);
uintptr_t   ttw_currency(const char* category_name);
int         ttw_currency_get(uintptr_t cat, int64_t* out);
int         ttw_currency_set(uintptr_t cat, int64_t v, int premium);
