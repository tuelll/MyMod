// ============================================================================
//  Realistic Vehicle Damage  -  GTA SA Android (libGTASA.so, ARM32)
//
//  Ideia: tiros em partes "fracas" do veiculo (motor, tanque, rotor, asa...)
//  causam MUITO mais dano que tiros em partes "fortes" (porta, vidro, carga).
//
//  Como funciona:
//   - Toda bala que atinge um veiculo passa por
//       CVehicle::InflictDamage(CEntity* damager, eWeaponType, float, CVector)
//     (chamada por DoBulletImpact, CBulletInfo::Update e FireOneInstantHitRound).
//   - Fazemos hook na entrada GOT/PLT dessa funcao (sem precisar de lib de
//     inline-hook e sem offsets fixos: o simbolo e achado pelo nome).
//   - Convertemos o ponto do impacto para coordenadas locais do veiculo,
//     normalizamos pelo bounding box do modelo e escolhemos um multiplicador.
//
//  Offsets usados (conferidos no seu libGTASA.so):
//     CPlaceable::m_matrix   = +0x14   (CMatrix*: right 0x00, fwd 0x10, up 0x20, pos 0x30)
//     CEntity::m_nModelIndex = +0x26   (int16)
//     CColModel bbox         = min +0x00 / max +0x0C
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <android/log.h>

#define TAG "RealisticDamage"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define DEBUG_HITS 0   // 1 = loga cada tiro (kind, zona, multiplicador) no logcat

// ----------------------------------------------------------------------------
//  Configuracao (mexa aqui)
// ----------------------------------------------------------------------------
namespace cfg {
    // multiplicador final e limitado a este intervalo
    constexpr float MIN_MULT = 0.05f;
    constexpr float MAX_MULT = 4.0f;

    // Multiplicador por tipo de arma de projetil (>0 = aplica; <0 = ignora)
    // IDs: 22 Colt, 23 Silenciada, 24 Deagle, 25 Shotgun, 26 Sawn-off, 27 Spas,
    //      28 Uzi, 29 MP5, 30 AK, 31 M4, 32 Tec9, 33 Rifle, 34 Sniper,
    //      38 Minigun / metralhadoras de veiculos, 52 Uzi driveby
    inline float WeaponMul(int w) {
        switch (w) {
            case 22: case 23:                 return 0.60f;  // pistolas leves
            case 24:                          return 0.80f;  // deagle
            case 25: case 26: case 27:        return 0.80f;  // escopetas
            case 28: case 29: case 32: case 52: return 0.60f;// SMGs
            case 30: case 31:                 return 1.00f;  // fuzis de assalto
            case 33:                          return 1.30f;  // rifle
            case 34:                          return 2.00f;  // sniper
            case 38:                          return 1.20f;  // minigun / armas de veiculo
            default:                          return -1.0f;  // explosao, fogo, melee...: nao mexe
        }
    }
}

// ----------------------------------------------------------------------------
//  Tipos / constantes
// ----------------------------------------------------------------------------
namespace {

struct CVector { float x, y, z; };

enum Kind { K_CAR, K_TRUCK, K_BIKE, K_HELI, K_PLANE, K_BOAT, K_TRAILER, K_OTHER, K_COUNT };

constexpr size_t OFF_MATRIX = 0x14;
constexpr size_t OFF_MODEL  = 0x26;

typedef int   (*InflictDamage_t)(void* veh, void* damager, int weapon, float dmg, CVector pos);
typedef void* (*GetColModel_t)(void* entity);

InflictDamage_t g_orig   = nullptr;
GetColModel_t   g_getCol = nullptr;

// vtables -> classe do veiculo (vptr = simbolo _ZTV + 8 no ABI ARM)
struct VtEntry { const char* sym; Kind kind; uintptr_t vptr; };
VtEntry g_vt[] = {
    { "_ZTV11CAutomobile",  K_CAR,     0 },
    { "_ZTV13CMonsterTruck",K_TRUCK,   0 },
    { "_ZTV9CQuadBike",     K_BIKE,    0 },
    { "_ZTV5CBike",         K_BIKE,    0 },
    { "_ZTV4CBmx",          K_BIKE,    0 },
    { "_ZTV5CHeli",         K_HELI,    0 },
    { "_ZTV6CPlane",        K_PLANE,   0 },
    { "_ZTV5CBoat",         K_BOAT,    0 },
    { "_ZTV8CTrailer",      K_TRAILER, 0 },
    { "_ZTV6CTrain",        K_OTHER,   0 },
};

// Modelos que sao tratados como caminhao/onibus/van pesada (motor na frente, carga atras)
const int16_t kTruckModels[] = {
    403,406,407,408,413,414,431,437,443,455,456,498,499,514,515,524,544,573,578,609, 427,428,433
};
// Blindados: resistem mais
const int16_t kArmored[] = { 427, 428, 432, 433, 601 };
// Motor central/traseiro
const int16_t kRearEngine[] = { 411, 415, 429, 451, 541 };

template <size_t N> bool In(const int16_t (&a)[N], int v) {
    for (size_t i = 0; i < N; i++) if (a[i] == v) return true;
    return false;
}

Kind Classify(void* veh, int model) {
    uintptr_t vptr = *(uintptr_t*)veh;
    for (auto& e : g_vt) if (e.vptr && e.vptr == vptr) {
        if (e.kind == K_CAR && In(kTruckModels, model)) return K_TRUCK;
        return e.kind;
    }
    return K_OTHER;
}

// ----------------------------------------------------------------------------
//  Zonas de dano
//   nx: -1..1 (esq..dir)   ny: -1..1 (traseira..frente)   nz: 0..1 (baixo..cima)
// ----------------------------------------------------------------------------
float ZoneMult(Kind k, int model, float nx, float ny, float nz, const char** zone) {
    const char* z = "?";
    float m = 1.0f;
    switch (k) {
    case K_CAR: {
        const bool rear = In(kRearEngine, model);
        const float ey = rear ? -ny : ny;          // ey > 0 => lado do motor
        if (ey > 0.30f && nz < 0.80f) {            // cofre / compartimento do motor
            if (nz < 0.15f) { m = 1.2f; z = "radiador/carter"; }
            else            { m = 1.8f; z = "MOTOR"; }
        } else if (nz > 0.72f)         { m = 0.25f; z = "teto/vidros"; }
        else if (ey < -0.30f) {                    // lado oposto ao motor
            if (nz < 0.30f && !rear)   { m = 1.3f; z = "tanque"; }
            else                       { m = 0.60f; z = "porta-malas"; }
        } else                         { m = 0.35f; z = "portas/cabine"; }
        break; }
    case K_TRUCK:
        if (ny > 0.55f) {
            if (nz < 0.55f)            { m = 1.6f; z = "MOTOR"; }
            else                       { m = 0.50f; z = "cabine/vidro"; }
        } else if (ny > 0.25f)         { m = 0.50f; z = "cabine"; }
        else                           { m = 0.30f; z = "carga"; }
        break;
    case K_TRAILER:                    m = 0.30f; z = "carroceria"; break;
    case K_BIKE:
        if (fabsf(ny) > 0.55f)         { m = 0.80f; z = "roda/garfo"; }
        else if (nz < 0.65f)           { m = 1.50f; z = "MOTOR/tanque"; }
        else                           { m = 1.00f; z = "quadro/guidao"; }
        break;
    case K_HELI:
        if (ny < -0.35f)               { m = 1.80f; z = "CAUDA/rotor traseiro"; }
        else if (nz > 0.70f)           { m = 2.00f; z = "MOTOR/rotor"; }
        else if (ny > 0.55f)           { m = 0.90f; z = "cockpit"; }
        else                           { m = 0.60f; z = "fuselagem"; }
        break;
    case K_PLANE:
        if (fabsf(nx) > 0.45f)         { m = 1.40f; z = "ASA/tanque/motor"; }
        else if (ny > 0.60f)           { m = 1.80f; z = "NARIZ/motor"; }
        else if (ny < -0.55f)          { m = 0.90f; z = "cauda"; }
        else                           { m = 0.60f; z = "fuselagem"; }
        break;
    case K_BOAT:
        if (ny < -0.55f)               { m = 1.80f; z = "MOTOR popa"; }
        else if (nz < 0.35f)           { m = 1.00f; z = "casco"; }
        else                           { m = 0.70f; z = "convés/cabine"; }
        break;
    default: break;
    }
    if (zone) *zone = z;
    return m;
}

// Bounding box padrao caso o CColModel pareca invalido
void DefaultBounds(Kind k, float mn[3], float mx[3]) {
    float hx = 1.0f, hy = 2.4f, h = 1.5f;
    switch (k) {
        case K_TRUCK: case K_TRAILER: hx = 1.3f; hy = 4.0f; h = 3.0f; break;
        case K_BIKE:  hx = 0.4f; hy = 1.0f; h = 1.4f; break;
        case K_HELI:  hx = 1.5f; hy = 6.0f; h = 3.0f; break;
        case K_PLANE: hx = 5.0f; hy = 7.0f; h = 3.0f; break;
        case K_BOAT:  hx = 1.5f; hy = 4.0f; h = 2.0f; break;
        default: break;
    }
    mn[0] = -hx; mn[1] = -hy; mn[2] = -0.3f;
    mx[0] =  hx; mx[1] =  hy; mx[2] = h;
}

void GetBounds(void* veh, Kind k, float mn[3], float mx[3]) {
    bool ok = false;
    if (g_getCol) {
        const float* c = (const float*)g_getCol(veh);   // CColModel: min @0x00, max @0x0C
        if (c) {
            ok = true;
            for (int i = 0; i < 3; i++) {
                mn[i] = c[i]; mx[i] = c[3 + i];
                float s = mx[i] - mn[i];
                if (!(isfinite(mn[i]) && isfinite(mx[i])) || s < 0.2f || s > 80.0f) ok = false;
            }
        }
    }
    if (!ok) DefaultBounds(k, mn, mx);
}

float Clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// Multiplicador de dano para um tiro em `p` (coordenadas de mundo)
float HitMultiplier(void* veh, const CVector& p) {
    const int model = *(int16_t*)((uint8_t*)veh + OFF_MODEL);
    const Kind k = Classify(veh, model);
    if (k == K_OTHER) return 1.0f;

    const float* m = *(const float**)((uint8_t*)veh + OFF_MATRIX);
    if (!m) return 1.0f;

    const float dx = p.x - m[12], dy = p.y - m[13], dz = p.z - m[14];
    const float d2 = dx*dx + dy*dy + dz*dz;
    if (!(d2 < 2500.0f)) return 1.0f;       // longe demais (ou NaN): ponto invalido

    // mundo -> local  (right = m[0..2], forward = m[4..6], up = m[8..10])
    const float lx = dx*m[0] + dy*m[1] + dz*m[2];
    const float ly = dx*m[4] + dy*m[5] + dz*m[6];
    const float lz = dx*m[8] + dy*m[9] + dz*m[10];

    float mn[3], mx[3];
    GetBounds(veh, k, mn, mx);

    const float nx = Clamp(((lx - (mn[0]+mx[0])*0.5f) / ((mx[0]-mn[0])*0.5f)), -1.2f, 1.2f);
    const float ny = Clamp(((ly - (mn[1]+mx[1])*0.5f) / ((mx[1]-mn[1])*0.5f)), -1.2f, 1.2f);
    const float nz = Clamp(((lz -  mn[2])             /  (mx[2]-mn[2])),       -0.2f, 1.2f);

    const char* zone = "";
    float mult = ZoneMult(k, model, nx, ny, nz, &zone);
    if (In(kArmored, model)) mult *= 0.5f;

#if DEBUG_HITS
    LOGI("hit kind=%d model=%d zone=%s n=(%.2f %.2f %.2f) zoneMult=%.2f", k, model, zone, nx, ny, nz, mult);
#endif
    return mult;
}

// ----------------------------------------------------------------------------
//  Hook
// ----------------------------------------------------------------------------
// Retorna int (e nao void) so para preservar r0 caso a funcao original devolva algo.
int Hook_InflictDamage(void* veh, void* damager, int weapon, float dmg, CVector pos) {
    const float wm = cfg::WeaponMul(weapon);
    if (veh && wm > 0.0f && dmg > 0.0f) {
        float mult = Clamp(wm * HitMultiplier(veh, pos), cfg::MIN_MULT, cfg::MAX_MULT);
        dmg *= mult;
    }
    return g_orig(veh, damager, weapon, dmg, pos);
}

// ----------------------------------------------------------------------------
//  Patch de import (GOT) pelo nome do simbolo
// ----------------------------------------------------------------------------
#ifdef __LP64__
  #define R_TYPE(i) ((unsigned)((i) & 0xffffffffu))
  #define R_SYM(i)  ((unsigned)((i) >> 32))
#else
  #define R_TYPE(i) ((unsigned)((i) & 0xffu))
  #define R_SYM(i)  ((unsigned)((i) >> 8))
#endif
constexpr unsigned R_GLOB_DAT  = 21;   // R_ARM_GLOB_DAT
constexpr unsigned R_JUMP_SLOT = 22;   // R_ARM_JUMP_SLOT

struct LibInfo { uintptr_t base = 0; const ElfW(Phdr)* ph = nullptr; int phn = 0; };

int PhdrCb(struct dl_phdr_info* i, size_t, void* d) {
    if (i->dlpi_name && strstr(i->dlpi_name, "libGTASA.so")) {
        auto* li = (LibInfo*)d;
        li->base = i->dlpi_addr; li->ph = i->dlpi_phdr; li->phn = i->dlpi_phnum;
        return 1;
    }
    return 0;
}

bool PatchImport(const LibInfo& li, const char* name, void* repl, void** orig) {
    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < li.phn; i++)
        if (li.ph[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(li.base + li.ph[i].p_vaddr);
    if (!dyn) return false;

    uintptr_t jmprel = 0, rel = 0, symtab = 0, strtab = 0; size_t pltsz = 0, relsz = 0;
    auto fix = [&](uintptr_t v) { return v < li.base ? li.base + v : v; };  // bionic pode nao relocar d_ptr
    for (; dyn->d_tag != DT_NULL; ++dyn) switch (dyn->d_tag) {
        case DT_JMPREL:   jmprel = fix(dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltsz  = dyn->d_un.d_val;      break;
        case DT_REL:      rel    = fix(dyn->d_un.d_ptr); break;
        case DT_RELSZ:    relsz  = dyn->d_un.d_val;      break;
        case DT_SYMTAB:   symtab = fix(dyn->d_un.d_ptr); break;
        case DT_STRTAB:   strtab = fix(dyn->d_un.d_ptr); break;
    }
    if (!symtab || !strtab) return false;

    const long pg = sysconf(_SC_PAGESIZE);
    bool patched = false;

    auto scan = [&](uintptr_t tbl, size_t sz) {
        if (!tbl) return;
        auto* r = (const ElfW(Rel)*)tbl;
        const size_t n = sz / sizeof(ElfW(Rel));
        for (size_t i = 0; i < n; i++) {
            const unsigned t = R_TYPE(r[i].r_info), s = R_SYM(r[i].r_info);
            if ((t != R_GLOB_DAT && t != R_JUMP_SLOT) || !s) continue;
            const char* nm = (const char*)strtab + ((const ElfW(Sym)*)symtab)[s].st_name;
            if (strcmp(nm, name) != 0) continue;

            auto* slot = (void**)(li.base + r[i].r_offset);
            void* page = (void*)((uintptr_t)slot & ~(uintptr_t)(pg - 1));
            if (mprotect(page, pg, PROT_READ | PROT_WRITE) != 0) continue;
            if (!*orig) *orig = *slot;      // guarda original so na primeira vez
            *slot = repl;
            mprotect(page, pg, PROT_READ);  // volta a RELRO
            patched = true;
        }
    };
    scan(jmprel, pltsz);
    scan(rel, relsz);
    return patched;
}

// ----------------------------------------------------------------------------
//  Instalacao
// ----------------------------------------------------------------------------
bool Install() {
    LibInfo li;
    dl_iterate_phdr(PhdrCb, &li);
    if (!li.base) return false;                       // libGTASA ainda nao carregada

    void* h = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { LOGI("dlopen(libGTASA.so) falhou"); return false; }

    g_getCol = (GetColModel_t)dlsym(h, "_ZN7CEntity11GetColModelEv");
    for (auto& e : g_vt) {
        void* v = dlsym(h, e.sym);
        e.vptr = v ? (uintptr_t)v + 8 : 0;            // vptr aponta para vtable+8
    }
    if (!g_getCol) LOGI("GetColModel nao encontrado: usando bounding box padrao");

    void* orig = nullptr;
    if (!PatchImport(li, "_ZN8CVehicle13InflictDamageEP7CEntity11eWeaponTypef7CVector",
                     (void*)&Hook_InflictDamage, &orig)) {
        LOGI("falha ao hookar CVehicle::InflictDamage");
        return false;
    }
    g_orig = (InflictDamage_t)orig;
    LOGI("OK: dano realista de veiculos ativo (orig=%p)", orig);
    return true;
}

} // namespace

// ----------------------------------------------------------------------------
//  Entrada
// ----------------------------------------------------------------------------
#ifdef USE_AML
// Com o Alternative Mod Loader (AML): compile com -DUSE_AML e inclua o SDK do AML.
#include <mod/amlmod.h>
MYMOD(com.example.realisticdamage, RealisticVehicleDamage, 1.0, you)
extern "C" void OnModLoad() { Install(); }
#else
// Sem AML: ao ser carregada (ex.: via loadLibrary), espera a libGTASA e instala.
static void* WaitAndInstall(void*) {
    for (int i = 0; i < 600; i++) {                   // ate ~60 s
        if (Install()) return nullptr;
        usleep(100 * 1000);
    }
    return nullptr;
}
__attribute__((constructor)) static void OnLoad() {
    pthread_t t; pthread_create(&t, nullptr, WaitAndInstall, nullptr); pthread_detach(t);
}
#endif
