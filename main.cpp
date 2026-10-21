// DetachableScope - port AML (GTA SA Android, armv7 / 32 bits)
// Mod original (PC .asi): vecondite (MIT). Port para AML: sem plugin-sdk, só funções
// exportadas pela libGTASA.so, resolvidas por nome (aml->GetSym).
//
// Botão: X (Cross = SprintJustDown) com um sniper/country rifle na mão.
// IFP: carregado pelo AnimLoader (files/v4rg05/animloader/scope.ifp), bloco "scope".

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

MYMODCFG(net.vecondite.detachablescope, DetachableScope, 1.0, vecondite)

// ---------------------------------------------------------------------------
// Tipos mínimos (layout idêntico ao RwMatrix / CMatrix: 0x40 bytes)
// ---------------------------------------------------------------------------
struct V3 { float x, y, z; };
struct M4 {
    V3 right; uint32_t flags;
    V3 up;    uint32_t pad1;
    V3 at;    uint32_t pad2;
    V3 pos;   uint32_t pad3;
};

enum { WEAPON_COUNTRYRIFLE = 33, WEAPON_SNIPERRIFLE = 34 };
enum { MODEL_COUNTRYRIFLE = 357, MODEL_SNIPERRIFLE = 358 };

// ---------------------------------------------------------------------------
// Offsets de campos (únicos pontos que dependem do layout da libGTASA).
// São validados em runtime e podem ser sobrescritos no .ini.
// ---------------------------------------------------------------------------
static int OFF_ENT_MATRIX   = 0x14;   // CPlaceable::m_matrix
static int OFF_ENT_RWOBJECT = 0x18;   // CEntity::m_pRwObject (clump no ped)
static int OFF_PED_WEAPONS  = 0x5A0;  // CPed::m_aWeapons[13]
static int OFF_PED_ACTSLOT  = 0x718;  // CPed::m_nActiveWeaponSlot (uint8)
static int OFF_PED_HEALTH   = 0x540;  // CPed::m_fHealth
static const int WEAPON_SIZE = 0x1C;  // sizeof(CWeapon)
static const int W_TYPE = 0x0, W_CLIP = 0x8, W_AMMO = 0xC;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static ConfigEntry* cfgScope = nullptr;
static int   boneId = 35;
static float xOffset = 0.03f, yOffset = 0.0f, zOffset = -0.01f;
static char  ifpName[64] = "scope";
static char  animName[64] = "scope";
static int   animDuration = 1167;
static int   animSpot = 600;
static int   scopeModel = 327;
static int   animFlags = 0x10;        // ANIM_PARTIAL (só parte de cima do corpo)
static bool  autoScan = true;
static bool  refreshWeapon = true;

// ---------------------------------------------------------------------------
// Funções da libGTASA
// ---------------------------------------------------------------------------
static void* hGTASA = nullptr;

static void*  (*FindPlayerPed)(int);
static void*  (*CPad_GetPad)(int);
static bool   (*CPad_SprintJustDown)(void*);
static int    (*CPed_GetWeaponSlot)(void*, int);
static int    (*CPed_GiveWeapon)(void*, int, unsigned, bool);
static void   (*CPed_SetCurrentWeapon)(void*, int);
static void   (*CStreaming_RequestModel)(int, int);
static void   (*CStreaming_LoadAllRequestedModels)(bool);
static void*  (*CObject_new)(unsigned, int);
static void   (*CObject_ctor)(void*, int, bool);
static void   (*CWorld_Add)(void*);
static void   (*CWorld_Remove)(void*);
static void   (*CEntity_UpdateRwFrame)(void*);
static void   (*CPlaceable_AllocateMatrix)(void*);
static void   (*CMatrix_UpdateRwMatrix)(void*, void*);
static void*  (*GetAnimHierarchyFromSkinClump)(void*);
static int    (*RpHAnimIDGetIndex)(void*, int);
static void*  (*RpHAnimHierarchyGetMatrixArray)(void*);
static int    (*CAnimManager_GetAnimationBlockIndex)(const char*);
static void*  (*CAnimManager_GetAnimationBlock)(const char*);
static void*  (*CAnimManager_GetAnimation)(const char*, const void*);
static void   (*CAnimManager_AddAnimBlockRef)(int);
static void*  (*CAnimManager_BlendAnimation)(void*, void*, int, float);
static void   (*CAnimAssoc_Start)(void*, float);
static uint32_t* pTimeMs = nullptr;

// dlsym direto (o patch_aml.h do projeto redefine a macro GetSym de forma quebrada)
#ifdef GetSym
#undef GetSym
#endif
static inline void* Sym(const char* name) { return dlsym(hGTASA, name); }

static bool Resolve(void** out, const char* name)
{
    *out = Sym(name);
    if (!*out) logger->Error("simbolo ausente: %s", name);
    return *out != nullptr;
}
#define RESOLVE(var, name) ok &= Resolve((void**)&(var), name)

// ---------------------------------------------------------------------------
// Estado do mod (igual ao original)
// ---------------------------------------------------------------------------
static bool     scope = false;
static bool     wasAlive = true;
static uint32_t lastWeapAmmo = 0, lastWeapClip = 0;
static int      lastWeapType = 0;
static bool     hasSavedAmmo = false;
static void*    scopeObject = nullptr;
static bool     scopeState = false;
static uint32_t animTime = 0, animEnd = 0;
static bool     wasModSwap = false;

static bool layoutChecked = false, layoutOK = false;
static bool animReady = false, animWarned = false, animRefd = false;
static int  animRetry = 0;

static inline bool isSniper(int t) { return t == WEAPON_COUNTRYRIFLE || t == WEAPON_SNIPERRIFLE; }

// ---------------------------------------------------------------------------
// Acesso a CPed / CWeapon
// ---------------------------------------------------------------------------
static inline uint8_t* Weapon(uint8_t* ped, int slot) { return ped + OFF_PED_WEAPONS + slot * WEAPON_SIZE; }
static inline int       WType(uint8_t* w) { return *(int*)(w + W_TYPE); }
static inline uint32_t& WAmmo(uint8_t* w) { return *(uint32_t*)(w + W_AMMO); }
static inline uint32_t& WClip(uint8_t* w) { return *(uint32_t*)(w + W_CLIP); }

static bool ValidLayout(uint8_t* ped, int wOff, int sOff)
{
    if (ped[sOff] > 12) return false;
    for (int i = 0; i < 13; i++) {
        int t = *(int*)(ped + wOff + i * WEAPON_SIZE + W_TYPE);
        if (t < 0 || t > 46) return false;
        if (t != 0 && CPed_GetWeaponSlot(ped, t) != i) return false;
    }
    return true;
}

static bool CheckLayout(uint8_t* ped)
{
    if (layoutChecked) return layoutOK;
    if (ValidLayout(ped, OFF_PED_WEAPONS, OFF_PED_ACTSLOT)) {
        layoutChecked = layoutOK = true;
        logger->Info("layout do CPed ok (weapons=0x%X, slot=0x%X)", OFF_PED_WEAPONS, OFF_PED_ACTSLOT);
        return true;
    }
    if (autoScan) {
        const int delta = 0x718 - 0x5A0;
        for (int off = 0x400; off <= 0x800; off += 4) {
            if (!ValidLayout(ped, off, off + delta)) continue;
            bool any = false;
            for (int i = 1; i < 13; i++) if (*(int*)(ped + off + i * WEAPON_SIZE) != 0) any = true;
            if (!any) continue;
            OFF_PED_WEAPONS = off; OFF_PED_ACTSLOT = off + delta;
            layoutChecked = layoutOK = true;
            logger->Info("layout do CPed encontrado por scan (weapons=0x%X, slot=0x%X)", off, off + delta);
            return true;
        }
    }
    layoutChecked = true; layoutOK = false;
    logger->Error("layout do CPed invalido; ajuste weaponsoffset/activeslotoffset no .ini. Mod inativo.");
    return false;
}

static bool IsAlive(uint8_t* ped)
{
    float h = *(float*)(ped + OFF_PED_HEALTH);
    if (isfinite(h) && h >= 0.0f && h <= 2000.0f) return h > 0.0f;
    return true;
}

// ---------------------------------------------------------------------------
// Animação (IFP via AnimLoader)
// ---------------------------------------------------------------------------
static void* FindHier()
{
    int idx = CAnimManager_GetAnimationBlockIndex(ifpName);
    if (idx < 0) return nullptr;
    void* blk = CAnimManager_GetAnimationBlock(ifpName);
    if (!blk) return nullptr;
    void* h = CAnimManager_GetAnimation(animName, blk);
    if (h && !animRefd) { CAnimManager_AddAnimBlockRef(idx); animRefd = true; }
    return h;
}

static bool AnimReady()
{
    if (animReady) return true;
    if ((animRetry++ % 60) != 0) return false;
    if (FindHier()) {
        animReady = true;
        logger->Info("animacao '%s' no bloco '%s' pronta", animName, ifpName);
        return true;
    }
    if (!animWarned) {
        animWarned = true;
        logger->Error("bloco '%s' / anim '%s' nao encontrado. Coloque o .ifp em files/v4rg05/animloader/ (AnimLoader).", ifpName, animName);
    }
    return false;
}

static void PlayScopeAnim(uint8_t* ped)
{
    void* clump = *(void**)(ped + OFF_ENT_RWOBJECT);
    void* hier = FindHier();
    if (!clump || !hier) return;
    void* assoc = CAnimManager_BlendAnimation(clump, hier, animFlags, 4.0f);
    if (assoc) CAnimAssoc_Start(assoc, 0.0f);
}

// ---------------------------------------------------------------------------
// Objeto da mira
// ---------------------------------------------------------------------------
static void DestroyScopeObject()
{
    if (!scopeObject) return;
    CWorld_Remove(scopeObject);
    // delete scopeObject; -> destrutor virtual deleting (slot 1 da vtable)
    void** vtbl = *(void***)scopeObject;
    ((void (*)(void*))vtbl[1])(scopeObject);
    scopeObject = nullptr;
}

static void CreateScopeObject()
{
    CStreaming_RequestModel(scopeModel, 0);
    CStreaming_LoadAllRequestedModels(false);
    void* mem = CObject_new(0x1A0, -1);
    if (!mem) { logger->Error("pool de objetos cheia"); return; }
    CObject_ctor(mem, scopeModel, true);
    scopeObject = mem;
    CWorld_Add(scopeObject);
}

static void AttachScopeToHand(uint8_t* ped)
{
    uint8_t* obj = (uint8_t*)scopeObject;
    CWorld_Remove(obj);

    void* clump = *(void**)(ped + OFF_ENT_RWOBJECT);
    void* hier = clump ? GetAnimHierarchyFromSkinClump(clump) : nullptr;
    if (hier) {
        int idx = RpHAnimIDGetIndex(hier, boneId);
        M4* arr = (M4*)RpHAnimHierarchyGetMatrixArray(hier);
        M4* m = *(M4**)(obj + OFF_ENT_MATRIX);
        if (!m) { CPlaceable_AllocateMatrix(obj); m = *(M4**)(obj + OFF_ENT_MATRIX); }
        if (idx >= 0 && arr && m) {
            M4* b = &arr[idx];
            m->at.x = -b->right.x; m->at.y = -b->right.y; m->at.z = -b->right.z;
            m->pos.x = b->pos.x + b->right.x * xOffset + b->up.x * yOffset + b->at.x * zOffset;
            m->pos.y = b->pos.y + b->right.y * xOffset + b->up.y * yOffset + b->at.y * zOffset;
            m->pos.z = b->pos.z + b->right.z * xOffset + b->up.z * yOffset + b->at.z * zOffset;
            m->up = b->up;
            m->right = b->at;
            // UpdateRwMatrix + UpdateRwFrame (CEntity)
            void* rwObj = *(void**)(obj + OFF_ENT_RWOBJECT);
            void* frame = rwObj ? *(void**)((uint8_t*)rwObj + 4) : nullptr;   // RwObject::parent
            if (frame) CMatrix_UpdateRwMatrix(m, (uint8_t*)frame + 0x10);     // RwFrame::modelling
            CEntity_UpdateRwFrame(obj);
        }
    }
    CWorld_Add(obj);
}

static void SetScope(bool v)
{
    if (scope == v) return;
    scope = v;
    if (cfgScope) { cfgScope->SetInt(v ? 1 : 0); cfg->Save(); }
}

static void ResetState()
{
    hasSavedAmmo = false;
    lastWeapType = 0; lastWeapAmmo = 0; lastWeapClip = 0;
    SetScope(false);
    animTime = 0; animEnd = 0;
    scopeState = false;
    wasModSwap = false;
}

// ---------------------------------------------------------------------------
// Lógica principal (porte de OnGameProcess)
// ---------------------------------------------------------------------------
static void Update()
{
    uint8_t* ped = (uint8_t*)FindPlayerPed(-1);
    if (!ped) return;
    if (!CheckLayout(ped)) return;

    bool alive = IsAlive(ped);
    if (!alive && wasAlive) {
        ResetState();
        DestroyScopeObject();
    }
    wasAlive = alive;

    if (!AnimReady()) return;

    int activeSlot = ped[OFF_PED_ACTSLOT];
    if (activeSlot > 12) return;
    uint8_t* curW = Weapon(ped, activeSlot);
    int      curType = WType(curW);
    uint32_t curAmmo0 = WAmmo(curW);
    uint32_t curClip0 = WClip(curW);

    if (alive) {
        if (isSniper(curType)) {
            if (lastWeapType != curType) {
                if (curType == WEAPON_SNIPERRIFLE) SetScope(true);
                if (wasModSwap) {
                    wasModSwap = false;
                } else {
                    if (isSniper(lastWeapType) && WAmmo(curW) == lastWeapAmmo && hasSavedAmmo) {
                        WAmmo(curW) = lastWeapAmmo + WAmmo(curW);
                        WClip(curW) = lastWeapClip;
                    }
                }
                if (WAmmo(curW) < lastWeapAmmo && hasSavedAmmo) {
                    WAmmo(curW) = lastWeapAmmo + WAmmo(curW);
                    WClip(curW) = lastWeapClip;
                }
                lastWeapAmmo = WAmmo(curW);
                lastWeapClip = WClip(curW);
                hasSavedAmmo = true;
            } else {
                lastWeapAmmo = WAmmo(curW);
                lastWeapClip = WClip(curW);
                hasSavedAmmo = true;
            }
        }
        lastWeapType = curType;
    }

    uint32_t now = *pTimeMs;
    bool giveWeapon = animTime != 0 && animTime < now;

    void* pad = CPad_GetPad(0);
    bool pressed = pad ? CPad_SprintJustDown(pad) : false;   // botão X

    if (alive && ((pressed && animTime == 0) || giveWeapon) && isSniper(curType) && scope) {
        if (!scopeObject) CreateScopeObject();

        if (giveWeapon) {
            int nextWeap  = (curType == WEAPON_COUNTRYRIFLE) ? WEAPON_SNIPERRIFLE : WEAPON_COUNTRYRIFLE;
            int nextModel = (nextWeap == WEAPON_COUNTRYRIFLE) ? MODEL_COUNTRYRIFLE : MODEL_SNIPERRIFLE;
            CStreaming_RequestModel(nextModel, 0);
            CStreaming_LoadAllRequestedModels(false);

            lastWeapAmmo = curAmmo0;
            lastWeapClip = curClip0;

            CPed_GiveWeapon(ped, nextWeap, lastWeapAmmo, false);
            int ns = CPed_GetWeaponSlot(ped, nextWeap);
            if (ns >= 0 && ns <= 12) {
                uint8_t* nw = Weapon(ped, ns);
                WAmmo(nw) = lastWeapAmmo;
                WClip(nw) = lastWeapClip;
                if (refreshWeapon) CPed_SetCurrentWeapon(ped, ns);
                wasModSwap = true;
                animTime = 0;
                scopeState = (WType(nw) == WEAPON_SNIPERRIFLE) ? false : true;
            } else {
                animTime = 0;
                scopeState = false;
            }
        } else {
            PlayScopeAnim(ped);
            animTime = now + (uint32_t)animSpot;
            animEnd = animTime + (uint32_t)animDuration;
            scopeState = (curType == WEAPON_SNIPERRIFLE) ? false : true;
        }
    }

    if (animEnd != 0 && animEnd < now) scopeState = false;

    if (scopeObject) {
        if (scopeState) AttachScopeToHand(ped);
        else DestroyScopeObject();
    }

    int rifleSlot = CPed_GetWeaponSlot(ped, WEAPON_COUNTRYRIFLE);
    if (rifleSlot >= 0 && rifleSlot <= 12 && !isSniper(WType(Weapon(ped, rifleSlot))) && hasSavedAmmo) {
        lastWeapAmmo = 0; lastWeapClip = 0; hasSavedAmmo = false;
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static void (*orig_CGame_Process)();
static void hook_CGame_Process()
{
    orig_CGame_Process();
    Update();
}

static void (*orig_Restart)();
static void hook_Restart()
{
    orig_Restart();
    scopeObject = nullptr;   // o mundo já foi limpo; não deletar
    ResetState();
    wasAlive = true;
    layoutChecked = false;
}

static void (*orig_Resurrect)(void*, V3, float);
static void hook_Resurrect(void* ped, V3 pos, float heading)
{
    orig_Resurrect(ped, pos, heading);
    DestroyScopeObject();
    ResetState();
    wasAlive = true;
}

// ---------------------------------------------------------------------------
// Entrada do mod
// ---------------------------------------------------------------------------
static void LoadConfig()
{
    boneId       = cfg->Bind("boneid", 35, "settings")->GetInt();
    xOffset      = cfg->Bind("xoffset", 0.03f, "settings")->GetFloat();
    yOffset      = cfg->Bind("yoffset", 0.0f, "settings")->GetFloat();
    zOffset      = cfg->Bind("zoffset", -0.01f, "settings")->GetFloat();
    strncpy(ifpName,  cfg->Bind("ifp",  "scope", "settings")->GetString(), sizeof(ifpName) - 1);
    strncpy(animName, cfg->Bind("anim", "scope", "settings")->GetString(), sizeof(animName) - 1);
    animDuration = cfg->Bind("animduration", 1167, "settings")->GetInt();
    animSpot     = cfg->Bind("animspot", 600, "settings")->GetInt();
    scopeModel   = cfg->Bind("scopemodel", 327, "settings")->GetInt();
    animFlags    = cfg->Bind("animflags", 0x10, "advanced")->GetInt();
    autoScan     = cfg->Bind("autoscan", 1, "advanced")->GetInt() != 0;
    refreshWeapon= cfg->Bind("refreshweapon", 1, "advanced")->GetInt() != 0;
    OFF_PED_WEAPONS = cfg->Bind("weaponsoffset", 0x5A0, "advanced")->GetInt();
    OFF_PED_ACTSLOT = cfg->Bind("activeslotoffset", 0x718, "advanced")->GetInt();
    OFF_PED_HEALTH  = cfg->Bind("healthoffset", 0x540, "advanced")->GetInt();
    cfgScope     = cfg->Bind("scope", 0, "saves");
    scope        = cfgScope->GetInt() != 0;
    ifpName[sizeof(ifpName) - 1] = 0;
    animName[sizeof(animName) - 1] = 0;
    cfg->Save();
}

extern "C" void OnModLoad()
{
    logger->SetTag("DetachableScope");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    if (!hGTASA) { logger->Error("libGTASA.so nao encontrada"); return; }

    LoadConfig();

    bool ok = true;
    RESOLVE(FindPlayerPed,                        "_Z13FindPlayerPedi");
    RESOLVE(CPad_GetPad,                          "_ZN4CPad6GetPadEi");
    RESOLVE(CPad_SprintJustDown,                  "_ZN4CPad14SprintJustDownEv");
    RESOLVE(CPed_GetWeaponSlot,                   "_ZN4CPed13GetWeaponSlotE11eWeaponType");
    RESOLVE(CPed_GiveWeapon,                      "_ZN4CPed10GiveWeaponE11eWeaponTypejb");
    RESOLVE(CPed_SetCurrentWeapon,                "_ZN4CPed16SetCurrentWeaponEi");
    RESOLVE(CStreaming_RequestModel,              "_ZN10CStreaming12RequestModelEii");
    RESOLVE(CStreaming_LoadAllRequestedModels,    "_ZN10CStreaming22LoadAllRequestedModelsEb");
    RESOLVE(CObject_new,                          "_ZN7CObjectnwEji");
    RESOLVE(CObject_ctor,                         "_ZN7CObjectC1Eib");
    RESOLVE(CWorld_Add,                           "_ZN6CWorld3AddEP7CEntity");
    RESOLVE(CWorld_Remove,                        "_ZN6CWorld6RemoveEP7CEntity");
    RESOLVE(CEntity_UpdateRwFrame,                "_ZN7CEntity13UpdateRwFrameEv");
    RESOLVE(CPlaceable_AllocateMatrix,            "_ZN10CPlaceable14AllocateMatrixEv");
    RESOLVE(CMatrix_UpdateRwMatrix,               "_ZNK7CMatrix14UpdateRwMatrixEP11RwMatrixTag");
    RESOLVE(GetAnimHierarchyFromSkinClump,        "_Z29GetAnimHierarchyFromSkinClumpP7RpClump");
    RESOLVE(RpHAnimIDGetIndex,                    "_Z17RpHAnimIDGetIndexP16RpHAnimHierarchyi");
    RESOLVE(RpHAnimHierarchyGetMatrixArray,       "_Z30RpHAnimHierarchyGetMatrixArrayP16RpHAnimHierarchy");
    RESOLVE(CAnimManager_GetAnimationBlockIndex,  "_ZN12CAnimManager22GetAnimationBlockIndexEPKc");
    RESOLVE(CAnimManager_GetAnimationBlock,       "_ZN12CAnimManager17GetAnimationBlockEPKc");
    RESOLVE(CAnimManager_GetAnimation,            "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    RESOLVE(CAnimManager_AddAnimBlockRef,         "_ZN12CAnimManager15AddAnimBlockRefEi");
    RESOLVE(CAnimManager_BlendAnimation,          "_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    RESOLVE(CAnimAssoc_Start,                     "_ZN21CAnimBlendAssociation5StartEf");
    RESOLVE(pTimeMs,                              "_ZN6CTimer22m_snTimeInMillisecondsE");

    void* sProcess = Sym("_ZN5CGame7ProcessEv");
    void* sRestart = Sym("_ZN5CGame24InitialiseWhenRestartingEv");
    void* sResurr  = Sym("_ZN10CGameLogic36RestorePlayerStuffDuringResurrectionEP10CPlayerPed7CVectorf");
    if (!sProcess) { logger->Error("simbolo ausente: CGame::Process"); ok = false; }

    if (!ok) { logger->Error(
