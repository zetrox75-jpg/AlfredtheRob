/* rr_game.c - Discover live ROB RUN objects and build render snapshots.
 *
 * The preferred roster comes from the active world dictionary. Bounded
 * instance scans provide a fallback when anchors cannot be resolved. A
 * background worker refreshes the roster and world caches; the sampling path
 * copies published entries under a lock, then reads transforms and rigs.
 *
 * Managed field offsets come from the generated headers. Native transforms,
 * generic wrappers and camera layouts use separately checked assumptions.
 * All cached addresses must be validated after scene or process changes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>

#include "rr_mem.h"
#include "rr_unity.h"
#include "rr_chain.h"
#include "menu_bridge.h"
#include "rr_game.h"
#include "robrun_offsets.h"
#include "robrun_sdk_offsets.h"
#include "rr_cham_model.h"
#include "rr_rig_map.h"

/* Discovery timing log, written beside the overlay. Kept small and only at
 * stage boundaries so a cold start can be timed without guessing. */
static void dlog(const char* fmt, ...) {
    FILE* f = fopen("discovery.log", "a");
    if (!f) return;
    fprintf(f, "%lu ", (unsigned long)GetTickCount());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
#include "rr_pose_gate.h"
#include "rr_cham_coherence.h"
#include "rr_world_marker_select.h"
#include "rr_hiding.h"

/* These fields are read as packed XYZ triples below. A new SDK layout must
 * fail compilation rather than silently keep using a twelve-byte read. */
#if RR_OFF_POS_Y != RR_OFF_POS_X + 4 || RR_OFF_POS_Z != RR_OFF_POS_X + 8 || \
    RR_OFF_INTERACT_POS_Y != RR_OFF_INTERACT_POS_X + 4 || \
    RR_OFF_INTERACT_POS_Z != RR_OFF_INTERACT_POS_X + 8
#error Position fields are no longer contiguous; update the position reader.
#endif

#define RUNTIME_FILE "robrun_runtime.txt"

/* Generic-wrapper offsets (measured, not in the metadata):
 *   PrimitiveData<T>.value +0x18, ReferenceData<T>.value +0x18,
 *   EnumItem.Name string +0x10. */
#define RR_OFF_PRIM_VALUE 0x18
#define RR_OFF_REF_VALUE  0x18

/* Party chain (BattleModel), from the layout dump. Used only to tag the local
 * player's group so the aim can skip them. All reads are read-only. */
#define RR_OFF_BATTLE_PARTY_ACCESS 0x448u /* BattleModel.PartyMemberAccessModel */
#define RR_OFF_PARTY_ACCESS_MODEL  0x18u  /* ClientPartyMemberAccessModel._partyModel */
#define RR_OFF_PARTY_MEMBERS       0x38u  /* PartyModel._actualPartyMemberModels dict */
#define RR_OFF_SLOT_PUBLICID       0x28u  /* BattleCharacterSlotData.PublicId ref */
#define RR_MAX_PARTY               16

/* Knockout (downed) state, verified from a live instance dump:
 * BattleCharacterModel +0x298 KnockoutModel -> +0x18 _knockoutPhase
 * (IPrimitiveData) -> +0x18 KnockoutPhaseDescription, null when not downed. */
#define RR_OFF_CHAR_KNOCKOUT       0x298u
#define RR_OFF_KO_PHASE            0x18u
#define RR_OFF_DESC_NAME  0x10
/* BattleCharacterEntityModel.SimulatedModel: the interpolated character model
 * the renderer draws (Game.Battle.EntityModel.BattleCharacter). */
#define RR_OFF_ENTITY_SIMULATED 0x30
#define TR_READ_LEN  (RR_OFF_CTM_ROLL + 4 - RR_OFF_CTM_X)   /* X..Roll = 24 B */
#define XF_THREADS 8
#define BATCH_MAX 64
static void xf_pool_init(void);
#define MAX_HITS 64
static uint32_t g_saved_pid = 0;
static uint64_t g_saved_world = 0;
static uint64_t g_k_world;
static uint64_t g_world;
static uint64_t g_battle = 0;
static uint64_t g_battle_model = 0;
static uint64_t g_views_updater = 0;
static volatile LONG64 g_time_model;
static volatile LONG64 g_time_world;
static volatile LONG g_tick_period;
/* Number of markers corrected by tick extrapolation in the last snapshot.
 * A successful mesh pose overrides extrapolation, so zero is valid when rigs
 * provide every displayed position.
 */
static volatile LONG g_ex_count;
static int g_pose_clock_changed;
static int g_pose_diag_enabled;
static RR_POSE_DIAG g_pose_diag;
void game_pose_diag_enable(void) { memset(&g_pose_diag,0,sizeof(g_pose_diag)); g_pose_diag_enabled=1; }
RR_POSE_DIAG game_pose_diag_results(void) { return g_pose_diag; }
static uint64_t g_aim_updater = 0;
#define MAX_CANDS 16
static uint64_t g_hits[MAX_HITS];
static volatile LONG g_nhits;
static uint64_t g_cands[MAX_CANDS];
static int g_ncands;
static void collect_hits(const char* name);
static void collect_candidates(const char* name);
static void find_heap_regions(void);
static uint64_t resolve_class(const char* name);
static int class_has_field(uint64_t klass, const char* want);
static void collect_hits_reset(void);
static void collect_hits_clear_hint(void);
static int  g_batch_state = 0;   /* 0 unknown, 1 batch works, -1 unsupported */

#define SCAN_THREADS 8

static uint64_t g_name_lo = 0x13800000000ULL, g_name_hi = 0x13A00000000ULL;
static uint64_t g_inst_lo = 0x13A00000000ULL, g_inst_hi = 0x13E00000000ULL;

static uint64_t g_module = 0;
static uint64_t g_cam_obj = 0;
static uint64_t g_k_cam = 0;
static uint64_t g_k_char = 0;
static uint64_t g_k_entry = 0;
static float    g_fov = 60.0f;
static DWORD    g_last_scan = 0;
static uint64_t g_anchor = 0;       /* first character of the last scan    */
static char     g_status[256] = "idle";

typedef struct { float p[4], q[4], s[4]; } MESH_TRS;
typedef struct {
    uint64_t obj, tr, skel;
    RR_V3 hidden_pos;
    RR_V3 hidden_anchor;
    int hidden_in_object;
    uint64_t mesh_h;
    int      mesh_idx[5];
    uint64_t mesh_native[5];
    int      mesh_first, mesh_span;
    int      cham_idx[RR_CHAM_BONES];
    uint64_t cham_native[RR_CHAM_BONES];
    int      cham_ready;
    DWORD    mesh_view_at;          /* last fully validated view mapping */
    int32_t  id;
    int      armor, crouch, local;
    int      is_party;               /* 1 = in the local player's party/group */
    int      knocked;                /* 1 = downed (KnockoutModel phase set) */
    char     knockphase[24];         /* KnockoutPhase name, empty = not downed */
    int      inveh, vehicle_id;
    float    dist;
    char     role[20];
    char     clan_tag[64];
    char     weapon[20];
    char     veh[20];
    char     name[RR_NAME_MAX];
} ENT_CACHE;
static void cache_cham_rig(ENT_CACHE* e, const ENT_CACHE* previous);

/* Per-character pose cache and its last use time. */
typedef struct {
    uint64_t obj;
    DWORD    t;
    int      ok;
    uint64_t cham_root;
    DWORD    cham_time;
    RR_V3    cham_origin;
    float    cham_skin[RR_CHAM_BONES][12];
    uint64_t mesh_h, mesh_head;
    DWORD    mesh_time;
    int      mesh_valid, mesh_cham_valid, mesh_cham_ready;
    int      mesh_nbones, mesh_full_rig, mesh_group[4];
    float    diag_quat[2][3][4];
    int      diag_quat_count;
    float    diag_rotation_history[3][8][4];
    int      diag_rotation_count;
    float    cham_yaw;
    unsigned cham_hold_since;
    int      cham_hold_pending;
    RR_V3    mesh_origin;
    float    mesh_bone[RR_MAX_BONES][3];
    RR_V3    mesh_feet, mesh_top;
    /* Last published feet/top, kept across frames so a fallback to the
     * replicated transform does not make the box jump between two anchors. */
    RR_V3    pub_pos, pub_top;
    DWORD    pub_time;
    int      pub_valid;
    RR_POSE_GATE arm_gate[2];
    RR_POSE_GATE torso_gate;
    RR_POSE_GATE head_gate;
    MESH_TRS torso_trs[4];
    MESH_TRS head_trs[RR_RIG_L_ARM_0_JNT-RR_RIG_M_HEAD_1_JNT];
    MESH_TRS arm_trs[RR_RIG_R_ARMTWIST_JNT-RR_RIG_L_ARM_0_JNT+1];
} MOTION;
#define MOTION_SLOTS 96
static MOTION g_motion[MOTION_SLOTS];

static MOTION* motion_slot(uint64_t obj) {
    int free_i = -1;
    for (int i = 0; i < MOTION_SLOTS; i++) {
        if (g_motion[i].ok && g_motion[i].obj == obj) return &g_motion[i];
        if (!g_motion[i].ok && free_i < 0) free_i = i;
    }
    if (free_i < 0) {
        free_i = 0;
        for (int i = 1; i < MOTION_SLOTS; i++)
            if (g_motion[i].t < g_motion[free_i].t) free_i = i;
    }
    memset(&g_motion[free_i], 0, sizeof(MOTION));
    g_motion[free_i].obj = obj;
    return &g_motion[free_i];
}

static ENT_CACHE g_cache[RR_MAX_PLAYERS];   /* published snapshot            */
static ENT_CACHE g_stage[RR_MAX_PLAYERS];   /* scan target                   */
#define RR_MAX_VEHICLES 128
#define RR_VEHICLE_POSE_DEPTH 8
typedef struct {
    uint64_t obj, klass, tr, marker_transform, marker_native, marker_h;
    uint64_t render_trs[RR_VEHICLE_POSE_DEPTH];
    int render_depth;
    uint64_t render_transform, render_native, render_hierarchy;
    int render_index;
    DWORD render_view_at;
    int32_t id; int kind, occupants, occupants_known, marker_idx;
    char name[RR_NAME_MAX];
    char role[20];
} VEH_CACHE;
static VEH_CACHE g_vehicles[RR_MAX_VEHICLES], g_vehicles_stage[RR_MAX_VEHICLES];
static int g_nvehicles;
static uint64_t g_vehicle_world;
static unsigned g_vehicle_generation;
#define RR_MAX_LOOT 192
typedef struct {
    int32_t id;
    int kind, rarity;
    RR_V3 pos;
    char name[RR_NAME_MAX];
    /* Addresses retained from the validated loot chain for later write
     * requests. Zero means unavailable. Revalidate before each write because
     * an object can move or be recycled after the cache is published.
     */
    uint64_t obj;        /* LootNodeModel / RandomLootObjectModel            */
    uint64_t pos_addr;   /* the Vector3 position inside its description      */
    uint64_t flag_addr;  /* the BoolData payload: taken (kind 1) / empty (2) */
    /* Retain the resolved description objects for diagnostics so a dump uses
     * the same chain that produced the displayed entry.
     */
    uint64_t desc;       /* LocationLootNodeDescription / RandomLootObject.. */
    uint64_t loot;       /* LootNodeLootDescription / LootObjectItemDesc..   */
    uint64_t inv;        /* InventoryItemDescription                         */
} LOOT_CACHE;
static LOOT_CACHE g_loot[2][RR_MAX_LOOT];
static int g_nloot[2];
static uint64_t g_loot_world[2];
static DWORD g_loot_stamp[2];
#define RR_MAX_WORLD_MARKERS 256
static LOOT_CACHE g_world_markers[RR_MAX_WORLD_MARKERS];
static int g_nworld_markers;
static uint64_t g_markers_world;
static DWORD g_markers_stamp;
static void cache_world_markers(void);
static int       g_ncache = 0;
static int       g_stage_n = 0;
static CRITICAL_SECTION g_cache_cs;
static int       g_cs_ready = 0;
static volatile LONG g_worker_started = 0;

typedef RR_HIDE_OBJECT HIDE_OBJECT;
static HIDE_OBJECT g_hide_objects[RR_MAX_HIDE_OBJECTS];
static RR_HIDE_SNAPSHOT g_hide_snapshot;
static int g_nhide_objects;
static uint64_t g_hide_world;
static DWORD g_hide_stamp;
static void cache_hide_objects(void);

/* Status. */

static void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

void game_status(char* out, size_t n) {
    if (!out || !n) return;
    LONG period = InterlockedCompareExchange(&g_tick_period, 0, 0);
    if (period > 0) snprintf(out, n, "%s tick=%ldms", g_status, period);
    else snprintf(out, n, "%s timing=unavailable", g_status);
}
/* ---------------------------------------------------------------- offsets --- */

/* Read a NUL-terminated C string from the target. Returns its length; zero
 * means the first byte was unreadable or NUL. */
static int rr_cstr(uint64_t addr, char* out, int cap) {
    if (!out || cap <= 1) return 0;
    int i = 0;
    for (; i < cap - 1; ++i) {
        char ch = 0;
        if (!rr_read(addr + (uint64_t)i, &ch, 1)) break;
        out[i] = ch;
        if (!ch) return i;
    }
    out[i] = 0;
    return i;
}

/* Dump Il2CppClass.fields for one class: name and instance offset per field.
 * The fields pointer sits at +0x80 and FieldInfo has a 0x20 stride with the
 * name at +0x00 and the offset at +0x18 for this runtime. The walk stops at the
 * first entry whose name is not a printable string, so a wrong count cannot
 * walk into unrelated memory. */
int game_dump_class(const char* name, FILE* f) {
    if (!f || !name) return 0;
    uint64_t cls = resolve_class(name);
    if (!PL(cls)) {
        fprintf(f, "== %s: NOT FOUND\n", name);
        return 0;
    }
    char cname[160] = {0};
    rr_cstr(rr_u64(cls + U_CLASS_NAME), cname, sizeof(cname));
    uint64_t fields = rr_u64(cls + 0x80);
    uint64_t statics = rr_u64(cls + U_CLASS_STATIC);
    unsigned c118 = rr_u32(cls + 0x118) & 0xFFFFu;
    unsigned c120 = rr_u32(cls + 0x120) & 0xFFFFu;
    fprintf(f, "== %s cls=0x%llX name='%s' statics=0x%llX fields=0x%llX "
               "count@118=%u count@120=%u\n",
            name, (unsigned long long)cls, cname, (unsigned long long)statics,
            (unsigned long long)fields, c118, c120);
    if (!PL(fields)) return 0;

    /* field_count is the low 16 bits at +0x120 on this runtime; +0x118 reads 1
     * (a different field). The printable-name check below is the second guard,
     * because walking past the array runs into the next class's FieldInfo. */
    int max_fields = (int)c120;
    if (max_fields <= 0 || max_fields > 1024) max_fields = 1024;

    int written = 0;
    for (int i = 0; i < max_fields; ++i) {
        uint64_t fi = fields + (uint64_t)i * 0x20;
        uint64_t np = rr_u64(fi);
        if (!PL(np)) break;
        char fn[160] = {0};
        if (rr_cstr(np, fn, sizeof(fn)) <= 0) break;
        unsigned char c0 = (unsigned char)fn[0];
        if (c0 < 0x20 || c0 > 0x7E) break;
        int32_t off = (int32_t)rr_u32(fi + 0x18);
        fprintf(f, "   %-52s 0x%X\n", fn, (unsigned)off);
        ++written;
    }
    fprintf(f, "   (%d fields)\n", written);
    return written;
}

/* Follow a live BattleCharacterModel's knock-related sub-models and dump each
 * one's class fields plus the raw head bytes, so the exact "is this player
 * downed" field can be read off a real instance instead of guessed. */
void game_dump_knockmodels(FILE* f) {
    if (!f) return;
    uint64_t obj = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; ++i)
        if (PL(g_cache[i].obj)) { obj = g_cache[i].obj; break; }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (!PL(obj)) { fprintf(f, "== knockmodels: no live character in cache\n"); return; }
    fprintf(f, "== knockmodels from BattleCharacterModel 0x%llX\n",
            (unsigned long long)obj);
    static const struct { unsigned off; const char* label; } sub[] = {
        { 0x1E0u, "StateModel@0x1E0" },
        { 0x298u, "KnockoutModel@0x298" },
        { 0x2D0u, "KnockdownModel@0x2D0" },
        { 0x2D8u, "KnockdownLeaveStateModel@0x2D8" },
    };
    for (int s = 0; s < (int)(sizeof(sub) / sizeof(sub[0])); ++s) {
        uint64_t m = rr_u64(obj + sub[s].off);
        if (!PL(m)) { fprintf(f, "-- %s: null\n", sub[s].label); continue; }
        uint64_t cls = rr_u64(m);
        char cn[160] = {0};
        if (PL(cls)) rr_cstr(rr_u64(cls + U_CLASS_NAME), cn, sizeof(cn));
        fprintf(f, "-- %s obj=0x%llX class='%s'\n", sub[s].label,
                (unsigned long long)m, cn);
        uint8_t raw[0x40];
        if (rr_read(m, raw, sizeof raw))
            for (int b = 0x10; b < 0x40; b += 8) {
                uint64_t v; memcpy(&v, raw + b, 8);
                fprintf(f, "     +%02X = 0x%llX\n", b, (unsigned long long)v);
            }
        if (PL(cls)) {
            uint64_t fields = rr_u64(cls + 0x80);
            unsigned c120 = rr_u32(cls + 0x120) & 0xFFFFu;
            int max_fields = (int)c120;
            if (max_fields <= 0 || max_fields > 1024) max_fields = 1024;
            int written = 0;
            for (int i = 0; PL(fields) && i < max_fields; ++i) {
                uint64_t fi = fields + (uint64_t)i * 0x20;
                uint64_t np = rr_u64(fi);
                if (!PL(np)) break;
                char fn[160] = {0};
                if (rr_cstr(np, fn, sizeof(fn)) <= 0) break;
                unsigned char c0 = (unsigned char)fn[0];
                if (c0 < 0x20 || c0 > 0x7E) break;
                int32_t off = (int32_t)rr_u32(fi + 0x18);
                fprintf(f, "        %-48s 0x%X\n", fn, (unsigned)off);
                ++written;
            }
            fprintf(f, "     (%d fields)\n", written);
        }
    }
}

/* Scan the target module's writable sections for a pointer equal to value. The
 * static-field global is a .data pointer to the class's static storage, which
 * is exactly what the generated RR_SF_* macro has to encode. */
static int rr_scan_module_ptr(uint64_t value, uint32_t* rva_out) {
    uint64_t base = g_module;
    if (!PL(base) || !PL(value)) return 0;

    uint8_t dos[0x40];
    if (!rr_read(base, dos, sizeof(dos))) return 0;
    int32_t e_lfanew = 0;
    memcpy(&e_lfanew, dos + 0x3C, 4);
    if (e_lfanew <= 0 || e_lfanew > 0x1000) return 0;
    uint32_t sig = 0;
    if (!rr_read(base + (uint64_t)e_lfanew, &sig, 4) || sig != 0x00004550u) return 0;
    uint16_t nsec = 0, optsz = 0;
    rr_read(base + (uint64_t)e_lfanew + 6, &nsec, 2);
    rr_read(base + (uint64_t)e_lfanew + 0x14, &optsz, 2);
    uint64_t sec = base + (uint64_t)e_lfanew + 24 + optsz;

    static uint8_t buf[0x10000];
    for (int i = 0; i < nsec && i < 96; ++i) {
        uint8_t sh[40];
        if (!rr_read(sec + (uint64_t)i * 40, sh, sizeof(sh))) break;
        uint32_t vsize = 0, vaddr = 0, chars = 0;
        memcpy(&vsize, sh + 8, 4);
        memcpy(&vaddr, sh + 12, 4);
        memcpy(&chars, sh + 36, 4);
        if (!(chars & 0x80000000u)) continue;       /* writable sections only */
        if (vsize == 0 || vsize > 0x4000000u) continue;
        for (uint64_t off = 0; off + 8 <= vsize; off += sizeof(buf)) {
            size_t n = (size_t)((vsize - off) < sizeof(buf) ? (vsize - off) : sizeof(buf));
            if (!rr_read(base + vaddr + off, buf, n)) continue;
            for (size_t k = 0; k + 8 <= n; k += 8) {
                uint64_t v = 0;
                memcpy(&v, buf + k, 8);
                if (v == value) {
                    *rva_out = vaddr + (uint32_t)off + (uint32_t)k;
                    return 1;
                }
            }
        }
    }
    return 0;
}

int game_dump_static(const char* name, FILE* f) {
    if (!f || !name) return 0;
    uint64_t cls = resolve_class(name);
    if (!PL(cls)) {
        fprintf(f, "STATIC %s: class not found\n", name);
        return 0;
    }
    uint64_t statics = rr_u64(cls + U_CLASS_STATIC);
    if (!PL(statics)) {
        fprintf(f, "STATIC %s: no static storage\n", name);
        return 0;
    }
    uint32_t rva = 0;
    if (rr_scan_module_ptr(statics, &rva)) {
        fprintf(f, "STATIC %s rva=0x%X statics=0x%llX\n", name, rva,
                (unsigned long long)statics);
        return 1;
    }
    fprintf(f, "STATIC %s: no .data pointer found (statics=0x%llX)\n", name,
            (unsigned long long)statics);
    return 0;
}

int  game_instance_mode(void) { return 1; }
uint64_t game_world_addr(void) { return g_world; }
int  game_prim_offset(void) { return 0; }
int  game_dict_offset(void) { return 0x10; }
uint64_t game_lens_debug(void) { return g_aim_updater; }

/* Runtime file. */

static void runtime_write_body(FILE* f) {
    fprintf(f, "# written by RobRunESP; delete to force a rescan\n");
    fprintf(f, "class_camera=0x%llX\n", (unsigned long long)g_k_cam);
    fprintf(f, "class_char=0x%llX\n", (unsigned long long)g_k_char);
    fprintf(f, "class_world=0x%llX\n", (unsigned long long)g_k_world);
    fprintf(f, "pid=%u\n", (unsigned)g_rr_pid);
    fprintf(f, "world=0x%llX\n", (unsigned long long)g_world);
    fprintf(f, "camera=0x%llX\n", (unsigned long long)g_cam_obj);
    fprintf(f, "fov_x100=%d\n", (int)(g_fov * 100.0f));
    fprintf(f, "scan_name_lo=0x%llX\n", (unsigned long long)g_name_lo);
    fprintf(f, "scan_name_hi=0x%llX\n", (unsigned long long)g_name_hi);
    fprintf(f, "scan_inst_lo=0x%llX\n", (unsigned long long)g_inst_lo);
    fprintf(f, "scan_inst_hi=0x%llX\n", (unsigned long long)g_inst_hi);
}

/* A stable cache location: a masqueraded copy runs from a temp folder, so the
 * cache beside the exe is lost on every launch and each run pays a cold class
 * scan. Mirroring it under LOCALAPPDATA makes the cache survive. */
static void runtime_alt_path(char* out, size_t n) {
    const char* local = getenv("LOCALAPPDATA");
    out[0] = 0;
    if (!local || !local[0] || n < 32) return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s\\RobRun", local);
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s\\robrun_runtime.txt", dir);
}

static void runtime_save(void) {
    /* Do not replace a usable runtime cache with a failed discovery result;
     * otherwise the next launch can inherit invalid anchors and narrowed ranges.
     */
    if (!PL(g_world) || !PL(g_k_world)) return;

    FILE* f = fopen(RUNTIME_FILE, "w");
    if (f) { runtime_write_body(f); fclose(f); }

    char alt[512];
    runtime_alt_path(alt, sizeof(alt));
    if (alt[0]) {
        f = fopen(alt, "w");
        if (f) { runtime_write_body(f); fclose(f); }
    }
}

static void runtime_load(void) {
    FILE* f = fopen(RUNTIME_FILE, "r");
    char alt[512] = {0};
    if (!f) {
        runtime_alt_path(alt, sizeof(alt));
        if (alt[0]) f = fopen(alt, "r");
    }
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line;
        const char* v = eq + 1;
        if (!strcmp(k, "class_camera")) g_k_cam = strtoull(v, NULL, 0);
        else if (!strcmp(k, "class_char")) g_k_char = strtoull(v, NULL, 0);
        else if (!strcmp(k, "class_world")) g_k_world = strtoull(v, NULL, 0);
        else if (!strcmp(k, "pid")) g_saved_pid = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(k, "world")) g_saved_world = strtoull(v, NULL, 0);
        else if (!strcmp(k, "camera")) g_cam_obj = strtoull(v, NULL, 0);
        else if (!strcmp(k, "fov_x100")) { int _i = atoi(v); g_fov = (float)_i / 100.0f; }
        else if (!strcmp(k, "scan_name_lo")) g_name_lo = strtoull(v, NULL, 0);
        else if (!strcmp(k, "scan_name_hi")) g_name_hi = strtoull(v, NULL, 0);
        else if (!strcmp(k, "scan_inst_lo")) g_inst_lo = strtoull(v, NULL, 0);
        else if (!strcmp(k, "scan_inst_hi")) g_inst_hi = strtoull(v, NULL, 0);
    }
    fclose(f);
    if (g_fov < 20.0f || g_fov > 140.0f) g_fov = 60.0f;

    /* If the cached scan found no world, keep candidate classes but discard
     * the failed address-range narrowing so the next scan can search broadly.
     */
    if (!PL(g_k_world) && !PL(g_saved_world)) {
        g_k_world = 0;
        g_saved_world = 0;
        g_saved_pid = 0;
        g_name_lo = g_name_hi = 0;
        g_inst_lo = g_inst_hi = 0;
    }
}

/* Validation. */

static int class_named(uint64_t k, const char* want) {
    if (!PL(k)) return 0;
    char nm[128];
    if (!u_class_name(k, nm, sizeof(nm))) return 0;
    return !_stricmp(nm, want);
}

static int camera_validate(uint64_t c) {
    if (!PL(c) || !class_named(u_klass(c), "BattleCameraModel")) return 0;
    RR_V3 fwd, pos;
    if (!rr_read(c + RR_OFF_CAM_FWD, &fwd, sizeof(fwd))) return 0;
    if (!rr_read(c + RR_OFF_CAM_POS, &pos, sizeof(pos))) return 0;
    float len = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (!(len > 0.9f && len < 1.1f)) return 0;
    if (!isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z)) return 0;
    return 1;
}

/* 1 = plausible, 0 = stale/invalid, -1 = local player unavailable. */
static int camera_near_local(uint64_t c) {
    if (!camera_validate(c)) return 0;
    uint64_t local_tr = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; ++i) {
        if (g_cache[i].local) { local_tr = g_cache[i].tr; break; }
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    float lp[3], cp[3];
    if (!PL(local_tr) || !rr_read(local_tr + RR_OFF_CTM_X, lp, sizeof(lp))) return -1;
    if (!rr_read(c + RR_OFF_CAM_POS, cp, sizeof(cp))) return 0;
    float x = cp[0]-lp[0], y = cp[1]-lp[1], z = cp[2]-lp[2];
    return x*x + y*y + z*z < 30.0f*30.0f;
}

/* Class discovery. */

static int ascii_at(uint64_t ptr, char* out, size_t n) {
    out[0] = 0;
    if (!PL(ptr)) return 0;
    static const size_t tries[] = { 96, 64, 32, 16, 8 };
    char tmp[96];
    int ok = 0;
    for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++)
        if (rr_read(ptr, tmp, tries[t])) { ok = 1; break; }
    if (!ok) return 0;
    size_t i = 0;
    while (i + 1 < sizeof(tmp) && i + 1 < n && tmp[i]) {
        unsigned char c = (unsigned char)tmp[i];
        if (c < 32 || c > 126) return 0;
        out[i++] = tmp[i];
    }
    out[i] = 0;
    return i > 0;
}

/* UnityPlayer's GOM and native component layout are build-specific. Treat the
 * dump as a hint: bound every walk and verify all managed types and links. */
#define RR_UNITY_GOM_RVA 0x2230768u
typedef struct { uint64_t world, battle, camera, battle_model; } ENTRY_ROOTS;

static uint64_t entry_component(uint64_t object) {
    uint64_t array = rr_u64(object + 0x20);
    int count = rr_i32(object + 0x30);
    if (!PL(array) || count < 1 || count > 256) return 0;
    for (int i = 0; i < count; ++i) {
        uint64_t native = rr_u64(array + (uint64_t)i * 0x10 + 8);
        if (!PL(native)) continue;
        uint64_t handle = rr_u64(native + 0x18) & ~7ULL;
        uint64_t managed = PL(handle) ? rr_u64(handle) : 0;
        if (u_is(managed, "UnityEntry")) return managed;
        managed = rr_u64(native + 0x28);
        if (u_is(managed, "UnityEntry")) return managed;
    }
    return 0;
}

/* Cached GameObjectManager pointer and its dynamic discovery. */
static uint64_t g_gom = 0;

static int gom_node_named(uint64_t gom, unsigned off) {
    uint64_t node = rr_u64(gom + off);
    uint64_t end = rr_u64(gom + off - 8);
    for (int n = 0; n < 4 && PL(node) && node != end; ++n) {
        uint64_t object = rr_u64(node + 0x10);
        char name[64] = {0};
        if (PL(object) && ascii_at(rr_u64(object + 0x50), name, sizeof(name))) return 1;
        uint64_t next = rr_u64(node + 8);
        if (!PL(next) || next == node) break;
        node = next;
    }
    return 0;
}

static int gom_lists_ok(uint64_t gom) {
    return gom_node_named(gom, 0x18) || gom_node_named(gom, 0x28) || gom_node_named(gom, 0x08);
}

static uint64_t find_gom_dynamic(uint64_t module) {
    uint8_t dos[0x40];
    if (!rr_read(module, dos, sizeof(dos))) return 0;
    int32_t e = 0;
    memcpy(&e, dos + 0x3C, 4);
    if (e <= 0 || e > 0x1000) return 0;
    uint16_t nsec = 0, opt = 0;
    rr_read(module + (uint64_t)e + 6, &nsec, 2);
    rr_read(module + (uint64_t)e + 0x14, &opt, 2);
    uint64_t sec = module + (uint64_t)e + 24 + opt;
    static uint8_t buf[0x10000];
    for (int i = 0; i < nsec && i < 96; ++i) {
        uint8_t sh[40];
        if (!rr_read(sec + (uint64_t)i * 40, sh, sizeof(sh))) break;
        uint32_t vsize = 0, vaddr = 0, chars = 0;
        memcpy(&vsize, sh + 8, 4);
        memcpy(&vaddr, sh + 12, 4);
        memcpy(&chars, sh + 36, 4);
        if (!(chars & 0x80000000u)) continue;          /* writable only */
        if (vsize == 0 || vsize > 0x2000000u) continue;
        for (uint64_t off = 0; off + 8 <= vsize; off += sizeof(buf)) {
            size_t n = (size_t)((vsize - off) < sizeof(buf) ? (vsize - off) : sizeof(buf));
            if (!rr_read(module + vaddr + off, buf, n)) continue;
            for (size_t k = 0; k + 8 <= n; k += 8) {
                uint64_t v = 0;
                memcpy(&v, buf + k, 8);
                if (!PL(v) || (v & 7)) continue;
                if (gom_lists_ok(v)) return v;
            }
        }
    }
    return 0;
}

static int entry_roots_gom(ENTRY_ROOTS* roots) {
    uint64_t module = 0; uint32_t size = 0;
    memset(roots, 0, sizeof(*roots));
    if (!rr_module("UnityPlayer.dll", &module, &size)) return 0;

    /* The GameObjectManager global moved with a UnityPlayer update, so the
     * hardcoded RVA can point at unrelated data and the walk silently fails
     * (which sends discovery down the slow heap-scan path). Locate the GOM by
     * validating candidates in UnityPlayer's writable sections instead, and
     * cache the result. */
    if (!PL(g_gom)) {
        uint64_t fixed = (size > RR_UNITY_GOM_RVA + 8) ? rr_u64(module + RR_UNITY_GOM_RVA) : 0;
        if (PL(fixed) && gom_lists_ok(fixed)) g_gom = fixed;
        else g_gom = find_gom_dynamic(module);
    }
    uint64_t gom = g_gom;
    if (!PL(gom)) return 0;
    const unsigned lists[] = { 0x18, 0x28, 0x08 };
    for (unsigned l = 0; l < sizeof(lists)/sizeof(lists[0]); ++l) {
        uint64_t node = rr_u64(gom + lists[l]);
        uint64_t end = rr_u64(gom + lists[l] - 8);
        for (int n = 0; PL(node) && node != end && n < 64; ++n) {
            uint64_t object = rr_u64(node + 0x10);
            char name[64];
            if (PL(object) && ascii_at(rr_u64(object + 0x50), name, sizeof(name)) &&
                !strcmp(name, "UnityEntry")) {
                uint64_t entry = entry_component(object);
                uint64_t game = PL(entry) ? rr_u64(entry + RR_OFF_ENTRY_GAME) : 0;
                uint64_t model = u_is(game, "Game") ? rr_u64(game + RR_OFF_GAME_MODEL) : 0;
                uint64_t battle_model = u_is(model, "GameModel") ? rr_u64(model + RR_OFF_GAME_BATTLE) : 0;
                if (!u_is(battle_model, "BattleModel")) return 0;
                uint64_t battle = rr_u64(battle_model + RR_OFF_BATTLE_ENTITIES);
                uint64_t world = u_is(battle, "BattleEntitiesModel") ? rr_u64(battle + RR_OFF_BATTLE_WORLD) : 0;
                if (!u_is(world, "WorldEntitiesModel")) return 0;
                roots->world = world;
                roots->battle = battle;
                roots->battle_model = battle_model;
                uint64_t camera = rr_u64(battle_model + RR_OFF_BATTLE_CAMERA);
                if (camera_validate(camera)) roots->camera = camera;
                return 1;
            }
            uint64_t next = rr_u64(node + 8);
            if (next == node) break;
            node = next;
        }
    }
    return 0;
}

/* Diagnostic: dump the area around the fixed GameObjectManager RVA and probe
 * whether nearby pointers look like GOM list nodes (managed class GameObject). */
void game_gom_list_diag(FILE* f) {
    if (!f) return;
    uint64_t up = 0; uint32_t us = 0;
    if (!rr_module("UnityPlayer.dll", &up, &us)) { fprintf(f, "no UnityPlayer\n"); return; }
    static const unsigned rvas[2] = { 0x22309A0u, 0x22309A8u };
    for (int r = 0; r < 2; ++r) {
        uint64_t g = rr_u64(up + rvas[r]);
        fprintf(f, "manager[0x%X]=0x%llX\n", rvas[r], (unsigned long long)g);
        if (!PL(g)) continue;
        uint8_t hdr[0x60];
        if (!rr_read(g, hdr, sizeof(hdr))) { fprintf(f, " manager read failed\n"); continue; }
        for (int o = 0; o < 0x60; o += 8) {
            uint64_t v = 0;
            memcpy(&v, hdr + o, 8);
            fprintf(f, " m[%02X]=0x%llX", o, (unsigned long long)v);
            if (PL(v) && (v & 7) == 0) {
                char cn[96] = {0};
                uint64_t cls = rr_u64(v);
                if (PL(cls)) u_class_name(cls, cn, sizeof(cn));
                fprintf(f, " class='%s'", cn);
            }
            fputc('\n', f);
        }
        uint64_t data = rr_u64(g + 0x18);
        fprintf(f, " data=0x%llX\n", (unsigned long long)data);
        if (!PL(data)) continue;
        for (int i = 0; i < 8; ++i) {
            uint64_t obj = rr_u64(data + (uint64_t)i * 8);
            char cn[96] = {0}, nm[64] = {0};
            uint64_t cls = PL(obj) ? rr_u64(obj) : 0;
            if (PL(cls)) u_class_name(cls, cn, sizeof(cn));
            if (PL(obj)) ascii_at(rr_u64(obj + 0x50), nm, sizeof(nm));
            fprintf(f, "  elem[%d]=0x%llX class='%s' name='%s'\n", i,
                    (unsigned long long)obj, cn, nm);
        }
    }
}

/* The root at UnityPlayer+0x2230768 is a hash table from a type key to a
 * circular list of instances: [root] = entries, [root+8] = mask, entry stride
 * 0x18 with key at +8 and list sentinel at +0x10; a node has next at +8 and the
 * object at +0x10. Enumerating it finds a class instance without the
 * GameObjectManager and without calling engine code. */
static uint64_t typeinfo_find(const char* want, FILE* f) {
    uint64_t up = 0; uint32_t us = 0;
    if (!rr_module("UnityPlayer.dll", &up, &us)) return 0;
    uint64_t root = rr_u64(up + 0x2230768ULL);
    if (!PL(root)) return 0;
    uint64_t entries = rr_u64(root);
    uint32_t mask = rr_u32(root + 8);
    if (f) fprintf(f, "typeinfo root=0x%llX entries=0x%llX mask=0x%X\n",
                   (unsigned long long)root, (unsigned long long)entries, mask);
    if (!PL(entries) || mask == 0 || mask > 0x40000) return 0;
    uint64_t cap = (uint64_t)mask + 1;
    uint64_t bytes = cap * 0x18ULL;
    static uint8_t buf[0x10000];
    int checked = 0;
    for (uint64_t off = 0; off < bytes; off += sizeof(buf)) {
        size_t n = (size_t)((bytes - off) < sizeof(buf) ? (bytes - off) : sizeof(buf));
        n -= n % 0x18;
        if (n < 0x18) break;
        if (!rr_read(entries + off, buf, n)) continue;
        for (size_t i = 0; i + 0x18 <= n; i += 0x18) {
            uint32_t key = 0;
            memcpy(&key, buf + i + 8, 4);
            if (key == 0xFFFFFFFFu) continue;
            uint64_t value = 0;
            memcpy(&value, buf + i + 0x10, 8);
            if (!PL(value)) continue;
            uint64_t node = rr_u64(value + 8);
            if (!PL(node) || node == value) continue;
            for (int step = 0; step < 64 && PL(node) && node != value; ++step) {
                uint64_t payload = rr_u64(node + 0x10);
                if (PL(payload)) {
                    uint64_t cls = rr_u64(payload);
                    char cn[96] = {0};
                    if (PL(cls)) u_class_name(cls, cn, sizeof(cn));
                    if (f && checked <= 40) {
                        char nm[64] = {0};
                        ascii_at(rr_u64(payload + 0x50), nm, sizeof(nm));
                        fprintf(f, "  entry key=%u payload=0x%llX class='%s' name='%s'\n",
                                key, (unsigned long long)payload, cn, nm);
                    }
                    ++checked;
                    if (PL(cls) && class_named(cls, want)) {
                        if (f) fprintf(f, "typeinfo hit key=%u sentinel=0x%llX object=0x%llX\n",
                                       key, (unsigned long long)value,
                                       (unsigned long long)payload);
                        return payload;
                    }
                }
                uint64_t next = rr_u64(node + 8);
                if (!PL(next) || next == node) break;
                node = next;
            }
        }
    }
    if (f) fprintf(f, "typeinfo no hit for '%s' (%d payloads checked)\n", want, checked);
    return 0;
}

static uint64_t typeinfo_find_named(const char* want, FILE* f) {
    uint64_t up = 0; uint32_t us = 0;
    if (!rr_module("UnityPlayer.dll", &up, &us)) return 0;
    uint64_t root = rr_u64(up + 0x2230768ULL);
    if (!PL(root)) return 0;
    uint64_t entries = rr_u64(root);
    uint32_t mask = rr_u32(root + 8);
    if (!PL(entries) || mask == 0 || mask > 0x40000) return 0;
    uint64_t cap = (uint64_t)mask + 1;
    uint64_t bytes = cap * 0x18ULL;
    static uint8_t buf[0x10000];
    for (uint64_t off = 0; off < bytes; off += sizeof(buf)) {
        size_t n = (size_t)((bytes - off) < sizeof(buf) ? (bytes - off) : sizeof(buf));
        n -= n % 0x18;
        if (n < 0x18) break;
        if (!rr_read(entries + off, buf, n)) continue;
        for (size_t i = 0; i + 0x18 <= n; i += 0x18) {
            uint32_t key = 0;
            memcpy(&key, buf + i + 8, 4);
            if (key == 0xFFFFFFFFu) continue;
            uint64_t value = 0;
            memcpy(&value, buf + i + 0x10, 8);
            if (!PL(value)) continue;
            uint64_t node = rr_u64(value + 8);
            for (int step = 0; step < 64 && PL(node) && node != value; ++step) {
                uint64_t payload = rr_u64(node + 0x10);
                if (PL(payload)) {
                    char nm[64] = {0};
                    if (ascii_at(rr_u64(payload + 0x50), nm, sizeof(nm)) &&
                        strstr(nm, want)) {
                        if (f) fprintf(f, "named hit key=%u payload=0x%llX name='%s'\n",
                                       key, (unsigned long long)payload, nm);
                        return payload;
                    }
                }
                uint64_t next = rr_u64(node + 8);
                if (!PL(next) || next == node) break;
                node = next;
            }
        }
    }
    return 0;
}

void game_objlist_diag(FILE* f) {
    if (!f) return;
    uint64_t up = 0; uint32_t us = 0;
    if (!rr_module("UnityPlayer.dll", &up, &us)) { fprintf(f, "no UnityPlayer\n"); return; }
    uint64_t root = rr_u64(up + 0x2230768ULL);
    if (!PL(root)) { fprintf(f, "no root\n"); return; }
    uint64_t entries = rr_u64(root);
    uint32_t mask = rr_u32(root + 8);
    fprintf(f, "root=0x%llX entries=0x%llX mask=0x%X\n",
            (unsigned long long)root, (unsigned long long)entries, mask);
    if (!PL(entries) || mask == 0 || mask > 0x40000) return;
    uint64_t cap = (uint64_t)mask + 1;
    uint64_t bytes = cap * 0x18ULL;
    static uint8_t buf[0x10000];
    int lines = 0;
    for (uint64_t off = 0; off < bytes && lines < 20000; off += sizeof(buf)) {
        size_t n = (size_t)((bytes - off) < sizeof(buf) ? (bytes - off) : sizeof(buf));
        n -= n % 0x18;
        if (n < 0x18) break;
        if (!rr_read(entries + off, buf, n)) continue;
        for (size_t i = 0; i + 0x18 <= n && lines < 20000; i += 0x18) {
            uint32_t key = 0;
            memcpy(&key, buf + i + 8, 4);
            if (key == 0xFFFFFFFFu) continue;
            uint64_t value = 0;
            memcpy(&value, buf + i + 0x10, 8);
            if (!PL(value)) continue;
            uint64_t node = rr_u64(value + 8);
            for (int step = 0; step < 64 && PL(node) && node != value && lines < 20000; ++step) {
                uint64_t payload = rr_u64(node + 0x10);
                if (PL(payload)) {
                    char nm[64] = {0}, cn[96] = {0};
                    ascii_at(rr_u64(payload + 0x50), nm, sizeof(nm));
                    uint64_t cls = rr_u64(payload);
                    if (PL(cls)) u_class_name(cls, cn, sizeof(cn));
                    fprintf(f, "key=%u obj=0x%llX class='%s' name='%s'\n",
                            key, (unsigned long long)payload, cn, nm);
                    ++lines;
                }
                uint64_t next = rr_u64(node + 8);
                if (!PL(next) || next == node) break;
                node = next;
            }
        }
    }
    fprintf(f, "total %d\n", lines);
}

static uint64_t resolve_named_class(const char* name, const char* field);
static uint64_t find_instance_of(uint64_t klass, unsigned want_off,
                                 const char* want_field_class);
static uint64_t gom_entry(void);

void game_chain_diag(FILE* f) {
    if (!f) return;
    uint64_t up = 0; uint32_t us = 0;
    if (!rr_module("UnityPlayer.dll", &up, &us)) { fprintf(f, "no UnityPlayer\n"); return; }
    find_heap_regions();
    fprintf(f, "regions enumerated\n");
    uint64_t root = rr_u64(up + 0x2230768ULL);
    fprintf(f, "root=0x%llX\n", (unsigned long long)root);
    fprintf(f, "gom_entry=0x%llX\n", (unsigned long long)gom_entry());
    if (!PL(root)) return;
    uint64_t ue = typeinfo_find("UnityEntry", f);
    if (!PL(ue)) {
        uint64_t named = typeinfo_find_named("UnityEntry", f);
        if (PL(named)) fprintf(f, "named UnityEntry object=0x%llX\n",
                               (unsigned long long)named);
        uint64_t klass = resolve_named_class("UnityEntry", "_game");
        fprintf(f, "resolved UnityEntry class=0x%llX\n", (unsigned long long)klass);
        if (PL(klass)) {
            uint64_t inst = find_instance_of(klass, 0x28, "Game");
            fprintf(f, "scanned UnityEntry instance=0x%llX\n", (unsigned long long)inst);
            if (PL(inst)) {
                uint64_t game = rr_u64(inst + 0x28);
                uint64_t model = 0;
                for (int offi = 0; offi < 2 && !PL(model); ++offi) {
                    unsigned off = offi ? 0xD8u : 0xD0u;
                    uint64_t cand = PL(game) ? rr_u64(game + off) : 0;
                    if (PL(cand) && class_named(rr_u64(cand), "GameModel")) model = cand;
                }
                uint64_t bm = PL(model) ? rr_u64(model + 0x10ULL) : 0;
                uint64_t be = PL(bm) ? rr_u64(bm + 0x90ULL) : 0;
                uint64_t w1 = PL(be) ? rr_u64(be + 0x60ULL) : 0;
                uint64_t w2 = PL(bm) ? rr_u64(bm + RR_OFF_BATTLE_WORLD_B) : 0;
                char cm[128] = {0}, cb[128] = {0}, ce[128] = {0};
                char cw1[128] = {0}, cw2[128] = {0};
                if (PL(model)) u_class_name(rr_u64(model), cm, sizeof(cm));
                if (PL(bm)) u_class_name(rr_u64(bm), cb, sizeof(cb));
                if (PL(be)) u_class_name(rr_u64(be), ce, sizeof(ce));
                if (PL(w1)) u_class_name(rr_u64(w1), cw1, sizeof(cw1));
                if (PL(w2)) u_class_name(rr_u64(w2), cw2, sizeof(cw2));
                fprintf(f, "class chain game=0x%llX model=0x%llX '%s' bm=0x%llX '%s' "
                           "be=0x%llX '%s' w60=0x%llX '%s' w510=0x%llX '%s'\n",
                        (unsigned long long)game, (unsigned long long)model, cm,
                        (unsigned long long)bm, cb, (unsigned long long)be, ce,
                        (unsigned long long)w1, cw1, (unsigned long long)w2, cw2);
                if (PL(w1) && class_named(rr_u64(w1), "WorldEntitiesModel"))
                    fprintf(f, "CHAIN OK world=0x%llX (be+0x60)\n", (unsigned long long)w1);
                else if (PL(w2) && class_named(rr_u64(w2), "WorldEntitiesModel"))
                    fprintf(f, "CHAIN OK world=0x%llX (bm+0x518)\n", (unsigned long long)w2);
            }
        }
    }
    if (PL(ue)) {
        uint64_t game = rr_u64(ue + RR_OFF_ENTRY_GAME);
        uint64_t model = PL(game) ? rr_u64(game + RR_OFF_GAME_MODEL) : 0;
        uint64_t bm = PL(model) ? rr_u64(model + RR_OFF_GAME_BATTLE) : 0;
        uint64_t be = PL(bm) ? rr_u64(bm + RR_OFF_BATTLE_ENTITIES) : 0;
        uint64_t w1 = PL(be) ? rr_u64(be + RR_OFF_BATTLE_WORLD) : 0;
        uint64_t w2 = PL(bm) ? rr_u64(bm + RR_OFF_BATTLE_WORLD_B) : 0;
        char cm[128] = {0}, cb[128] = {0}, ce[128] = {0}, cw1[128] = {0}, cw2[128] = {0};
        if (PL(model)) u_class_name(rr_u64(model), cm, sizeof(cm));
        if (PL(bm)) u_class_name(rr_u64(bm), cb, sizeof(cb));
        if (PL(be)) u_class_name(rr_u64(be), ce, sizeof(ce));
        if (PL(w1)) u_class_name(rr_u64(w1), cw1, sizeof(cw1));
        if (PL(w2)) u_class_name(rr_u64(w2), cw2, sizeof(cw2));
        fprintf(f, "chain ue=0x%llX game=0x%llX model=0x%llX '%s' bm=0x%llX '%s' "
                   "be=0x%llX '%s' w60=0x%llX '%s' w510=0x%llX '%s'\n",
                (unsigned long long)ue, (unsigned long long)game,
                (unsigned long long)model, cm, (unsigned long long)bm, cb,
                (unsigned long long)be, ce, (unsigned long long)w1, cw1,
                (unsigned long long)w2, cw2);
    }
    uint64_t cand[2] = { rr_u64(root + 0xD0ULL), rr_u64(root + 0xD8ULL) };
    for (int i = 0; i < 2; ++i) {
        uint64_t g = cand[i];
        char nm[128] = {0};
        if (PL(g)) u_class_name(rr_u64(g), nm, sizeof(nm));
        fprintf(f, "cand%d=0x%llX class='%s'\n", i, (unsigned long long)g, nm);
        if (!PL(g)) continue;
        uint64_t model = 0;
        if (!strcmp(nm, "Game")) model = rr_u64(g + 0xD8ULL);
        else model = g;
        char mm[128] = {0};
        if (PL(model)) u_class_name(rr_u64(model), mm, sizeof(mm));
        fprintf(f, "  model=0x%llX '%s'\n", (unsigned long long)model, mm);
        if (!PL(model)) continue;
        uint64_t battle = rr_u64(model + 0x10ULL);
        char bm[128] = {0};
        if (PL(battle)) u_class_name(rr_u64(battle), bm, sizeof(bm));
        uint64_t be = PL(battle) ? rr_u64(battle + 0x90ULL) : 0;
        char ben[128] = {0};
        if (PL(be)) u_class_name(rr_u64(be), ben, sizeof(ben));
        uint64_t w1 = PL(be) ? rr_u64(be + 0x60ULL) : 0;
        uint64_t w2 = PL(battle) ? rr_u64(battle + RR_OFF_BATTLE_WORLD_B) : 0;
        char nw1[128] = {0}, nw2[128] = {0};
        if (PL(w1)) u_class_name(rr_u64(w1), nw1, sizeof(nw1));
        if (PL(w2)) u_class_name(rr_u64(w2), nw2, sizeof(nw2));
        fprintf(f, "  battle=0x%llX '%s' be=0x%llX '%s' w60=0x%llX '%s' w510=0x%llX '%s'\n",
                (unsigned long long)battle, bm, (unsigned long long)be, ben,
                (unsigned long long)w1, nw1, (unsigned long long)w2, nw2);
    }
}

void game_gom_diag(FILE* f) {
    if (!f) return;
    uint64_t module = 0; uint32_t size = 0;
    if (!rr_module("UnityPlayer.dll", &module, &size)) { fprintf(f, "no UnityPlayer\n"); return; }
    fprintf(f, "module=0x%llX size=0x%X fixed_rva=0x%X\n",
            (unsigned long long)module, size, RR_UNITY_GOM_RVA);
    uint64_t center = module + RR_UNITY_GOM_RVA;
    uint64_t base = center - 0x100;
    uint8_t raw[0x200];
    if (!rr_read(base, raw, sizeof(raw))) { fprintf(f, "read failed\n"); return; }
    for (int i = 0; i < (int)(0x200 / 8); ++i) {
        uint64_t v = 0;
        memcpy(&v, raw + i * 8, 8);
        uint64_t addr = base + (uint64_t)i * 8;
        fprintf(f, "[%+04llX] 0x%016llX", (long long)(addr - center), (unsigned long long)v);
        if (PL(v) && (v & 7) == 0) {
            uint64_t obj = rr_u64(v + 0x10);
            char nm[96] = {0};
            uint64_t cls = PL(obj) ? rr_u64(obj) : 0;
            if (PL(cls)) u_class_name(cls, nm, sizeof(nm));
            fprintf(f, "  obj=0x%llX class='%s'", (unsigned long long)obj, nm);
            if (nm[0] && !strcmp(nm, "GameObject")) {
                for (int off = 0x48; off <= 0x60; off += 8) {
                    uint64_t s = rr_u64(obj + off);
                    char txt[64] = {0};
                    if (PL(s) && u_string(s, txt, sizeof(txt)) > 0)
                        fprintf(f, " name@%X='%s'", off, txt);
                }
            }
        }
        fputc('\n', f);
    }

    /* The .data global at the fixed RVA may still point at the manager even if
     * the RVA shifted: dump the pointed-to structure and probe each qword as a
     * possible intrusive-list node. */
    uint64_t gom = rr_u64(module + RR_UNITY_GOM_RVA);
    fprintf(f, "gom_ptr=0x%llX\n", (unsigned long long)gom);
    if (PL(gom)) {
        uint8_t g[0x100];
        if (rr_read(gom, g, sizeof(g))) {
            for (int i = 0; i < (int)(sizeof(g) / 8); ++i) {
                uint64_t v = 0;
                memcpy(&v, g + i * 8, 8);
                fprintf(f, "  gom[%+03X] 0x%016llX", i * 8, (unsigned long long)v);
                if (PL(v) && (v & 7) == 0) {
                    uint64_t obj = rr_u64(v + 0x10);
                    char nm[96] = {0};
                    uint64_t cls = PL(obj) ? rr_u64(obj) : 0;
                    if (PL(cls)) u_class_name(cls, nm, sizeof(nm));
                    fprintf(f, "  node_obj=0x%llX class='%s'",
                            (unsigned long long)obj, nm);
                    if (nm[0] && !strcmp(nm, "GameObject")) {
                        for (int off = 0x40; off <= 0x68; off += 8) {
                            uint64_t s = rr_u64(obj + off);
                            char txt[64] = {0};
                            if (PL(s) && u_string(s, txt, sizeof(txt)) > 0)
                                fprintf(f, " name@%X='%s'", off, txt);
                        }
                    }
                }
                fputc('\n', f);
            }
        }

        uint64_t nodes[4] = { rr_u64(gom + 0x18), rr_u64(gom + 0x20),
                              rr_u64(gom + 0x30), rr_u64(gom + 0x60) };
        for (int ni = 0; ni < 4; ++ni) {
            uint64_t nd = nodes[ni];
            if (!PL(nd)) continue;
            uint8_t nb[0x40];
            if (!rr_read(nd, nb, sizeof(nb))) continue;
            fprintf(f, "node[0x%llX]:\n", (unsigned long long)nd);
            for (int i = 0; i < 8; ++i) {
                uint64_t w = 0;
                memcpy(&w, nb + i * 8, 8);
                char cn[96] = {0};
                if (PL(w)) {
                    uint64_t cls = rr_u64(w);
                    if (PL(cls)) u_class_name(cls, cn, sizeof(cn));
                }
                fprintf(f, "   [%02X] 0x%016llX class='%s'\n", i * 8,
                        (unsigned long long)w, cn);
            }
        }

        /* Validate statically-ranked writable globals as GameObject managers: a
         * candidate pointer whose structure holds a list whose node points at an
         * object of managed class GameObject. */
        FILE* cands = fopen("gomcands.txt", "r");
        if (cands) {
            char line[64];
            while (fgets(line, sizeof(line), cands)) {
                unsigned long long rva = strtoull(line, NULL, 0);
                if (!rva) continue;
                uint64_t v = rr_u64(module + rva);
                if (!PL(v) || (v & 7)) continue;
                uint8_t hdr[0x60];
                if (!rr_read(v, hdr, sizeof(hdr))) continue;
                for (int o = 0; o < 0x60; o += 8) {
                    uint64_t head = 0;
                    memcpy(&head, hdr + o, 8);
                    if (!PL(head) || (head & 7)) continue;
                    uint8_t nb[0x30];
                    if (!rr_read(head, nb, sizeof(nb))) continue;
                    for (int x = 0; x < 0x30; x += 8) {
                        uint64_t obj = 0;
                        memcpy(&obj, nb + x, 8);
                        if (!PL(obj) || (obj & 7)) continue;
                        uint64_t cls = rr_u64(obj);
                        char nm[96] = {0};
                        if (PL(cls)) u_class_name(cls, nm, sizeof(nm));
                        if (nm[0] && !strcmp(nm, "GameObject"))
                            fprintf(f, "HIT rva=0x%llX gom=0x%llX list_off=%X node=0x%llX obj_off=%X obj=0x%llX\n",
                                    rva, (unsigned long long)v, o,
                                    (unsigned long long)head, x, (unsigned long long)obj);
                    }
                }
            }
            fclose(cands);
        }

        /* Walk the strongest candidate list and try to read GameObject names,
         * which reveals the node->object and name offsets. */
        {
            uint64_t v = rr_u64(module + 0x22309A0);
            if (PL(v)) {
                fprintf(f, "walk gom=0x%llX\n", (unsigned long long)v);
                uint64_t node = rr_u64(v + 0x18);
                for (int k = 0; k < 64 && PL(node); ++k) {
                    uint8_t nb[0x30];
                    if (!rr_read(node, nb, sizeof(nb))) break;
                    uint64_t obj = 0;
                    memcpy(&obj, nb + 0x20, 8);
                    char nm[96] = {0};
                    if (PL(obj)) {
                        uint64_t cls = rr_u64(obj);
                        if (PL(cls)) u_class_name(cls, nm, sizeof(nm));
                    }
                    fprintf(f, " walk[%d] node=0x%llX obj=0x%llX class='%s'",
                            k, (unsigned long long)node, (unsigned long long)obj, nm);
                    if (PL(obj)) {
                        for (int off = 0x40; off <= 0x70; off += 8) {
                            uint64_t s = rr_u64(obj + off);
                            char txt[64] = {0};
                            if (PL(s) && u_string(s, txt, sizeof(txt)) > 0)
                                fprintf(f, " name@%X='%s'", off, txt);
                        }
                    }
                    fputc('\n', f);
                    uint64_t next = 0;
                    memcpy(&next, nb, 8);
                    if (!PL(next) || next == node) memcpy(&next, nb + 8, 8);
                    if (!PL(next) || next == node) break;
                    node = next;
                }
            }
        }

        /* Direct root chain from the supplied header:
         * GOM/Root + 0xD0 (or 0xD8) -> GameModel -> +0x10 BattleModel
         * -> +0x90 BattleEntities -> +0x60 (or BattleModel +0x518) World.
         * Try both modules: the offset may be a GameAssembly RVA. */
        {
            uint64_t ga = 0; uint32_t gasz = 0;
            rr_module("GameAssembly.dll", &ga, &gasz);
            uint64_t roots[2] = { PL(ga) ? rr_u64(ga + 0x2230768ULL) : 0,
                                  rr_u64(module + 0x2230768ULL) };
            const char* names[2] = { "GameAssembly", "UnityPlayer" };
            for (int r = 0; r < 2; ++r) {
                uint64_t root = roots[r];
                fprintf(f, "chain[%s] root=0x%llX\n", names[r], (unsigned long long)root);
                if (!PL(root)) continue;
                for (int a = 0; a < 2; ++a) {
                    uint64_t gm = rr_u64(root + (a ? 0xD8ULL : 0xD0ULL));
                    char nm[96] = {0};
                    if (PL(gm)) u_class_name(rr_u64(gm), nm, sizeof(nm));
                    fprintf(f, "  A%d gm=0x%llX class='%s'\n", a,
                            (unsigned long long)gm, nm);
                    if (!PL(gm)) continue;
                    uint64_t bm = rr_u64(gm + 0x10);
                    char nb[96] = {0};
                    if (PL(bm)) u_class_name(rr_u64(bm), nb, sizeof(nb));
                    uint64_t be = PL(bm) ? rr_u64(bm + 0x90) : 0;
                    char ne[96] = {0};
                    if (PL(be)) u_class_name(rr_u64(be), ne, sizeof(ne));
                    uint64_t w1 = PL(be) ? rr_u64(be + 0x60) : 0;
                    uint64_t w2 = PL(bm) ? rr_u64(bm + RR_OFF_BATTLE_WORLD_B) : 0;
                    char nw1[96] = {0}, nw2[96] = {0};
                    if (PL(w1)) u_class_name(rr_u64(w1), nw1, sizeof(nw1));
                    if (PL(w2)) u_class_name(rr_u64(w2), nw2, sizeof(nw2));
                    fprintf(f, "    bm=0x%llX '%s' be=0x%llX '%s' w60=0x%llX '%s' w510=0x%llX '%s'\n",
                            (unsigned long long)bm, nb, (unsigned long long)be, ne,
                            (unsigned long long)w1, nw1, (unsigned long long)w2, nw2);
                }
            }
        }

        /* Focused layout probe: walk the two GOM candidates assuming the object
         * pointer is at node+0x20, then try the UnityEntry component chain. */
        {
            static const unsigned rvas[2] = { 0x22309A0u, 0x22309A8u };
            for (int ri = 0; ri < 2; ++ri) {
                uint64_t g = rr_u64(module + rvas[ri]);
                fprintf(f, "probe rva=0x%X g=0x%llX\n", rvas[ri], (unsigned long long)g);
                if (!PL(g)) continue;
                uint64_t node = rr_u64(g + 0x18);
                fprintf(f, "  head=0x%llX\n", (unsigned long long)node);
                for (int k = 0; k < 8 && PL(node) && (node & 7) == 0; ++k) {
                    uint8_t nb[0x40];
                    if (!rr_read(node, nb, sizeof(nb))) break;
                    uint64_t obj = 0;
                    memcpy(&obj, nb + 0x20, 8);
                    char cn[96] = {0};
                    if (PL(obj)) {
                        uint64_t cls = rr_u64(obj);
                        if (PL(cls)) u_class_name(cls, cn, sizeof(cn));
                    }
                    fprintf(f, "   n%d node=0x%llX obj=0x%llX '%s'",
                            k, (unsigned long long)node, (unsigned long long)obj, cn);
                    if (PL(obj)) {
                        char an[64] = {0};
                        if (ascii_at(rr_u64(obj + 0x50), an, sizeof(an)))
                            fprintf(f, " native50='%s'", an);
                        uint64_t ue = entry_component(obj);
                        fprintf(f, " entry=0x%llX", (unsigned long long)ue);
                        if (PL(ue)) {
                            uint64_t game = rr_u64(ue + RR_OFF_ENTRY_GAME);
                            uint64_t model = PL(game) ? rr_u64(game + RR_OFF_GAME_MODEL) : 0;
                            uint64_t bm = PL(model) ? rr_u64(model + RR_OFF_GAME_BATTLE) : 0;
                            uint64_t be = PL(bm) ? rr_u64(bm + RR_OFF_BATTLE_ENTITIES) : 0;
                            uint64_t world = PL(be) ? rr_u64(be + RR_OFF_BATTLE_WORLD) : 0;
                            char w[96] = {0};
                            if (PL(world)) u_class_name(rr_u64(world), w, sizeof(w));
                            fprintf(f, " game=0x%llX model=0x%llX bm=0x%llX be=0x%llX world=0x%llX '%s'",
                                    (unsigned long long)game, (unsigned long long)model,
                                    (unsigned long long)bm, (unsigned long long)be,
                                    (unsigned long long)world, w);
                        }
                    }
                    fputc('\n', f);
                    uint64_t next = 0;
                    memcpy(&next, nb + 8, 8);
                    if (!PL(next) || next == node) memcpy(&next, nb + 0x10, 8);
                    if (!PL(next) || next == node) break;
                    node = next;
                }
            }
        }

        /* Layout probe: which node qword points to another node, and what the
         * GameObject's component container looks like. */
        {
            uint64_t g = rr_u64(module + 0x22309A0u);
            uint64_t head = PL(g) ? rr_u64(g + 0x18) : 0;
            fprintf(f, "walkprobe g=0x%llX head=0x%llX\n",
                    (unsigned long long)g, (unsigned long long)head);
            uint8_t nb[0x40];
            if (PL(head) && rr_read(head, nb, sizeof(nb))) {
                for (int o = 0; o < 0x40; o += 8) {
                    uint64_t w = 0;
                    memcpy(&w, nb + o, 8);
                    fprintf(f, " nb[%02X]=0x%016llX", o, (unsigned long long)w);
                    if (PL(w) && (w & 7) == 0) {
                        uint64_t obj = rr_u64(w + 0x20);
                        char nm[96] = {0};
                        if (PL(obj)) {
                            uint64_t cls = rr_u64(obj);
                            if (PL(cls)) u_class_name(cls, nm, sizeof(nm));
                        }
                        if (nm[0] && !strcmp(nm, "GameObject"))
                            fprintf(f, " NODE obj=0x%llX", (unsigned long long)obj);
                    }
                    fputc('\n', f);
                }
                uint64_t obj = 0;
                memcpy(&obj, nb + 0x20, 8);
                if (PL(obj)) {
                    fprintf(f, "object 0x%llX ptr fields:\n", (unsigned long long)obj);
                    uint8_t ob[0x90];
                    if (rr_read(obj, ob, sizeof(ob))) {
                        for (int o = 0; o < 0x90; o += 8) {
                            uint64_t w = 0;
                            memcpy(&w, ob + o, 8);
                            if (!PL(w) || (w & 7)) continue;
                            char nm[96] = {0};
                            uint64_t cls = rr_u64(w);
                            if (PL(cls)) u_class_name(cls, nm, sizeof(nm));
                            fprintf(f, " obj[%02X]=0x%llX class='%s'",
                                    o, (unsigned long long)w, nm);
                            int cnt = rr_i32(w + 0x18);
                            uint64_t items = rr_u64(w + 0x10);
                            if (cnt > 0 && cnt < 64 && PL(items)) {
                                fprintf(f, " list cnt=%d", cnt);
                                for (int k = 0; k < 3 && k < cnt; ++k) {
                                    uint64_t it = rr_u64(items + 0x20 + (uint64_t)k * 8);
                                    char inm[96] = {0};
                                    uint64_t ic = PL(it) ? rr_u64(it) : 0;
                                    if (PL(ic)) u_class_name(ic, inm, sizeof(inm));
                                    fprintf(f, " [%d]0x%llX '%s'", k,
                                            (unsigned long long)it, inm);
                                }
                            }
                            int alen = rr_i32(w + 0x18);
                            uint64_t adata = rr_u64(w + 0x20);
                            if (alen > 0 && alen < 64 && PL(adata)) {
                                fprintf(f, " array len=%d", alen);
                                for (int k = 0; k < 3 && k < alen; ++k) {
                                    uint64_t it = rr_u64(adata + (uint64_t)k * 8);
                                    char inm[96] = {0};
                                    uint64_t ic = PL(it) ? rr_u64(it) : 0;
                                    if (PL(ic)) u_class_name(ic, inm, sizeof(inm));
                                    fprintf(f, " [%d]0x%llX '%s'", k,
                                            (unsigned long long)it, inm);
                                }
                            }
                            fputc('\n', f);
                        }
                    }
                }
            }
        }

        /* Walk the list with the discovered node layout (next at +0x00,
         * GameObject at +0x20) and print names to locate UnityEntry. */
        {
            uint64_t g = rr_u64(module + 0x22309A0u);
            uint64_t node = PL(g) ? rr_u64(g + 0x18) : 0;
            fprintf(f, "listwalk g=0x%llX\n", (unsigned long long)g);
            for (int k = 0; k < 32 && PL(node) && (node & 7) == 0; ++k) {
                uint8_t nb[0x40];
                if (!rr_read(node, nb, sizeof(nb))) break;
                uint64_t obj = 0;
                memcpy(&obj, nb + 0x20, 8);
                char nm[64] = {0}, s18[64] = {0}, s38[64] = {0};
                if (PL(obj)) {
                    ascii_at(rr_u64(obj + 0x50), nm, sizeof(nm));
                    uint64_t s = rr_u64(obj + 0x18);
                    if (PL(s)) u_string(s, s18, sizeof(s18));
                    s = rr_u64(obj + 0x38);
                    if (PL(s)) u_string(s, s38, sizeof(s38));
                }
                fprintf(f, " n%d node=0x%llX obj=0x%llX name='%s' s18='%s' s38='%s'\n",
                        k, (unsigned long long)node, (unsigned long long)obj, nm, s18, s38);
                uint64_t next = 0;
                memcpy(&next, nb, 8);
                if (!PL(next) || next == node) break;
                node = next;
            }
        }

        /* Brute-force the list layout over high-vote globals: require a run of
         * consecutive managed GameObjects with a printable native name. */
        {
            static const unsigned cand[15] = {
                0x2230758u, 0x2230760u, 0x2230768u, 0x2230D80u, 0x2230E08u,
                0x2231B50u, 0x2231BA0u, 0x2254010u, 0x21D85B0u, 0x2152AE0u,
                0x212D1C0u, 0x21DDE50u, 0x22309A0u, 0x22309A8u, 0x2136600u
            };
            static const int nexts[4] = { 0x00, 0x08, 0x10, 0x18 };
            static const int objs[5]  = { 0x08, 0x10, 0x18, 0x20, 0x28 };
            for (int ci = 0; ci < 15; ++ci) {
                uint64_t g = rr_u64(module + cand[ci]);
                if (!PL(g) || (g & 7)) continue;
                for (int lo = 0; lo <= 0x40; lo += 8) {
                    uint64_t head = rr_u64(g + lo);
                    if (!PL(head) || (head & 7)) continue;
                    for (int ni = 0; ni < 4; ++ni) {
                        for (int oi = 0; oi < 5; ++oi) {
                            uint64_t node = head;
                            int ok = 0;
                            char firstName[64] = {0};
                            for (int k = 0; k < 4; ++k) {
                                uint8_t nb[0x30];
                                if (!PL(node) || (node & 7) || !rr_read(node, nb, sizeof(nb))) break;
                                uint64_t obj = 0;
                                memcpy(&obj, nb + objs[oi], 8);
                                if (!PL(obj)) break;
                                char nm[96] = {0};
                                uint64_t cls = rr_u64(obj);
                                if (PL(cls)) u_class_name(cls, nm, sizeof(nm));
                                if (strcmp(nm, "GameObject")) break;
                                char native[64] = {0};
                                if (!ascii_at(rr_u64(obj + 0x50), native, sizeof(native))) break;
                                if (k == 0) snprintf(firstName, sizeof(firstName), "%s", native);
                                ++ok;
                                uint64_t nx = 0;
                                memcpy(&nx, nb + nexts[ni], 8);
                                if (!PL(nx) || nx == node) break;
                                node = nx;
                            }
                            if (ok >= 4)
                                fprintf(f, "HITLAYOUT g_rva=0x%X g=0x%llX list=%X next=%X obj=%X first='%s'\n",
                                        cand[ci], (unsigned long long)g, lo,
                                        nexts[ni], objs[oi], firstName);
                        }
                    }
                }
            }
        }
    }
}

static int entry_roots_class(ENTRY_ROOTS* roots);   /* defined below */

/* Entry discovery: the native GameObjectManager walk first, then the managed
 * UnityEntry singleton chain (class scan + one instance scan), which does not
 * depend on the engine's native manager layout. */
static int entry_roots(ENTRY_ROOTS* roots) {
    if (entry_roots_class(roots)) return 1;
    memset(roots, 0, sizeof(*roots));
    return entry_roots_gom(roots);
}

int game_entry_probe(uint64_t* world, uint64_t* battle, uint64_t* camera) {
    ENTRY_ROOTS roots;
    int ok = entry_roots(&roots);
    if (world) *world = roots.world;
    if (battle) *battle = roots.battle;
    if (camera) *camera = roots.camera;
    return ok;
}

static int class_plausible(uint64_t k, const char* want) {
    /* Do not require an image or namespace string: valid global-namespace
     * classes may lack them. Check the class name here and validate discovered
     * instances separately.
     */
    if (!PL(k)) return 0;
    return class_named(k, want);
}

static int find_names(const char* name, uint64_t* out, int max) {
    size_t len = strlen(name);
    uint8_t pat[128];
    if (len + 2 > sizeof(pat) || max <= 0) return 0;
    pat[0] = 0;
    memcpy(pat + 1, name, len);
    pat[len + 1] = 0;
    size_t plen = len + 2;

    static uint8_t buf[0x10000];
    static uint8_t win[0x10000 + 128];
    size_t winlen = 0;
    int found = 0;
    for (uint64_t a = g_name_lo; a < g_name_hi; a += sizeof(buf)) {
        size_t n = (size_t)((g_name_hi - a) < sizeof(buf) ? (g_name_hi - a) : sizeof(buf));
        if (!rr_read(a, buf, n)) { winlen = 0; continue; }
        memcpy(win + winlen, buf, n);
        size_t total = winlen + n;
        uint64_t base = a - (uint64_t)winlen;
        for (size_t i = 0; i + plen <= total && found < max; i++)
            if (memcmp(win + i, pat, plen) == 0)
                out[found++] = base + i + 1;
        size_t k = (total < plen - 1) ? total : plen - 1;
        memmove(win, win + total - k, k);
        winlen = k;
    }
    return found;
}

static uint64_t find_class(const char* name) {
    uint64_t names[8];
    int n = find_names(name, names, 8);
    if (!n) return 0;
    static uint8_t buf[0x10000];
    for (int c = 0; c < n; c++) {
        uint64_t nameptr = names[c];
        for (uint64_t a = g_name_lo; a < g_name_hi; a += sizeof(buf)) {
            size_t sz = (size_t)((g_name_hi - a) < sizeof(buf) ? (g_name_hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != nameptr) continue;
                uint64_t k = a + i - 0x10;
                if (class_plausible(k, name)) return k;
            }
        }
    }
    return 0;
}

/* Character scan. */

/* Resolve only a live, explicitly referenced hiding object in this world.
 * Managed field offsets are generated from the supplied CppSDK; replication of this
 * state for other players still needs live verification. */
static int hidden_object_find(uint64_t character, HIDE_OBJECT* out) {
    uint64_t world = g_world;
    if (!PL(world) || rr_u64(world) != g_k_world) return 0;
    uint64_t state = rr_u64(character + RR_OFF_CHAR_STATE);
    if (!u_is(state, "BattleCharacterStateModel")) return 0;
    uint64_t hidden = rr_u64(state + RR_OFF_STATE_HIDDEN);
    if (!u_is(hidden, "CharacterHideObjectStateModel")) return 0;
    uint64_t data = rr_u64(hidden + RR_OFF_HIDDEN_ID_DATA);
    if (!u_is(data, "BattleEntityIdData") && !u_is(data, "IntData")) return 0;
    int32_t id = -1;
    if (!rr_read(data + RR_OFF_PRIM_VALUE, &id, sizeof(id)) || id < 0) return 0;
    uint64_t dict = rr_u64(world + RR_OFF_WEM_HIDE_OBJECTS), entries = 0;
    int n = 0, stride = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) return 0;
    for (int i = 0; i < n; ++i) {
        uint8_t entry[0x18]; int32_t key,hash; uint64_t obj;
        if (!rr_read(entries + (uint64_t)i * 0x18, entry, sizeof(entry))) return 0;
        memcpy(&key, entry + 8, sizeof(key));
        memcpy(&hash,entry,4);if(hash<0)continue;
        memcpy(&obj, entry + 0x10, sizeof(obj));
        if (key != id || !u_is(obj, "HideObjectModel")) continue;
        return rr_hide_object_read(obj,id,out)&&world==g_world&&rr_u64(world)==g_k_world;
    }
    return 0;
}
static int hidden_object_position(uint64_t character,RR_V3* out) {
    HIDE_OBJECT object;if(!hidden_object_find(character,&object))return 0;
    *out=object.pos;return 1;
}

/* Server -> simulated model pairs from BattleEntitiesModel._models. The
 * simulated model is the client-interpolated copy the renderer draws, so its
 * transform and skeleton move smoothly between replication ticks. Identity and
 * the local-player flag stay on the server model. */
typedef struct { uint64_t server, sim; } SIM_PAIR;
static SIM_PAIR g_sim_pair[512];
static int g_nsim_pair;

static void build_sim_map(void) {
    g_nsim_pair = 0;
    if (!PL(g_battle) || !g_k_char) return;
    uint64_t dict = rr_u64(g_battle + RR_OFF_BATTLE_MODELS);
    int n = 0, stride = 0;
    uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride)) return;
    if (n <= 0 || n > 4096 || stride < 0x18) return;
    static uint8_t buf[0x10000];
    size_t bytes = (size_t)n * (size_t)stride;
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    int per = (int)(bytes / (size_t)stride);
    if (per <= 0 || !rr_read(entries, buf, bytes)) return;
    for (int i = 0; i < per && g_nsim_pair < (int)(sizeof(g_sim_pair) / sizeof(g_sim_pair[0])); ++i) {
        uint64_t ent = 0;
        memcpy(&ent, buf + (size_t)i * (size_t)stride + 0x10, 8);
        if (!PL(ent)) continue;
        uint64_t ek = rr_u64(ent);
        if (!ek || !class_named(ek, "BattleCharacterEntityModel")) continue;
        uint64_t server = rr_u64(ent + 0x28);
        uint64_t sim = rr_u64(ent + 0x30);
        if (!PL(server) || !PL(sim) || rr_u64(sim) != g_k_char) continue;
        g_sim_pair[g_nsim_pair].server = server;
        g_sim_pair[g_nsim_pair].sim = sim;
        ++g_nsim_pair;
    }
}

static uint64_t sim_for_server(uint64_t server) {
    if (!PL(server)) return 0;
    for (int i = 0; i < g_nsim_pair; ++i)
        if (g_sim_pair[i].server == server) return g_sim_pair[i].sim;
    return 0;
}

static void fill_entry(ENT_CACHE* c, uint64_t p) {
    memset(c, 0, sizeof(*c));
    c->obj = p;
    c->armor = -1;
    c->crouch = -1;
    c->vehicle_id = -1;
    c->local = PL(rr_u64(p + RR_OFF_CHAR_PBM));

    uint8_t head[0x60];
    if (!rr_read(p, head, sizeof(head))) { c->obj = 0; return; }
    /* Recheck the class pointer before using a cached object. Freed objects
     * can retain readable bytes until the allocator reuses their storage.
     */
    {
        uint64_t klass = 0;
        memcpy(&klass, head, 8);
        if (klass != g_k_char) { c->obj = 0; return; }
    }
    memcpy(&c->id, head + RR_OFF_CHAR_ID, 4);
    memcpy(&c->tr, head + RR_OFF_CHAR_TRANSFORM, 8);
    /* SkeletonModel is at +0x138, outside the 0x60-byte header read. */
    c->skel = rr_u64(p + RR_OFF_CHAR_SKELETON);
    /* Prefer the interpolated model for transform and skeleton: its position is
     * what the renderer shows, and it updates smoothly instead of stepping at
     * the replication rate (which also jitters while riding in vehicles). */
    {
        uint64_t sim = sim_for_server(p);
        if (PL(sim)) {
            uint64_t tr = rr_u64(sim + RR_OFF_CHAR_TRANSFORM);
            uint64_t sk = rr_u64(sim + RR_OFF_CHAR_SKELETON);
            if (PL(tr)) c->tr = tr;
            if (PL(sk)) c->skel = sk;
        }
    }
    if (!c->local || br_get()->show_self) {
        HIDE_OBJECT hidden;
        if(hidden_object_find(p,&hidden)) {
            c->hidden_in_object=1;c->hidden_pos=hidden.pos;c->hidden_anchor=hidden.anchor;
        }
    }
    if (!PL(c->tr) && !c->hidden_in_object) { c->obj = 0; return; }

    uint64_t data = rr_u64(p + RR_OFF_CHAR_DATA);
    if (PL(data)) {
        uint64_t common = rr_u64(data + RR_OFF_DATA_COMMON);
        uint64_t pub = rr_u64(data + RR_OFF_DATA_PUBLIC);
        if (PL(common)) {
            uint64_t ref = rr_u64(common + RR_OFF_COMMON_ROLE);
            uint64_t desc = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
            uint64_t str = PL(desc) ? rr_u64(desc + RR_OFF_DESC_NAME) : 0;
            if (PL(str)) u_string(str, c->role, sizeof(c->role));
            c->role[sizeof(c->role) - 1] = 0;
            /* Require a replicated role to reject templates and unspawned objects
             * that would otherwise appear as stationary markers.
             */
            if (!c->role[0]) { c->obj = 0; return; }
            /* The item in hands is replicated for remote characters. */
            ref = rr_u64(common + RR_OFF_COMMON_ITEM_IN_HANDS);
            desc = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
            str = PL(desc) ? rr_u64(desc + RR_OFF_DESC_NAME) : 0;
            if (PL(str)) u_string(str, c->weapon, sizeof(c->weapon));
        }
        if (PL(pub)) {
            uint64_t bd = rr_u64(pub + RR_OFF_PUBLIC_ARMOR);
            if (PL(bd)) { uint8_t b = 0; if (rr_read(bd + RR_OFF_PRIM_VALUE, &b, 1)) c->armor = b; }
        }
    }
    /* Knockout (downed) state: read the phase description name string and
     * compare to "KnockedOut". This is more reliable than checking the enum
     * index or EndTs, which can have false positives/negatives.
     * Chain: character+0x298 -> KnockoutModel -> +0x18 _knockoutPhase (IPrimitiveData)
     *        -> +0x18 value (KnockoutPhaseDescription) -> +0x10 Name string */
    {
        uint64_t ko = rr_u64(p + RR_OFF_CHAR_KNOCKOUT);
        uint64_t phd = PL(ko) ? rr_u64(ko + RR_OFF_KO_PHASE) : 0;
        uint64_t phase = PL(phd) ? rr_u64(phd + RR_OFF_PRIM_VALUE) : 0;
        /* Read the phase description name string */
        char phasename[32] = {0};
        if (PL(phase)) {
            uint64_t namestr = rr_u64(phase + RR_OFF_DESC_NAME);
            if (PL(namestr)) u_string(namestr, phasename, sizeof(phasename));
        }
        /* Also read IsDead for completeness */
        uint64_t isd = rr_u64(p + RR_OFF_CHAR_ISDEAD);
        uint64_t isdm = PL(isd) ? rr_u64(isd + 0x10) : 0;
        int isdead = 0; if (PL(isdm)) { uint8_t b = 0; if (rr_read(isdm + 0x18, &b, 1)) isdead = b; }
        snprintf(c->knockphase, sizeof(c->knockphase), "i%d p='%s'",
                 isdead, phasename);
        /* Player is knocked if IsDead is true OR phase name is exactly "KnockedOut" */
        c->knocked = (isdead || strcmp(phasename, "KnockedOut") == 0) ? 1 : 0;
    }
    uint64_t wm = rr_u64(p + RR_OFF_CHAR_WEAPON);
    if (!c->weapon[0] && PL(wm)) {
        uint64_t eq = rr_u64(wm + RR_OFF_WEAPON_ITEM);
        uint64_t ref = PL(eq) ? rr_u64(eq + RR_OFF_WEAPON_ITEM) : 0;
        uint64_t desc = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
        uint64_t str = PL(desc) ? rr_u64(desc + RR_OFF_DESC_NAME) : 0;
        if (PL(str)) u_string(str, c->weapon, sizeof(c->weapon));
        c->weapon[sizeof(c->weapon) - 1] = 0;
    }
    /* seated in a vehicle: InVehicleModel._entityId is -1 when on foot */
    uint64_t iv = rr_u64(p + RR_OFF_CHAR_INVEH);
    if (PL(iv)) {
        uint64_t eid = rr_u64(iv + RR_OFF_INVEH_ID);
        int v = -1;
        if (PL(eid) && !rr_read(eid + 0x18, &v, sizeof(v))) v = -1;
        c->inveh = (v >= 0) ? 1 : 0;
        c->vehicle_id = v;
        if (c->inveh) {
            uint64_t vt = rr_u64(iv + RR_OFF_INVEH_TYPE);
            uint64_t d = PL(vt) ? rr_u64(vt + 0x18) : 0;
            uint64_t s = PL(d) ? rr_u64(d + 0x10) : 0;
            if (PL(s)) u_string(s, c->veh, sizeof(c->veh));
        }
    }
    uint64_t cr = rr_u64(p + RR_OFF_CHAR_CROUCH);
    if (PL(cr)) {
        uint64_t bd = rr_u64(cr + RR_OFF_CROUCH_BOOL);
        if (PL(bd)) { uint8_t b = 0; if (rr_read(bd + RR_OFF_PRIM_VALUE, &b, 1)) c->crouch = b; }
    }
}

typedef struct { uint64_t lo, hi; volatile LONG count; } SCAN_CTX;
static volatile LONG g_scan_idx;   /* shared across regions in one pass */

#define RR_MAX_REGIONS 32768
#define RR_MAX_HEAP 512
typedef struct { uint64_t base, size; } RR_HEAP;
typedef struct { uint64_t world, battle; int nregions; RR_HEAP regions[RR_MAX_REGIONS]; } BATTLE_SCAN;
static volatile LONG g_battle_scan_busy = 0;
void game_scene_debug(uint64_t* battle, uint64_t* updater, int* scanning) {
    if (battle) *battle=g_battle;
    if (updater) *updater=g_views_updater;
    if (scanning) *scanning=(int)InterlockedCompareExchange(&g_battle_scan_busy,0,0);
}
static char g_dbg[160] = "?";
static RR_HEAP g_regions[RR_MAX_REGIONS];
static int     g_nregions;
static RR_HEAP g_heap[RR_MAX_HEAP];
static int     g_nheap;
static RR_HEAP g_inst_regions[16];   /* regions that produced instances */

/* Size-ordered region list: instance heaps are the largest regions, so a
 * single-instance search finds its target sooner than an address-order sweep. */
static int g_scan_order[RR_MAX_REGIONS];
static int g_scan_order_n = 0;
static int g_scan_order_src = -1;

static void build_region_order(void) {
    if (g_scan_order_src == g_nregions && g_scan_order_n == g_nregions) return;
    g_scan_order_src = g_nregions;
    g_scan_order_n = g_nregions;
    for (int i = 0; i < g_nregions; ++i) g_scan_order[i] = i;
    for (int i = 1; i < g_nregions; ++i) {
        int idx = g_scan_order[i];
        uint64_t size = g_regions[idx].size;
        int j = i - 1;
        while (j >= 0 && g_regions[g_scan_order[j]].size < size) {
            g_scan_order[j + 1] = g_scan_order[j];
            --j;
        }
        g_scan_order[j + 1] = idx;
    }
}

/* Resolve a class by name and validate it from its field table. Retries the
 * time-boxed string scan so a cold start can cover the process over several
 * slices instead of a single 2 s pass. */
static uint64_t resolve_named_class(const char* name, const char* field) {
    collect_hits_reset();
    for (int attempt = 0; attempt < 8; ++attempt) {
        collect_hits(name);
        dlog("resolve_named %s attempt=%d hits=%ld", name, attempt,
             (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS));
        if (!g_nhits) continue;
        collect_candidates(name);
        for (int i = 0; i < g_ncands; ++i) {
            if (class_named(g_cands[i], name) && class_has_field(g_cands[i], field))
                return g_cands[i];
        }
        /* The hit(s) did not yield a matching class; drop the hint so the next
         * attempt continues the sweep instead of rescanning the same window. */
        collect_hits_clear_hint();
    }
    return 0;
}

/* Find an instance of a class whose field at want_off points at a managed object
 * of class want_field_class. Stops at the first validated candidate. */
static uint64_t find_instance_of(uint64_t klass, unsigned want_off,
                                 const char* want_field_class) {
    if (!PL(klass)) return 0;
    build_region_order();
    static uint8_t buf[0x10000];
    for (int oi = 0; oi < g_scan_order_n; ++oi) {
        const RR_HEAP* r = &g_regions[g_scan_order[oi]];
        if (r->size < 0x1000) continue;
        for (uint64_t off = 0; off + 8 <= r->size; off += sizeof(buf)) {
            size_t n = (size_t)((r->size - off) < sizeof(buf) ? (r->size - off) : sizeof(buf));
            if (!rr_read(r->base + off, buf, n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uint64_t v = 0;
                memcpy(&v, buf + i, 8);
                if (v != klass) continue;
                uint64_t obj = r->base + off + i;
                uint64_t ref = rr_u64(obj + want_off);
                if (!PL(ref)) continue;
                uint64_t ref_cls = rr_u64(ref);
                if (PL(ref_cls) && class_named(ref_cls, want_field_class)) return obj;
            }
        }
    }
    return 0;
}

/* Resolve the UnityEntry component through the native GameObjectManager list.
 *
 * Layout on the current build (from the community chain and validated against
 * the live GOM):
 *   gom  = [UnityPlayer + 0x2230768]
 *   head = [gom + 0x18], end = [gom + 0x10]
 *   node: next [node+0x8], GameObject [node+0x10]
 *   GameObject: native name char* [go+0x50], component array [go+0x20],
 *               component count [go+0x30], slots 0x10 bytes with the native
 *               component at +8
 *   managed: GC handle at [native+0x18] & ~7 dereferenced, or [native+0x28]
 *
 * This avoids the multi-second metadata string scan. Returns 0 when the list
 * or the component cannot be resolved; the caller then falls back to the
 * class-name scan. */
static uint64_t gom_entry(void) {
    uint64_t up = 0; uint32_t us = 0;
    if (!rr_module("UnityPlayer.dll", &up, &us)) return 0;
    uint64_t gom = rr_u64(up + 0x2230768ULL);
    if (!PL(gom)) return 0;
    uint64_t node = rr_u64(gom + 0x18);
    uint64_t end  = rr_u64(gom + 0x10);
    if (!PL(node)) return 0;
    for (int n = 0; n < 4096; ++n) {
        if (!PL(node) || node == end) break;
        uint64_t go = rr_u64(node + 0x10);
        if (PL(go)) {
            char name[64] = {0};
            if (ascii_at(rr_u64(go + 0x50), name, sizeof(name)) && !strcmp(name, "UnityEntry")) {
                uint64_t comps = rr_u64(go + 0x20);
                int count = rr_i32(go + 0x30);
                if (PL(comps) && count > 0 && count <= 256) {
                    for (int c = 0; c < count; ++c) {
                        uint64_t native = rr_u64(comps + (uint64_t)c * 0x10 + 8);
                        if (!PL(native)) continue;
                        uint64_t handle = rr_u64(native + 0x18) & ~(uint64_t)7;
                        uint64_t managed = PL(handle) ? rr_u64(handle) : 0;
                        if (PL(managed) && class_named(rr_u64(managed), "UnityEntry")) return managed;
                        managed = rr_u64(native + 0x28);
                        if (PL(managed) && class_named(rr_u64(managed), "UnityEntry")) return managed;
                    }
                }
                return 0;
            }
        }
        uint64_t next = rr_u64(node + 8);
        if (!PL(next) || next == node) break;
        node = next;
    }
    return 0;
}

/* Resolve the UnityEntry singleton and walk its chain to the world. The entry
 * class is found by name (metadata string scan) and its instance by one
 * size-ordered scan; after that the chain is a handful of reads. */
static uint64_t g_entry_inst = 0;

static int entry_roots_class(ENTRY_ROOTS* roots) {
    uint64_t inst = 0;
    /* Cached entry first: the worker re-runs this every few seconds and the GOM
     * walk is only needed once per process. */
    if (PL(g_entry_inst) && PL(g_k_entry) && rr_u64(g_entry_inst) == g_k_entry)
        inst = g_entry_inst;
    if (!PL(inst)) {
        inst = gom_entry();
        if (PL(inst)) {
            g_k_entry = rr_u64(inst);
            g_entry_inst = inst;
            dlog("entry_gom=0x%llX class=0x%llX",
                 (unsigned long long)inst, (unsigned long long)g_k_entry);
        }
    }
    if (!PL(inst)) {
        if (!PL(g_k_entry)) g_k_entry = resolve_named_class("UnityEntry", "_game");
        dlog("entry_class=0x%llX", (unsigned long long)g_k_entry);
        if (!PL(g_k_entry)) return 0;
        inst = find_instance_of(g_k_entry, 0x28, "Game");
        g_entry_inst = inst;
        dlog("entry_inst=0x%llX", (unsigned long long)inst);
    }
    if (!PL(inst)) return 0;
    uint64_t game = rr_u64(inst + 0x28);
    if (!PL(game) || !class_named(rr_u64(game), "Game")) return 0;
    uint64_t model = 0;
    for (int i = 0; i < 2 && !PL(model); ++i) {
        unsigned off = i ? 0xD8u : 0xD0u;
        uint64_t cand = rr_u64(game + off);
        if (PL(cand) && class_named(rr_u64(cand), "GameModel")) model = cand;
    }
    if (!PL(model)) return 0;
    uint64_t bm = rr_u64(model + 0x10);
    if (!PL(bm) || !class_named(rr_u64(bm), "BattleModel")) return 0;
    uint64_t be = rr_u64(bm + 0x90);
    if (!PL(be) || !class_named(rr_u64(be), "BattleEntitiesModel")) return 0;
    uint64_t world = rr_u64(be + 0x60);
    if (!PL(world) || !class_named(rr_u64(world), "WorldEntitiesModel")) {
        world = rr_u64(bm + RR_OFF_BATTLE_WORLD_B);
        if (!PL(world) || !class_named(rr_u64(world), "WorldEntitiesModel")) return 0;
    }
    roots->world = world;
    roots->battle = be;
    roots->battle_model = bm;
    uint64_t camera = rr_u64(bm + RR_OFF_BATTLE_CAMERA);
    if (camera_validate(camera)) roots->camera = camera;
    dlog("entry_roots_class world=0x%llX battle=0x%llX bm=0x%llX camera=0x%llX",
         (unsigned long long)world, (unsigned long long)be,
         (unsigned long long)bm, (unsigned long long)roots->camera);
    return 1;
}
static int     g_ninst_regions;
/* The heap fallback is bounded so a scan can never hold the worker (and the
 * world re-check at the top of its loop) for minutes. Regions are visited from
 * a rotating cursor, so consecutive passes eventually cover all of them. */
#define SCAN_BUDGET_MS 3000
static volatile LONG g_scan_deadline;   /* GetTickCount deadline, 0 = none */
static volatile LONG g_region_cursor;

static DWORD WINAPI char_scan_thread(LPVOID param) {
    SCAN_CTX* s = (SCAN_CTX*)param;
    static __declspec(thread) uint8_t buf[0x10000];
    for (uint64_t a = s->lo; a < s->hi; a += sizeof(buf)) {
        LONG deadline = InterlockedCompareExchange(&g_scan_deadline, 0, 0);
        if (deadline && (DWORD)GetTickCount() >= (DWORD)deadline) return 0;
        size_t n = (size_t)((s->hi - a) < sizeof(buf) ? (s->hi - a) : sizeof(buf));
        if (!rr_read(a, buf, n)) continue;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t v;
            memcpy(&v, buf + i, 8);
            if (v != g_k_char) continue;
            uint64_t p = a + i;
            if (!PL(rr_u64(p))) continue;
            LONG idx = InterlockedIncrement(&g_scan_idx) - 1;   /* shared across regions */
            if (idx < RR_MAX_PLAYERS) fill_entry(&g_stage[idx], p);
        }
    }
    return 0;
}

static int scan_window(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return 0;
    LONG before = g_scan_idx;          /* the shared counter is the tally */
    SCAN_CTX s[SCAN_THREADS];
    HANDLE th[SCAN_THREADS];
    uint64_t span = (hi - lo) / SCAN_THREADS;
    if (span < 0x10000) span = 0x10000;
    for (int i = 0; i < SCAN_THREADS; i++) {
        s[i].lo = lo + span * (uint64_t)i;
        s[i].hi = (i == SCAN_THREADS - 1) ? hi : lo + span * (uint64_t)(i + 1);
        s[i].count = 0;
        th[i] = CreateThread(NULL, 0, char_scan_thread, &s[i], 0, NULL);
    }
    WaitForMultipleObjects(SCAN_THREADS, th, TRUE, INFINITE);
    for (int i = 0; i < SCAN_THREADS; i++) CloseHandle(th[i]);
    return (int)(g_scan_idx - before);
}

static int cache_from_world(void);
static int cache_from_entities(void);
static uint64_t find_world_instance(void);
static DWORD WINAPI battle_scan_worker(LPVOID p);
static void cache_vehicles(void);
static void cache_loot(int kind);
static int views_updater_valid(uint64_t updater, uint64_t battle);

static int instance_scan(uint64_t lo, uint64_t hi) {
    if (!g_k_char) return 0;
    if (PL(g_world)) {
        /* Refresh the server -> simulated pairs before rebuilding the roster,
         * so entries pick up the interpolated transform and skeleton. */
        build_sim_map();
        /* The world dictionary is authoritative for identity and for the local
         * player. The entity-map roster (SimulatedModel) is only a fallback:
         * taking it first can lose the local player, which invalidates the
         * camera and hides every world-space ESP layer while the FOV circle
         * keeps drawing. */
        int wc = cache_from_world();
        if (wc < 0) wc = cache_from_entities();
        if (wc >= 0) return wc;
        if (wc == -2) {
            /* An incomplete read is not proof that the world was destroyed. */
            if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
            int count = g_ncache;
            if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
            return count;
        }
        /* A recycled world pointer must never turn into a heap-wide roster. */
        g_world = 0;
        if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
        g_ncache = 0;
        g_nvehicles = 0;
        if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
        return 0;
    }
    if (g_k_world) return 0; /* wait for the current world's roster */
    /* Authoritative path: the game's own roster dictionary. Object scanning is
     * only the fallback - it also finds pre-allocated, not-spawned characters,
     * which show up as markers on people who are not in the world. */
    int total = 0;
    if (g_nregions > 0) {
        int start = (int)((DWORD)InterlockedCompareExchange(&g_region_cursor, 0, 0) % (DWORD)g_nregions);
        DWORD deadline = GetTickCount() + SCAN_BUDGET_MS;
        InterlockedExchange(&g_scan_deadline, (LONG)deadline);
        for (int k = 0; k < g_nregions; k++) {
            int i = (start + k) % g_nregions;
            total += scan_window(g_regions[i].base, g_regions[i].base + g_regions[i].size);
            if ((DWORD)GetTickCount() >= deadline) {
                InterlockedExchange(&g_region_cursor, (LONG)((i + 1) % g_nregions));
                break;
            }
        }
        InterlockedExchange(&g_scan_deadline, 0);
    } else {
        total = scan_window(lo, hi);
    }

    int limit = (total < RR_MAX_PLAYERS) ? total : RR_MAX_PLAYERS;
    int n = 0;
    for (int i = 0; i < limit; i++) {
        if (!g_stage[i].obj) continue;
        if (n != i) g_stage[n] = g_stage[i];
        n++;
    }
    g_stage_n = n;

    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_cache, g_stage, sizeof(ENT_CACHE) * (size_t)n);
    g_ncache = n;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);

    if (n) g_anchor = g_cache[0].obj;
    return n;
}

static uint64_t find_camera_scan(void);

/* Refresh the roster on a worker thread so long scans do not block
 * position sampling.
 */
static DWORD WINAPI scan_worker(LPVOID p) {
    (void)p;
    DWORD last_full = GetTickCount();
    DWORD last_camera_scan = GetTickCount() - 5000;
    DWORD last_battle_scan = GetTickCount() - 30000;
    DWORD last_world_scan = GetTickCount() - 8000;
    DWORD last_loot_scan = GetTickCount() - 2000;
    DWORD last_markers_scan = GetTickCount() - 2000;
    DWORD last_hide_scan = GetTickCount() - 10000;
    DWORD last_entry_scan = GetTickCount() - 8000;
    DWORD last_entry_success = 0;
    int entry_seen = 0;
    for (;;) {
        /* The live world dictionary is cheap to read. Refresh it promptly so
         * characters entering replication range don't wait four seconds to
         * appear; keep the slower interval for the heap-scan fallback. */
        Sleep(PL(g_world) && g_ncache > 0 ? 500 : 4000);
        DWORD now = GetTickCount();
        /* Without a world anchor the fallback scan cannot produce a roster, so
         * run it periodically instead of on every pass: the entry_roots check
         * above must stay the fast path that notices a loading match. */
        int full = (now - last_full >= 40000) || (PL(g_world) && g_ncache == 0);

        if (now - last_entry_scan >= 8000) {
            ENTRY_ROOTS roots;
            last_entry_scan = now;
            if (entry_roots(&roots)) {
                last_entry_success = now;
                entry_seen = 1;
                if (roots.world != g_world) {
                    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
                    g_ncache = 0;
                    g_nvehicles = 0;
                    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
                    /* Poses and view mappings belong to the previous scene; a
                     * recycled object address must not reuse its old anchor. */
                    memset(g_motion, 0, sizeof(g_motion));
                    g_world = roots.world;
                    g_k_world = u_klass(roots.world);
                    InterlockedExchange64((volatile LONG64*)&g_views_updater, 0);
                    InterlockedExchange64((volatile LONG64*)&g_aim_updater, 0);
                    g_cam_obj = 0;
                    last_battle_scan = now - 30000;
                    full = 1;
                }
                InterlockedExchange64((volatile LONG64*)&g_battle, (LONG64)roots.battle);
                InterlockedExchange64((volatile LONG64*)&g_battle_model,(LONG64)roots.battle_model);
                if (roots.camera) g_cam_obj = roots.camera;
            }
        }

        /* Re-validate the class after a scene change; cheap when it holds. A
         * miss here is time-boxed (collect_hits) and retried next cycle, so a
         * cold discovery cannot stall the worker or the read thread. */
        if (!g_k_char || !class_named(g_k_char, "BattleCharacterModel")) {
            uint64_t k = resolve_class("BattleCharacterModel");
            if (k) { g_k_char = k; full = 1; }
        }

        /* World objects are recycled during scene changes. Stop publishing the
         * old roster and locate the new one before reading characters again. */
        uint64_t world_class = 0;
        int world_invalid = !PL(g_world);
        if (!world_invalid && rr_read(g_world, &world_class, sizeof(world_class)))
            world_invalid = world_class != g_k_world;
        if (g_k_world && world_invalid) {
            /* Keep the old address as the hint: find_world_instance() looks
             * near it first instead of sweeping the heap. */
            uint64_t was = g_world;
            if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
            g_ncache = 0;
            g_nvehicles = 0;
            if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
            memset(g_motion, 0, sizeof(g_motion));
            g_cam_obj = 0;
            g_world = 0;
            if (PL(was)) g_saved_world = was;
            find_heap_regions();
            g_world = find_world_instance();
            last_full = GetTickCount();
            full = 0;
        }

        if (full) {
            last_full = now;
            find_heap_regions();                 /* VAD walk + all regions */
            instance_scan(0, 0);
        } else if (g_ninst_regions > 0) {
            /* Fast path: only the regions that held instances last time. */
            g_scan_idx = 0;
            int total = 0;
            for (int i = 0; i < g_ninst_regions; i++)
                total += scan_window(g_inst_regions[i].base,
                                     g_inst_regions[i].base + g_inst_regions[i].size);
            int limit = (total < RR_MAX_PLAYERS) ? total : RR_MAX_PLAYERS;
            int n = 0;
            for (int i = 0; i < limit; i++) {
                if (!g_stage[i].obj) continue;
                if (n != i) g_stage[n] = g_stage[i];
                n++;
            }
            if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
            memcpy(g_cache, g_stage, sizeof(ENT_CACHE) * (size_t)n);
            g_ncache = n;
            if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
        } else {
            if (PL(g_world)) instance_scan(0, 0);
        }
        now = GetTickCount();
        if (PL(g_world) && now - last_hide_scan >= 500) {
            last_hide_scan = now;
            cache_hide_objects();
        }
        if (!g_ncache && g_k_world && now - last_world_scan >= 2000 &&
            (!entry_seen || now - last_entry_success >= 12000)) {
            last_world_scan = now;
            find_heap_regions();
            uint64_t current = find_world_instance();
            if (PL(current) && current != g_world) {
                g_world = current;
                if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
                g_nvehicles = 0;
                if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
                instance_scan(0, 0);
                last_battle_scan = GetTickCount() - 30000;
                last_camera_scan = GetTickCount() - 5000;
            }
        }
        int scan_vehicles = br_get()->vehicle_chams || br_get()->show_vehicle_markers;
#ifdef RR_PORT_DIAGNOSTICS
        scan_vehicles |= br_get()->show_vehicles;
#endif
        if (scan_vehicles) cache_vehicles();
        if ((br_get()->show_loot || br_get()->show_loot_nodes) && now - last_loot_scan >= 250) {
            last_loot_scan = now;
            if (br_get()->show_loot) cache_loot(1);
            if (br_get()->show_loot_nodes) cache_loot(2);
        }

        if ((br_get()->show_containers || br_get()->show_doors || br_get()->show_dropped_loot || br_get()->show_vehicle_loot) && now - last_markers_scan >= 250) {
            last_markers_scan = now;
            cache_world_markers();
        }

        /* Resolve the scene's slot table on the background worker, once per
         * world. Validate the world link whenever the cached object is used. */
        now = GetTickCount();
        if (PL(g_views_updater) && PL(g_battle) && !views_updater_valid(g_views_updater, g_battle)) {
            InterlockedExchange64((volatile LONG64*)&g_views_updater, 0);
            last_battle_scan = now - 5000;
        }
        if (PL(g_aim_updater) && (rr_u64(g_aim_updater + RR_OFF_AIM_BATTLE) != g_battle ||
            !class_named(rr_u64(g_aim_updater), "BattleAimTargetUpdater")))
            InterlockedExchange64((volatile LONG64*)&g_aim_updater, 0);
        if (PL(g_world) && (!PL(g_battle) || rr_u64(g_battle + RR_OFF_BATTLE_WORLD) != g_world ||
            !PL(g_views_updater) || !PL(g_aim_updater)) &&
            now - last_battle_scan >= 5000 &&
            InterlockedCompareExchange(&g_battle_scan_busy, 1, 0) == 0) {
            last_battle_scan = now;
            BATTLE_SCAN* scan = (BATTLE_SCAN*)malloc(sizeof(*scan));
            if (scan) {
                scan->world = g_world; scan->battle = g_battle; scan->nregions = g_nregions;
                memcpy(scan->regions, g_regions, sizeof(RR_HEAP) * (size_t)g_nregions);
                HANDLE thread = CreateThread(NULL, 0, battle_scan_worker, scan, 0, NULL);
                if (thread) CloseHandle(thread);
                else { free(scan); InterlockedExchange(&g_battle_scan_busy, 0); }
            } else InterlockedExchange(&g_battle_scan_busy, 0);
        }

        /* The active battle model owns the actual camera; a proximity scan can
         * otherwise select a dormant camera that happens to be nearby. */
        if (PL(g_views_updater) && PL(g_battle) &&
            views_updater_valid(g_views_updater, g_battle) && rr_u64(g_battle + RR_OFF_BATTLE_WORLD) == g_world) {
            uint64_t model = rr_u64(g_views_updater + RR_OFF_VIEWS_MODEL);
            uint64_t timing = rr_u64(model + 0x38);
            if (u_is(timing, "TimeModel")) {
                InterlockedExchange64(&g_time_model, (LONG64)timing);
                InterlockedExchange64(&g_time_world, (LONG64)g_world);
            }
            uint64_t scene_camera = rr_u64(model + RR_OFF_BATTLE_CAMERA);
            if (camera_validate(scene_camera)) g_cam_obj = scene_camera;
        }
        int camera_state = camera_near_local(g_cam_obj);
        now = GetTickCount();
        if (g_ncache && camera_state == 0 && now - last_camera_scan >= 5000) {
            last_camera_scan = now;
            find_heap_regions(); /* scene changes can allocate new regions */
            g_cam_obj = find_camera_scan();
            camera_state = camera_near_local(g_cam_obj);
            if (camera_state == 1) runtime_save();
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "characters=%d camera=%s ex=%ld [%s]", g_ncache,
                 camera_state == 1 ? "model" : "local",
                 (long)InterlockedCompareExchange(&g_ex_count, 0, 0),
                 g_dbg);
        set_status("%s", msg);
    }
    return 0;
}

/* Camera scan. */

typedef struct { uint64_t lo, hi; volatile LONG found; uint64_t addr; } CAM_CTX;

static DWORD WINAPI cam_scan_thread(LPVOID param) {
    CAM_CTX* c = (CAM_CTX*)param;
    static __declspec(thread) uint8_t buf[0x10000];
    for (uint64_t a = c->lo; a < c->hi; a += sizeof(buf)) {
        if (InterlockedCompareExchange(&c->found, 0, 0)) return 0;
        size_t n = (size_t)((c->hi - a) < sizeof(buf) ? (c->hi - a) : sizeof(buf));
        if (!rr_read(a, buf, n)) continue;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t v;
            memcpy(&v, buf + i, 8);
            if (v != g_k_cam) continue;
            uint64_t p = a + i;
            if (camera_validate(p)) { c->addr = p; InterlockedExchange(&c->found, 1); return 0; }
        }
    }
    return 0;
}

static uint64_t find_camera_scan(void) {
    if (!g_k_cam) return 0;

    /* Use distance from the local player as a camera-selection heuristic to
     * reduce matches on dormant preview cameras.
     */
    float lp[3];
    int have_lp = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; i++) {
        if (!g_cache[i].local) continue;
        if (rr_read(g_cache[i].tr + RR_OFF_CTM_X, lp, sizeof(lp))) have_lp = 1;
        break;
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (!have_lp) return 0;

    uint64_t best = 0;
    float best_d = 1e30f;
    static uint8_t buf[0x10000];
    /* The live camera usually shares the active world's heap neighborhood.
     * Search it first; fall back to all regions when it is elsewhere. */
    for (int pass = 0; pass < 2; ++pass) {
      uint64_t radius = 512ULL << 20;
      uint64_t near_lo = g_world > radius ? g_world - radius : 0;
      uint64_t near_hi = g_world + radius;
      for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        if (pass == 0 && PL(g_world)) {
            if (lo < near_lo) lo = near_lo;
            if (hi > near_hi) hi = near_hi;
            if (lo >= hi) continue;
        } else if (pass == 0) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != g_k_cam) continue;
                uint64_t c = a + i;
                if (!camera_validate(c)) continue;
                float cp[3];
                if (!rr_read(c + RR_OFF_CAM_POS, cp, sizeof(cp))) continue;
                float d;
                if (have_lp) {
                    float dx = cp[0] - lp[0], dy = cp[1] - lp[1], dz = cp[2] - lp[2];
                    d = sqrtf(dx * dx + dy * dy + dz * dz);
                } else {
                    d = 0.0f;              /* no local entity: first valid wins */
                }
                if (d < best_d) { best_d = d; best = c; }
            }
        }
      }
      if (best && best_d < 30.0f) break;
    }
    if (best_d >= 30.0f) best = 0;
    if (best) snprintf(g_dbg, sizeof(g_dbg), "cam=%llX d=%.1f", (unsigned long long)best, best_d);
    return best;
}

/* World/dictionary. */
/* The authoritative roster is WorldEntitiesModel._battleCharacters (+0x18).
 * Scanning for character objects also finds the pre-allocated "not spawned"
 * set, which reads as markers on people who are not in the world. */

static int dict_looks_valid(uint64_t dict) {
    if (!PL(dict)) return 0;
    uint64_t k = u_klass(dict);
    if (!PL(k)) return 0;
    char nm[64];
    if (!u_class_name(k, nm, sizeof(nm))) return 0;
    return strstr(nm, "Dictionary") != NULL;
}

/* Read a class's field table and look for a field name. Il2CppClass.fields is
 * at +0x80, the field count is the low word at +0x120 and FieldInfo has a 0x20
 * stride with the name at +0x00. This identifies a class from metadata alone. */
static int class_has_field(uint64_t klass, const char* want) {
    if (!PL(klass)) return 0;
    uint64_t fields = rr_u64(klass + 0x80);
    if (!PL(fields)) return 0;
    int count = (int)(rr_u32(klass + 0x120) & 0xFFFFu);
    if (count <= 0 || count > 512) count = 128;
    for (int i = 0; i < count; ++i) {
        uint64_t np = rr_u64(fields + (uint64_t)i * 0x20);
        if (!PL(np)) break;
        char name[96] = {0};
        if (rr_cstr(np, name, sizeof(name)) <= 0) break;
        unsigned char c0 = (unsigned char)name[0];
        if (c0 < 0x20 || c0 > 0x7E) break;
        /* Property fields appear as compiler-generated backing fields such as
         * "<Transform>k__BackingField", so match the stem rather than the exact
         * name. */
        if (strstr(name, want)) return 1;
    }
    return 0;
}

static int probe_world_class(uint64_t klass) {
    /* Static metadata check instead of hunting for an instance. The previous
     * full-address-space sweep took tens of seconds on a cold process and ran
     * once per candidate class. The field names uniquely identify the model. */
    char nm[128] = {0};
    u_class_name(klass, nm, sizeof(nm));
    int name_ok = _stricmp(nm, "WorldEntitiesModel") == 0;
    int battles = class_has_field(klass, "_battleCharacters");
    int others = class_has_field(klass, "_vehicles") || class_has_field(klass, "_loots");
    if (!(name_ok && battles && others)) {
        dlog("probe_world reject 0x%llX name='%s' fields=0x%llX count=%d bc=%d o=%d",
             (unsigned long long)klass, nm, (unsigned long long)rr_u64(klass + 0x80),
             (int)(rr_u32(klass + 0x120) & 0xFFFFu), battles, others);
        return 0;
    }
    return 1;
}

static uint64_t resolve_world_class(void) {
    collect_hits("WorldEntitiesModel");
    dlog("resolve_world hits=%ld", (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS));
    if (!g_nhits) return 0;
    collect_candidates("WorldEntitiesModel");
    dlog("resolve_world cands=%d", g_ncands);
    for (int i = 0; i < g_ncands; i++)
        if (probe_world_class(g_cands[i])) return g_cands[i];
    dlog("resolve_world no candidate accepted");
    return 0;
}

/* Scan one address range for a world instance. Split out so the hinted pass
 * below reuses exactly the same validation rather than keeping a second copy. */
/* Overall deadline for one world resolution attempt. Each stage shares it, so
 * a scene change cannot hold the worker through several full sweeps. */
static DWORD g_world_scan_deadline = 0;
/* Next address of the fallback sweep, so attempts resume instead of restarting. */
static uint64_t g_world_scan_next = 0;
/* Region-based fallback: address gaps between regions (including the DLL space
 * near the top of the address range) would otherwise be walked 64 MB at a time
 * and the sweep would never reach the game's heaps. */
static volatile LONG g_world_region_cursor = 0;
static uint64_t      g_world_slice_off = 0;

typedef struct {
    volatile LONG stop;
    volatile LONG found;
    uint64_t      result;
    volatile LONG best_live;
    uint64_t      best;
    DWORD         deadline;
} WORLD_SCAN;

typedef struct { WORLD_SCAN* shared; uint64_t lo, hi; } WORLD_RANGE;

static DWORD WINAPI world_scan_thread(LPVOID arg) {
    WORLD_RANGE* r = (WORLD_RANGE*)arg;
    WORLD_SCAN* s = r->shared;
    static __declspec(thread) uint8_t buf[0x10000];
    for (uint64_t a = r->lo; a < r->hi; a += sizeof(buf)) {
        if (InterlockedCompareExchange(&s->stop, 0, 0)) break;
        if (s->deadline && (DWORD)GetTickCount() > s->deadline) break;
        size_t sz = (size_t)((r->hi - a) < sizeof(buf) ? (r->hi - a) : sizeof(buf));
        if (!rr_read(a, buf, sz)) continue;
        for (size_t i = 0; i + 8 <= sz; i += 8) {
            uint64_t v;
            memcpy(&v, buf + i, 8);
            if (v != g_k_world) continue;
            uint64_t w = a + i;
            uint64_t dict = rr_u64(w + RR_OFF_WEM_BATTLE_CHARS);
            if (!dict_looks_valid(dict)) continue;
            int n = 0, stride = 0, live = 0, has_local = 0;
            uint64_t entries = 0;
            if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) continue;
            for (int k = 0; k < n; ++k) {
                uint64_t obj = rr_u64(entries + (uint64_t)k * stride + 0x10);
                if (!PL(obj) || rr_u64(obj) != g_k_char) continue;
                ++live;
                if (PL(rr_u64(obj + RR_OFF_CHAR_PBM))) has_local = 1;
            }
            if (has_local) {
                if (InterlockedCompareExchange(&s->found, 1, 0) == 0) s->result = w;
                InterlockedExchange(&s->stop, 1);
                return 0;
            }
            if (live > s->best_live) { s->best_live = live; s->best = w; }
        }
    }
    return 0;
}

/* Search one range in parallel. Only a world with a confirmed local player is
 * accepted, matching the previous behaviour; the deadline keeps a scene change
 * from blocking the worker for the whole heap span. */
static uint64_t world_scan_range(uint64_t lo, uint64_t hi) {
    if (!g_k_world || hi <= lo) return 0;
    WORLD_SCAN shared;
    memset(&shared, 0, sizeof(shared));
    shared.deadline = g_world_scan_deadline ? g_world_scan_deadline
                                            : GetTickCount() + 2500;

    for (int r = 0; r < g_nregions && !shared.found; r++) {
        if (g_regions[r].size < 0x1000) continue;
        uint64_t rlo = g_regions[r].base, rhi = rlo + g_regions[r].size;
        if (rhi <= lo || rlo >= hi) continue;
        uint64_t a0 = (rlo > lo ? rlo : lo) & ~(uint64_t)7;
        uint64_t a1 = rhi < hi ? rhi : hi;
        if (a1 <= a0) continue;

        if (a1 - a0 < 0x200000) {
            WORLD_RANGE one = { &shared, a0, a1 };
            world_scan_thread(&one);
            continue;
        }
        uint64_t span = (a1 - a0) / SCAN_THREADS;
        if (span < 0x10000) span = 0x10000;
        WORLD_RANGE parts[SCAN_THREADS];
        HANDLE th[SCAN_THREADS];
        for (int i = 0; i < SCAN_THREADS; i++) {
            parts[i].shared = &shared;
            parts[i].lo = a0 + span * (uint64_t)i;
            parts[i].hi = (i == SCAN_THREADS - 1) ? a1 : a0 + span * (uint64_t)(i + 1);
            th[i] = CreateThread(NULL, 0, world_scan_thread, &parts[i], 0, NULL);
        }
        WaitForMultipleObjects(SCAN_THREADS, th, TRUE, INFINITE);
        for (int i = 0; i < SCAN_THREADS; i++) CloseHandle(th[i]);
    }
    if (shared.found) return shared.result;
    return 0;
}

static uint64_t find_world_instance(void) {
    if (!g_k_world) return 0;
    /* One attempt is bounded so the scan worker keeps publishing; a missed
     * scene is retried on the next cycle instead of stalling the overlay. */
    g_world_scan_deadline = GetTickCount() + 3000;
    /* Only trust the saved address when it came from this same process: after
     * a game restart it is a stale pointer into a dead heap and the window
     * sweep would be wasted work. */
    uint64_t hint = PL(g_world) ? g_world
                                : (g_saved_pid == g_rr_pid ? g_saved_world : 0);
    if (PL(hint)) {
        const uint64_t window = 0x08000000ull;   /* 128 MB either side */
        uint64_t lo = hint > window ? hint - window : 0;
        uint64_t found = world_scan_range(lo, hint + window);
        if (found) return found;

        /* Arena walk: the replacement scene usually lands in the same or an
         * adjacent 256 MB arena rather than anywhere in the address space. */
        const uint64_t arena = 0x10000000ull;
        uint64_t base = hint & ~(arena - 1);
        for (int ring = 1; ring <= 4; ++ring) {
            if ((DWORD)GetTickCount() > g_world_scan_deadline) return 0;
            for (int dir = -1; dir <= 1; dir += 2) {
                uint64_t a = base + (uint64_t)dir * (uint64_t)ring * arena;
                if (a < 0x10000) continue;
                found = world_scan_range(a, a + arena);
                if (found) return found;
            }
        }
    }
    if ((DWORD)GetTickCount() > g_world_scan_deadline) return 0;
    uint64_t lo = ~(uint64_t)0, hi = 0;
    for (int r = 0; r < g_nregions; r++) {
        if (g_regions[r].size < 0x1000) continue;
        if (g_regions[r].base < lo) lo = g_regions[r].base;
        uint64_t end = g_regions[r].base + g_regions[r].size;
        if (end > hi) hi = end;
    }
    if (hi <= lo) return 0;

    /* Resume the region sweep where the previous attempt stopped. Only mapped
     * regions are visited, and each is scanned in 64 MB slices so a large heap
     * cannot blow the budget. */
    (void)lo; (void)hi;
    if (g_nregions <= 0) return 0;
    int start = (int)((DWORD)InterlockedCompareExchange(&g_world_region_cursor, 0, 0)
                      % (DWORD)g_nregions);
    uint64_t off = g_world_slice_off;
    for (int n = 0; n < g_nregions; n++) {
        int idx = (start + n) % g_nregions;
        uint64_t base = g_regions[idx].base, size = g_regions[idx].size;
        if (size < 0x1000) continue;
        if (idx != start) off = 0;
        while (off < size) {
            uint64_t b = off + 0x4000000ULL;    /* 64 MB slice */
            if (b > size) b = size;
            uint64_t found = world_scan_range(base + off, base + b);
            if (found) {
                InterlockedExchange(&g_world_region_cursor, 0);
                g_world_slice_off = 0;
                return found;
            }
            if ((DWORD)GetTickCount() > g_world_scan_deadline) {
                InterlockedExchange(&g_world_region_cursor, idx);
                g_world_slice_off = off;        /* retry this slice next time */
                return 0;
            }
            off = b;
            g_world_slice_off = off;
        }
        off = 0;
        g_world_slice_off = 0;
        InterlockedExchange(&g_world_region_cursor, (LONG)((idx + 1) % g_nregions));
    }
    InterlockedExchange(&g_world_region_cursor, 0);
    g_world_slice_off = 0;
    return 0;
}

/* Fill the cache from the roster dictionary. */
/* Battle scene entities. */
/* BattleEntitiesModel is the scene-level container: it holds the entity map at
 * +0x50, a reference to the world entities model at +0x60 and the local player
 * at +0xD0. Reading that map avoids depending on which WorldEntitiesModel
 * instance happens to be current. */
static uint64_t g_k_battle = 0;

static int views_updater_valid(uint64_t updater, uint64_t battle) {
    if (!PL(updater) || !PL(battle) || rr_u64(updater + RR_OFF_VIEWS_BATTLE) != battle ||
        !class_named(rr_u64(updater), "BattleEntitiesUpdater")) return 0;
    uint64_t model=rr_u64(updater+RR_OFF_VIEWS_MODEL);
    if (!PL(model) || !class_named(rr_u64(model), "BattleModel") || rr_u64(model+RR_OFF_BATTLE_ENTITIES)!=battle) return 0;
    uint64_t aim=rr_u64(model+RR_OFF_BATTLE_AIM);
    return PL(aim) && class_named(rr_u64(aim), "AimTargetModel");
}

typedef struct { uint64_t needle, world, result; int updater; volatile LONG found; } SCENE_SEARCH;
typedef struct { SCENE_SEARCH* shared; uint64_t lo, hi; } SCENE_RANGE;
static DWORD WINAPI scene_search_thread(LPVOID arg) {
    SCENE_RANGE* range=(SCENE_RANGE*)arg; SCENE_SEARCH* q=range->shared;
    static __declspec(thread) uint8_t buf[0x10000];
    for (uint64_t a=range->lo; a<range->hi; a+=sizeof(buf)) {
        if (InterlockedCompareExchange(&q->found,0,0) || q->world!=g_world) break;
        size_t n=(size_t)((range->hi-a)<sizeof(buf)?range->hi-a:sizeof(buf));
        if (!rr_read(a,buf,n)) continue;
        for (size_t i=0;i+8<=n;i+=8) {
            uint64_t value; memcpy(&value,buf+i,8);
            uint64_t off=q->updater==2?RR_OFF_AIM_BATTLE:(q->updater?RR_OFF_VIEWS_BATTLE:RR_OFF_BATTLE_WORLD);
            if (value!=q->needle || a+i<off) continue;
            uint64_t obj=a+i-off;
            int valid=q->updater==2 ? (class_named(rr_u64(obj),"BattleAimTargetUpdater") &&
                                      class_named(rr_u64(rr_u64(obj+RR_OFF_AIM_BRAIN)),"CinemachineBrain"))
                : q->updater ? (views_updater_valid(obj,q->needle) && PL(rr_u64(obj+RR_OFF_VIEWS_CHARACTERS)))
                : (class_named(rr_u64(obj),"BattleEntitiesModel") && PL(rr_u64(obj+RR_OFF_BATTLE_SLOTS)));
            if (valid && q->world==g_world && InterlockedCompareExchange(&q->found,1,0)==0) { q->result=obj; return 0; }
        }
    }
    return 0;
}
static uint64_t scene_search(const BATTLE_SCAN* scan, uint64_t needle, int updater) {
    if (!PL(needle)) return 0;
    for (int pass=0;pass<2;++pass) for (int r=0;r<scan->nregions;++r) {
        uint64_t lo=scan->regions[r].base, hi=lo+scan->regions[r].size;
        uint64_t nearlo=needle>0x40000000ULL?needle-0x40000000ULL:0, nearhi=needle+0x40000000ULL;
        if (pass==0) { if (lo<nearlo) lo=nearlo; if (hi>nearhi) hi=nearhi; }
        else if (lo>=nearlo && hi<=nearhi) continue;
        if (hi<=lo || hi-lo<8) continue;
        SCENE_SEARCH q={0}; q.needle=needle; q.world=scan->world; q.updater=updater;
        if (hi-lo < 0x100000) {
            SCENE_RANGE one={&q,lo,hi}; scene_search_thread(&one);
            if (q.result) return q.result;
            if (scan->world!=g_world) return 0;
            continue;
        }
        SCENE_RANGE ranges[SCAN_THREADS]; HANDLE threads[SCAN_THREADS]={0};
        uint64_t words=(hi-lo)/8;
        for (int i=0;i<SCAN_THREADS;++i) {
            ranges[i].shared=&q; ranges[i].lo=lo+(words*(uint64_t)i/SCAN_THREADS)*8;
            ranges[i].hi=lo+(words*(uint64_t)(i+1)/SCAN_THREADS)*8;
            threads[i]=CreateThread(NULL,0,scene_search_thread,&ranges[i],0,NULL);
            if (!threads[i]) scene_search_thread(&ranges[i]);
        }
        for (int i=0;i<SCAN_THREADS;++i) if (threads[i]) { WaitForSingleObject(threads[i],INFINITE); CloseHandle(threads[i]); }
        if (q.result) return q.result;
        if (scan->world!=g_world) return 0;
    }
    return 0;
}
static uint64_t find_battle_for_world(const BATTLE_SCAN* scan) { return scene_search(scan,scan->world,0); }
static uint64_t find_views_updater(const BATTLE_SCAN* scan, uint64_t battle) { return scene_search(scan,battle,1); }

static DWORD WINAPI battle_scan_worker(LPVOID p) {
    BATTLE_SCAN* scan = (BATTLE_SCAN*)p;
    uint64_t battle = scan->battle;
    if (!PL(battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != scan->world)
        battle = find_battle_for_world(scan);
    if (battle && scan->world == g_world) {
        InterlockedExchange64((volatile LONG64*)&g_battle, (LONG64)battle);
        uint64_t updater = find_views_updater(scan, battle);
        if (updater && scan->world == g_world && battle == g_battle)
            InterlockedExchange64((volatile LONG64*)&g_views_updater, (LONG64)updater);
        uint64_t aim = scene_search(scan, battle, 2);
        if (aim && scan->world == g_world && battle == g_battle)
            InterlockedExchange64((volatile LONG64*)&g_aim_updater, (LONG64)aim);
    }
    free(scan);
    InterlockedExchange(&g_battle_scan_busy, 0);
    return 0;
}

/* Collect the local player's party/group member account ids (the keys of
 * PartyModel._actualPartyMemberModels, a Dictionary<long, PartyMemberModel>).
 * Read-only; returns 0 (no party) if any link is missing, which simply means
 * nobody is treated as a group member. */
static int collect_party_ids(int64_t* ids, int max) {
    int cnt = 0;
    uint64_t bm = g_battle_model;
    if (!PL(bm)) return 0;
    uint64_t access = rr_u64(bm + RR_OFF_BATTLE_PARTY_ACCESS);
    uint64_t party  = PL(access) ? rr_u64(access + RR_OFF_PARTY_ACCESS_MODEL) : 0;
    uint64_t dict   = PL(party)  ? rr_u64(party + RR_OFF_PARTY_MEMBERS) : 0;
    int n = 0, stride = 0; uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n < 0 || n > 4096 || stride != 0x18) return 0;
    for (int i = 0; i < n && cnt < max; ++i) {
        uint8_t e[0x18];
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof(e))) continue;
        int64_t key = 0; uint64_t val = 0;
        memcpy(&key, e + 8, 8); memcpy(&val, e + 0x10, 8);
        if (!PL(val) || key == 0) continue;
        ids[cnt++] = key;
    }
    return cnt;
}

/* Match both entity IDs before using the roster identity. */
static void cache_roster_identity(ENT_CACHE* stage, int count) {
    uint64_t battle = g_battle;
    if (!PL(battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != g_world) return;
    uint64_t slots = rr_u64(battle + RR_OFF_BATTLE_SLOTS);
    uint64_t dict = PL(slots) ? rr_u64(slots + 0x18) : 0;
    int n = 0, stride = 0; uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) return;
    int64_t party_ids[RR_MAX_PARTY];
    int nparty = collect_party_ids(party_ids, RR_MAX_PARTY);
    for (int j = 0; j < count; ++j) stage[j].is_party = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t e[0x18];
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof(e))) continue;
        int32_t key; uint64_t slot;
        memcpy(&key, e + 8, 4); memcpy(&slot, e + 0x10, 8);
        if (!PL(slot)) continue;
        int j; for (j = 0; j < count && stage[j].id != key; ++j) {}
        if (j == count) continue;
        uint64_t data = rr_u64(slot + 0x10);
        uint64_t idref = PL(data) ? rr_u64(data + 0x20) : 0;
        int32_t id = -1;
        if (!PL(idref) || !rr_read(idref + 0x18, &id, sizeof(id)) || id != key) continue;
        uint64_t ref = rr_u64(data + 0x30); /* BattleCharacterSlotData.Nickname */
        uint64_t str = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
        if (PL(str)) u_string(str, stage[j].name, sizeof(stage[j].name));
        ref = rr_u64(data + 0x38); /* BattleCharacterSlotData.ClanTag */
        str = PL(ref) ? rr_u64(ref + RR_OFF_REF_VALUE) : 0;
        if (PL(str)) u_string(str, stage[j].clan_tag, sizeof(stage[j].clan_tag));
        /* Tag group members so the aim can skip them. */
        if (nparty > 0) {
            uint64_t pref = rr_u64(data + RR_OFF_SLOT_PUBLICID);
            int64_t pid = 0;
            if (PL(pref) && rr_read(pref + RR_OFF_PRIM_VALUE, &pid, sizeof(pid)) && pid != 0) {
                for (int k = 0; k < nparty; ++k)
                    if (party_ids[k] == pid) { stage[j].is_party = 1; break; }
            }
        }
    }
}

/* Link each roster ID to its character view and the four animated transforms
 * explicitly exposed by the head and torso view components. */
static void cache_mesh_views(ENT_CACHE* stage, int count) {
    static ENT_CACHE previous[RR_MAX_PLAYERS];
    int previous_count=0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    previous_count=g_ncache;
    if (previous_count>0) memcpy(previous,g_cache,(size_t)previous_count*sizeof(previous[0]));
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    uint64_t updater = g_views_updater, battle = g_battle;
    if (!views_updater_valid(updater, battle) || rr_u64(battle + RR_OFF_BATTLE_WORLD) != g_world) goto retain;
    uint64_t dict = rr_u64(updater + RR_OFF_VIEWS_CHARACTERS), entries = 0;
    int n = 0, stride = 0;
    if (!u_dict(dict, &n, &entries, &stride) || n > 4096 || stride != 0x18) goto retain;
    for (int i = 0; i < n; ++i) {
        uint8_t e[0x18]; int32_t key; uint64_t view;
        if (!rr_read(entries + (uint64_t)i * 0x18, e, sizeof(e))) continue;
        memcpy(&key, e + 8, 4); memcpy(&view, e + 0x10, 8);
        if (!PL(view)) continue;
        int j; for (j = 0; j < count && stage[j].id != key; ++j) {}
        if (j == count || !class_named(rr_u64(view), "BattleCharacterEntityView")) continue;
        uint64_t anim = rr_u64(view + 0x28);
        uint64_t head = PL(anim) ? rr_u64(anim + 0x28) : 0;
        uint64_t torso = PL(anim) ? rr_u64(anim + 0x30) : 0;
        if (!PL(head) || !PL(torso)) continue;
        uint64_t transforms[5] = { rr_u64(head + 0x28), rr_u64(head + 0x20),
                                   rr_u64(torso + 0x30), rr_u64(torso + 0x28), rr_u64(torso + 0x20) };
        uint64_t hierarchy = 0, natives[5]; int indices[5]; int ok = 1;
        for (int k = 0; k < 5; ++k) {
            uint64_t native = PL(transforms[k]) ? rr_u64(transforms[k] + 0x10) : 0;
            uint64_t h = PL(native) ? rr_u64(native + 0x28) : 0;
            int idx = PL(native) ? rr_i32(native + 0x30) : -1;
            int capacity=PL(h)?rr_i32(h+0x10):0;
            uint64_t pointers=PL(h)?rr_u64(h+0x30):0;
            if (!PL(h) || (k && h != hierarchy) || capacity<1 || capacity>2048 || idx<0 || idx>=capacity ||
                !PL(pointers) || rr_u64(pointers+(uint64_t)idx*8)!=native) { ok = 0; break; }
            hierarchy = h; indices[k] = idx; natives[k]=native;
        }
        if (ok) {
            stage[j].mesh_h = hierarchy; memcpy(stage[j].mesh_idx, indices, sizeof(indices));
            memcpy(stage[j].mesh_native,natives,sizeof(natives));
            stage[j].mesh_view_at = GetTickCount();
            if ((br_get()->show_skeleton || br_get()->cham_on || br_get()->aa_on) && (!stage[j].local || br_get()->show_self)) {
                const ENT_CACHE* old=NULL;
                for (int q=0;q<previous_count;++q)
                    if (previous[q].obj==stage[j].obj && previous[q].mesh_h==hierarchy && previous[q].cham_ready) { old=&previous[q]; break; }
                cache_cham_rig(&stage[j],old);
            }
        }
    }
retain:
    /* A partial dictionary/rig refresh should not make a rendered character
     * jump back to its 20 Hz network position. Retain only a recent mapping
     * for the same object and verify all native slots before reusing it. */
    for (int j=0; j<count; ++j) {
        if (stage[j].mesh_h && stage[j].cham_ready) continue;
        for (int q=0; q<previous_count; ++q) {
            const ENT_CACHE* old=&previous[q];
            if (old->obj!=stage[j].obj || old->id!=stage[j].id ||
                !old->mesh_view_at || GetTickCount()-old->mesh_view_at>1500 || !PL(old->mesh_h)) continue;
            if (stage[j].mesh_h && (stage[j].mesh_h!=old->mesh_h ||
                stage[j].mesh_native[0]!=old->mesh_native[0])) break;
            int capacity=rr_i32(old->mesh_h+0x10);
            uint64_t slots=rr_u64(old->mesh_h+0x30);
            if (capacity<1 || capacity>2048 || !PL(slots)) break;
            int valid=1;
            for (int k=0; k<5; ++k) {
                int idx=old->mesh_idx[k];
                if (idx<0 || idx>=capacity || !PL(old->mesh_native[k]) ||
                    rr_u64(slots+(uint64_t)idx*8)!=old->mesh_native[k]) { valid=0; break; }
            }
            if (!valid) break;
            if (!stage[j].mesh_h) {
                stage[j].mesh_h=old->mesh_h;
                memcpy(stage[j].mesh_idx,old->mesh_idx,sizeof(old->mesh_idx));
                memcpy(stage[j].mesh_native,old->mesh_native,sizeof(old->mesh_native));
                stage[j].mesh_view_at=old->mesh_view_at;
            }
            if (old->cham_ready) {
                /* The five torso anchors cannot validate the other 89 joints.
                 * Recheck the complete palette before retaining a full rig. */
                ENT_CACHE validated=*old;
                validated.cham_ready=0;
                cache_cham_rig(&validated,old);
                if (validated.cham_ready) {
                    memcpy(stage[j].cham_idx,validated.cham_idx,sizeof(validated.cham_idx));
                    memcpy(stage[j].cham_native,validated.cham_native,sizeof(validated.cham_native));
                    stage[j].mesh_first=validated.mesh_first; stage[j].mesh_span=validated.mesh_span;
                    stage[j].cham_ready=1;
                }
            }
            break;
        }
    }
}

static int probe_battle_class(uint64_t klass) {
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != klass) continue;
                uint64_t obj = a + i;
                if (dict_looks_valid(rr_u64(obj + RR_OFF_BATTLE_MODELS))) return 1;
                uint64_t w = rr_u64(obj + RR_OFF_BATTLE_WORLD);
                if (PL(w) && dict_looks_valid(rr_u64(w + RR_OFF_WEM_BATTLE_CHARS))) return 1;
            }
        }
    }
    return 0;
}

static uint64_t resolve_battle_class(void) {
    collect_hits("BattleEntitiesModel");
    if (!g_nhits) return 0;
    collect_candidates("BattleEntitiesModel");
    for (int i = 0; i < g_ncands; i++)
        if (probe_battle_class(g_cands[i])) return g_cands[i];
    return 0;
}

static uint64_t find_battle_instance(void) {
    if (!g_k_battle) return 0;
    uint64_t best = 0;
    LONG best_n = -1;
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != g_k_battle) continue;
                uint64_t b = a + i;
                uint64_t dict = rr_u64(b + RR_OFF_BATTLE_MODELS);
                if (!dict_looks_valid(dict)) continue;
                LONG n = rr_i32(dict + 0x20);
                if (n > best_n) { best_n = n; best = b; }
            }
        }
    }
    return best;
}

/* Fill from BattleEntitiesModel._models (+0x50). The map holds entity models;
 * each BattleCharacterEntityModel exposes its client-interpolated
 * SimulatedModel at +0x30, which is the copy the renderer draws and therefore
 * moves smoothly between replication ticks. Older layouts put the character
 * model in the map directly, which is still accepted.
 * Returns the entry count, or -1 when the map is not usable so the caller can
 * fall back to the world dictionary. */
static int cache_from_entities(void) {
    if (!PL(g_battle) || !g_k_char || !PL(g_world)) return -1;
    uint64_t battle_klass = rr_u64(g_battle);
    if (!battle_klass || !class_named(battle_klass, "BattleEntitiesModel")) return -1;
    uint64_t world_ref = rr_u64(g_battle + RR_OFF_BATTLE_WORLD);
    if (PL(world_ref) && world_ref != g_world) return -1;

    uint64_t dict = rr_u64(g_battle + RR_OFF_BATTLE_MODELS);
    if (!PL(dict)) return -1;
    int n = 0, stride = 0;
    uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride)) return -1;
    if (n <= 0 || n > 4096 || stride < 0x18) return -1;

    static uint8_t buf[0x10000];
    size_t bytes = (size_t)n * (size_t)stride;
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    int per = (int)(bytes / (size_t)stride);
    if (per <= 0) return -1;
    if (!rr_read(entries, buf, bytes)) return -1;

    int count = 0;
    for (int i = 0; i < per && count < RR_MAX_PLAYERS; i++) {
        uint64_t obj = 0;
        memcpy(&obj, buf + (size_t)i * (size_t)stride + 0x10, 8);
        if (!PL(obj)) continue;

        uint64_t model = 0;
        uint64_t klass = rr_u64(obj);
        if (klass == g_k_char) {
            model = obj;                       /* character model in the map */
        } else if (klass && class_named(klass, "BattleCharacterEntityModel")) {
            uint64_t sim = rr_u64(obj + RR_OFF_ENTITY_SIMULATED);
            if (PL(sim) && rr_u64(sim) == g_k_char) model = sim;
        }
        if (!model) continue;

        ENT_CACHE tmp;
        fill_entry(&tmp, model);
        if (!tmp.obj) continue;

        int dup = 0;
        for (int k = 0; k < count; ++k) if (g_stage[k].obj == tmp.obj) { dup = 1; break; }
        if (dup) continue;
        g_stage[count++] = tmp;
    }
    if (count == 0) return -1;

    cache_roster_identity(g_stage, count);
    cache_mesh_views(g_stage, count);
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_cache, g_stage, sizeof(ENT_CACHE) * (size_t)count);
    g_ncache = count;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (count) g_anchor = g_cache[0].obj;
    return count;
}

static int cache_from_world(void) {
    if (!PL(g_world) || !g_k_world) return -1;
    uint64_t klass = 0, dict = 0;
    if (!rr_read(g_world, &klass, sizeof(klass))) return -2;
    if (klass != g_k_world) return -1;
    if (!rr_read(g_world + RR_OFF_WEM_BATTLE_CHARS, &dict, sizeof(dict))) return -2;
    /* The game can tear down a world without recycling its class word. A
     * successfully read null roster is not a transient read failure. */
    if (!PL(dict)) return -1;
    uint64_t dict_class = 0;
    if (!rr_read(dict, &dict_class, sizeof(dict_class))) return -2;
    if (!PL(dict_class)) return -1;
    char dict_name[64];
    if (!u_class_name(dict_class, dict_name, sizeof(dict_name))) return -2;
    if (!strstr(dict_name, "Dictionary")) return -1;
    int used = 0;
    if (!rr_read(dict + U_DICT_COUNT, &used, sizeof(used)) || used < 0 || used > 4096) return -2;
    if (used == 0) {
        if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
        g_ncache = 0;
        if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
        return 0;
    }
    int n = 0, stride = 0;
    uint64_t entries = 0;
    if (!u_dict(dict, &n, &entries, &stride)) return -2;
    if (n <= 0 || n > 4096 || stride < 0x18) return -2;

    static uint8_t buf[0x10000];
    size_t bytes = (size_t)n * (size_t)stride;
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    int per = (int)(bytes / (size_t)stride);
    if (per <= 0) return -2;
    if (!rr_read(entries, buf, bytes)) return -2;

    int count = 0;
    for (int i = 0; i < per && count < RR_MAX_PLAYERS; i++) {
        uint64_t obj = 0;
        memcpy(&obj, buf + (size_t)i * (size_t)stride + 0x10, 8);
        if (!PL(obj)) continue;
        ENT_CACHE tmp;
        fill_entry(&tmp, obj);
        if (!tmp.obj) continue;
        g_stage[count++] = tmp;
    }

    cache_roster_identity(g_stage, count);
    cache_mesh_views(g_stage, count);
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_cache, g_stage, sizeof(ENT_CACHE) * (size_t)count);
    g_ncache = count;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (count) g_anchor = g_cache[0].obj;
    return count;
}

/* Read world collections independently. Each position is validated against its
 * managed type so a stale world cannot turn arbitrary memory into a marker. */
static void cache_world_markers(void) {
    static const struct { unsigned dict_off, pos_off; int kind; const char *klass, *name; } sources[] = {
        {RR_OFF_WEM_CONTAINERS, RR_OFF_CONTAINER_POS, 3, "ContainerModel", "Container"},
        {RR_OFF_WEM_DOORS, RR_OFF_DOOR_POS, 4, "DoorModel", "Door"},
        {RR_OFF_WEM_DROPPED_LOOT, RR_OFF_DROPPED_LOOT_POS, 5, "LootModel", "Dropped loot"},
        {RR_OFF_WEM_VEHICLE_LOOT, RR_OFF_VEHICLE_LOOT_POS, 6, "VehicleLootModel", "Vehicle loot"}
    };
    LOOT_CACHE staged[RR_MAX_WORLD_MARKERS];
    float distance2[RR_MAX_WORLD_MARKERS]; RR_V3 origin = {0, 0, 0};
    int total = 0; uint64_t world = g_world;
    if (!PL(world) || rr_u64(world) != g_k_world) return;
    uint64_t camera = g_cam_obj;
    int have_origin = camera_validate(camera) && rr_read(camera + RR_OFF_CAM_POS, &origin, sizeof origin) &&
        isfinite(origin.x) && isfinite(origin.y) && isfinite(origin.z);
    for (int s = 0; s < 4; ++s) {
        BR_Config* cfg = br_get();
        int enabled = s == 0 ? cfg->show_containers : s == 1 ? cfg->show_doors : s == 2 ? cfg->show_dropped_loot : cfg->show_vehicle_loot;
        if (!enabled) continue;
        uint64_t dict = rr_u64(world + sources[s].dict_off), entries = 0;
        int n = 0, stride = 0;
        if (!u_dict(dict, &n, &entries, &stride) || n < 0 || n > 4096 || stride != 0x18) continue;
        int category_start = total;
        for (int i = 0; i < n; ++i) {
            uint8_t row[0x18]; uint64_t obj; int32_t id;
            if (!rr_read(entries + (uint64_t)i * 0x18, row, sizeof row)) break;
            memcpy(&id, row + 8, 4); memcpy(&obj, row + 0x10, 8);
            if (!u_is(obj, sources[s].klass)) continue;
            uint64_t pos = rr_u64(obj + sources[s].pos_off);
            unsigned position_offset;
            if (u_is(pos, "PositionModel")) position_offset = RR_OFF_POS_X;
            else if (u_is(pos, "InteractPositionModel")) position_offset = RR_OFF_INTERACT_POS_X;
            else continue;
            RR_V3 v;
            if (!rr_read(pos + position_offset, &v, sizeof v) || !isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z) ||
                fabsf(v.x) > 500000 || fabsf(v.y) > 500000 || fabsf(v.z) > 500000) continue;
            float dx = v.x - origin.x, dy = v.y - origin.y, dz = v.z - origin.z;
            float score = dx * dx + dy * dy + dz * dz;
            int at = rr_world_marker_reserve(distance2, category_start, &total,
                                             RR_MAX_WORLD_MARKERS / 4, score, have_origin);
            if (at < 0) continue;
            LOOT_CACHE* m = &staged[at]; memset(m, 0, sizeof *m);
            m->id = id; m->kind = sources[s].kind; m->pos = v; m->obj = obj;
            snprintf(m->name, sizeof m->name, "%s", sources[s].name);
            if (sources[s].kind == 4) {
                uint64_t state = rr_u64(obj + RR_OFF_DOOR_STATE);
                uint64_t data = u_is(state, "DoorStateModel") ? rr_u64(state + RR_OFF_DOOR_STATE_DATA) : 0;
                uint64_t value = u_is(data, "ReferenceData`1") ? rr_u64(data + RR_OFF_PRIM_VALUE) : 0;
                char name[24] = {0};
                if (u_is(value, "DoorStateDescription") && u_string(rr_u64(value + 0x10), name, sizeof name))
                    snprintf(m->name, sizeof m->name, "Door: %s", name);
            }
        }
    }
    if (world != g_world || rr_u64(world) != g_k_world) return;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_world_markers, staged, sizeof(staged[0]) * (size_t)total);
    g_nworld_markers = total; g_markers_world = world; g_markers_stamp = GetTickCount();
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

/* World membership defines hiding-object coverage. Cosmetic type delegates
 * and compiler-generated closure names must not exclude boxes or toilets. */
static void cache_hide_objects(void) {
    uint64_t world = g_world;
    static RR_HIDE_SNAPSHOT staged;
    if (!PL(world) || rr_u64(world) != g_k_world) return;
    if(!rr_hide_read(g_battle_model,world,&staged))memset(&staged,0,sizeof(staged));
    if (world != g_world || rr_u64(world) != g_k_world) return;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    g_hide_snapshot=staged;
    memcpy(g_hide_objects,staged.objects,sizeof(staged.objects[0])*(size_t)staged.nobjects);
    g_nhide_objects=staged.nobjects;g_hide_world=world;g_hide_stamp=GetTickCount();
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

static uint64_t loot_delegate_value(uint64_t func, int direct_offset, const char* closure) {
    if (!u_is(func,"Func`1")) return 0;
    uint64_t target=rr_u64(func+0x20);
    if (closure && u_is(target,closure)) return rr_u64(target+(uint64_t)direct_offset);
    if (!u_is(target,"<>c__DisplayClass5_0`1")) return 0;
    uint64_t en=rr_u64(target+0x10); int index=rr_i32(target+0x18);
    if (!u_is(en,"Enum`1")) return 0;
    uint64_t list=rr_u64(en+0x10);
    if (!u_is(list,"List`1")) return 0;
    int count=rr_i32(list+0x18); uint64_t array=rr_u64(list+0x10);
    if (!PL(array) || count<0 || count>100000 || index<0 || index>=count ||
        rr_u64(array+0x18)<(uint64_t)count) return 0;
    return rr_u64(array+0x20+(uint64_t)index*8);
}

/* Runs on the discovery worker. A slot is published only after its world,
 * state, description, position and item have all been validated. */
/* Track rejection reasons per loot kind. An invalid chain must remain
 * distinguishable from a successfully read empty/taken flag.
 */
enum {
    LST_SEEN = 0,   /* dictionary entries                                  */
    LST_CLASS,      /* obj is not the expected node class                  */
    LST_DESC,       /* description class wrong, or its key does not match  */
    LST_POS,        /* position unreadable or not finite                   */
    LST_EMPTY,      /* node is already empty/taken - the normal skip        */
    LST_FLAG,       /* the empty/taken flag itself is unreadable            */
    LST_LOOT,       /* loot description class wrong                        */
    LST_INV,        /* item description class wrong                        */
    LST_RARITY,     /* rarity outside 1..6                                 */
    LST_NAME,       /* item name unreadable                                */
    LST_N
};
static int g_loot_stat[2][LST_N];

static void cache_loot(int kind) {
    uint64_t world=g_world;
    int st[LST_N];
    if (!PL(world) || rr_u64(world)!=g_k_world) return;
    LOOT_CACHE staged[RR_MAX_LOOT];
    int total=0;
    uint64_t dict=rr_u64(world+(kind==1?0xA0:0xB0)), entries=0;
    int used=0,n=0,stride=0;
    memset(st,0,sizeof st);
    if (!rr_ok() || (dict && (!PL(dict) || !rr_read(dict+U_DICT_COUNT,&used,4) || used<0 || used>4096))) return;
    if (used && (!u_dict(dict,&n,&entries,&stride) || n!=used || stride!=0x18)) return;
    for (int i=0;i<n && total<RR_MAX_LOOT;++i) {
        uint8_t entry[0x18]; int32_t key; uint64_t obj;
        if (!rr_read(entries+(uint64_t)i*0x18,entry,sizeof(entry))) break;
        st[LST_SEEN]++;
        memcpy(&key,entry+8,4); memcpy(&obj,entry+0x10,8);
        if (!u_is(obj,kind==1?"RandomLootObjectModel":"LootNodeModel")) { st[LST_CLASS]++; continue; }
        uint64_t desc=rr_u64(obj+(kind==1?0x20:0x10)), inventory=0, lootobj=0;
        if (!u_is(desc,kind==1?"RandomLootObjectDescription":"LocationLootNodeDescription") ||
            rr_i32(desc+0x20)!=key) { st[LST_DESC]++; continue; }
        RR_V3 pos;
        uint64_t pos_addr = desc + (kind==1?0x24:0x38);
        if (!rr_read(pos_addr,&pos,sizeof(pos)) ||
            !isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z) ||
            fabsf(pos.x)>500000 || fabsf(pos.y)>500000 || fabsf(pos.z)>500000) { st[LST_POS]++; continue; }
        uint64_t flag_addr = 0;
        if (kind==1) {
            uint64_t state=rr_u64(obj+0x10),ref=rr_u64(obj+0x18); uint8_t taken=255;
            if (!u_is(state,"BoolData") || !rr_read(state+0x18,&taken,1)) { st[LST_FLAG]++; continue; }
            if (taken!=0) { st[LST_EMPTY]++; continue; }
            if (!u_is(ref,"ReferenceData`1")) { st[LST_FLAG]++; continue; }
            flag_addr = state + 0x18;
            uint64_t item=rr_u64(ref+0x18);
            if (!u_is(item,"LootObjectItemDescription")) { st[LST_LOOT]++; continue; }
            lootobj=item;
            inventory=loot_delegate_value(rr_u64(item+0x20),0,NULL);
        } else {
            uint64_t emptyflag=rr_u64(obj+0x30),data=PL(emptyflag)?rr_u64(emptyflag+0x10):0;
            uint8_t value=255;
            if (!u_is(emptyflag,"PrimitiveModel`1") || !u_is(data,"BoolData") ||
                !rr_read(data+0x18,&value,1)) { st[LST_FLAG]++; continue; }
            if (value!=0) { st[LST_EMPTY]++; continue; }
            flag_addr = data + 0x18;
            uint64_t loot=loot_delegate_value(rr_u64(desc+0x30),0x18,"<>c__DisplayClass46_0");
            if (!u_is(loot,"LootNodeLootDescription")) { st[LST_LOOT]++; continue; }
            lootobj=loot;
            inventory=loot_delegate_value(rr_u64(loot+0x30),0x10,"<>c__DisplayClass3_0");
        }
        if (!u_is(inventory,"InventoryItemDescription")) { st[LST_INV]++; continue; }
        int rarity=rr_i32(inventory+0x30);
        if (!rr_ok() || rarity<0 || rarity>6) { st[LST_RARITY]++; continue; }
        LOOT_CACHE *l=&staged[total]; memset(l,0,sizeof(*l));
        if (!u_string(rr_u64(inventory+0x10),l->name,sizeof(l->name))) { st[LST_NAME]++; continue; }
        l->id=key; l->kind=kind; l->rarity=rarity; l->pos=pos;
        l->obj=obj; l->pos_addr=pos_addr; l->flag_addr=flag_addr;
        l->desc=desc; l->loot=lootobj; l->inv=inventory;
        ++total;
    }
    if (world!=g_world || rr_u64(world)!=g_k_world) return;
    int slot=kind-1;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_loot[slot],staged,sizeof(LOOT_CACHE)*(size_t)total);
    g_nloot[slot]=total; g_loot_world[slot]=world; g_loot_stamp[slot]=GetTickCount();
    memcpy(g_loot_stat[slot],st,sizeof st);
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

/* Loot vacuum. */

/* Writes to loot state, from the addresses cache_loot already validated.
 *
 *   mode 0 "move"  - write the local player's position over each loot
 *                    position, so proximity pickup sees the item there
 *   mode 1 "taken" - set the taken (loose loot) / empty (node) flag
 *
 * apply == 0 resolves and counts only: that is how to see what would be
 * touched before anything changes.
 *
 * Every entry is re-validated immediately before its write - the model still
 * has the expected class name, and its flag is still 0 - so a cache entry that
 * aged out is skipped instead of writing through a stale address. Writes are
 * capped per call for the same reason.
 *
 * Not re-entrant: one caller at a time (the diagnostic mode or the overlay's
 * worker), which is why the staging buffer below can be static.
 */
int rr_loot_vacuum(int mode, int apply, RR_V3* local_pos_out,
                   int* out_ok, int* out_skipped) {
    static LOOT_CACHE items[RR_MAX_LOOT];
    RR_V3 local_pos;
    int have_local = 0, ok = 0, skipped = 0;
    enum { MAX_WRITES = 64 };

    if (out_ok) *out_ok = 0;
    if (out_skipped) *out_skipped = 0;
    memset(&local_pos, 0, sizeof(local_pos));
    if (local_pos_out) memset(local_pos_out, 0, sizeof(*local_pos_out));
    if (!g_cs_ready) return 0;

    /* The local player's transform is the destination for mode 0. */
    for (int i = 0; i < g_ncache; ++i) {
        RR_V3 p;

        if (!g_cache[i].local || !PL(g_cache[i].tr)) continue;
        if (!rr_read(g_cache[i].tr + RR_OFF_CTM_X, &p, sizeof(p))) continue;
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) continue;
        local_pos = p;
        have_local = 1;
        break;
    }
    if (local_pos_out && have_local) memcpy(local_pos_out, &local_pos, sizeof(local_pos));
    if (mode == 0 && !have_local) return 0;

    for (int kind = 0; kind < 2; ++kind) {
        const char* want = kind == 1 ? "RandomLootObjectModel" : "LootNodeModel";
        int n;

        /* Copy under the lock; the writes below are IOCTLs and must not hold it. */
        EnterCriticalSection(&g_cache_cs);
        n = g_nloot[kind];
        if (n > RR_MAX_LOOT) n = RR_MAX_LOOT;
        memcpy(items, g_loot[kind], sizeof(LOOT_CACHE) * (size_t)n);
        LeaveCriticalSection(&g_cache_cs);

        for (int i = 0; i < n; ++i) {
            const LOOT_CACHE* e = &items[i];
            uint8_t flag = 1;

            if (!PL(e->obj) || !PL(e->flag_addr) || !u_is(e->obj, want)) { skipped++; continue; }
            if (!rr_read(e->flag_addr, &flag, 1) || flag != 0) { skipped++; continue; }

            if (!apply) { ok++; continue; }
            if (ok >= MAX_WRITES) { skipped++; continue; }

            if (mode == 1) {
                uint8_t one = 1;
                if (rr_write(e->flag_addr, &one, 1)) ok++; else skipped++;
            } else {
                if (rr_write(e->pos_addr, &local_pos, sizeof(local_pos))) ok++; else skipped++;
            }
        }
    }

    if (out_ok) *out_ok = ok;
    if (out_skipped) *out_skipped = skipped;
    return ok + skipped;
}

/* Clear addresses and caches tied to the previous process before discovery
 * in a new attachment. Heap addresses from the old process are not reusable
 * even if its replacement has the same executable name.
 */
void game_reattach_reset(void) {
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    g_ncache = 0;
    g_nvehicles = 0;
    g_nloot[0] = g_nloot[1] = 0;
    g_nworld_markers = 0; g_markers_world = 0; g_markers_stamp = 0;
    g_nhide_objects = 0;
    memset(&g_hide_snapshot,0,sizeof(g_hide_snapshot));
    g_hide_world = 0;
    g_hide_stamp = 0;
    g_loot_world[0] = g_loot_world[1] = 0;
    g_loot_stamp[0] = g_loot_stamp[1] = 0;
    g_vehicle_world = 0;
    g_vehicle_generation++;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    g_world = 0;
    g_cam_obj = 0;
    g_k_world = 0;
    g_k_char = 0;
    g_k_cam = 0;
    g_k_battle = 0;
    g_battle = 0;
    g_battle_model = 0;
    g_views_updater = 0;
    g_aim_updater = 0;
    g_saved_pid = 0;
    g_saved_world = 0;
    g_nregions = 0;
    g_nheap = 0;
}

/* Address of the local player's BattleCharacterModel, 0 when the roster is not
 * published yet. Read-only view of the cache under the same lock the scan
 * worker uses. */
uint64_t rr_local_player(void) {
    uint64_t obj = 0;
    int i;
    if (!g_cs_ready) return 0;
    EnterCriticalSection(&g_cache_cs);
    for (i = 0; i < g_ncache; ++i)
        if (g_cache[i].local) { obj = g_cache[i].obj; break; }
    LeaveCriticalSection(&g_cache_cs);
    return obj;
}

int rr_loot_snapshot(RR_LOOT_ENT* out, int max) {    int total = 0, slot;
    if (!out || max <= 0 || !g_cs_ready) return 0;
    for (slot = 0; slot < 2 && total < max; ++slot) {
        int i, n;
        EnterCriticalSection(&g_cache_cs);
        n = g_nloot[slot];
        if (n > RR_MAX_LOOT) n = RR_MAX_LOOT;
        for (i = 0; i < n && total < max; ++i) {
            const LOOT_CACHE* s = &g_loot[slot][i];
            RR_LOOT_ENT* d = &out[total++];
            memset(d, 0, sizeof(*d));
            d->obj = s->obj; d->desc = s->desc;
            d->loot = s->loot; d->inv = s->inv;
            d->pos_addr = s->pos_addr; d->flag_addr = s->flag_addr;
            d->id = s->id; d->kind = s->kind; d->rarity = s->rarity; d->pos = s->pos;
            strncpy(d->name, s->name, sizeof(d->name) - 1);
        }
        LeaveCriticalSection(&g_cache_cs);
    }
    return total;
}

int rr_loot_refresh(RR_LOOT_DEBUG* dbg) {
    uint64_t dict;
    int used;

    if (dbg) {
        memset(dbg, 0, sizeof(*dbg));
        dbg->world = g_world;
        dbg->entries_nodes = -1;
        dbg->entries_loose = -1;
    }
    if (!PL(g_world)) return 0;

    dict = rr_u64(g_world + 0xB0);            /* nodes */
    if (dbg) dbg->dict_nodes = dict;
    if (PL(dict)) {
        used = -1;
        if (rr_read(dict + U_DICT_COUNT, &used, 4) && dbg) dbg->entries_nodes = used;
    }
    dict = rr_u64(g_world + 0xA0);            /* loose loot */
    if (dbg) dbg->dict_loose = dict;
    if (PL(dict)) {
        used = -1;
        if (rr_read(dict + U_DICT_COUNT, &used, 4) && dbg) dbg->entries_loose = used;
    }

    /* Both kinds, because the caller does not know which one it wants and each
     * one writes only its own slot. */
    cache_loot(1);
    cache_loot(2);

    if (dbg) {
        int s;
        for (s = 0; s < 2; ++s) {
            int seen = g_loot_stat[s][LST_SEEN];
            int emp = g_loot_stat[s][LST_EMPTY];
            int bad = seen - g_nloot[s] - emp;
            if (bad < 0) bad = 0;
            if (s == 0) { dbg->loose_seen = seen; dbg->loose_empty = emp; dbg->loose_bad = bad; }
            else        { dbg->nodes_seen = seen; dbg->nodes_empty = emp; dbg->nodes_bad = bad; }
        }
        dbg->cached = g_nloot[0] + g_nloot[1];
    }
    return g_nloot[0] + g_nloot[1];
}

/* Count a vehicle's seat flags only if every entry can be read and validated.
 * A failed read is unknown, not evidence that the vehicle is empty. */
static int vehicle_seat_count(uint64_t seats, int* occupied) {
    if (!PL(seats) || !class_named(rr_u64(seats), "VehicleSeatsModel")) return 0;
    uint64_t map=rr_u64(seats+RR_OFF_SEATS_DATA);
    if (!PL(map) || !class_named(rr_u64(map), "ReferenceMapData`2")) return 0;
    uint64_t list=rr_u64(map+0x18);
    if (!PL(list) || !class_named(rr_u64(list), "List`1")) return 0;
    int n=-1; uint64_t array=rr_u64(list+0x10), length=0;
    if (!rr_read(list+0x18,&n,4) || n<1 || n>32 || !PL(array) ||
        !rr_read(array+0x18,&length,8) || length<(uint64_t)n || length>256) return 0;
    uint64_t items[32];
    if (!rr_read(array+0x20,items,(size_t)n*sizeof(items[0]))) return 0;
    int count=0;
    for (int i=0;i<n;++i) {
        uint64_t seat=items[i];
        if (!PL(seat) || !class_named(rr_u64(seat),"VehicleSeatData")) return 0;
        uint64_t flag=rr_u64(seat+RR_OFF_SEAT_OCCUPIED);
        uint8_t value=255;
        if (!PL(flag) || !class_named(rr_u64(flag),"BoolData") ||
            !rr_read(flag+0x18,&value,1) || value>1) return 0;
        if (value) {
            uint64_t iddata=rr_u64(seat+RR_OFF_SEAT_ENTITY_ID); int32_t id=-1;
            if (!PL(iddata) || !rr_read(iddata+0x18,&id,4) || id<0) return 0;
            ++count;
        }
    }
    *occupied=count; return 1;
}

static void cache_vehicle_render_root(VEH_CACHE* vehicle, uint64_t transform) {
    if (!u_is(transform, "Transform")) return;
    uint64_t native = rr_u64(transform + 0x10);
    uint64_t hierarchy = PL(native) ? rr_u64(native + 0x28) : 0;
    int index = PL(native) ? rr_i32(native + 0x30) : -1;
    int capacity = PL(hierarchy) ? rr_i32(hierarchy + 0x10) : 0;
    if (capacity <= 0 || capacity > 65536 || index < 0 || index >= capacity) return;
    uint64_t arrays[4];
    if (!rr_read(hierarchy + 0x18, arrays, sizeof(arrays)) || !PL(arrays[0]) ||
        !PL(arrays[1]) || !PL(arrays[3]) || rr_u64(arrays[3] + (uint64_t)index * 8) != native) return;
    int depth = 0;
    while (index >= 0 && index < capacity && depth < RR_VEHICLE_POSE_DEPTH) {
        vehicle->render_trs[depth++] = arrays[0] + (uint64_t)index * sizeof(MESH_TRS);
        if (!rr_read(arrays[1] + (uint64_t)index * sizeof(index), &index, sizeof(index))) return;
    }
    if (index == -1) {
        vehicle->render_depth = depth;
        vehicle->render_transform = transform;
        vehicle->render_native = native;
        vehicle->render_hierarchy = hierarchy;
        vehicle->render_index = rr_i32(native + 0x30);
        vehicle->render_view_at = GetTickCount();
    }
}

/* Match model IDs to the vehicle roots used by the rendered scene. */
static void cache_vehicle_views(VEH_CACHE* vehicles, int count) {
    uint64_t updater=g_views_updater, battle=g_battle;
    if (!views_updater_valid(updater,battle) || rr_u64(battle+RR_OFF_BATTLE_WORLD)!=g_world) return;
    for (int kind=0;kind<2;++kind) {
        uint64_t dict=rr_u64(updater+(kind?RR_OFF_VIEWS_HELICOPTERS:RR_OFF_VIEWS_CARS)), entries=0;
        int n=0,stride=0;
        if (!u_dict(dict,&n,&entries,&stride) || n>4096 || stride!=0x18) continue;
        for (int i=0;i<n;++i) {
            uint8_t entry[0x18]; int32_t id; uint64_t view;
            if (!rr_read(entries+(uint64_t)i*0x18,entry,sizeof(entry))) break;
            memcpy(&id,entry+8,4); memcpy(&view,entry+0x10,8);
            if (!PL(view)) continue;
            int j; for (j=0;j<count;++j) if (vehicles[j].kind==kind && vehicles[j].id==id) break;
            if (j==count || !class_named(rr_u64(view),kind?"HelicopterEntityView":"CarEntityView")) continue;
            if (br_get()->vehicle_chams)
                cache_vehicle_render_root(&vehicles[j], rr_u64(view + (kind ? 0x30 : 0x28)));
#ifdef RR_PORT_DIAGNOSTICS
            if (!br_get()->show_vehicles) continue;
            uint64_t ve=rr_u64(view+0x20);
            if (!PL(ve) || !class_named(rr_u64(ve),"VehicleEntityView")) continue;
            uint64_t marker=rr_u64(ve+0x58);
            if (!PL(marker) || !class_named(rr_u64(marker),"VehicleMarkerRootView")) continue;
            uint64_t tr=rr_u64(marker+0x20);
            if (!PL(tr) || !class_named(rr_u64(tr),"Transform")) continue;
            uint64_t native=rr_u64(tr+0x10), h=PL(native)?rr_u64(native+0x28):0;
            int idx=PL(native)?rr_i32(native+0x30):-1;
            int capacity=PL(h)?rr_i32(h+0x10):0;
            uint64_t pointers=PL(h)?rr_u64(h+0x30):0;
            if (capacity<1 || capacity>2048 || idx<0 || idx>=capacity || !PL(pointers) ||
                rr_u64(pointers+(uint64_t)idx*8)!=native) continue;
            vehicles[j].marker_transform=tr; vehicles[j].marker_native=native;
            vehicles[j].marker_h=h; vehicles[j].marker_idx=idx;
#endif
        }
    }
}

/* Authoritative world dictionaries contain only currently replicated cars and
 * helicopters. Validate both dictionary key and object ID before publishing. */
static void vehicle_owner_role(uint64_t object, int kind, char* role, size_t capacity) {
    if (!capacity) return;
    role[0] = 0;
    uint64_t model = rr_u64(object + (kind ? RR_OFF_HELI_OWNER_ROLE : RR_OFF_CAR_OWNER_ROLE));
    if (!u_is(model, "PrimitiveModel`1")) return;
    uint64_t data = rr_u64(model + 0x10);
    if (!u_is(data, "ReferenceData`1")) return;
    uint64_t description = rr_u64(data + RR_OFF_REF_VALUE);
    if (!u_is(description, "PlayerRoleDescription")) return;
    if (!u_string(rr_u64(description + RR_OFF_DESC_NAME), role, capacity)) role[0] = 0;
}

static void cache_vehicles(void) {
    uint64_t world = g_world, klass = 0;
    if (!PL(world) || !rr_read(world, &klass, 8) || klass != g_k_world) return;
    int count = 0;
    for (int kind = 0; kind < 2; ++kind) {
        int begin = count, failed = 0;
        uint64_t dict = rr_u64(world + (kind ? RR_OFF_WEM_HELICOPTERS : RR_OFF_WEM_CARS));
        int used = 0;
        if (!rr_ok() || (dict && (!PL(dict) || !rr_read(dict + U_DICT_COUNT, &used, 4) || used < 0 || used > 4096))) failed = 1;
        int n = 0, stride = 0; uint64_t entries = 0;
        if (!failed && used && (!u_dict(dict, &n, &entries, &stride) || stride != 0x18 || n != used)) failed = 1;
        for (int i = 0; !failed && i < n && count < RR_MAX_VEHICLES; ++i) {
            uint8_t entry[0x18], head[(RR_OFF_CAR_ID > RR_OFF_HELI_ID ? RR_OFF_CAR_ID : RR_OFF_HELI_ID) + 4];
            int32_t key, id; uint64_t obj, k;
            if (!rr_read(entries + (uint64_t)i * 0x18, entry, sizeof(entry))) { failed = 1; break; }
            memcpy(&key, entry + 8, 4); memcpy(&obj, entry + 0x10, 8);
            if (!PL(obj) || !rr_read(obj, head, sizeof(head))) continue;
            memcpy(&k, head, 8); memcpy(&id, head + (kind ? RR_OFF_HELI_ID : RR_OFF_CAR_ID), 4);
            if (id != key || !class_named(k, kind ? "HelicopterModel" : "CarModel")) continue;
            uint64_t tr = rr_u64(obj + (kind ? RR_OFF_HELI_TRANSFORM : RR_OFF_CAR_TRANSFORM));
            if (!PL(tr)) continue;
            VEH_CACHE* v = &g_vehicles_stage[count];
            memset(v, 0, sizeof(*v));
            v->obj = obj; v->klass = k; v->tr = tr; v->id = id; v->kind = kind;
            vehicle_owner_role(obj, kind, v->role, sizeof(v->role));
            int want_seats = br_get()->show_vehicle_markers && br_get()->show_vehicle_occupants;
#ifdef RR_PORT_DIAGNOSTICS
            want_seats |= br_get()->show_vehicles;
#endif
            if (want_seats) {
                uint64_t seats=rr_u64(obj+(kind?RR_OFF_HELI_SEATS:RR_OFF_CAR_SEATS));
                v->occupants_known=vehicle_seat_count(seats,&v->occupants);
            }
            uint64_t desc = rr_u64(obj + (kind ? RR_OFF_HELI_DESC : RR_OFF_CAR_DESC));
            uint64_t name = PL(desc) ? rr_u64(desc + 0x10) : 0;
            if (PL(name)) u_string(name, v->name, sizeof(v->name));
            if (!v->name[0]) snprintf(v->name, sizeof(v->name), "%s", kind ? "HELICOPTER" : "CAR");
            ++count;
        }
        if (failed) {
            count = begin;
            if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
            for (int i = 0; g_vehicle_world == world && i < g_nvehicles && count < RR_MAX_VEHICLES; ++i)
                if (g_vehicles[i].kind == kind) {
                    g_vehicles_stage[count] = g_vehicles[i];
                    g_vehicles_stage[count].occupants_known=0;
                    g_vehicles_stage[count++].occupants=0;
                }
            if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
        }
    }
    if (world != g_world) return;
    for (int i = 0; i < count; ++i) g_vehicles_stage[i].render_depth = 0;
    cache_vehicle_views(g_vehicles_stage,count);
    if (world != g_world) return;
    static VEH_CACHE previous_vehicles[RR_MAX_VEHICLES];
    int previous_count=0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    if (g_vehicle_world == world) {
        previous_count=g_nvehicles;
        if (previous_count>0) memcpy(previous_vehicles,g_vehicles,
                                     (size_t)previous_count*sizeof(previous_vehicles[0]));
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    /* Keep a recently validated render root through an incomplete view-table
     * refresh. The model, transform, hierarchy and native slot must still
     * match; sampling validates the TRS chain again before it is drawn. */
    for (int i=0; i<count; ++i) {
        VEH_CACHE* v=&g_vehicles_stage[i];
        if (v->render_depth) continue;
        for (int j=0; j<previous_count; ++j) {
            const VEH_CACHE* old=&previous_vehicles[j];
            if (old->obj!=v->obj || old->id!=v->id || old->kind!=v->kind ||
                !old->render_depth || !old->render_view_at ||
                GetTickCount()-old->render_view_at>1500) continue;
            uint64_t h=old->render_hierarchy, native=old->render_native;
            int capacity=PL(h)?rr_i32(h+0x10):0;
            uint64_t slots=PL(h)?rr_u64(h+0x30):0;
            if (old->render_index<0 || old->render_index>=capacity || capacity>65536 ||
                !PL(slots) || rr_u64(old->render_transform+0x10)!=native ||
                rr_u64(native+0x28)!=h || rr_i32(native+0x30)!=old->render_index ||
                rr_u64(slots+(uint64_t)old->render_index*8)!=native) break;
            memcpy(v->render_trs,old->render_trs,sizeof(v->render_trs));
            v->render_depth=old->render_depth;
            v->render_transform=old->render_transform; v->render_native=native;
            v->render_hierarchy=h; v->render_index=old->render_index;
            v->render_view_at=old->render_view_at;
            break;
        }
    }
    if (world != g_world) return;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    memcpy(g_vehicles, g_vehicles_stage, sizeof(VEH_CACHE) * (size_t)count);
    g_nvehicles = count;
    g_vehicle_world = world;
    ++g_vehicle_generation;
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
}

/* Refresh only the displayed pose just before painting; keep the snapshot's
 * validated lens and leave the aim worker on its coherent snapshot. */
int game_refresh_render_camera(RR_CAMERA* cam) {
    uint64_t model = g_cam_obj, world = g_world;
    uint8_t buf[0x50];
    if (!cam || !cam->valid || cam->model_source != model || !PL(model) || !PL(world) ||
        !rr_read(model + RR_OFF_CAM_POS, buf, sizeof(buf))) return 0;
    RR_CAMERA next = *cam;
    memcpy(&next.pos, buf, sizeof(RR_V3));
    memcpy(&next.rot, buf + (RR_OFF_CAM_ROT - RR_OFF_CAM_POS), sizeof(RR_Q4));
    memcpy(&next.fwd, buf + (RR_OFF_CAM_FWD - RR_OFF_CAM_POS), sizeof(RR_V3));
    memcpy(&next.right, buf + (RR_OFF_CAM_RIGHT - RR_OFF_CAM_POS), sizeof(RR_V3));
    memcpy(&next.up, buf + (RR_OFF_CAM_UP - RR_OFF_CAM_POS), sizeof(RR_V3));
    float fl = next.fwd.x*next.fwd.x + next.fwd.y*next.fwd.y + next.fwd.z*next.fwd.z;
    float rl = next.right.x*next.right.x + next.right.y*next.right.y + next.right.z*next.right.z;
    float ul = next.up.x*next.up.x + next.up.y*next.up.y + next.up.z*next.up.z;
    float fr = next.fwd.x*next.right.x + next.fwd.y*next.right.y + next.fwd.z*next.right.z;
    float fu = next.fwd.x*next.up.x + next.fwd.y*next.up.y + next.fwd.z*next.up.z;
    float ru = next.right.x*next.up.x + next.right.y*next.up.y + next.right.z*next.up.z;
    float dx = next.pos.x-cam->pos.x, dy = next.pos.y-cam->pos.y, dz = next.pos.z-cam->pos.z;
    if (!isfinite(next.pos.x) || !isfinite(next.pos.y) || !isfinite(next.pos.z) ||
        !isfinite(fl) || !isfinite(rl) || !isfinite(ul) ||
        fl < .81f || fl > 1.21f || rl < .81f || rl > 1.21f || ul < .81f || ul > 1.21f ||
        fabsf(fr) > .05f || fabsf(fu) > .05f || fabsf(ru) > .05f ||
        dx*dx + dy*dy + dz*dz > 100.0f ||
        model != g_cam_obj || world != g_world) return 0;
    *cam = next;
    return 1;
}

/* Init. */

/* VAD discovery. */
/* Rebuild the region list from the target process before discovery.
 * Committed regions can be freed or reused during a session, so saved ranges
 * are search hints rather than proof that an address remains readable.
 */


static int list_regions(RR_HEAP* out, int max, uint64_t min_bytes) {
    uint64_t a = 0x10000ULL, hi = 0x7FFFFFFFFFFFULL;
    int n = 0;
    while (a < hi && n < max) {
        RR_REGION r;
        if (!rr_query_region(a, &r)) { a += 0x10000; continue; }
        if (r.size == 0) break;
        if (r.state == 0x1000 && r.size >= min_bytes && (r.protect & 0xEE)) {
            out[n].base = r.base;
            out[n].size = r.size;
            n++;
        }
        uint64_t next = r.base + r.size;
        if (next <= a) break;
        a = next;
    }
    return n;
}

static void find_heap_regions(void) {
    g_nregions = list_regions(g_regions, RR_MAX_REGIONS, 1ULL << 12);
    g_nheap = 0;
    for (int k = 0; k < g_nregions && g_nheap < RR_MAX_HEAP; k++)
        g_heap[g_nheap++] = g_regions[k];
}

/* Parallel pattern scan inside one region (metadata strings). */
typedef struct {
    uint64_t lo, hi;
    const uint8_t* pat;
    int plen;
    volatile LONG found;
    uint64_t addr;
} PAT_CTX;

static DWORD WINAPI pat_thread(LPVOID p) {
    PAT_CTX* c = (PAT_CTX*)p;
    static __declspec(thread) uint8_t buf[0x10000 + 128];
    for (uint64_t a = c->lo; a < c->hi; a += 0x10000) {
        if (InterlockedCompareExchange(&c->found, 0, 0)) return 0;
        size_t n = (size_t)((c->hi - a) < 0x10000 ? (c->hi - a) : 0x10000);
        if (!rr_read(a, buf, n)) continue;
        for (size_t i = 0; i + (size_t)c->plen <= n; i++) {
            if (memcmp(buf + i, c->pat, (size_t)c->plen) != 0) continue;
            c->addr = a + i;
            InterlockedExchange(&c->found, 1);
            return 0;
        }
    }
    return 0;
}

static uint64_t scan_pattern(uint64_t base, uint64_t size, const uint8_t* pat, int plen) {
    if (size < (uint64_t)plen) return 0;
    PAT_CTX c[SCAN_THREADS];
    HANDLE th[SCAN_THREADS];
    uint64_t span = size / SCAN_THREADS;
    if (span < 0x10000) span = 0x10000;
    for (int i = 0; i < SCAN_THREADS; i++) {
        c[i].lo = base + span * (uint64_t)i;
        c[i].hi = (i == SCAN_THREADS - 1) ? base + size : base + span * (uint64_t)(i + 1);
        c[i].pat = pat;
        c[i].plen = plen;
        c[i].found = 0;
        c[i].addr = 0;
        th[i] = CreateThread(NULL, 0, pat_thread, &c[i], 0, NULL);
    }
    WaitForMultipleObjects(SCAN_THREADS, th, TRUE, INFINITE);
    uint64_t hit = 0;
    for (int i = 0; i < SCAN_THREADS; i++) { CloseHandle(th[i]); if (c[i].found) hit = c[i].addr; }
    return hit;
}

/* Resolve an Il2CppClass without any cached address: scan the committed
 * regions for the class name string, then look for the class struct that
 * points at it. */
/* Class resolution. */
/* Resolve a class in three stages: collect name strings, collect candidate
 * class structures that reference them, then seek a plausible live instance.
 * These checks reduce false matches in metadata that repeats the same name.
 */

#define MAX_HITS 64
#define MAX_CANDS 16

static uint64_t g_hits[MAX_HITS];
static volatile LONG g_nhits;
static uint64_t g_cands[MAX_CANDS];
static int g_ncands;

/* Collect every occurrence of the name inside one region (parallel). */
typedef struct {
    uint64_t lo, hi;
    const uint8_t* pat;
    int plen;
} HIT_CTX;

/* A cold scan can cover several gigabytes. Bound one attempt so the read thread
 * is never blocked for the whole sweep: the scan returns with whatever hits it
 * has and the next attempt resumes at the saved region, walking the process
 * across several short passes instead of one long one. */
static volatile LONG g_hit_stop;
static volatile LONG g_hit_cursor;
#define HIT_SCAN_BUDGET_MS 2000

static DWORD WINAPI hit_thread(LPVOID p) {
    HIT_CTX* c = (HIT_CTX*)p;
    static __declspec(thread) uint8_t buf[0x10000 + 128];
    for (uint64_t a = c->lo; a < c->hi; a += 0x10000) {
        if (InterlockedCompareExchange(&g_hit_stop, 0, 0)) return 0;
        size_t n = (size_t)((c->hi - a) < 0x10000 ? (c->hi - a) : 0x10000);
        if (!rr_read(a, buf, n)) continue;
        for (size_t i = 0; i + (size_t)c->plen <= n; i++) {
            if (memcmp(buf + i, c->pat, (size_t)c->plen) != 0) continue;
            LONG k = InterlockedIncrement(&g_nhits) - 1;
            if (k < MAX_HITS) g_hits[k] = a + i;
        }
    }
    return 0;
}

/* Scan the process in small slices so every attempt finishes inside the
 * budget. A cursor walks the slice list across attempts, so partial scans do
 * not lose coverage: a slice is either completed or retried on a later pass. */
typedef struct { uint64_t base, size; } HIT_SLICE;
#define HIT_SLICE_BYTES 0x2000000ULL    /* 32 MB */
#define HIT_MAX_SLICES  2048

static HIT_SLICE g_hit_slices[HIT_MAX_SLICES];
static int       g_hit_slice_count;

static void hit_build_slices(void) {
    g_hit_slice_count = 0;
    for (int r = 0; r < g_nregions && g_hit_slice_count < HIT_MAX_SLICES; r++) {
        if (g_regions[r].size < (1ULL << 20)) continue;
        uint64_t base = g_regions[r].base, size = g_regions[r].size;
        for (uint64_t off = 0; off < size && g_hit_slice_count < HIT_MAX_SLICES; off += HIT_SLICE_BYTES) {
            uint64_t len = size - off;
            if (len > HIT_SLICE_BYTES) len = HIT_SLICE_BYTES;
            g_hit_slices[g_hit_slice_count].base = base + off;
            g_hit_slices[g_hit_slice_count].size = len;
            g_hit_slice_count++;
        }
    }
}

/* Metadata name strings are clustered: once one class name is found, the other
 * class names are usually within the same few slices. Remember that window and
 * search it before walking the rest of the process. */
static volatile LONG64 g_hit_hint_lo;
static volatile LONG64 g_hit_hint_hi;
static volatile LONG  g_hit_hint_strikes;

static int hit_scan_slice(int s, const uint8_t* pat, int len, DWORD deadline) {
    if ((DWORD)GetTickCount() > deadline) {
        InterlockedExchange(&g_hit_stop, 1);
        return 0;
    }
    uint64_t base = g_hit_slices[s].base, size = g_hit_slices[s].size;
    HIT_CTX c[SCAN_THREADS];
    HANDLE th[SCAN_THREADS];
    uint64_t span = size / SCAN_THREADS;
    if (span < 0x10000) span = 0x10000;
    for (int i = 0; i < SCAN_THREADS; i++) {
        c[i].lo = base + span * (uint64_t)i;
        c[i].hi = (i == SCAN_THREADS - 1) ? base + size : base + span * (uint64_t)(i + 1);
        c[i].pat = pat;
        c[i].plen = len;
        th[i] = CreateThread(NULL, 0, hit_thread, &c[i], 0, NULL);
    }
    WaitForMultipleObjects(SCAN_THREADS, th, TRUE, INFINITE);
    for (int i = 0; i < SCAN_THREADS; i++) CloseHandle(th[i]);
    return 1;
}

static void collect_hits_clear_hint(void) {
    InterlockedExchange64(&g_hit_hint_lo, 0);
    InterlockedExchange64(&g_hit_hint_hi, 0);
    InterlockedExchange(&g_hit_hint_strikes, 0);
}

static void collect_hits_reset(void) {
    InterlockedExchange(&g_hit_cursor, 0);
    collect_hits_clear_hint();
}

static void collect_hits(const char* name) {
    uint8_t pat[96];
    size_t len = strlen(name);
    if (len >= sizeof(pat)) return;
    memcpy(pat, name, len);
    g_nhits = 0;
    InterlockedExchange(&g_hit_stop, 0);
    hit_build_slices();
    if (g_hit_slice_count <= 0) return;
    DWORD deadline = GetTickCount() + HIT_SCAN_BUDGET_MS;

    uint64_t hlo = (uint64_t)InterlockedCompareExchange64(&g_hit_hint_lo, 0, 0);
    uint64_t hhi = (uint64_t)InterlockedCompareExchange64(&g_hit_hint_hi, 0, 0);
    if (hlo && hhi > hlo) {
        int interrupted = 0;
        for (int s = 0; s < g_hit_slice_count; s++) {
            uint64_t b = g_hit_slices[s].base, e = b + g_hit_slices[s].size;
            if (e <= hlo || b >= hhi) continue;
            if (!hit_scan_slice(s, pat, (int)len, deadline)) { interrupted = 1; break; }
            if (g_nhits) break;
        }
        if (g_nhits) {
            InterlockedExchange(&g_hit_hint_strikes, 0);
        } else if (!interrupted) {
            /* The window is exhausted without a hit: it is not this cluster. */
            InterlockedExchange64(&g_hit_hint_lo, 0);
            InterlockedExchange64(&g_hit_hint_hi, 0);
        } else if (InterlockedIncrement(&g_hit_hint_strikes) >= 3) {
            /* Repeatedly budget-expired inside the window: drop it so the
             * cursor sweep can make progress. */
            InterlockedExchange64(&g_hit_hint_lo, 0);
            InterlockedExchange64(&g_hit_hint_hi, 0);
            InterlockedExchange(&g_hit_hint_strikes, 0);
            InterlockedExchange(&g_hit_stop, 0);
        }
    }

    if (!g_nhits) {
        int start = (int)((DWORD)InterlockedCompareExchange(&g_hit_cursor, 0, 0)
                          % (DWORD)g_hit_slice_count);
        for (int k = 0; k < g_hit_slice_count; k++) {
            int s = (start + k) % g_hit_slice_count;
            if (!hit_scan_slice(s, pat, (int)len, deadline)) break;
            InterlockedExchange(&g_hit_cursor, (LONG)((s + 1) % g_hit_slice_count));
            if (g_nhits >= MAX_HITS) break;
        }
    }

    if (g_nhits) {
        uint64_t lo = g_hits[0] > 0x4000000ULL ? g_hits[0] - 0x4000000ULL : 0;
        InterlockedExchange64(&g_hit_hint_lo, (LONG64)lo);
        InterlockedExchange64(&g_hit_hint_hi, (LONG64)(g_hits[0] + 0x4000000ULL));
        InterlockedExchange(&g_hit_hint_strikes, 0);
    }
}

static int is_hit(uint64_t v) {
    LONG n = g_nhits < MAX_HITS ? g_nhits : MAX_HITS;
    for (LONG i = 0; i < n; i++) if (g_hits[i] == v) return 1;
    return 0;
}

static void add_cand(uint64_t k) {
    for (int i = 0; i < g_ncands; i++) if (g_cands[i] == k) return;
    if (g_ncands < MAX_CANDS) g_cands[g_ncands++] = k;
}

/* One pass over the committed regions: any qword matching a name occurrence is
 * a potential Il2CppClass.name pointer. */
static void collect_candidates(const char* name) {
    g_ncands = 0;
    static uint8_t buf[0x10000];
    for (int r = 0; r < g_nregions && g_ncands < MAX_CANDS; r++) {
        uint64_t lo = g_regions[r].base, hi = lo + g_regions[r].size;
        if (g_regions[r].size < 0x1000) continue;
        for (uint64_t a = lo; a < hi; a += sizeof(buf)) {
            size_t sz = (size_t)((hi - a) < sizeof(buf) ? (hi - a) : sizeof(buf));
            if (!rr_read(a, buf, sz)) continue;
            for (size_t i = 0; i + 8 <= sz; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (!is_hit(v)) continue;
                uint64_t k = a + i - 0x10;
                if (class_named(k, name)) add_cand(k);
            }
        }
    }
}

/* Accept candidates with a matching class word, a readable transform
 * pointer and finite coordinates. These are plausibility checks.
 */
static int probe_class(uint64_t klass) {
    /* Metadata-only validation: the class name plus its field table identify a
     * character model. Probing by sweeping the whole address space for an
     * instance cost ~15 s per class on a cold process. */
    char nm[128] = {0};
    u_class_name(klass, nm, sizeof(nm));
    int name_ok = _stricmp(nm, "BattleCharacterModel") == 0;
    int f1 = class_has_field(klass, "Transform");
    int f2 = class_has_field(klass, "SkeletonModel");
    if (!(name_ok && (f1 || f2))) {
        uint64_t fp = rr_u64(klass + 0x80);
        char n0[48] = {0}, n1[48] = {0}, n2[48] = {0}, n3[48] = {0};
        if (PL(fp)) {
            rr_cstr(rr_u64(fp + 0x00), n0, sizeof(n0));
            rr_cstr(rr_u64(fp + 0x20), n1, sizeof(n1));
            rr_cstr(rr_u64(fp + 0x40), n2, sizeof(n2));
            rr_cstr(rr_u64(fp + 0x60), n3, sizeof(n3));
        }
        dlog("probe_class reject 0x%llX name='%s' fields=0x%llX count=%d T=%d S=%d ['%s' '%s' '%s' '%s']",
             (unsigned long long)klass, nm, (unsigned long long)fp,
             (int)(rr_u32(klass + 0x120) & 0xFFFFu), f1, f2, n0, n1, n2, n3);
        return 0;
    }
    return 1;
}

/* Camera classes cannot be validated with the character probe: their fields are
 * floats, not a transform pointer. Probe with camera_validate instead. */
static int probe_camera_class(uint64_t klass) {
    static uint8_t buf[0x10000];
    (void)buf;
    /* Same rationale as probe_class: validate from metadata, not by scanning
     * for a live camera instance. */
    if (!class_named(klass, "BattleCameraModel")) return 0;
    return class_has_field(klass, "Position") || class_has_field(klass, "Forward");
}

static uint64_t resolve_camera_class(const char* name) {
    collect_hits(name);
    if (!g_nhits) return 0;
    collect_candidates(name);
    for (int i = 0; i < g_ncands; i++)
        if (probe_camera_class(g_cands[i])) return g_cands[i];
    return 0;
}

static uint64_t resolve_class(const char* name) {
    /* Retry across slices: a hit that no class references (for example a
     * method-name string) must not wedge the scan on the same window. */
    for (int attempt = 0; attempt < 6; ++attempt) {
        collect_hits(name);
        snprintf(g_dbg, sizeof(g_dbg), "hits=%ld", (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS));
        if (!g_nhits) continue;
        collect_candidates(name);
        snprintf(g_dbg, sizeof(g_dbg), "hits=%ld cands=%d",
                 (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS), g_ncands);
        for (int i = 0; i < g_ncands; i++) {
            if (probe_class(g_cands[i])) {
                snprintf(g_dbg, sizeof(g_dbg), "hits=%ld cands=%d accepted=%llX",
                         (long)(g_nhits < MAX_HITS ? g_nhits : MAX_HITS), g_ncands,
                         (unsigned long long)g_cands[i]);
                return g_cands[i];
            }
        }
        collect_hits_clear_hint();
    }
    return 0;
}

/* Transform pool. */
typedef struct {
    const ENT_CACHE* src;
    const uint16_t*  idx;
    int              count;
    int              need;
    float           (*dst)[6];
    uint8_t*         ok;
    volatile LONG    next;
    volatile LONG    done;
    volatile LONG    run;
} XF_JOB;

static XF_JOB   g_xf;
static HANDLE   g_xf_sem;
static HANDLE   g_xf_done;
static int      g_xf_started;

static DWORD WINAPI xf_thread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_xf_sem, INFINITE);
        if (!InterlockedCompareExchange(&g_xf.run, 0, 0)) return 0;
        for (;;) {
            LONG k = InterlockedIncrement(&g_xf.next) - 1;
            if (k >= g_xf.count) break;
            int i = g_xf.idx ? g_xf.idx[k] : (int)k;
            if (i >= RR_MAX_PLAYERS) continue;
            g_xf.ok[i] = rr_read(g_xf.src[i].tr + RR_OFF_CTM_X, g_xf.dst[i], TR_READ_LEN) ? 1 : 0;
        }
        if (InterlockedIncrement(&g_xf.done) == g_xf.need) SetEvent(g_xf_done);
    }
    return 0;
}

static void xf_pool_init(void) {
    if (g_xf_started) return;
    g_xf_sem = CreateSemaphore(NULL, 0, XF_THREADS * 4, NULL);
    g_xf_done = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_xf_sem || !g_xf_done) return;
    for (int i = 0; i < XF_THREADS; i++)
        CreateThread(NULL, 0, xf_thread, NULL, 0, NULL);
    g_xf_started = 1;
}

static int xf_read_idx(const ENT_CACHE* src, const uint16_t* idx, int count,
                       float (*dst)[6], uint8_t* ok) {
    if (count <= 0) return 0;

    /* Prefer the batch IOCTL when supported. Retry it periodically after a
     * failure so a transient transport error does not permanently select the
     * thread-pool fallback.
     */
    static unsigned batch_retries;
    if (g_batch_state < 0 && (++batch_retries % 128u) == 0u) g_batch_state = 0;
    if (g_batch_state >= 0 && count <= BATCH_MAX) {
        static RR_RD_REQ reqs[BATCH_MAX];
        for (int k = 0; k < count; k++) {
            int i = idx[k];
            reqs[k].addr = src[i].tr + RR_OFF_CTM_X;
            reqs[k].size = TR_READ_LEN;
            reqs[k].dst = dst[i];
        }
        int r = rr_read_batch(reqs, count);
        if (r == -1) {
            g_batch_state = -1;
        } else {
            int good = 0;
            for (int k = 0; k < count; k++) {
                int i = idx[k];
                const float* f = dst[i];
                ok[i] = (f[0] != 0.0f || f[1] != 0.0f || f[2] != 0.0f ||
                         f[3] != 0.0f || f[4] != 0.0f || f[5] != 0.0f) ? 1 : 0;
                good += ok[i];
            }
            return good;
        }
    }

    if (!g_xf_started || count <= 3) {
        for (int k = 0; k < count; k++) {
            int i = idx[k];
            ok[i] = rr_read(src[i].tr + RR_OFF_CTM_X, dst[i], TR_READ_LEN) ? 1 : 0;
        }
        return count;
    }

    g_xf.src = src;
    g_xf.idx = idx;
    g_xf.count = count;
    g_xf.dst = dst;
    g_xf.ok = ok;
    g_xf.next = 0;
    g_xf.done = 0;
    int workers = count < XF_THREADS ? count : XF_THREADS;
    g_xf.need = workers;
    InterlockedExchange(&g_xf.run, 1);
    ReleaseSemaphore(g_xf_sem, workers, NULL);
    WaitForSingleObject(g_xf_done, 50);
    return count;
}

static int xf_read(const ENT_CACHE* src, int n, float (*dst)[6], uint8_t* ok) {
    static uint16_t all[RR_MAX_PLAYERS];
    int c = n < RR_MAX_PLAYERS ? n : RR_MAX_PLAYERS;
    for (int i = 0; i < c; i++) all[i] = (uint16_t)i;
    return xf_read_idx(src, all, c, dst, ok);
}

/* Skeleton. */
typedef struct { uint64_t cfg; int count, stride, loff, length, group[4]; int ok; } SKELCFG;
static SKELCFG g_skelcfg;

static int skel_config(uint64_t cfg) {
    if (g_skelcfg.ok && g_skelcfg.cfg == cfg) return 1;
    memset(&g_skelcfg, 0, sizeof(g_skelcfg));
    int count = rr_i32(cfg + 0x10);
    int stride = rr_i32(cfg + 0x14);
    int loff = rr_i32(cfg + 0x18);
    int length = rr_i32(cfg + 0x20);
    int group[4] = { rr_i32(cfg + 0x48), rr_i32(cfg + 0x44), rr_i32(cfg + 0x40), rr_i32(cfg + 0x38) };
    if (count < 2 || count > RR_MAX_BONES || stride < 9 || stride > 16 || loff < 0 || loff + 3 >= stride || length < 0 || length >= stride) return 0;
    for (int i = 0; i < 4; ++i) if (group[i] < 0 || group[i] >= count) return 0;
    g_skelcfg.cfg = cfg;
    g_skelcfg.count = count;
    g_skelcfg.stride = stride;
    g_skelcfg.loff = loff;
    g_skelcfg.length = length;
    memcpy(g_skelcfg.group, group, sizeof(group));
    g_skelcfg.ok = 1;
    return 1;
}

static int build_bones(uint64_t skel, float yaw, float px, float py, float pz,
                       float out[RR_MAX_BONES][3]) {
    if (!PL(skel)) return 0;
    uint64_t cfg = rr_u64(skel + 0x10);
    uint64_t arr = rr_u64(skel + 0x18);
    if (!PL(cfg) || !PL(arr)) return 0;
    if (!skel_config(cfg)) return 0;
    int count = g_skelcfg.count, stride = g_skelcfg.stride, loff = g_skelcfg.loff;

    static float bd[128];
    int need = count * stride;
    if (need > (int)(sizeof(bd) / sizeof(bd[0]))) return 0;
    if (!rr_read(arr + 0x20, bd, (size_t)need * 4)) return 0;

    float q[4] = { 0, 0, 0, 1 };
    float p[3] = { 0, 0, 0 };
    float s = sinf(yaw), c = cosf(yaw);
    for (int i = 0; i < count; i++) {
        const float* b = bd + (size_t)i * (size_t)stride;
        float lx = b[loff+0], ly = b[loff+1], lz = b[loff+2], lw = b[loff+3];
        float len = b[g_skelcfg.length];
        float qlen = lx*lx + ly*ly + lz*lz + lw*lw;
        if (!isfinite(qlen) || qlen < 0.25f || qlen > 2.25f || !isfinite(len) || len < 0 || len > 3.0f) return 0;
        float ax=q[0], ay=q[1], az=q[2], aw=q[3];
        q[0] = aw*lx + ax*lw + ay*lz - az*ly;
        q[1] = aw*ly - ax*lz + ay*lw + az*lx;
        q[2] = aw*lz + ax*ly - ay*lx + az*lw;
        q[3] = aw*lw - ax*lx - ay*ly - az*lz;

        float vx = 0, vy = 0, vz = len;
        float cx = q[1]*vz - q[2]*vy;
        float cy = q[2]*vx - q[0]*vz;
        float cz = q[0]*vy - q[1]*vx;
        float dx = vx + 2*(q[3]*cx + q[1]*cz - q[2]*cy);
        float dy = vy + 2*(q[3]*cy + q[2]*cx - q[0]*cz);
        float dz = vz + 2*(q[3]*cz + q[0]*cy - q[1]*cx);
        if (i > 0) { p[0] += dx; p[1] += dy; p[2] += dz; }

        float wx = p[0], wy = p[2], wz = p[1];
        out[i][0] = px + wx * c + wz * s;
        out[i][1] = py + wy;
        out[i][2] = pz - wx * s + wz * c;
        if (!isfinite(out[i][0]) || !isfinite(out[i][1]) || !isfinite(out[i][2]) ||
            fabsf(out[i][0] - px) > 4 || fabsf(out[i][1] - py) > 4 || fabsf(out[i][2] - pz) > 4) return 0;
    }
    return count;
}

/* Unity 6 native TransformAccess: a 48-byte local TRS array and a parent
 * index array. Both are read in two bounded snapshots per character. */
static int mesh_point(const MESH_TRS* trs, const int* parents, int count, int index, RR_V3* out) {
    if (index < 0 || index >= count) return 0;
    RR_V3 v = { trs[index].p[0], trs[index].p[1], trs[index].p[2] };
    int parent = parents[index], steps = 0;
    while (parent >= 0 && parent < count && steps++ < 128) {
        const MESH_TRS* m = &trs[parent];
        float x=v.x*m->s[0], y=v.y*m->s[1], z=v.z*m->s[2];
        float qx=m->q[0], qy=m->q[1], qz=m->q[2], qw=m->q[3];
        float tx=2*(qy*z-qz*y), ty=2*(qz*x-qx*z), tz=2*(qx*y-qy*x);
        v.x=m->p[0]+x+qw*tx+(qy*tz-qz*ty);
        v.y=m->p[1]+y+qw*ty+(qz*tx-qx*tz);
        v.z=m->p[2]+z+qw*tz+(qx*ty-qy*tx);
        parent = parents[parent];
    }
    if (parent != -1 || !isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) return 0;
    *out = v; return 1;
}

static int read_mesh_array(uint64_t address, void* dst, size_t size) {
    for (size_t off=0;off<size;) {
        size_t part=size-off; if (part>0x10000) part=0x10000;
        if (!rr_read(address+off,(uint8_t*)dst+off,part)) return 0;
        off+=part;
    }
    return 1;
}

static void diag_native_names(FILE* f, uint64_t native) {
    for (unsigned off=0;off<0x100;off+=8) {
        uint64_t p=rr_u64(native+off); char name[128];
        if (!PL(p)) continue;
        if (ascii_at(rr_u64(p+0x50),name,sizeof(name)))
            fprintf(f,"  offset=%X possible_gameobject_name=%s\n",off,name);
        if (ascii_at(p,name,sizeof(name)) && (strstr(name,"jnt") || strstr(name,"arm")))
            fprintf(f,"  offset=%X possible_name=%s\n",off,name);
    }
}

static void diag_validate_bone_names(const int* map, const uint64_t* slots) {
    if (!g_pose_diag_enabled) return;
    for (int b=0;b<RR_CHAM_BONES;++b) {
        if(map[b]<0)continue;
        uint64_t native=slots[map[b]], object=PL(native)?rr_u64(native+0x20):0;
        char name[128];
        if (!PL(object) || !ascii_at(rr_u64(object+0x50),name,sizeof(name))) continue;
        ++g_pose_diag.named_joints;
        if (strcmp(name,rr_cham_bones[b].name)) {
            ++g_pose_diag.mismatched_joints; ++g_pose_diag.mismatch_by_bone[b];
            FILE* f=fopen("rig-name-mismatches.log","a");
            if (f) { fprintf(f,"bone=%d expected=%s actual=%s\n",b,rr_cham_bones[b].name,name); fclose(f); }
        }
    }
}

/* Bound the hot-path reads to the mapped joints and their ancestors. */
static void cache_mesh_span(ENT_CACHE* e, const int* parents, int count) {
    int first = count, last = -1;
    for (int i = 0; i < 5 + RR_CHAM_BONES; ++i) {
        int index = i < 5 ? e->mesh_idx[i] : e->cham_idx[i - 5];
        int depth = 0;
        while (index >= 0 && index < count && depth++ < 128) {
            if (index < first) first = index;
            if (index > last) last = index;
            index = parents[index];
        }
        if (index != -1) return;
    }
    e->mesh_first = first;
    e->mesh_span = last - first + 1;
}

/* Bind the complete renderer palette by exact native joint names within the
 * anchored spine subtree. Facial joints and attachment placeholders cannot
 * safely be disambiguated from similar rest translations alone. */
static int native_joint_belongs(uint64_t native, uint64_t hierarchy, int index) {
    uint8_t fields[12]; uint64_t owner; int slot;
    if (!PL(native) || !rr_read(native+0x28,fields,sizeof(fields))) return 0;
    memcpy(&owner,fields,8); memcpy(&slot,fields+8,4);
    return owner==hierarchy && slot==index;
}

static void cache_cham_rig(ENT_CACHE* e, const ENT_CACHE* previous) {
    uint64_t h=e->mesh_h, matrices=rr_u64(h+0x18), parent_addr=rr_u64(h+0x20), pointer_addr=rr_u64(h+0x30);
    int count=rr_i32(h+0x10);
    if (count<1 || count>2048 || !PL(matrices) || !PL(parent_addr) || !PL(pointer_addr)) return;
    int parents[2048], map[RR_CHAM_BONES]; uint64_t slots[2048];
    if (!read_mesh_array(pointer_addr,slots,(size_t)count*sizeof(slots[0])) ||
        !read_mesh_array(parent_addr,parents,(size_t)count*sizeof(parents[0]))) return;
    int spine2=e->mesh_idx[3];
    if (spine2<0 || spine2>=count) return;
    map[0]=parents[spine2]; map[1]=spine2; map[2]=e->mesh_idx[2]; map[3]=e->mesh_idx[1]; map[4]=e->mesh_idx[0];
    for (int b=0;b<5;++b) {
        if (map[b]<0 || map[b]>=count || !PL(slots[map[b]]) ||
            (b>0 && parents[map[b]]!=map[b-1])) return;
    }
    if (previous && previous->cham_ready && previous->mesh_native[0]==e->mesh_native[0]) {
        int valid=1,complete=1;
        for (int b=0;b<5;++b) if (previous->cham_idx[b]!=map[b]) valid=0;
        for (int b=0;b<RR_CHAM_BONES;++b) {
            int idx=previous->cham_idx[b];
            if(idx<0 && !rr_cham_bones[b].required){complete=0;continue;}
            if (idx<0 || idx>=count || !PL(previous->cham_native[b]) || slots[idx]!=previous->cham_native[b] ||
                !native_joint_belongs(slots[idx],h,idx)) { valid=0; break; }
        }
        if (valid && complete && rr_rig_mapping_valid(previous->cham_idx,parents,count)) {
            memcpy(e->cham_idx,previous->cham_idx,sizeof(e->cham_idx));
            memcpy(e->cham_native,previous->cham_native,sizeof(e->cham_native));
            cache_mesh_span(e, parents, count);
            e->cham_ready=1; return;
        }
    }
    for (int b=5;b<RR_CHAM_BONES;++b) map[b]=-1;
    for (int i=0;i<count;++i) {
        if (!PL(slots[i]) || !rr_rig_descendant(parents,count,i,map[0]) || !native_joint_belongs(slots[i],h,i)) continue;
        uint64_t object=rr_u64(slots[i]+0x20);
        char name[128];
        if (!PL(object) || !ascii_at(rr_u64(object+0x50),name,sizeof(name))) continue;
        for (int b=0;b<RR_CHAM_BONES;++b) {
            if (strcmp(name,rr_cham_bones[b].name)) continue;
            if(map[b]==-2)break;
            if (map[b]>=0 && map[b]!=i) {
                if (g_pose_diag_enabled) { ++g_pose_diag.mapping_ambiguous; ++g_pose_diag.ambiguous_by_bone[b]; }
                if(rr_cham_bones[b].required)return;
                map[b]=-2;break;
            }
            map[b]=i;
            if (g_pose_diag_enabled) ++g_pose_diag.mapping_joints;
            break;
        }
    }
    for(int b=0;b<RR_CHAM_BONES;++b)if(map[b]<0)map[b]=-1;
    rr_rig_prune_optional(map,parents,count);
    if (!rr_rig_mapping_valid(map,parents,count)) {
        if (g_pose_diag_enabled) {
            static uint64_t recorded_root;
            if (recorded_root!=e->mesh_native[0]) {
                recorded_root=e->mesh_native[0];
                FILE* f=fopen("rig-mapping-missing.log","w");
                if (f) {
                    for (int b=0;b<RR_CHAM_BONES;++b)
                        if (map[b]<0) fprintf(f,"missing %d %s\n",b,rr_cham_bones[b].name);
                    fprintf(f,"head native name candidates:\n");
                    diag_native_names(f,e->mesh_native[0]);
                    fclose(f);
                }
            }
        }
        return;
    }
    diag_validate_bone_names(map,slots);
    for (int b=0;b<RR_CHAM_BONES;++b) { e->cham_idx[b]=map[b]; e->cham_native[b]=map[b]>=0?slots[map[b]]:0; }
    cache_mesh_span(e, parents, count);
    e->cham_ready=1;
}

static void cham_matrix_multiply(const float a[12], const float b[12], float out[12]) {
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c)
            out[r*4+c]=a[r*4]*b[c]+a[r*4+1]*b[4+c]+a[r*4+2]*b[8+c];
        out[r*4+3]=a[r*4]*b[3]+a[r*4+1]*b[7]+a[r*4+2]*b[11]+a[r*4+3];
    }
}

static int cham_local_matrix(const MESH_TRS* t, float m[12]) {
    float x=t->q[0],y=t->q[1],z=t->q[2],w=t->q[3], sx=t->s[0],sy=t->s[1],sz=t->s[2];
    float norm=x*x+y*y+z*z+w*w;
    if (!isfinite(norm) || norm<0.25f || norm>2.25f || !isfinite(sx) || !isfinite(sy) || !isfinite(sz) ||
        !isfinite(t->p[0]) || !isfinite(t->p[1]) || !isfinite(t->p[2])) return 0;
    m[0]=(1-2*(y*y+z*z))*sx; m[1]=2*(x*y-z*w)*sy; m[2]=2*(x*z+y*w)*sz; m[3]=t->p[0];
    m[4]=2*(x*y+z*w)*sx; m[5]=(1-2*(x*x+z*z))*sy; m[6]=2*(y*z-x*w)*sz; m[7]=t->p[1];
    m[8]=2*(x*z-y*w)*sx; m[9]=2*(y*z+x*w)*sy; m[10]=(1-2*(x*x+y*y))*sz; m[11]=t->p[2];
    return 1;
}

static int vehicle_cham_matrix(const MESH_TRS* chain, int depth, float matrix[12]) {
    if (depth < 1 || depth > RR_VEHICLE_POSE_DEPTH) return 0;
    for (int i = depth - 1; i >= 0; --i) {
        MESH_TRS trs = chain[i];
        float norm = 0.0f;
        for (int axis = 0; axis < 4; ++axis) norm += trs.q[axis] * trs.q[axis];
        if (!(norm >= 0.9f && norm <= 1.1f)) return 0;
        float inverse_norm = 1.0f / sqrtf(norm);
        for (int axis = 0; axis < 4; ++axis) trs.q[axis] *= inverse_norm;
        float local[12];
        if (!cham_local_matrix(&trs, local)) return 0;
        if (i == depth - 1) memcpy(matrix, local, sizeof(local));
        else {
            float combined[12];
            cham_matrix_multiply(matrix, local, combined);
            memcpy(matrix, combined, sizeof(combined));
        }
    }
    return fabsf(matrix[3]) <= 500000.0f && fabsf(matrix[7]) <= 500000.0f &&
           fabsf(matrix[11]) <= 500000.0f;
}

typedef struct { int valid; float matrix[12]; } VEH_CHAM_POSE;
typedef struct {
    uint64_t world, object, links[RR_VEHICLE_POSE_DEPTH];
    int id, depth;
    DWORD valid_at;
} VEH_CHAM_KEY;

static void read_vehicle_pose_batch(RR_RD_REQ* requests, int count) {
    if (g_batch_state >= 0 && rr_read_batch(requests, count) >= 0) {
        g_batch_state = 1;
        return;
    }
    if (g_batch_state == 0) g_batch_state = -1;
    for (int i = 0; i < count; ++i) {
        MESH_TRS* trs = requests[i].dst;
        if (!rr_read(requests[i].addr, trs, sizeof(*trs))) trs->q[3] = NAN;
    }
}

static const VEH_CHAM_POSE* sample_vehicle_poses(const VEH_CACHE* vehicles, int count,
                                                 unsigned generation, DWORD now) {
    static VEH_CHAM_POSE poses[RR_MAX_VEHICLES];
    static MESH_TRS chains[RR_MAX_VEHICLES][RR_VEHICLE_POSE_DEPTH];
    static VEH_CHAM_KEY keys[RR_MAX_VEHICLES];
    static unsigned last_generation;
    static DWORD last_sample;
    if (!g_pose_clock_changed && last_generation == generation &&
        now - last_sample < (g_batch_state < 0 ? 8u : 4u)) return poses;
    last_generation = generation;
    last_sample = now;

    RR_RD_REQ requests[BATCH_MAX];
    int requested = 0;
    for (int i = 0; i < count; ++i) {
        for (int link = 0; link < vehicles[i].render_depth; ++link) {
            MESH_TRS* trs = &chains[i][link];
            trs->q[3] = NAN;
            requests[requested++] = (RR_RD_REQ){ vehicles[i].render_trs[link], sizeof(*trs), trs };
            if (requested == BATCH_MAX) {
                read_vehicle_pose_batch(requests, requested);
                requested = 0;
            }
        }
    }
    if (requested) read_vehicle_pose_batch(requests, requested);
    for (int i = 0; i < count; ++i) {
        const VEH_CACHE* vehicle = &vehicles[i];
        VEH_CHAM_KEY* key = &keys[i];
        int same_pose = key->world == g_world && key->object == vehicle->obj && key->id == vehicle->id &&
            key->depth == vehicle->render_depth && vehicle->render_depth > 0 &&
            memcmp(key->links, vehicle->render_trs, vehicle->render_depth * sizeof(key->links[0])) == 0;
        float matrix[12];
        if (vehicle_cham_matrix(chains[i], vehicle->render_depth, matrix)) {
            DWORD elapsed = now - key->valid_at;
            if (same_pose && poses[i].valid && elapsed > 0 && elapsed < 100) {
                float dx=matrix[3]-poses[i].matrix[3], dy=matrix[7]-poses[i].matrix[7];
                float dz=matrix[11]-poses[i].matrix[11];
                if (dx*dx+dy*dy+dz*dz < 4.0f) {
                    float blend=(float)elapsed/((float)elapsed+3.0f);
                    for (int c=0;c<12;++c)
                        matrix[c]=poses[i].matrix[c]+(matrix[c]-poses[i].matrix[c])*blend;
                }
            }
            memcpy(poses[i].matrix, matrix, sizeof(matrix));
            poses[i].valid = 1;
            key->world = g_world;
            key->object = vehicle->obj;
            key->id = vehicle->id;
            key->depth = vehicle->render_depth;
            memcpy(key->links, vehicle->render_trs, key->depth * sizeof(key->links[0]));
            key->valid_at = now;
        } else if (!same_pose || now - key->valid_at > 80) {
            poses[i].valid = 0;
        }
    }
    return poses;
}

static int cham_world_matrix(int index, int count, const MESH_TRS* trs, const int* parents,
                             float world[][12], uint8_t* ready) {
    int path[128], length=0, i=index;
    while (i>=0 && i<count && !ready[i]) {
        if (length>=128) return 0;
        for (int k=0;k<length;++k) if (path[k]==i) return 0;
        path[length++]=i; i=parents[i];
    }
    if (i!=-1 && (i<0 || i>=count)) return 0;
    while (length) {
        int node=path[--length], parent=parents[node]; float local[12];
        if (!cham_local_matrix(&trs[node],local)) return 0;
        if (parent<0) memcpy(world[node],local,sizeof(local));
        else { if (!ready[parent]) return 0; cham_matrix_multiply(world[parent],local,world[node]); }
        ready[node]=1;
    }
    return 1;
}

static int build_cham_skin(const ENT_CACHE* e, RR_PLAYER* p, const MESH_TRS* trs,
                            const int* parents, const uint64_t* slots, int first, int count) {
    if (!e->cham_ready) return 0;
    float world[2048][12]; uint8_t ready[2048]={0};
    for (int b=0;b<RR_CHAM_BONES;++b) {
        for(int k=0;k<3;++k)p->bone[b][k]=NAN;
        for(int k=0;k<12;++k)p->cham_skin[b][k]=NAN;
        int idx=e->cham_idx[b]-first;
        if (idx<0 || idx>=count || !PL(e->cham_native[b]) || slots[idx]!=e->cham_native[b] ||
            !cham_world_matrix(idx,count,trs,parents,world,ready)) {
            if(rr_cham_bones[b].required)return 0;continue;
        }
        float dx=world[idx][3]-p->pos.x, dy=world[idx][7]-p->pos.y, dz=world[idx][11]-p->pos.z;
        if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) || dx*dx+dy*dy+dz*dz>36.0f) {
            if(rr_cham_bones[b].required)return 0;continue;
        }
        float skin[12]; int valid=1;
        cham_matrix_multiply(world[idx],rr_cham_bones[b].bind,skin);
        for (int component=0;component<12;++component)
            if (!isfinite(skin[component])) valid=0;
        if(!valid){if(rr_cham_bones[b].required)return 0;continue;}
        memcpy(p->cham_skin[b],skin,sizeof(skin));
        p->bone[b][0]=world[idx][3];
        p->bone[b][1]=world[idx][7];
        p->bone[b][2]=world[idx][11];
    }
    p->nbones=RR_CHAM_BONES; p->full_rig=1;
    p->bone_group[0]=RR_RIG_M_HEAD_1_JNT; p->bone_group[1]=RR_RIG_M_NECK_JNT;
    p->bone_group[2]=RR_RIG_M_SPINE_3_JNT; p->bone_group[3]=RR_RIG_M_SPINE_1_JNT;
    p->cham_valid=1; return 1;
}

static void reuse_cham_pose(const ENT_CACHE* e, const MOTION* m, RR_PLAYER* p,
                            RR_V3 origin, DWORD now) {
    if (!m->mesh_cham_valid || now - m->cham_time >= 100 ||
        (e->mesh_h && (m->mesh_h != e->mesh_h || m->mesh_head != e->mesh_native[0])) ||
        (e->cham_ready && m->cham_root != e->cham_native[0])) return;
    float dx = origin.x - m->cham_origin.x;
    float dy = origin.y - m->cham_origin.y;
    float dz = origin.z - m->cham_origin.z;
    if (dx*dx + dy*dy + dz*dz >= 0.25f) return;
    memcpy(p->cham_skin, m->cham_skin, sizeof(p->cham_skin));
    p->cham_sample_at = m->cham_time;
    p->cham_rig = m->cham_root;
    p->cham_valid = 1;
}

/* A single failed mesh read must not jump the box and skeleton back to the
 * replicated transform while the previous rendered pose is still valid. */
static int reuse_mesh_pose(const ENT_CACHE* e, const MOTION* m, RR_PLAYER* p,
                           RR_V3 origin, DWORD now) {
    if (!m->mesh_valid || m->mesh_nbones<1 || m->mesh_nbones>RR_MAX_BONES ||
        now - m->mesh_time >= 100 || !PL(e->mesh_h) ||
        m->mesh_h != e->mesh_h || m->mesh_head != e->mesh_native[0] ||
        m->mesh_cham_ready != e->cham_ready ||
        (e->cham_ready && m->cham_root != e->cham_native[0])) return 0;
    float dx = origin.x - m->mesh_origin.x, dy = origin.y - m->mesh_origin.y;
    float dz = origin.z - m->mesh_origin.z;
    if (dx*dx + dy*dy + dz*dz >= 0.25f) return 0;
    memcpy(p->bone, m->mesh_bone, sizeof(m->mesh_bone));
    p->nbones=m->mesh_nbones; p->mesh_bones=1; p->full_rig=m->mesh_full_rig;
    memcpy(p->bone_group,m->mesh_group,sizeof(p->bone_group));
    if (p->full_rig) { p->pos=m->mesh_feet; p->head=m->mesh_top; }
    return 1;
}

static float diag_quat_similarity(const float a[4],const float b[4]) {
    float dot=0, aa=0, bb=0;
    for (int i=0;i<4;++i) { dot+=a[i]*b[i]; aa+=a[i]*a[i]; bb+=b[i]*b[i]; }
    if (!isfinite(dot) || aa<0.25f || bb<0.25f) return 0;
    return fabsf(dot)/sqrtf(aa*bb);
}

/* Diagnostic only: compare local bone rotations, before bind matrices or
 * smoothing. Also compare two immediate reads of the same native slots. */
static void diagnose_local_rotations(const ENT_CACHE* e, MOTION* motion,
                                     const MESH_TRS* trs, int first, int count,
                                     uint64_t matrices) {
    if (!g_pose_diag_enabled || !motion || !e->cham_ready) return;
    const int bone[3]={RR_RIG_M_NECK_JNT,RR_RIG_L_ARM_1_JNT,RR_RIG_R_ARM_1_JNT};
    float q[3][4]; RR_RD_REQ reads[3]; MESH_TRS again[3];
    for (int i=0;i<3;++i) {
        int index=e->cham_idx[bone[i]]-first;
        if (index<0 || index>=count) return;
        memcpy(q[i],trs[index].q,sizeof(q[i]));
        reads[i]=(RR_RD_REQ){matrices+(uint64_t)e->cham_idx[bone[i]]*sizeof(MESH_TRS),sizeof(MESH_TRS),&again[i]};
    }
    ++g_pose_diag.quat_samples;
    const float rest_arm[4]={.0322450772f,-.0249513127f,.499164045f,.865547776f};
    int both_rest=diag_quat_similarity(q[1],rest_arm)>.9990f && diag_quat_similarity(q[2],rest_arm)>.9990f;
    if (both_rest) ++g_pose_diag.both_arms_rest;
    if (motion->diag_quat_count>=2 &&
        diag_quat_similarity(motion->diag_quat[0][1],rest_arm)>.9990f &&
        diag_quat_similarity(motion->diag_quat[0][2],rest_arm)>.9990f &&
        !both_rest && diag_quat_similarity(q[1],motion->diag_quat[1][1])>.9962f &&
        diag_quat_similarity(q[2],motion->diag_quat[1][2])>.9962f)
        ++g_pose_diag.rest_reversions;
    if (motion->diag_quat_count>=2) for (int i=0;i<3;++i) {
        if (diag_quat_similarity(q[i],motion->diag_quat[0][i]) < .9763f &&
            diag_quat_similarity(q[i],motion->diag_quat[1][i]) > .9962f) {
            if (i==0) ++g_pose_diag.neck_reversals;
            else if (i==1) ++g_pose_diag.left_reversals;
            else ++g_pose_diag.right_reversals;
        }
    }
    for (int axis=0;axis<3;++axis) {
        int available=motion->diag_rotation_count;
        for (int length=1;length<=6 && length<available;++length) {
            if (diag_quat_similarity(q[axis],motion->diag_rotation_history[axis][length])<.9962f) continue;
            int apart=1;
            for (int j=0;j<length;++j)
                if (diag_quat_similarity(q[axis],motion->diag_rotation_history[axis][j])>=.9763f) { apart=0; break; }
            if (apart) { ++g_pose_diag.rotation_run_return[length-1]; break; }
        }
        for (int j=7;j>0;--j)
            memcpy(motion->diag_rotation_history[axis][j],motion->diag_rotation_history[axis][j-1],sizeof(q[axis]));
        memcpy(motion->diag_rotation_history[axis][0],q[axis],sizeof(q[axis]));
    }
    if (motion->diag_rotation_count<8) ++motion->diag_rotation_count;
    memcpy(motion->diag_quat[1],motion->diag_quat[0],sizeof(motion->diag_quat[0]));
    memcpy(motion->diag_quat[0],q,sizeof(q));
    if (motion->diag_quat_count<2) ++motion->diag_quat_count;
    if ((g_pose_diag.quat_samples & 7u)==0 && rr_read_batch(reads,3)==3) {
        ++g_pose_diag.pair_samples;
        for (int i=0;i<3;++i) if (diag_quat_similarity(q[i],again[i].q)<.9763f) {
            ++g_pose_diag.pair_changes; break;
        }
    }
}


static void filter_arm_branches(const ENT_CACHE* e,MOTION* motion,MESH_TRS* trs,int first,int count,
                                DWORD now) {
    if(!motion || !e->cham_ready)return;
    int reset=!motion->mesh_valid || motion->mesh_h!=e->mesh_h || motion->mesh_head!=e->mesh_native[0] ||
              motion->cham_root!=e->cham_native[0] || now-motion->mesh_time>=100;
    const int chain_parents[4]={-1,0,1,2};
    MESH_TRS torso[4];float torso_q[4][4];int valid_torso=1;
    for(int b=0;b<4;++b) {
        int index=e->cham_idx[b]-first;
        if(index<0 || index>=count){valid_torso=0;break;}
        torso[b]=trs[index];memcpy(torso_q[b],torso[b].q,sizeof(torso_q[b]));
    }
    RR_V3 torso_tip;
    if(valid_torso && mesh_point(torso,chain_parents,4,3,&torso_tip)) {
        int hold=rr_pose_gate_hold(&motion->torso_gate,torso_q,4,&torso_tip.x,now,reset);
        for(int b=0;b<4;++b) {
            if(hold)trs[e->cham_idx[b]-first]=motion->torso_trs[b];
            else motion->torso_trs[b]=torso[b];
        }
    }
    /* Head rotation is outside the spine/neck chain. Hold its facial subtree
       with it, so the face never mixes a transient head with accepted bones. */
    int head_index=e->cham_idx[RR_RIG_M_HEAD_1_JNT]-first;
    if(head_index>=0 && head_index<count) {
        float head_q[4][4]={0};memcpy(head_q[0],trs[head_index].q,sizeof(head_q[0]));
        int hold=rr_pose_gate_hold(&motion->head_gate,head_q,1,trs[head_index].p,now,reset);
        for(int b=RR_RIG_M_HEAD_1_JNT;b<RR_RIG_L_ARM_0_JNT;++b) {
            int index=e->cham_idx[b]-first,saved=b-RR_RIG_M_HEAD_1_JNT;
            if(index<0 || index>=count)continue;
            if(hold)trs[index]=motion->head_trs[saved];
            else motion->head_trs[saved]=trs[index];
        }
    }
    const int starts[2]={RR_RIG_L_ARM_0_JNT,RR_RIG_R_ARM_0_JNT};
    const int ends[2]={RR_RIG_L_ARMTWIST_JNT,RR_RIG_R_ARMTWIST_JNT};
    for(int side=0;side<2;++side) {
        float q[4][4];MESH_TRS branch[4];int valid=1;
        for(int j=0;j<4;++j) {
            int index=e->cham_idx[starts[side]+j]-first;
            if(index<0 || index>=count) {valid=0;break;}
            memcpy(q[j],trs[index].q,sizeof(q[j]));
            branch[j]=trs[index];
        }
        if(!valid)continue;
        RR_V3 tip;
        if(!mesh_point(branch,chain_parents,4,3,&tip))continue;
        int hold=rr_pose_gate_hold(&motion->arm_gate[side],q,4,&tip.x,now,reset);
        for(int b=starts[side];b<=ends[side];++b) {
            int index=e->cham_idx[b]-first, saved=b-RR_RIG_L_ARM_0_JNT;
            if(index<0 || index>=count)continue;
            if(hold) trs[index]=motion->arm_trs[saved];
            else motion->arm_trs[saved]=trs[index];
        }
        if(hold && g_pose_diag_enabled)++g_pose_diag.arm_branch_holds[side];
    }
}

/* Keep the published feet/top stable when a frame falls back to a different
 * anchor (rendered mesh pose vs replicated transform). Only small deltas are
 * blended, so real movement and teleports still pass through unchanged. */
static void stabilize_published(MOTION* m, RR_PLAYER* p, DWORD now) {
    if (!m || !p) return;
    if (m->pub_valid) {
        DWORD el = now - m->pub_time;
        if (el > 0 && el < 60) {
            float b = (float)el / ((float)el + 10.0f);
            float dx = p->pos.x - m->pub_pos.x, dy = p->pos.y - m->pub_pos.y, dz = p->pos.z - m->pub_pos.z;
            if (isfinite(dx) && isfinite(dy) && isfinite(dz) && dx*dx + dy*dy + dz*dz < 1.44f) {
                p->pos.x = m->pub_pos.x + dx * b;
                p->pos.y = m->pub_pos.y + dy * b;
                p->pos.z = m->pub_pos.z + dz * b;
            }
            float hx = p->head.x - m->pub_top.x, hy = p->head.y - m->pub_top.y, hz = p->head.z - m->pub_top.z;
            if (isfinite(hx) && isfinite(hy) && isfinite(hz) && hx*hx + hy*hy + hz*hz < 1.44f) {
                p->head.x = m->pub_top.x + hx * b;
                p->head.y = m->pub_top.y + hy * b;
                p->head.z = m->pub_top.z + hz * b;
            }
        }
    }
    m->pub_pos = p->pos; m->pub_top = p->head;
    m->pub_time = now; m->pub_valid = 1;
}

static int build_mesh_bones(const ENT_CACHE* e, RR_PLAYER* p, DWORD now) {
    static int mesh_batch_state;
    if (!PL(e->mesh_h)) return 0;
    int use_cache=(br_get()->show_skeleton || br_get()->cham_on || br_get()->aa_on) && !p->local;
    MOTION* motion=use_cache ? motion_slot(e->obj) : NULL;
    RR_V3 origin=p->pos;
    if (motion && motion->mesh_valid && motion->mesh_h==e->mesh_h &&
        motion->mesh_head==e->mesh_native[0] && motion->mesh_cham_ready==e->cham_ready &&
        (!e->cham_ready || motion->cham_root==e->cham_native[0]) &&
        motion->mesh_nbones>0 && motion->mesh_nbones<=RR_MAX_BONES &&
        !g_pose_clock_changed && now-motion->mesh_time<4) {
        float dx=origin.x-motion->mesh_origin.x, dy=origin.y-motion->mesh_origin.y, dz=origin.z-motion->mesh_origin.z;
        if (dx*dx+dy*dy+dz*dz<0.25f) {
            memcpy(p->bone, motion->mesh_bone, sizeof(motion->mesh_bone));
            p->nbones=motion->mesh_nbones; p->mesh_bones=1; p->full_rig=motion->mesh_full_rig;
            memcpy(p->bone_group,motion->mesh_group,sizeof(p->bone_group));
            if (p->full_rig) { p->pos=motion->mesh_feet; p->head=motion->mesh_top; }
            reuse_cham_pose(e, motion, p, origin, now);
            stabilize_published(motion, p, now);
            return 1;
        }
    }
    if (motion && (motion->mesh_h!=e->mesh_h || motion->mesh_head!=e->mesh_native[0])) {
        motion->mesh_valid=0;
        motion->mesh_cham_valid=0;
        motion->diag_quat_count=0;
        motion->diag_rotation_count=0;
        motion->cham_hold_pending=0;
    }
    if (motion && e->cham_ready && motion->cham_root!=e->cham_native[0]) {
        motion->mesh_cham_valid=0;
        motion->diag_quat_count=0;
        motion->diag_rotation_count=0;
        motion->cham_hold_pending=0;
    }
    /* +0x10 is the allocated array size; +0x38 is a free-list index and can
     * be smaller than a valid index or -1. Verify each slot's native pointer. */
    uint8_t header[0x28]; int count=0; uint64_t matrices=0,parents=0,pointers=0;
    if (!rr_read(e->mesh_h+0x10,header,sizeof(header))) return 0;
    memcpy(&count,header,4); memcpy(&matrices,header+8,8);
    memcpy(&parents,header+0x10,8); memcpy(&pointers,header+0x20,8);
    if (count <= 0 || count > 2048 || !PL(matrices) || !PL(parents) || !PL(pointers)) return 0;
    int first = e->mesh_span > 0 ? e->mesh_first : 0;
    if (e->mesh_span > 0) {
        if (first < 0 || first >= count || e->mesh_span > count - first) return 0;
        count = e->mesh_span;
    }
    MESH_TRS trs[2048]; int hierarchy[2048]; uint64_t slots[2048]; RR_V3 pt[5];
    RR_RD_REQ reads[] = {
        {matrices + (uint64_t)first * sizeof(trs[0]), count * sizeof(trs[0]), trs},
        {parents + (uint64_t)first * sizeof(hierarchy[0]), count * sizeof(hierarchy[0]), hierarchy},
        {pointers + (uint64_t)first * sizeof(slots[0]), count * sizeof(slots[0]), slots},
    };
    int copied = -1;
    if (mesh_batch_state >= 0 && reads[0].size + reads[1].size + reads[2].size <= 0xF000) {
        copied = rr_read_batch(reads, (int)_countof(reads));
        if (copied >= 0 && copied != _countof(reads)) return 0;
        if (copied >= 0) mesh_batch_state = 1;
        else if (!mesh_batch_state) mesh_batch_state = -1;
    }
    if (copied < 0) for (size_t i = 0; i < _countof(reads); ++i) {
        if (!read_mesh_array(reads[i].addr, reads[i].dst, reads[i].size)) return 0;
    }
    for (int i = 0; i < count; ++i) {
        int parent = hierarchy[i];
        if (parent != -1) hierarchy[i] = parent >= first && parent < first + count ? parent - first : -2;
    }
    diagnose_local_rotations(e,motion,trs,first,count,matrices);
#ifdef RR_CAPTURE_LOCAL
    RR_CAPTURE_LOCAL(e,trs,first,count,now);
#endif
    filter_arm_branches(e,motion,trs,first,count,now);
    for (int i = 0; i < 5; ++i) {
        int index = e->mesh_idx[i] - first;
        if (index < 0 || index >= count || !PL(e->mesh_native[i]) || slots[index] != e->mesh_native[i]) return 0;
        if (!mesh_point(trs, hierarchy, count, index, &pt[i])) return 0;
        if (fabsf(pt[i].x-p->pos.x)>2 || fabsf(pt[i].y-p->pos.y)>3 || fabsf(pt[i].z-p->pos.z)>2) return 0;
    }
    RR_V3 chain[7] = { pt[4], pt[4], pt[3], pt[2], pt[1], pt[0], pt[0] };
    chain[1].y += p->crouch == 1 ? 0.48f : 0.88f; /* pelvis estimate */
    chain[6].y += 0.10f; /* top of head estimate */
    for (int i = 0; i < 7; ++i) {
        p->bone[i][0]=chain[i].x; p->bone[i][1]=chain[i].y; p->bone[i][2]=chain[i].z;
    }
    p->nbones=7;
    p->full_rig=0;
    p->bone_group[0]=5; p->bone_group[1]=4; p->bone_group[2]=3; p->bone_group[3]=1;
    p->mesh_bones=1;
    if (e->cham_ready && !build_cham_skin(e,p,trs,hierarchy,slots,first,count)) {
        /* A single failed sample should not blink the cham off; reuse the last
         * good pose when it is recent and belongs to the same rig. */
        if (motion && motion->mesh_cham_valid && motion->cham_root == e->cham_native[0] &&
            now - motion->cham_time < 50) {
            memcpy(p->cham_skin, motion->cham_skin, sizeof(p->cham_skin));
            p->cham_valid = 1;
            p->cham_sample_at = motion->cham_time;
        } else {
            p->cham_valid=0; p->nbones=0; p->full_rig=0; p->mesh_bones=0;
            return 0;
        }
    }
    if (p->cham_valid) {
        /* Blend the new joint matrices with the previous frame's pose. The
         * mesh is sampled asynchronously while the animator writes it, so raw
         * samples carry per-frame noise; the vehicle chams already smooth this
         * way. Keep the blend short so intentional motion is not delayed. */
        if (motion && motion->mesh_cham_valid && motion->cham_root == e->cham_native[0]) {
            DWORD elapsed = now - motion->cham_time;
            if (elapsed > 0 && elapsed < 60) {
                /* Separate sampling noise from real motion: blend only when the
                 * joints barely moved, otherwise the smoothing itself would lag
                 * behind a running or strafing target. */
                float move = 0.0f;
                int counted = 0;
                for (int b = 0; b < RR_CHAM_BONES; ++b) {
                    float dx = p->cham_skin[b][3]  - motion->cham_skin[b][3];
                    float dy = p->cham_skin[b][7]  - motion->cham_skin[b][7];
                    float dz = p->cham_skin[b][11] - motion->cham_skin[b][11];
                    if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz)) continue;
                    move += dx*dx + dy*dy + dz*dz;
                    ++counted;
                }
                float blend = 1.0f;
                if (counted > 0 && move / (float)counted < 0.0025f)   /* < 5 cm rms */
                    blend = (float)elapsed / ((float)elapsed + 4.0f);
                for (int b = 0; b < RR_CHAM_BONES; ++b) {
                    for (int c = 0; c < 12; ++c) {
                        float prev = motion->cham_skin[b][c];
                        if (!isfinite(prev) || !isfinite(p->cham_skin[b][c])) continue;
                        p->cham_skin[b][c] = prev + (p->cham_skin[b][c] - prev) * blend;
                    }
                }
            }
        }
        p->cham_sample_at=now;
    }
    int top_bone=isfinite(p->bone[RR_RIG_M_HEAD_2_JNT][0]) ? RR_RIG_M_HEAD_2_JNT : RR_RIG_M_HEAD_1_JNT;
    RR_V3 top=p->full_rig ? (RR_V3){p->bone[top_bone][0],p->bone[top_bone][1],p->bone[top_bone][2]} : chain[6];
    if (motion) {
        if (p->cham_valid) {
            float hx=origin.x-motion->cham_origin.x, hy=origin.y-motion->cham_origin.y, hz=origin.z-motion->cham_origin.z;
            if (hx*hx+hy*hy+hz*hz>.0225f) motion->cham_hold_pending=0;
            if (g_pose_diag_enabled && !p->local) {
                if (p->inveh) {
                    ++g_pose_diag.seated_samples;
                    if (hx*hx+hy*hy+hz*hz>.0225f) ++g_pose_diag.seated_root_bypass;
                } else ++g_pose_diag.standing_samples;
            }
            if (g_pose_diag_enabled && motion->mesh_cham_valid && motion->mesh_valid && motion->mesh_h==e->mesh_h &&
                motion->mesh_head==e->mesh_native[0] && motion->cham_root==e->cham_native[0] &&
                hx*hx+hy*hy+hz*hz<=.0225f && now-motion->cham_time<100) {
                const int bones[3]={RR_RIG_M_HEAD_1_JNT,RR_RIG_L_ARM_1_JNT,RR_RIG_R_ARM_1_JNT}; int mask=0;
                for (int b=0;b<3;++b) {
                    float sum=0;
                    for(int row=0;row<3;++row) for(int col=0;col<3;++col) {
                        int idx=row*4+col; float v=p->cham_skin[bones[b]][idx]-motion->cham_skin[bones[b]][idx];sum+=v*v;
                    }
                    if (sum>.16f) mask|=1<<b;
                }
                ++g_pose_diag.change_mask[mask];
            }
            if (motion->mesh_cham_valid && motion->mesh_valid && motion->mesh_h==e->mesh_h &&
                motion->mesh_head==e->mesh_native[0] && motion->cham_root==e->cham_native[0] &&
                hx*hx+hy*hy+hz*hz<=.0225f &&
                rr_cham_coherence_hold(motion->cham_skin,p->cham_skin,motion->cham_yaw,p->rot_yaw,
                                       now,now-motion->cham_time,&motion->cham_hold_since,&motion->cham_hold_pending)) {
                if (g_pose_diag_enabled) { ++g_pose_diag.held_spikes; if (p->inveh) ++g_pose_diag.seated_holds; }
                /* Hold the previous pose for this frame instead of dropping the
                 * character: the spike is discarded, but the cham does not
                 * blink off for a frame. */
                memcpy(p->cham_skin, motion->cham_skin, sizeof(p->cham_skin));
                p->cham_sample_at = motion->cham_time;
            }
            p->cham_sample_at = now;
            motion->cham_root = e->cham_native[0];
            motion->cham_time = now;
            motion->cham_origin = origin;
            motion->cham_yaw = p->rot_yaw;
            motion->mesh_cham_valid = 1;
            memcpy(motion->cham_skin,p->cham_skin,sizeof(motion->cham_skin));
        }
        motion->mesh_h=e->mesh_h; motion->mesh_head=e->mesh_native[0];
        motion->mesh_time=now; motion->mesh_origin=origin; motion->mesh_cham_ready=e->cham_ready;
        motion->mesh_nbones=p->nbones; motion->mesh_full_rig=p->full_rig;
        memcpy(motion->mesh_group,p->bone_group,sizeof(motion->mesh_group));
        motion->mesh_feet=pt[4]; motion->mesh_top=top;
        memcpy(motion->mesh_bone,p->bone,sizeof(motion->mesh_bone));
        motion->mesh_valid=1;
    }
    if (p->full_rig) { p->pos=pt[4]; p->head=top; }
    stabilize_published(motion, p, now);
    return 1;
}

#ifdef RR_PORT_DIAGNOSTICS
static int vehicle_marker_position(const VEH_CACHE* v, const RR_V3* model, RR_V3* out) {
    if (!PL(v->marker_transform) || !PL(v->marker_native) || !PL(v->marker_h) ||
        rr_u64(v->marker_transform+0x10)!=v->marker_native) return 0;
    uint8_t native_fields[12], header[0x28]; uint64_t h=0, matrices=0,parents=0,pointers=0;
    int capacity=0,idx=-1;
    if (!rr_read(v->marker_native+0x28,native_fields,sizeof(native_fields))) return 0;
    memcpy(&h,native_fields,8); memcpy(&idx,native_fields+8,4);
    if (h!=v->marker_h || !rr_read(h+0x10,header,sizeof(header))) return 0;
    memcpy(&capacity,header,4); memcpy(&matrices,header+8,8);
    memcpy(&parents,header+0x10,8); memcpy(&pointers,header+0x20,8);
    if (capacity<1 || capacity>2048 || idx!=v->marker_idx || idx<0 || idx>=capacity ||
        !PL(matrices) || !PL(parents) || !PL(pointers) || rr_u64(pointers+(uint64_t)idx*8)!=v->marker_native) return 0;
    MESH_TRS trs[2048]; int hierarchy[2048]; RR_V3 pos;
    if (!read_mesh_array(matrices,trs,(size_t)capacity*sizeof(trs[0])) ||
        !read_mesh_array(parents,hierarchy,(size_t)capacity*sizeof(hierarchy[0])) ||
        !mesh_point(trs,hierarchy,capacity,idx,&pos)) return 0;
    float x=pos.x-model->x,y=pos.y-model->y,z=pos.z-model->z;
    if (x*x+y*y+z*z>225.0f) return 0;
    *out=pos; return 1;
}

/* Camera. */
#endif
static int read_camera_from_local(RR_CAMERA* cam) {
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    int count = g_ncache;
    static ENT_CACHE loc[RR_MAX_PLAYERS];
    memcpy(loc, g_cache, sizeof(ENT_CACHE) * (size_t)count);
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    for (int i = 0; i < count; i++) {
        if (!loc[i].local) continue;
        float pos[3] = { 0, 0, 0 }, look[3] = { 0, 0, 0 };
        if (!rr_read(loc[i].tr + RR_OFF_CTM_X, pos, sizeof(pos))) return 0;
        uint64_t lm = rr_u64(loc[i].obj + RR_OFF_LOOK_MODEL);
        if (!PL(lm)) return 0;
        if (!rr_read(lm + 0x10, look, sizeof(look))) return 0;
        float len = sqrtf(look[0]*look[0] + look[1]*look[1] + look[2]*look[2]);
        if (len < 0.5f) return 0;
        look[0] /= len; look[1] /= len; look[2] /= len;
        /* Eye position, no lateral offset: the 2.5 m third-person pull-back made
         * the projection drift as the look direction changed. */
        cam->pos.x = pos[0];
        cam->pos.y = pos[1] + 1.60f;
        cam->pos.z = pos[2];
        cam->fwd.x = look[0]; cam->fwd.y = look[1]; cam->fwd.z = look[2];
        cam->rot.w = 1.0f;
        cam->fov = g_fov;
        cam->valid = 1;
        return 1;
    }
    return 0;
}

static int read_camera(RR_CAMERA* cam) {
    memset(cam, 0, sizeof(*cam));
    /* The real camera is third-person and sits on the player: use it whenever it
     * is near them. The local-eye fallback is first-person, which offsets every
     * marker, so it is only used when no plausible camera exists. */
    if (camera_near_local(g_cam_obj) == 1) {
        uint8_t buf[0x50];
        if (rr_read(g_cam_obj + RR_OFF_CAM_POS, buf, sizeof(buf))) {
            memcpy(&cam->pos, buf + 0x00, sizeof(RR_V3));
            memcpy(&cam->rot, buf + (RR_OFF_CAM_ROT - RR_OFF_CAM_POS), sizeof(RR_Q4));
            memcpy(&cam->fwd, buf + (RR_OFF_CAM_FWD - RR_OFF_CAM_POS), sizeof(RR_V3));
            memcpy(&cam->right, buf + (RR_OFF_CAM_RIGHT - RR_OFF_CAM_POS), sizeof(RR_V3));
            memcpy(&cam->up, buf + (RR_OFF_CAM_UP - RR_OFF_CAM_POS), sizeof(RR_V3));
            cam->fov = g_fov;
            /* CinemachineBrain.CurrentCameraState.Lens.FieldOfView is the
             * blended vertical FOV, including aiming and camera transitions. */
            uint64_t aim = g_aim_updater;
            if (PL(aim) && PL(g_battle) && rr_u64(g_battle+RR_OFF_BATTLE_WORLD)==g_world &&
                rr_u64(aim+RR_OFF_AIM_BATTLE)==g_battle && class_named(rr_u64(aim),"BattleAimTargetUpdater")) {
                uint64_t brain = rr_u64(aim+RR_OFF_AIM_BRAIN);
                float fov = 0; RR_V3 raw;
                if (PL(brain) && class_named(rr_u64(brain),"CinemachineBrain") &&
                    rr_read(brain+0xB0,&fov,sizeof(fov)) && rr_read(brain+0xFC,&raw,sizeof(raw)) &&
                    isfinite(fov) && fov>10.0f && fov<170.0f &&
                    isfinite(raw.x) && isfinite(raw.y) && isfinite(raw.z)) {
                    float dx=raw.x-cam->pos.x, dy=raw.y-cam->pos.y, dz=raw.z-cam->pos.z;
                    if (dx*dx+dy*dy+dz*dz < 25.0f) cam->fov=fov;
                }
            }
            cam->valid = 1;
            cam->model_source = g_cam_obj;
            return 1;
        }
    }
    return read_camera_from_local(cam);
}

/* Init. */
/* Diagnostic-only bootstrap: identify the character class from the live
 * UnityEntry roster instead of doing a full class discovery scan. */
int game_pose_diag_init(uint64_t world_hint) {
    ENTRY_ROOTS roots;
    if (!entry_roots(&roots)) {
        memset(&roots,0,sizeof(roots));
        if (!u_is(world_hint,"WorldEntitiesModel")) return 0;
        roots.world=world_hint;
    }
    uint64_t entries=0; int n=0, stride=0;
    uint64_t dict=rr_u64(roots.world + RR_OFF_WEM_BATTLE_CHARS);
    if (!u_dict(dict,&n,&entries,&stride) || stride!=0x18 || n<1 || n>4096) return 0;
    for (int i=0;i<n;++i) {
        uint64_t obj=rr_u64(entries+(uint64_t)i*stride+0x10);
        if (u_is(obj,"BattleCharacterModel")) { g_k_char=u_klass(obj); break; }
    }
    if (!g_k_char) return 0;
    g_world=roots.world; g_k_world=u_klass(roots.world); g_battle=roots.battle;g_battle_model=roots.battle_model;
    g_cam_obj=roots.camera; g_k_cam=roots.camera?u_klass(roots.camera):0;
    if (!g_cs_ready) { InitializeCriticalSection(&g_cache_cs); g_cs_ready=1; }
    xf_pool_init();
    find_heap_regions();
    instance_scan(g_inst_lo,g_inst_hi);
    if (InterlockedCompareExchange(&g_worker_started,1,0)==0) {
        HANDLE worker=CreateThread(NULL,0,scan_worker,NULL,0,NULL);
        if (worker) CloseHandle(worker); else InterlockedExchange(&g_worker_started,0);
    }
    return 1;
}
int game_init(void) {
    uint32_t size = 0;
    if (!rr_module("GameAssembly.dll", &g_module, &size)) {
        set_status("GameAssembly.dll not found");
        return 0;
    }
    runtime_load();
    chain_load("robrun_chain.txt");

    if (!g_cs_ready) { InitializeCriticalSection(&g_cache_cs); g_cs_ready = 1; }
    xf_pool_init();
    DWORD t0 = GetTickCount();
    dlog("init start pid=%u", g_rr_pid);

    /* Dynamic discovery: nothing from the previous process is trusted. */
    find_heap_regions();
    dlog("regions=%d after %lu ms", g_nregions, (unsigned long)(GetTickCount() - t0));

    /* Fast path: the managed UnityEntry chain reaches the world without the
     * native manager and without scanning every model class; the world's roster
     * then supplies the character class. */
    {
        ENTRY_ROOTS early;
        if (entry_roots_class(&early) && PL(early.world)) {
            g_world = early.world;
            g_battle = early.battle;
            g_battle_model = early.battle_model;
            g_k_world = u_klass(early.world);
            if (early.camera) {
                g_cam_obj = early.camera;
                g_k_cam = u_klass(early.camera);
            }
            if (!PL(g_k_char)) {
                uint64_t dict = rr_u64(g_world + RR_OFF_WEM_BATTLE_CHARS);
                int dn = 0, ds = 0;
                uint64_t de = 0;
                if (u_dict(dict, &dn, &de, &ds) && dn > 0 && ds >= 0x18) {
                    /* Skip empty hash slots (hash < 0): their value is not an
                     * object, and taking it used to poison the class. */
                    static uint8_t rowbuf[0x20000];
                    size_t bytes = (size_t)dn * (size_t)ds;
                    if (bytes <= sizeof(rowbuf) && rr_read(de, rowbuf, bytes)) {
                        for (int i = 0; i < dn && !PL(g_k_char); ++i) {
                            int32_t hash = 0;
                            memcpy(&hash, rowbuf + (size_t)i * (size_t)ds, 4);
                            if (hash < 0) continue;
                            uint64_t obj = 0;
                            memcpy(&obj, rowbuf + (size_t)i * (size_t)ds + 0x10, 8);
                            if (!PL(obj)) continue;
                            uint64_t k = rr_u64(obj);
                            if (PL(k) && class_named(k, "BattleCharacterModel")) g_k_char = k;
                        }
                    }
                }
            }
            dlog("fast entry world=0x%llX battle=0x%llX char=0x%llX",
                 (unsigned long long)g_world, (unsigned long long)g_battle,
                 (unsigned long long)g_k_char);
        }
    }
    if (!g_k_char || !class_named(g_k_char, "BattleCharacterModel"))
        g_k_char = 0;
    if (!g_k_char) g_k_char = resolve_class("BattleCharacterModel");
    if (!g_k_char) { set_status("BattleCharacterModel class not found [%s]", g_dbg); return 0; }
    dlog("char=0x%llX after %lu ms", (unsigned long long)g_k_char, (unsigned long)(GetTickCount() - t0));
    if (!g_k_cam || !class_named(g_k_cam, "BattleCameraModel")) g_k_cam = 0;
    if (!g_k_cam) g_k_cam = resolve_camera_class("BattleCameraModel");

    /* The scene-level battle container is not used for roster reads. Avoid
     * scanning the entire process for it during every initialization. */
    ENTRY_ROOTS roots;
    int entry_ok = entry_roots(&roots);
    dlog("entry_roots=%d world=0x%llX battle=0x%llX after %lu ms", entry_ok,
         (unsigned long long)roots.world, (unsigned long long)roots.battle,
         (unsigned long)(GetTickCount() - t0));
    if (entry_ok) {
        g_world = roots.world;
        g_k_world = u_klass(roots.world);
        g_battle = roots.battle;
        g_battle_model = roots.battle_model;
        if (roots.camera) g_cam_obj = roots.camera;
    }
    if (g_k_world && !class_named(g_k_world, "WorldEntitiesModel")) g_k_world = 0;
    if (!g_k_world) g_k_world = resolve_world_class();
    dlog("world_class=0x%llX after %lu ms", (unsigned long long)g_k_world,
         (unsigned long)(GetTickCount() - t0));
    if (g_k_world && !PL(g_world) && g_saved_pid == g_rr_pid &&
        PL(g_saved_world) && rr_u64(g_saved_world) == g_k_world)
        g_world = g_saved_world;
    if (!PL(g_world) && g_k_world) g_world = find_world_instance();
    dlog("world_instance=0x%llX after %lu ms", (unsigned long long)g_world,
         (unsigned long)(GetTickCount() - t0));
    /* Only the dictionary path may run here. A fresh attach often happens
     * before the world is loaded, and the heap fallback then scanned for
     * minutes inside this call, blocking the reader and the first snapshots.
     * The scan worker owns the fallback scan, so initialization stays short
     * and discovery can pick up the world as soon as it appears. */
    int n = PL(g_world) ? instance_scan(g_inst_lo, g_inst_hi) : 0;
    dlog("instance_scan=%d after %lu ms", n, (unsigned long)(GetTickCount() - t0));
    g_last_scan = GetTickCount();
    if (n > 0 && (!g_cam_obj || !camera_validate(g_cam_obj)))
        g_cam_obj = find_camera_scan();
    runtime_save();
    dlog("init done after %lu ms", (unsigned long)(GetTickCount() - t0));

    if (InterlockedCompareExchange(&g_worker_started, 1, 0) == 0) {
        HANDLE worker = CreateThread(NULL, 0, scan_worker, NULL, 0, NULL);
        if (worker) CloseHandle(worker);
        else InterlockedExchange(&g_worker_started, 0);
    }
    set_status("instance scan: %d characters, camera=%s", n,
               camera_near_local(g_cam_obj) == 1 ? "model" : "local");
    return n > 0;
}

int game_discovery_active(void) { return InterlockedCompareExchange(&g_worker_started, 0, 0) != 0; }

#ifdef RR_PORT_DIAGNOSTICS
int game_is_aiming(void) {
    uint64_t local = 0, world = g_world;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i = 0; i < g_ncache; ++i) if (g_cache[i].local) { local = g_cache[i].obj; break; }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (!PL(world) || !PL(local) || rr_u64(world) != g_k_world || rr_u64(local) != g_k_char) return 0;
    uint64_t priv = rr_u64(local + 0x10);
    if (!PL(priv) || !class_named(rr_u64(priv), "BattleCharacterPrivateModel")) return 0;
    uint64_t aiming = rr_u64(priv + 0x1F0);
    if (!PL(aiming) || !class_named(rr_u64(aiming), "AimingModel")) return 0;
    uint64_t data = rr_u64(aiming + 0x10);
    if (!PL(data) || !class_named(rr_u64(data), "BoolData")) return 0;
    uint8_t value = 0;
    return rr_read(data + 0x18, &value, 1) && value == 1 && world == g_world;
}

int game_shot_event(void) {
    static __declspec(thread) uint64_t cached_world, cached_local, cached_event;
    static __declspec(thread) DWORD validated_at;
    uint64_t local=0, world=g_world;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    for (int i=0;i<g_ncache;++i) if (g_cache[i].local) { local=g_cache[i].obj; break; }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);
    if (!PL(world) || !PL(local)) { cached_event=0; return -1; }
    DWORD now=GetTickCount();
    if (!PL(cached_event) || cached_world!=world || cached_local!=local || now-validated_at>=100) {
        cached_event=0;
        if (rr_u64(world)!=g_k_world || rr_u64(local)!=g_k_char) return -1;
        uint64_t priv=rr_u64(local+0x10);
        if (!PL(priv) || !class_named(rr_u64(priv),"BattleCharacterPrivateModel")) return -1;
        uint64_t event=rr_u64(priv+0x18);
        if (!PL(event) || !class_named(rr_u64(event),"EventModel") || world!=g_world) return -1;
        cached_world=world; cached_local=local; cached_event=event; validated_at=now;
    }
    uint8_t invoked=0;
    if (!rr_read(cached_event+0x10,&invoked,1) || invoked>1 || world!=g_world) { cached_event=0; return -1; }
    return invoked ? 1 : 0;
}


#endif
/* Extrapolate the replicated position between observed updates.
 * The sampled build used roughly 50 ms network ticks. Estimate velocity from
 * position changes, then lead by elapsed time within a bounded window. This
 * is an estimate: stops and direction changes can temporarily be predicted
 * incorrectly. A successfully sampled live rig overrides this fallback.
 */
typedef struct { uint64_t key; RR_V3 raw, vel; DWORD changed; } EX_SLOT;
static EX_SLOT g_ex[RR_MAX_PLAYERS];

/* This is an inference, not replicated hidden state. Keep it bounded and
 * world-scoped so a disconnect cannot create a permanent ghost. */
typedef struct {
    int used, id, near_object, seen;
    int object_id, voice_candidate;
    uint64_t identity;
    DWORD last_seen, last_near_check, last_near;
    RR_V3 object_pos;
    RR_V3 object_anchor;
    unsigned relationships;
    char name[RR_NAME_MAX], role[20], weapon[20];
} HIDE_GUESS;
static HIDE_GUESS g_hide_guesses[RR_MAX_PLAYERS];
static uint64_t g_guess_world;

static int update_hide_guesses(RR_PLAYER* out, int count, int max, uint64_t world,
                               const HIDE_OBJECT* objects, int nobjects, DWORD now) {
    if (!world || g_guess_world != world) {
        memset(g_hide_guesses, 0, sizeof(g_hide_guesses));
        g_guess_world = world;
    }
    if (!world) return count;
    int have_local = 0;
    for (int i = 0; i < count; ++i) if (out[i].valid && out[i].local == RR_LOCAL_CONFIRMED) have_local = 1;
    if (!have_local) return count;
    for (int j = 0; j < RR_MAX_PLAYERS; ++j) g_hide_guesses[j].seen = 0;
    for (int i = 0; i < count; ++i) {
        RR_PLAYER* p = &out[i];
        if (!p->valid || p->local || p->is_loot || p->is_vehicle || p->id <= 0) continue;
        HIDE_GUESS* slot = NULL; HIDE_GUESS* free_slot = NULL;
        for (int j = 0; j < RR_MAX_PLAYERS; ++j) {
            HIDE_GUESS* g = &g_hide_guesses[j];
            if (g->used && g->id == p->id) { slot = g; break; }
            if (!free_slot && (!g->used || now - g->last_seen > 60000)) free_slot = g;
        }
        if (!slot) { if (!free_slot) continue; slot = free_slot; memset(slot, 0, sizeof(*slot)); slot->used = 1; slot->id = p->id; }
        slot->seen = 1; slot->last_seen = now; slot->relationships = p->relationships;
        snprintf(slot->name, sizeof(slot->name), "%s", p->name);
        snprintf(slot->role, sizeof(slot->role), "%s", p->role);
        snprintf(slot->weapon, sizeof(slot->weapon), "%s", p->weapon);
        if (p->hidden_in_object || p->inveh) { slot->near_object = 0; continue; }
        if (!slot->last_near_check || now - slot->last_near_check >= 200) {
            slot->last_near_check = now; slot->near_object = 0; float closest = 4.0f;
            for (int j = 0; j < nobjects; ++j) {
                RR_V3 interaction=objects[j].has_interaction?objects[j].anchor:objects[j].pos;
                float dx = p->pos.x - interaction.x, dz = p->pos.z - interaction.z;
                float dy = fabsf(p->pos.y - objects[j].pos.y), d2 = dx*dx + dz*dz;
                if (dy <= 2.5f && d2 <= closest) {
                    closest = d2; slot->near_object = 1; slot->object_pos = objects[j].pos;
                    slot->object_id=objects[j].id;slot->object_anchor=objects[j].anchor;
                }
            }
            if (slot->near_object) slot->last_near = now;
        }
    }
    for (int j = 0; j < RR_MAX_PLAYERS; ++j) {
        HIDE_GUESS* g = &g_hide_guesses[j];
        if (!g->used || g->seen) continue;
        DWORD age = now - g->last_seen;
        if (age > 60000) { memset(g, 0, sizeof(*g)); continue; }
        if (age < 700 || !g->near_object || g->last_seen - g->last_near > 1000 || count >= max) continue;
        int still_present=0;
        for(int o=0;o<nobjects;++o)if(objects[o].id==g->object_id){still_present=1;break;}
        if(!still_present)continue;
        RR_PLAYER* p = &out[count++]; memset(p, 0, sizeof(*p));
        p->health = -1;
        p->valid = 1; p->id = g->id; p->relationships = g->relationships;
        p->hidden_in_object = 2; p->pos=g->object_pos;p->head=g->object_anchor;
        snprintf(p->name, sizeof(p->name), "%s", g->name);
        snprintf(p->role, sizeof(p->role), "%s", g->role);
        snprintf(p->weapon, sizeof(p->weapon), "%s", g->weapon);
    }
    return count;
}

/* Slot membership survives streamed-out character models. Account identity
 * prevents a recycled slot from inheriting the previous player's guess. */
static int apply_hide_signals(RR_PLAYER* out,int count,int max,uint64_t world,
                              const RR_HIDE_SLOT* slots,int nslots,int slots_known,
                              const HIDE_OBJECT* objects,int nobjects) {
    if(g_guess_world!=world){memset(g_hide_guesses,0,sizeof(g_hide_guesses));g_guess_world=world;}
    if(!slots_known)return count;
    int local_id=-1;
    for(int i=0;i<count;++i)if(out[i].valid&&out[i].local==RR_LOCAL_CONFIRMED)local_id=out[i].id;
    for(int j=0;j<RR_MAX_PLAYERS;++j) {
        HIDE_GUESS* g=&g_hide_guesses[j];if(!g->used)continue;
        const RR_HIDE_SLOT* slot=NULL;
        for(int s=0;s<nslots;++s)if(slots[s].id==g->id){slot=&slots[s];break;}
        if(!slot||(g->identity&&slot->identity&&g->identity!=slot->identity)) {memset(g,0,sizeof(*g));continue;}
        if(slot->identity)g->identity=slot->identity;
        /* A default -1 alone has not been validated as an absence signal.
           Only clear a voice candidate after observing its positive->-1. */
        if(g->voice_candidate&&slot->voice_known&&slot->voice_hide_id==-1) {
            g->near_object=0;g->voice_candidate=0;
        }
    }
    if(local_id<0)return count;
    for(int s=0;s<nslots;++s) {
        const RR_HIDE_SLOT* slot=&slots[s];
        if(slot->id==local_id||!slot->voice_known||slot->voice_hide_id<0)continue;
        const HIDE_OBJECT* object=NULL;
        for(int o=0;o<nobjects;++o)if(objects[o].id==slot->voice_hide_id){object=&objects[o];break;}
        if(!object)continue;
        int confirmed=object->occupant_known&&object->occupant==slot->id;
        if(object->occupant_known&&object->occupant>=0&&!confirmed)continue;
        RR_PLAYER* p=NULL;
        for(int i=0;i<count;++i)if(out[i].valid&&!out[i].is_vehicle&&!out[i].is_loot&&out[i].id==slot->id){p=&out[i];break;}
        HIDE_GUESS* history=NULL;
        for(int j=0;j<RR_MAX_PLAYERS;++j)if(g_hide_guesses[j].used&&g_hide_guesses[j].id==slot->id){history=&g_hide_guesses[j];break;}
        if(p&&(p->hidden_in_object==1||!confirmed)) {
            if(p->hidden_in_object==2&&history){history->voice_candidate=1;history->identity=slot->identity;}
            continue;
        }
        if(!p) {
            if(count>=max)continue;
            p=&out[count++];memset(p,0,sizeof(*p));p->id=slot->id;p->health=-1;p->armor=-1;p->crouch=-1;
            if(history) {
                p->relationships=history->relationships;
                snprintf(p->role,sizeof(p->role),"%s",history->role);
                snprintf(p->weapon,sizeof(p->weapon),"%s",history->weapon);
            }
            snprintf(p->name,sizeof(p->name),"%s",slot->name);
        }
        p->valid=1;p->hidden_in_object=confirmed?1:2;p->pos=object->pos;p->head=object->anchor;
        p->cham_valid=0;p->nbones=0;p->mesh_bones=0;p->full_rig=0;
        if(history){history->voice_candidate=1;history->identity=slot->identity;}
    }
    return count;
}

static int snapshot_read(RR_PLAYER* out, int max, RR_CAMERA* cam) {
    if (!out || max <= 0) return 0;
    BR_Config* cfg = br_get();
    LONG ex_n = 0;   /* markers led past the simulated tick this pass */

    /* Published snapshot: copied under the lock so the scan worker can swap in
     * a new roster without the read thread ever seeing a half-written entry. */
    static ENT_CACHE snap[RR_MAX_PLAYERS];
    static VEH_CACHE vsnap[RR_MAX_VEHICLES];
    static LOOT_CACHE lsnap[2 * RR_MAX_LOOT + RR_MAX_WORLD_MARKERS];
    static HIDE_OBJECT hide_snap[RR_MAX_HIDE_OBJECTS];
    static RR_HIDE_SLOT hide_slots[RR_MAX_PLAYERS];
    int nl = 0;
    if (g_cs_ready) EnterCriticalSection(&g_cache_cs);
    int n = g_ncache < max ? g_ncache : max;
    if (n > 0) memcpy(snap, g_cache, sizeof(ENT_CACHE) * (size_t)n);
    uint64_t vehicle_world = g_world;
    unsigned vehicle_generation = g_vehicle_generation;
    int want_vehicles = cfg->vehicle_chams || cfg->show_vehicle_markers;
#ifdef RR_PORT_DIAGNOSTICS
    want_vehicles |= cfg->show_vehicles;
#endif
    int nv = want_vehicles && g_vehicle_world == vehicle_world ? g_nvehicles : 0;
    if (nv > 0) memcpy(vsnap, g_vehicles, sizeof(VEH_CACHE) * (size_t)nv);
    uint64_t loot_world = g_world;
    DWORD now = GetTickCount();
    int hide_fresh=g_hide_world==g_world&&now-g_hide_stamp<1500;
    int nhide = hide_fresh ? g_nhide_objects : 0;
    if (nhide > 0) memcpy(hide_snap, g_hide_objects, sizeof(hide_snap[0]) * (size_t)nhide);
    int hide_slots_known=hide_fresh&&g_hide_snapshot.slots_known;
    int nhide_slots=hide_slots_known?g_hide_snapshot.nslots:0;
    if(nhide_slots>0)memcpy(hide_slots,g_hide_snapshot.slots,sizeof(hide_slots[0])*(size_t)nhide_slots);
    const int loot_enabled[] = { cfg->show_loot, cfg->show_loot_nodes };
    for (int slot = 0; slot < 2; ++slot) {
        if (!loot_enabled[slot] || g_loot_world[slot] != loot_world || now - g_loot_stamp[slot] >= 5000) continue;
        int count = g_nloot[slot];
        memcpy(lsnap + nl, g_loot[slot], sizeof(LOOT_CACHE) * (size_t)count);
        nl += count;
    }
    if (g_markers_world == loot_world && now - g_markers_stamp < 5000) {
        memcpy(lsnap + nl, g_world_markers, sizeof(LOOT_CACHE) * (size_t)g_nworld_markers);
        nl += g_nworld_markers;
    }
    if (g_cs_ready) LeaveCriticalSection(&g_cache_cs);

    RR_CAMERA camv;
    memset(&camv, 0, sizeof(camv));

    DWORD tnow = GetTickCount();
    DWORD pose_now = timeGetTime();

    const ENT_CACHE* local = NULL;
    for (int i = 0; i < n; ++i) {
        if (snap[i].local) { local = &snap[i]; break; }
    }
    int local_is_police = local && strcmp(local->role, "cop") == 0;
    const char* local_clan = local ? local->clan_tag : "";

    int written = 0;
    for (int i = 0; i < n && written < max; i++) {
        float f[6] = {0};
        if (snap[i].hidden_in_object) {
            f[0] = snap[i].hidden_pos.x; f[1] = snap[i].hidden_pos.y; f[2] = snap[i].hidden_pos.z;
        } else if (!rr_read(snap[i].tr + RR_OFF_CTM_X, f, TR_READ_LEN)) continue;
        if (!isfinite(f[0]) || !isfinite(f[1]) || !isfinite(f[2])) continue;
        if (fabsf(f[0]) > 500000.0f || fabsf(f[1]) > 500000.0f || fabsf(f[2]) > 500000.0f) continue;

        /* velocity from the previous sample, smoothed; resets on teleports */
        MOTION* m = motion_slot(snap[i].obj);
        m->t = tnow;
        m->ok = 1;
        RR_V3 sampled_pos = { f[0], f[1], f[2] };

        RR_PLAYER* p = &out[written];
        memset(p, 0, sizeof(*p));
        p->addr = snap[i].obj;
        p->id = snap[i].id;
        if (snap[i].cham_ready) p->cham_rig = snap[i].cham_native[0];
        p->health = -1;
        if (cfg->show_health) {
            uint64_t data = rr_u64(snap[i].obj + RR_OFF_CHAR_DATA);
            uint64_t private_data = PL(data) ? rr_u64(data + RR_OFF_DATA_PRIVATE) : 0;
            if (u_is(private_data, "BattleCharacterPrivateData")) {
                uint64_t health = rr_u64(private_data + RR_OFF_PRIVATE_HEALTH); int32_t value = -1;
                if (u_is(health, "IntData") && rr_read(health + RR_OFF_PRIM_VALUE, &value, sizeof value) &&
                    value >= 0 && value <= 10000) p->health = value;
            }
        }
        p->armor = snap[i].armor;
        p->crouch = snap[i].crouch;
        p->local = snap[i].local;
        p->hidden_in_object = snap[i].hidden_in_object;
        if (!p->local) {
            if (local_clan[0] && strcmp(local_clan, snap[i].clan_tag) == 0)
                p->relationships |= RR_REL_CLAN;
            if (local_is_police && strcmp(snap[i].role, "cop") == 0)
                p->relationships |= RR_REL_POLICE;
            if (snap[i].is_party)
                p->relationships |= RR_REL_PARTY;
        }
        p->inveh = snap[i].inveh;
        p->vehicle_id = snap[i].vehicle_id;
        snprintf(p->veh, sizeof(p->veh), "%s", snap[i].veh);
        snprintf(p->role, sizeof(p->role), "%s", snap[i].role);
        snprintf(p->weapon, sizeof(p->weapon), "%s", snap[i].weapon);
        p->knocked = snap[i].knocked;
        snprintf(p->knockphase, sizeof(p->knockphase), "%s", snap[i].knockphase);
        snprintf(p->name, sizeof(p->name), "%s", snap[i].name);
        p->pos.x = f[0]; p->pos.y = f[1]; p->pos.z = f[2];
        p->rot_yaw = f[4];
        p->head = p->pos;
        p->head.y += (snap[i].crouch == 1) ? 1.05f : 1.75f;
        if(p->hidden_in_object)p->head=snap[i].hidden_anchor;

        /* Lead the replicated transform to the present. This runs before the
         * mesh override below, so a character whose rig was read successfully
         * keeps its rendered pose and is left untouched - only the markers
         * that actually fall back to the 20 Hz transform are corrected. */
        if (!p->hidden_in_object) {
            EX_SLOT* s = &g_ex[i];
            if (s->key != snap[i].obj) {
                s->key = snap[i].obj;
                s->raw = p->pos;
                s->vel = (RR_V3){0.0f, 0.0f, 0.0f};
                s->changed = pose_now;
            } else {
                if (p->pos.x != s->raw.x || p->pos.y != s->raw.y || p->pos.z != s->raw.z) {
                    DWORD dt = pose_now - s->changed;
                    if (dt >= 8 && dt <= 250) {
                        s->vel.x = (p->pos.x - s->raw.x) * 1000.0f / (float)dt;
                        s->vel.y = (p->pos.y - s->raw.y) * 1000.0f / (float)dt;
                        s->vel.z = (p->pos.z - s->raw.z) * 1000.0f / (float)dt;
                    } else {
                        s->vel = (RR_V3){0.0f, 0.0f, 0.0f};
                    }
                    s->raw = p->pos;
                    s->changed = pose_now;
                }
                DWORD lead = pose_now - s->changed;
                float speed = sqrtf(s->vel.x * s->vel.x + s->vel.z * s->vel.z);
                if (lead >= 4 && lead <= 70 && speed >= 0.3f && speed <= 20.0f) {
                    float lead_s = (float)lead * 0.001f;
                    p->pos.x += s->vel.x * lead_s;
                    p->pos.y += s->vel.y * lead_s;
                    p->pos.z += s->vel.z * lead_s;
                    p->head = p->pos;
                    p->head.y += (snap[i].crouch == 1) ? 1.05f : 1.75f;
                    ++ex_n;
                }
            }
        }

        if (!p->hidden_in_object && (cfg->show_skeleton || cfg->aa_on) && snap[i].skel) {
            int nb = build_bones(snap[i].skel, f[4], p->pos.x, p->pos.y, p->pos.z, p->bone);
            p->nbones = (nb > 0 && nb <= RR_MAX_BONES) ? nb : 0;
            if (p->nbones) memcpy(p->bone_group, g_skelcfg.group, sizeof(p->bone_group));
        }
        int mesh_ok=0;
        if (!p->hidden_in_object && (cfg->enabled || cfg->aa_on) && snap[i].mesh_h &&
            (!p->inveh || cfg->cham_on || cfg->aa_on)) {
            /* A malformed or recycled rig must not suppress every ESP marker. */
            if (g_pose_diag_enabled && !p->local) ++g_pose_diag.attempts;
            __try { mesh_ok=build_mesh_bones(&snap[i],p,pose_now); }
            __except (EXCEPTION_EXECUTE_HANDLER) { mesh_ok=0; p->cham_valid=0; }
            if (g_pose_diag_enabled && !p->local) { if (mesh_ok) ++g_pose_diag.success; else ++g_pose_diag.failed; }
        }
        else if (g_pose_diag_enabled && !p->local && !p->hidden_in_object && !snap[i].mesh_h) ++g_pose_diag.missing_rig;
        if (!mesh_ok && !p->hidden_in_object && !p->local &&
            (cfg->show_skeleton || cfg->cham_on || cfg->aa_on)) {
            mesh_ok = reuse_mesh_pose(&snap[i], m, p, sampled_pos, pose_now);
            if (g_pose_diag_enabled && mesh_ok) ++g_pose_diag.reused_mesh;
        }
        if (mesh_ok && !p->full_rig) {
            /* Anchor markers to the resolved rig for standing and seated players.
             * This keeps the head marker aligned with the displayed body instead of a
             * fixed-height estimate. build_mesh_bones() validates the pose before use.
             */
            p->pos.x=p->bone[0][0]; p->pos.y=p->bone[0][1]; p->pos.z=p->bone[0][2];
            p->head.x=p->bone[6][0]; p->head.y=p->bone[6][1]; p->head.z=p->bone[6][2];
        }
        /* Retain the last rendered pose through a brief read gap. */
        if (!p->hidden_in_object && (cfg->cham_on || cfg->aa_on) && !p->local && !p->cham_valid) {
            reuse_cham_pose(&snap[i], m, p, sampled_pos, pose_now);
            if (g_pose_diag_enabled && p->cham_valid) ++g_pose_diag.reused_cham;
        }
        p->valid = 1;
        written++;
    }

    written=apply_hide_signals(out,written,max,vehicle_world,hide_slots,nhide_slots,hide_slots_known,hide_snap,nhide);
    written = update_hide_guesses(out, written, max, vehicle_world, hide_snap, nhide, tnow);
    /* New inference slots are created by update_hide_guesses; associate their
       current account before they can survive a disconnect or slot reuse. */
    written=apply_hide_signals(out,written,max,vehicle_world,hide_slots,nhide_slots,hide_slots_known,hide_snap,nhide);
    for(int i=0;i<written;++i)if(out[i].hidden_in_object) {
        float dx=out[i].pos.x-camv.pos.x,dy=out[i].pos.y-camv.pos.y,dz=out[i].pos.z-camv.pos.z;
        out[i].dist=sqrtf(dx*dx+dy*dy+dz*dz);
    }

    const VEH_CHAM_POSE* poses = cfg->vehicle_chams && nv > 0 && vehicle_world == g_world
        ? sample_vehicle_poses(vsnap, nv, vehicle_generation, GetTickCount()) : NULL;
    for (int i = 0; i < nv && written < max && vehicle_world == g_world; ++i) {
        const VEH_CACHE* vehicle = &vsnap[i];
        RR_V3 pos;
        int cham_ok = poses && poses[i].valid;
        if (cham_ok) pos = (RR_V3){poses[i].matrix[3], poses[i].matrix[7], poses[i].matrix[11]};
        int marker_ok = 0;
#ifdef RR_PORT_DIAGNOSTICS
        if (!cham_ok && cfg->show_vehicles) {
            uint8_t head[0x18]; uint64_t klass; int32_t id;
            if (!rr_read(vehicle->obj, head, sizeof(head))) continue;
            memcpy(&klass, head, 8); memcpy(&id, head + 0x10, 4);
            if (klass != vehicle->klass || id != vehicle->id) continue;
            if (!rr_read(vehicle->tr + 0x38, &pos, sizeof(pos)) ||
                !(fabsf(pos.x) <= 500000 && fabsf(pos.y) <= 500000 && fabsf(pos.z) <= 500000)) continue;
            RR_V3 rendered;
            marker_ok = vehicle_marker_position(vehicle, &pos, &rendered);
            if (marker_ok) pos = rendered;
        } else
#endif
        if (!cham_ok && cfg->show_vehicle_markers) {
            uint8_t head[0x18]; uint64_t klass; int32_t id;
            if (!rr_read(vehicle->obj, head, sizeof head)) continue;
            memcpy(&klass, head, 8); memcpy(&id, head + 0x10, 4);
            if (klass != vehicle->klass || id != vehicle->id ||
                !rr_read(vehicle->tr + 0x38, &pos, sizeof pos) ||
                !isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z) ||
                fabsf(pos.x) > 500000 || fabsf(pos.y) > 500000 || fabsf(pos.z) > 500000) continue;
        } else if (!cham_ok) continue;
        RR_PLAYER* p = &out[written++];
        memset(p, 0, sizeof(*p));
        p->valid = 1; p->is_vehicle = vehicle->kind + 1; p->id = vehicle->id; p->addr = vehicle->obj;
        p->pos = p->head = pos;
        p->occupants=vehicle->occupants; p->occupants_known=vehicle->occupants_known; p->vehicle_rendered=marker_ok;
        p->vehicle_cham_valid = cham_ok;
        if (cham_ok) memcpy(p->vehicle_pose, poses[i].matrix, sizeof(p->vehicle_pose));
        snprintf(p->name, sizeof(p->name), "%s", vehicle->name);
        snprintf(p->role, sizeof(p->role), "%s", vehicle->role);
    }

#ifdef RR_PORT_DIAGNOSTICS
    /* Occupancy is partial: only characters present in the current roster can
     * be counted. An absent count does not imply the vehicle is empty. */
    for (int i = 0; i < written; ++i) if (out[i].is_vehicle && !out[i].occupants_known)
        for (int j = 0; j < written; ++j)
            if (!out[j].is_vehicle && !out[j].is_loot && out[j].inveh && out[j].vehicle_id == out[i].id)
                ++out[i].occupants;

#endif
    if (loot_world == g_world) for (int i = 0; i < nl && written < max; ++i) {
        const LOOT_CACHE* loot = &lsnap[i];
        RR_PLAYER* p = &out[written++];
        memset(p, 0, sizeof(*p));
        p->valid = 1;
        p->is_loot = loot->kind;
        p->loot_rarity = loot->rarity;
        p->id = loot->id;
        p->pos = p->head = loot->pos;
        snprintf(p->name, sizeof(p->name), "%s", loot->name);
    }
    InterlockedExchange(&g_ex_count, ex_n);
    /* Sample the camera after entity reads so turning the view does not wait
     * behind the whole player/skeleton update. */
    int cam_ok = read_camera(&camv);
    if (cam) *cam = camv;
    if (cam_ok) for (int i = 0; i < written; ++i) {
        float dx = out[i].pos.x - camv.pos.x, dy = out[i].pos.y - camv.pos.y, dz = out[i].pos.z - camv.pos.z;
        out[i].dist = sqrtf(dx * dx + dy * dy + dz * dz);
    }

    int have_local = 0;
    for (int i = 0; i < written; i++) if (out[i].local) have_local = 1;
    if (!have_local) {
        int best = -1; float best_d = 1e30f;
        for (int i = 0; i < written; i++)
            if (!out[i].is_vehicle && !out[i].is_loot && out[i].dist > 0.0f && out[i].dist < best_d) { best_d = out[i].dist; best = i; }
        if (best >= 0 && best_d < 10.0f) out[best].local = RR_LOCAL_ESTIMATED;
    }
    return written;
}
typedef struct {
    uint64_t world, model;
    int period;
    int64_t server, simulated, player;
} FRAME_CLOCK;

static int read_frame_clock(FRAME_CLOCK* clock) {
    memset(clock, 0, sizeof(*clock));
    uint64_t world = (uint64_t)InterlockedCompareExchange64(&g_time_world, 0, 0);
    uint64_t model = (uint64_t)InterlockedCompareExchange64(&g_time_model, 0, 0);
    if (!world || world != g_world || !u_is(model, "TimeModel")) return 0;
    unsigned char fields[0x68];
    if (!rr_read(model + 0x10, fields, sizeof(fields))) return 0;
    uint64_t simulated, player;
    memcpy(&clock->period, fields, 4);
    memcpy(&simulated, fields + 0x50, 8);
    memcpy(&player, fields + 0x58, 8);
    memcpy(&clock->server, fields + 0x60, 8);
    if (clock->period < 1 || clock->period > 1000 ||
        !u_is(simulated, "SetNow") || !u_is(player, "SetNow") ||
        !rr_read(simulated + 0x10, &clock->simulated, 8) ||
        !rr_read(player + 0x10, &clock->player, 8) || world != g_world) return 0;
    clock->world = world;
    clock->model = model;
    InterlockedExchange(&g_tick_period, clock->period);
    return 1;
}

static int frame_clock_changed(const FRAME_CLOCK* a, const FRAME_CLOCK* b) {
    return a->world != b->world || a->model != b->model || a->period != b->period ||
           a->server != b->server || a->simulated != b->simulated || a->player != b->player;
}

int game_snapshot(RR_PLAYER* out, int max, RR_CAMERA* cam) {
    static FRAME_CLOCK previous;
    FRAME_CLOCK before, after;
    int have_clock = read_frame_clock(&before);
    g_pose_clock_changed = have_clock && frame_clock_changed(&before, &previous);
    int count = snapshot_read(out, max, cam);
    /* A boundary can split entity and camera reads. Retry once with fresh rigs.
       The external reader cannot make Unity's transform writes atomic. */
    if (have_clock && read_frame_clock(&after)) {
        if (frame_clock_changed(&before, &after)) {
            g_pose_clock_changed = 1;
            count = snapshot_read(out, max, cam);
        }
        previous = after;
    } else {
        memset(&previous, 0, sizeof(previous));
        InterlockedExchange(&g_tick_period, 0);
    }
    g_pose_clock_changed = 0;
    return count;
}

/* Project. */

int game_w2s(const RR_CAMERA* cam, const RR_V3* world, int w, int h,
             float* sx, float* sy) {
    if (!cam || !cam->valid || !world) return 0;

    float fx = cam->fwd.x, fy = cam->fwd.y, fz = cam->fwd.z;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) return 0;
    fx /= fl; fy /= fl; fz /= fl;

    /* Prefer the camera's own basis (+0x48 right, +0x54 up): deriving it from
     * world up ignores camera roll and skews markers when the view tilts. */
    float rx = cam->right.x, ry = cam->right.y, rz = cam->right.z;
    float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    float vx = cam->up.x, vy = cam->up.y, vz = cam->up.z;
    float vl = sqrtf(vx * vx + vy * vy + vz * vz);
    if (rl < 0.5f || rl > 1.5f || vl < 0.5f || vl > 1.5f) {
        const float ux = 0.0f, uy = 1.0f, uz = 0.0f;
        rx = uy * fz - uz * fy;
        ry = uz * fx - ux * fz;
        rz = ux * fy - uy * fx;
        rl = sqrtf(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-4f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; rl = 1.0f; }
        vx = fy * rz - fz * ry;
        vy = fz * rx - fx * rz;
        vz = fx * ry - fy * rx;
        vl = sqrtf(vx * vx + vy * vy + vz * vz);
        if (vl < 1e-4f) vl = 1.0f;
    }
    rx /= rl; ry /= rl; rz /= rl;
    vx /= vl; vy /= vl; vz /= vl;

    float dx = world->x - cam->pos.x;
    float dy = world->y - cam->pos.y;
    float dz = world->z - cam->pos.z;
    float cx = dx * rx + dy * ry + dz * rz;
    float cy = dx * vx + dy * vy + dz * vz;
    float cz = dx * fx + dy * fy + dz * fz;

    if (cz <= 0.05f) return 0;

    float aspect = (h > 0) ? (float)w / (float)h : 1.7778f;
    float tan_half = tanf(cam->fov * 3.14159265f / 360.0f);
    float ndc_x = (cx / cz) / (tan_half * aspect);
    float ndc_y = (cy / cz) / tan_half;

    if (sx) *sx = (ndc_x * 0.5f + 0.5f) * (float)w;
    if (sy) *sy = (0.5f - ndc_y * 0.5f) * (float)h;
    return (ndc_x > -1.2f && ndc_x < 1.2f && ndc_y > -1.2f && ndc_y < 1.2f) ? 1 : 0;
}

/* Normalize the camera and compute the lens once per mesh projection
 * batch instead of repeating square roots and tanf for every vertex.
 */
void game_w2s_many(const RR_CAMERA* cam, const RR_V3* world, int count, int w, int h, float* xy) {
    if (!world || !xy || count<=0) return;
    for (int i=0;i<count;++i) xy[i*2]=xy[i*2+1]=NAN;
    if (!cam || !cam->valid || w<=0 || h<=0) return;
    float fx=cam->fwd.x,fy=cam->fwd.y,fz=cam->fwd.z;
    float fl=sqrtf(fx*fx+fy*fy+fz*fz); if (fl<1e-6f || !isfinite(fl)) return;
    fx/=fl;fy/=fl;fz/=fl;
    float rx=cam->right.x,ry=cam->right.y,rz=cam->right.z,rl=sqrtf(rx*rx+ry*ry+rz*rz);
    float ux=cam->up.x,uy=cam->up.y,uz=cam->up.z,ul=sqrtf(ux*ux+uy*uy+uz*uz);
    if (rl<0.5f || rl>1.5f || ul<0.5f || ul>1.5f) {
        rx=fz;ry=0;rz=-fx;rl=sqrtf(rx*rx+rz*rz);
        if (rl<1e-4f) { rx=1;ry=rz=0;rl=1; }
        ux=fy*rz-fz*ry;uy=fz*rx-fx*rz;uz=fx*ry-fy*rx;
        ul=sqrtf(ux*ux+uy*uy+uz*uz);if(ul<1e-4f)ul=1;
    }
    rx/=rl;ry/=rl;rz/=rl;ux/=ul;uy/=ul;uz/=ul;
    float t=tanf(cam->fov*3.14159265f/360.0f);
    if (!isfinite(t) || fabsf(t)<1e-6f) return;
    float px=(float)h*0.5f/t,py=px;
    for (int i=0;i<count;++i) {
        float dx=world[i].x-cam->pos.x,dy=world[i].y-cam->pos.y,dz=world[i].z-cam->pos.z;
        float depth=dx*fx+dy*fy+dz*fz;
        if (!isfinite(depth) || depth<=0.05f) continue;
        float x=(dx*rx+dy*ry+dz*rz)/depth, y=(dx*ux+dy*uy+dz*uz)/depth;
        if (isfinite(x) && isfinite(y)) { xy[2*i]=w*0.5f+x*px; xy[2*i+1]=h*0.5f-y*py; }
    }
}





















































