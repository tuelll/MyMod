// Oil Leaking - port do script CLEO (PC) para plugin AML (GTA SA Android, ARM32, v2.00)
// Veiculos com vida < 500 deixam pocas de oleo sob o motor. Fogo ou tiro perto da poca
// incendeia o oleo e (opcional) explode o veiculo.
#include <mod/amlmod.h>
#include <android/log.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

MYMODCFG(net.oilleaking.aml, Oil Leaking, 1.0.0, Port)

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OilLeaking", __VA_ARGS__)

struct CVector { float x, y, z; };
struct RwTexture;

// ---------------- Offsets (layout do CVehicle = PC 1.0 / Android 2.00) ----------------
// Se algo nao funcionar no seu libGTASA.so, os suspeitos sao estes valores.
static constexpr int OFF_MATRIX_PTR  = 0x14;   // CPlaceable::m_matrix
static constexpr int OFF_MODEL_INDEX = 0x22;   // CEntity::m_nModelIndex (u16)
static constexpr int OFF_HANDLING    = 0x384;  // CVehicle::m_pHandlingData
static constexpr int OFF_HEALTH      = 0x4C0;  // CVehicle::m_fHealth (float)
static constexpr int OFF_SUBCLASS    = 0x594;  // CVehicle::m_nVehicleSubClass
static constexpr int OFF_ENGINETYPE  = 0x75;   // handling + 0x75 (igual ao script CLEO)
static constexpr int OFF_MI_VEHSTRUCT= 0x58;   // CVehicleModelInfo::m_pVehicleStruct
static constexpr int DUMMY_ENGINE    = 5;      // m_avDummyPos[5]

enum { SUB_AUTOMOBILE = 0, SUB_MTRUCK = 1, SUB_QUAD = 2, SUB_BIKE = 9 };

static constexpr uint32_t FIRE_TIME_LIMIT = 10000;
static constexpr uint32_t DROP_IDLE_TIME  = 20000;
static constexpr int      DROP_INTENSITY  = 140;
static constexpr size_t   MAX_DROPS       = 160;
static constexpr int      SHADOW_TYPE     = 0;   // SHADOW_DEFAULT (preto com alpha). Ajuste se ficar estranho.

// ---------------- Config ----------------
static float g_minSize = 0.1f, g_maxSize = 0.5f, g_radius = 4.0f;
static bool  g_explode = true;

// ---------------- Funcoes / dados do jogo ----------------
typedef void* (*GetVehicle_t)(int);
typedef void  (*Store_t)(uint8_t, RwTexture*, CVector*, float, float, float, float, int16_t,
                         uint8_t, uint8_t, uint8_t, float, bool, float, void*, bool);
typedef int   (*StartFire_t)(void*, const CVector&, void*, float, uint8_t, int8_t, int);
typedef int   (*NumFires_t)(void*, CVector*, float);
typedef void  (*ClearFlags_t)(void*);
typedef void  (*Blow_t)(void*, void*, uint8_t);

static GetVehicle_t  fnGetVehicle;
static Store_t       fnStore;
static StartFire_t   fnStartFire;
static NumFires_t    fnNumFires;
static ClearFlags_t  fnClearFlags;
static Blow_t        fnBlowAuto, fnBlowBike;
static void**        pVehiclePool;
static uint32_t*     pTimeMs;
static RwTexture**   ppBloodTex;
static void**        pModelInfoPtrs;
static void*         g_fireMgr = nullptr;      // capturado de CFireManager::Update (this == &gFireManager)

static void (*Update_orig)(void*);
static void (*DoBulletImpact_orig)(void*, void*, void*, CVector*, CVector*, void*, int);

// ---------------- Estado ----------------
struct Drop {
    CVector pos; float angle, size; int intensity; int vehicle;
    uint32_t tA, tB, tSub; int phase;
    bool createFire, waitFire, explodeCar, endIdle; int fires;
};
static std::vector<Drop> g_drops;
static std::unordered_map<int, uint32_t> g_nextSpawn;
static CVector g_ext; static bool g_extValid = false;   // "ExternalCreateFire" do script
static uint32_t g_lastClean = 0;

static inline uint8_t* P(void* p) { return (uint8_t*)p; }
static inline float dist3(const CVector& a, const CVector& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return sqrtf(dx*dx + dy*dy + dz*dz);
}
static bool Ok(const void* p) { return (uintptr_t)p > 0x10000; }

static bool VehPos(void* veh, CVector& out) {
    uint8_t* m = *(uint8_t**)(P(veh) + OFF_MATRIX_PTR);
    if (!Ok(m)) return false;
    float* f = (float*)m; out = { f[12], f[13], f[14] }; return true;
}

static bool EnginePos(void* veh, CVector& out) {
    uint8_t* m = *(uint8_t**)(P(veh) + OFF_MATRIX_PTR);
    if (!Ok(m)) return false;
    float* f = (float*)m;
    CVector l = { 0.0f, 1.5f, 0.3f };  // fallback
    uint16_t mi = *(uint16_t*)(P(veh) + OFF_MODEL_INDEX);
    if (pModelInfoPtrs && mi < 20000) {
        uint8_t* info = (uint8_t*)pModelInfoPtrs[mi];
        if (Ok(info)) {
            uint8_t* vs = *(uint8_t**)(info + OFF_MI_VEHSTRUCT);
            if (Ok(vs)) {
                CVector d = ((CVector*)vs)[DUMMY_ENGINE];
                if (fabsf(d.x) + fabsf(d.y) + fabsf(d.z) > 0.01f && fabsf(d.y) < 10.f && fabsf(d.x) < 10.f && fabsf(d.z) < 10.f) l = d;
            }
        }
    }
    out.x = f[12] + f[0]*l.x + f[4]*l.y + f[8]*l.z;
    out.y = f[13] + f[1]*l.x + f[5]*l.y + f[9]*l.z;
    out.z = f[14] + f[2]*l.x + f[6]*l.y + f[10]*l.z;
    return true;
}

static void DrawDrop(const Drop& d) {
    if (!fnStore || !ppBloodTex || !*ppBloodTex || d.intensity <= 0) return;
    float a = d.angle * 0.0174532925f, s = sinf(a), c = cosf(a);
    CVector p = d.pos;
    fnStore(SHADOW_TYPE, *ppBloodTex, &p, -s * d.size, c * d.size, c * d.size, s * d.size,
            (int16_t)d.intensity, 0, 0, 0, 15.0f, false, 1.0f, nullptr, false);
}

static void ApplyFire(Drop& d, uint32_t now) {
    uint32_t tb = now - d.tB;
    if (!d.createFire && tb > FIRE_TIME_LIMIT && g_extValid) {
        if (dist3(d.pos, g_ext) < g_radius / 4.0f) { g_extValid = false; d.tB = now; tb = 0; d.createFire = true; }
    }
    if (!d.createFire && tb > FIRE_TIME_LIMIT) {
        d.fires = fnNumFires(g_fireMgr, &d.pos, g_radius);
        if (d.fires > 0) { d.tB = now; tb = 0; d.createFire = true; }
    }
    if (d.createFire && tb > 100) {
        if (fnNumFires(g_fireMgr, &d.pos, 0.2f) == 0) {
            d.pos.z -= 0.6f;
            fnStartFire(g_fireMgr, d.pos, nullptr, 0.8f, 0, 1, 0);
            d.endIdle = true;
        }
        d.createFire = false; d.waitFire = true;
        if (d.fires > 0 && g_explode) {
            void* veh = d.vehicle >= 0 ? fnGetVehicle(d.vehicle) : nullptr;
            CVector vp;
            if (veh) { if (VehPos(veh, vp) && dist3(vp, d.pos) < g_radius) d.explodeCar = true; }
            else d.vehicle = -1;
        }
    }
    if (d.waitFire && tb > 300) { fnClearFlags(g_fireMgr); d.waitFire = false; }
    if (d.explodeCar && tb > 1000) {
        void* veh = d.vehicle >= 0 ? fnGetVehicle(d.vehicle) : nullptr;
        if (veh) {
            if (*(float*)(P(veh) + OFF_HEALTH) > 0.0f) {
                int sub = *(int*)(P(veh) + OFF_SUBCLASS);
                Blow_t fn = (sub == SUB_BIKE) ? fnBlowBike : fnBlowAuto;
                if (fn) fn(veh, nullptr, 0);
                d.explodeCar = false;
            }
        } else d.vehicle = -1;
    }
}

static float Rand(float a, float b) { return a + (b - a) * ((float)rand() / (float)RAND_MAX); }

static void SpawnDrops(uint32_t now) {
    uint8_t* pool = (uint8_t*)*pVehiclePool;
    if (!Ok(pool)) return;
    uint8_t* map = *(uint8_t**)(pool + 4);
    int size = *(int*)(pool + 8);
    if (!Ok(map) || size <= 0 || size > 4096) return;
    for (int i = 0; i < size; ++i) {
        if (map[i] & 0x80) continue;
        int handle = (i << 8) | map[i];
        void* veh = fnGetVehicle(handle);
        if (!veh) continue;
        float hp = *(float*)(P(veh) + OFF_HEALTH);
        if (!(hp > 0.0f && hp < 500.0f)) continue;
        int sub = *(int*)(P(veh) + OFF_SUBCLASS);
        if (sub != SUB_AUTOMOBILE && sub != SUB_MTRUCK && sub != SUB_QUAD && sub != SUB_BIKE) continue;
        uint8_t* h = *(uint8_t**)(P(veh) + OFF_HANDLING);
        if (!Ok(h) || h[OFF_ENGINETYPE] == 'E') continue;
        auto it = g_nextSpawn.find(handle);
        if (it != g_nextSpawn.end() && now < it->second) continue;
        int delay = (int)hp - 100; if (delay < 100) delay = 100;
        g_nextSpawn[handle] = now + delay;
        if (g_drops.size() >= MAX_DROPS) continue;
        Drop d{}; if (!EnginePos(veh, d.pos)) continue;
        d.angle = Rand(0.f, 360.f); d.size = Rand(g_minSize, g_maxSize);
        d.intensity = DROP_INTENSITY; d.vehicle = handle;
        d.tA = now; d.tB = now - FIRE_TIME_LIMIT; d.tSub = now; d.phase = 0;
        g_drops.push_back(d);
    }
    if (now - g_lastClean > 10000) {
        g_lastClean = now;
        for (auto it = g_nextSpawn.begin(); it != g_nextSpawn.end();)
            it = fnGetVehicle(it->first) ? ++it : g_nextSpawn.erase(it);
    }
}

static void UpdateDrops(uint32_t now) {
    for (size_t i = 0; i < g_drops.size();) {
        Drop& d = g_drops[i];
        bool remove = false;
        if (d.phase == 0) {
            ApplyFire(d, now); DrawDrop(d);
            if (d.endIdle || now - d.tA >= DROP_IDLE_TIME) { d.phase = 1; d.tSub = now; }
        } else {
            if (d.intensity > 50) ApplyFire(d, now);
            DrawDrop(d);
            if (now - d.tSub >= 100) { d.intensity -= 2; d.tSub = now; }
            if (d.intensity <= 0) remove = true;
        }
        if (remove) { g_drops[i] = g_drops.back(); g_drops.pop_back(); } else ++i;
    }
}

static void Tick() {
    if (!g_fireMgr || !pTimeMs) return;
    uint32_t now = *pTimeMs;
    SpawnDrops(now);
    UpdateDrops(now);
}

static void Update_hook(void* self) {
    Update_orig(self);
    g_fireMgr = self;
    Tick();
}

static void DoBulletImpact_hook(void* self, void* firer, void* victim, CVector* s, CVector* e, void* col, int i) {
    DoBulletImpact_orig(self, firer, victim, s, e, col, i);
    if (!g_fireMgr || !Ok(col) || g_drops.empty()) return;
    CVector hit = *(CVector*)col;   // CColPoint::m_vecPoint
    for (auto& d : g_drops) {
        if (dist3(d.pos, hit) < 1.0f && fnNumFires(g_fireMgr, &d.pos, 0.2f) == 0) {
            g_ext = d.pos; g_extValid = true; return;
        }
    }
}

// ---------------- INI ----------------
static std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static void LoadIni() {
    char pkg[256] = {0};
    if (FILE* f = fopen("/proc/self/cmdline", "r")) { fread(pkg, 1, sizeof(pkg) - 1, f); fclose(f); }
    std::string base = std::string("/storage/emulated/0/Android/data/") + pkg;
    const char* cand[] = { "/files/configs/OilLeaking.ini", "/configs/OilLeaking.ini",
                           "/files/cleo/Oil Leaking.ini", "/files/OilLeaking.ini" };
    for (auto c : cand) {
        FILE* f = fopen((base + c).c_str(), "r");
        if (!f) continue;
        LOGI("ini: %s", (base + c).c_str());
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string s = line;
            size_t cm = s.find_first_of("#;"); if (cm != std::string::npos) s = s.substr(0, cm);
            size_t eq = s.find('='); if (eq == std::string::npos) continue;
            std::string k = Trim(s.substr(0, eq)), v = Trim(s.substr(eq + 1));
            if (k == "MinSize") g_minSize = (float)atof(v.c_str());
            else if (k == "MaxSize") g_maxSize = (float)atof(v.c_str());
            else if (k == "Radius") g_radius = (float)atof(v.c_str());
            else if (k == "ExplodeCar") g_explode = atoi(v.c_str()) != 0;
        }
        fclose(f);
        break;
    }
    if (g_maxSize < g_minSize) g_maxSize = g_minSize;
}

// ---------------- Load ----------------
static void* Sym(void* h, const char* n) { void* p = aml->GetSym(h, n); if (!p) LOGI("simbolo ausente: %s", n); return p; }

ON_MOD_LOAD() {
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) { LOGI("libGTASA.so nao encontrada"); return; }
    LoadIni();
    fnGetVehicle   = (GetVehicle_t)Sym(h, "_ZN6CPools10GetVehicleEi");
    fnStore        = (Store_t)Sym(h, "_ZN8CShadows23StoreShadowToBeRenderedEhP9RwTextureP7CVectorffffshhhfbfP15CRealTimeShadowb");
    fnStartFire    = (StartFire_t)Sym(h, "_ZN12CFireManager15StartScriptFireERK7CVectorP7CEntityfhai");
    fnNumFires     = (NumFires_t)Sym(h, "_ZN12CFireManager18GetNumFiresInRangeEP7CVectorf");
    fnClearFlags   = (ClearFlags_t)Sym(h, "_ZN12CFireManager23ClearAllScriptFireFlagsEv");
    fnBlowAuto     = (Blow_t)Sym(h, "_ZN11CAutomobile9BlowUpCarEP7CEntityh");
    fnBlowBike     = (Blow_t)Sym(h, "_ZN5CBike9BlowUpCarEP7CEntityh");
    pVehiclePool   = (void**)Sym(h, "_ZN6CPools15ms_pVehiclePoolE");
    pTimeMs        = (uint32_t*)Sym(h, "_ZN6CTimer22m_snTimeInMillisecondsE");
    ppBloodTex     = (RwTexture**)Sym(h, "gpBloodPoolTex");
    pModelInfoPtrs = (void**)Sym(h, "_ZN10CModelInfo16ms_modelInfoPtrsE");
    void* upd = Sym(h, "_ZN12CFireManager6UpdateEv");
    void* bul = Sym(h, "_ZN7CWeapon14DoBulletImpactEP7CEntityS1_P7CVectorS3_P9CColPointi");
    if (!fnGetVehicle || !fnStore || !fnStartFire || !fnNumFires || !fnClearFlags || !fnBlowAuto ||
        !pVehiclePool || !pTimeMs || !ppBloodTex || !upd) { LOGI("falta simbolo essencial, abortando"); return; }
    aml->Hook(upd, (void*)Update_hook, (void**)&Update_orig);
    if (bul) aml->Hook(bul, (void*)DoBulletImpact_hook, (void**)&DoBulletImpact_orig);
    LOGI("Oil Leaking carregado (min=%.2f max=%.2f radius=%.1f explode=%d)", g_minSize, g_maxSize, g_radius, (int)g_explode);
}
