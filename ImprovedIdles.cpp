// Improved Idles (port AML 32 bits do script CLEO de X3N-0N284, v2.3)
// GTA SA Android 2.00 armeabi-v7a. Logica baseada no "Improved Idles.txt".
#include <mod/amlmod.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "Audio.h"

MYMODCFG(net.x3n.improvedidles, Improved Idles, 2.3, X3N-0N284 (port AML))

#define TAG "ImprovedIdles"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ---------------------------------------------------------------- offsets
// Layout do CPed igual ao PC 1.0. Se o log mostrar "offsets invalidos", ajuste aqui.
namespace Off {
    constexpr int Clump = 0x18, MoveSpeed = 0x44, Intelligence = 0x440, PlayerData = 0x444;
    constexpr int Weapons = 0x5A0, WeaponStride = 0x1C, Health = 0x540, Armour = 0x548, ActiveSlot = 0x718;
    constexpr int Vehicle = 0x58C;
    constexpr int AssocTime = 0x1C;        // CAnimBlendAssociation::m_fCurrentTime
    constexpr int ClothesTexKeys = 0x28;   // CPedClothesDesc::m_anTextureKeys[18]
    constexpr int BlockLoaded = 0x10;      // CAnimBlock::bLoaded
}
static int  kWantedOffset = 0x20;          // CWanted::m_nWantedLevel (configuravel no .ini: WantedOffset)
static const int kIfpBase = 25575;         // id de streaming do 1o bloco de animacao
enum { F_STARTED = 0x1, F_LOOPED = 0x2, F_PARTIAL = 0x10, F_WALK = 0x100 };

// ---------------------------------------------------------------- tipos
struct CAnimBlendAssociation; struct CAnimBlendHierarchy; struct CAnimBlock;
typedef void (*AssocCB)(CAnimBlendAssociation*, void*);

static void*  (*FindPlayerPed)(int);
static void*  (*FindPlayerWanted)(int);
static int    (*GetAnimationBlockIndex)(const char*);
static CAnimBlock* (*GetAnimationBlock)(const char*);
static CAnimBlendHierarchy* (*GetAnimation)(const char*, const CAnimBlock*);
static CAnimBlendAssociation* (*BlendAnimation)(void*, CAnimBlendHierarchy*, int, float);
static void   (*AddAnimBlockRef)(int);
static void   (*RequestModel)(int, int);
static void   (*LoadAllRequestedModels)(bool);
static void   (*AssocSetBlend)(CAnimBlendAssociation*, float, float);
static void   (*AssocSetCurrentTime)(CAnimBlendAssociation*, float);
static void   (*AssocSetFinishCB)(CAnimBlendAssociation*, AssocCB, void*);
static void   (*AssocSetDeleteCB)(CAnimBlendAssociation*, AssocCB, void*);
static void   (*PedSay)(void*, unsigned short, unsigned int, float, unsigned char, unsigned char, unsigned char);
static bool   (*PedInControl)(void*);
static void*  (*GetTaskDuck)(void*, bool);
static float  (*GetStatValue)(unsigned short);
static bool   (*GangWarGoingOn)();
static unsigned (*KeyUpper)(const char*);
static void*  (*GetPad)(int);
static bool   (*PadGetTarget)(void*, bool);
static bool   (*PadGetWeapon)(void*, void*, bool);
static bool   (*PadGetDuck)(void*);
static bool   (*PadGetJump)(void*);
static short* pNewWeather; static float* pRain; static unsigned* pCurrArea;
static bool*  pCutscene; static unsigned* pTimeMs;
static void   (*ProcessControl_orig)(void*) = nullptr;

// ---------------------------------------------------------------- config
struct Cfg { int timeToIdle=12, percent=90; bool pause=true, speak=true, inspect=true, fat=true, watch=true, weather=true; } C;
static std::string g_dir;

static std::string Trim(std::string s){ size_t a=s.find_first_not_of(" \t\r\n"); if(a==std::string::npos) return ""; size_t b=s.find_last_not_of(" \t\r\n"); return s.substr(a,b-a+1); }
static void LoadIni()
{
    FILE* f = fopen((g_dir + "Improved_Idles.ini").c_str(), "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string s = line; size_t c = s.find_first_of("#;"); if (c != std::string::npos) s = s.substr(0, c);
        size_t eq = s.find('='); if (eq == std::string::npos) continue;
        std::string k = Trim(s.substr(0, eq)); int v = atoi(Trim(s.substr(eq + 1)).c_str());
        if (k=="TimeToIdle") C.timeToIdle=v; else if (k=="RandomPercent") C.percent=v;
        else if (k=="PauseMenu") C.pause=v; else if (k=="IdleSpeaking") C.speak=v;
        else if (k=="WeaponInspecting") C.inspect=v; else if (k=="TiringFatBoy") C.fat=v;
        else if (k=="WatchingTime") C.watch=v; else if (k=="WeatherReact") C.weather=v;
        else if (k=="WantedOffset") kWantedOffset=v;
    }
    fclose(f);
    if (C.timeToIdle < 6) C.timeToIdle += 6;
}

// ---------------------------------------------------------------- utilidades
static unsigned NowReal(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (unsigned)(t.tv_sec*1000u + t.tv_nsec/1000000u); }
static int  Rand(int a, int b){ return a + rand() % (b - a + 1); }          // inclusivo
static bool Pct(int p){ return (rand() % 100) + 1 <= p; }                    // "Random_Percent == p"
template<class T> static T& At(void* p, int off){ return *(T*)((char*)p + off); }
template<class T> static void Sym(void* h, T& out, const char* n){ out = (T)dlsym(h, n); if(!out) LOG("simbolo ausente: %s", n); }

// ---------------------------------------------------------------- estado
enum Mood { M_CALM, M_MAD, M_TIRED };
enum Kind { K_NONE, K_PISTOL, K_MG, K_ASSAULT, K_RIFLE, K_SAWNOFF, K_SMG, K_BAT };
enum Speak { S_NONE, S_SING, S_ANGRY, S_TIRING };
enum Weather { W_NONE, W_RAIN, W_SUN };

static struct {
    bool ready=false, dead=false, selfTested=false, blockOK=false;
    int  blockTries=0, blockIdx=-1; CAnimBlock* block=nullptr;
    bool armed=false, allowed=false, idleMode=true;
    unsigned fireAt=0, lastReal=0, lastIniCheck=0, sayAt=0;
    CAnimBlendAssociation* assoc=nullptr; char anim[24]={0};
    Speak speak=S_NONE; int lastWeapon=0; float lastHealth=0;
} S;

static void OnAssocGone(CAnimBlendAssociation*, void*){ S.assoc=nullptr; Audio::Stop(); S.anim[0]=0; }

static int CurWeapon(void* ped){ int slot=At<int>(ped,Off::ActiveSlot); if(slot<0||slot>12) return 0;
    return At<int>(ped, Off::Weapons + slot*Off::WeaponStride); }
static int SlotOf(int w){
    if (w>=22&&w<=24) return 2; if (w>=25&&w<=27) return 3; if (w==28||w==29||w==32) return 4;
    if (w==30||w==31) return 5; if (w==33||w==34) return 6; if (w>=35&&w<=38) return 7; return 0; }
static Kind KindOf(int w){
    if (w==5||w==6||w==7||w==15||w==3||w==8) return K_BAT;
    switch (SlotOf(w)) {
        case 2: return K_PISTOL;
        case 3: return w==26 ? K_SAWNOFF : K_RIFLE;      // como no script: shotgun -> "Idle_Shotgun"
        case 4: return w==29 ? K_SMG : K_MG;
        case 5: return K_ASSAULT;
        case 6: return K_RIFLE; }
    return K_NONE; }

static int WantedLevel(){ void* w = FindPlayerWanted ? FindPlayerWanted(-1) : nullptr; return w ? At<int>(w, kWantedOffset) : 0; }

static bool HasWatch(void* ped)
{
    static const char* names[] = {"watchpink","watchyellow","watchpro","watchpro2","watchsub1","watchsub2",
        "watchzip1","watchzip2","watchgno","watchgno2","watchcro","watchcro2"};
    void* pd = At<void*>(ped, Off::PlayerData); if (!pd || !KeyUpper) return false;
    void* cd = At<void*>(pd, 4); if (!cd) return false;
    unsigned key = ((unsigned*)((char*)cd + Off::ClothesTexKeys))[14];
    for (auto n : names) if (KeyUpper(n) == key) return true;
    return false;
}

static Weather CurWeather()
{
    if (!C.weather || (pCurrArea && *pCurrArea > 0)) return W_NONE;
    int w = *pNewWeather;
    if (w==8 || w==16 || (pRain && *pRain > 0.f)) return W_RAIN;
    if (w==0||w==6||w==11||w==13||w==17) return W_SUN;
    return W_NONE;
}

// ------------------------------------------------- escolha da animacao (CheckCase + SetIdle)
struct Pick { char anim[24]; Speak speak; const char* sound; };
static bool Choose(void* ped, bool idle, Pick& out)
{
    memset(&out, 0, sizeof out);
    Weather wx = CurWeather(); bool watch = C.watch && HasWatch(ped);
    int w = CurWeapon(ped); Kind k = KindOf(w);
    float fat = GetStatValue ? GetStatValue(21) : 0.f;
    Mood mood = M_CALM;
    if (idle && fat > 600.f && C.fat && Pct(60)) mood = M_TIRED;
    else if (idle && (w > 1 || (GangWarGoingOn && GangWarGoingOn()) || WantedLevel() > 0)) mood = M_MAD;

    auto moodMan = [&]{
        if (mood == M_MAD)   { out.speak=S_ANGRY;  snprintf(out.anim,24,"Idle_mad%d",Rand(1,3)); }
        if (mood == M_TIRED) { out.speak=S_TIRING; snprintf(out.anim,24,"Idle_tired%d",Rand(1,2)); }
        if (mood == M_CALM)  { out.speak=S_SING;
            if (idle) { int i = Rand(1, watch ? 7 : 5); if (i==7) strcpy(out.anim,"Walk_1"); else snprintf(out.anim,24,"Idle_%d",i); }
            else if (watch) strcpy(out.anim,"Walk_1"); }
    };
    auto inspect = [&]{
        out.speak = Pct(50) ? S_SING : S_ANGRY;
        const char* a=nullptr; const char* s=nullptr;
        switch (k) { case K_PISTOL: a="Idle_Pistol"; s="pistol_inspect.mp3"; break;
            case K_ASSAULT: a="Idle_Rifle"; s="rifle_inspect.mp3"; break;
            case K_RIFLE: a="Idle_Shotgun"; s="shotgun_inspect.mp3"; break;
            case K_SMG: a="Idle_smg"; s="smg_inspect.mp3"; break;
            case K_SAWNOFF: a="Idle_sawnoff"; s="sawnoff_inspect.mp3"; break;
            case K_MG: a="Idle_Uzi"; s="uzi_inspect.mp3"; break;
            case K_BAT: a="Idle_Bat"; break; default: break; }
        if (a) strcpy(out.anim, a);
        if (k != K_BAT) out.sound = s;
    };

    if (wx != W_NONE && w < 2 && Pct(70)) {                       // WeatherTrap
        if (wx == W_RAIN) { if (idle) snprintf(out.anim,24,"Idle_rain%d",Rand(1,2)); else strcpy(out.anim,"Idle_rain1"); }
        else { out.speak = S_TIRING; strcpy(out.anim,"Idle_hot"); }
    } else {
        int r = Rand(40, 70);
        if (k==K_PISTOL||k==K_SMG||k==K_SAWNOFF||k==K_MG||k==K_BAT) {
            if (k == K_BAT) r = 20;
            if (C.inspect) { if (Pct(r)) moodMan(); else inspect(); } else moodMan();
        } else if (C.inspect && (k==K_RIFLE||k==K_ASSAULT)) inspect();
        else if (w < 21) moodMan();
    }
    return out.anim[0] != 0;
}

static void DoSay(void* ped, Speak s)
{
    if (!C.speak || !PedSay) return;
    if (s == S_SING)  PedSay(ped, 336, 0, 1.f, 0, 0, 0);
    if (s == S_ANGRY) { static const int ids[4] = {182,176,89,44}; PedSay(ped, ids[Rand(0,3)], 0, 1.f, 0, 0, 0); }
    if (s == S_TIRING) { PedSay(ped, 351, 0, 1.f, 0, 0, 0); S.sayAt = NowReal() + 500; }
}

// ------------------------------------------------- bloco de animacoes
static bool EnsureBlock()
{
    if (S.blockOK) return true;
    if (S.blockTries > 600) return false;
    if (!GetAnimationBlockIndex) return false;
    if (S.blockIdx < 0) {
        S.blockIdx = GetAnimationBlockIndex("Idle_Anims");
        if (S.blockIdx < 0) { if (++S.blockTries == 1) LOG("bloco Idle_Anims nao esta no gta3.img (adicione Idle_Anims.ifp)"); return false; }
        S.block = GetAnimationBlock("Idle_Anims");
        RequestModel(S.blockIdx + kIfpBase, 4); LoadAllRequestedModels(false); AddAnimBlockRef(S.blockIdx);
    }
    if (S.block && *(bool*)((char*)S.block + Off::BlockLoaded)) { S.blockOK = true; LOG("Idle_Anims carregado (bloco %d)", S.blockIdx); return true; }
    if (++S.blockTries % 120 == 0) LOG("Idle_Anims nao carregou; confira kIfpBase");
    return false;
}

static void StopAnim()
{
    if (S.assoc) { AssocSetBlend(S.assoc, 0.f, -4.f); }          // fade out; o callback de delete zera o ponteiro
    Audio::Stop(); S.anim[0] = 0; S.armed = false;
}

static void Play(void* ped, const Pick& p, bool idle)
{
    void* clump = At<void*>(ped, Off::Clump); if (!clump) return;
    CAnimBlendHierarchy* h = GetAnimation(p.anim, S.block); if (!h) { LOG("anim %s nao encontrada", p.anim); return; }
    int flags = idle ? F_STARTED : (F_STARTED | F_PARTIAL | F_WALK);
    CAnimBlendAssociation* a = BlendAnimation(clump, h, flags, 4.0f); if (!a) return;
    if (AssocSetFinishCB) AssocSetFinishCB(a, OnAssocGone, nullptr);
    if (AssocSetDeleteCB) AssocSetDeleteCB(a, OnAssocGone, nullptr);
    S.assoc = a; strcpy(S.anim, p.anim); S.idleMode = idle;
    if (p.sound) Audio::Play((g_dir + "cleo_sounds/Improved_Idles/" + p.sound).c_str(), 0.6f);
    DoSay(ped, p.speak);
}

// ------------------------------------------------- loop por frame
static void SelfTest(void* ped)
{
    float hp = At<float>(ped, Off::Health), ar = At<float>(ped, Off::Armour);
    void* clump = At<void*>(ped, Off::Clump);
    LOG("selftest: hp=%.1f armor=%.1f slot=%d weapon=%d clump=%p wanted=%d", hp, ar, At<int>(ped,Off::ActiveSlot), CurWeapon(ped), clump, WantedLevel());
    if (hp < 0.f || hp > 2000.f || ar < 0.f || ar > 2000.f || !clump) { S.dead = true; LOG("offsets invalidos - plugin desligado"); }
    S.selfTested = true;
}

static bool Frame(void* ped)
{
    unsigned real = NowReal(); unsigned now = *pTimeMs;
    bool resumed = S.lastReal && real - S.lastReal > 1000;   // voltou do menu de pausa
    S.lastReal = real;
    if (real - S.lastIniCheck > 3000) { LoadIni(); S.lastIniCheck = real; }

    float hp = At<float>(ped, Off::Health);
    float v[3]; memcpy(v, (char*)ped + Off::MoveSpeed, 12);
    float speed = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) * 50.f;   // m/s, como GET_CHAR_SPEED
    bool idle = speed < 0.5f;
    void* pad = GetPad ? GetPad(0) : nullptr;
    bool aiming = pad && PadGetTarget(pad, false), firing = pad && PadGetWeapon(pad, ped, false);
    void* intel = At<void*>(ped, Off::Intelligence);
    bool crouch = (pad && PadGetDuck(pad)) || (intel && GetTaskDuck && GetTaskDuck(intel, false));
    int w = CurWeapon(ped);
    bool heavy = SlotOf(w) == 7 || w == 9 || w == 42;
    bool blocked = speed > 4.f || aiming || firing || crouch || hp < 20.f || heavy
                   || (PedInControl && !PedInControl(ped)) || (pCutscene && *pCutscene) || At<void*>(ped, Off::Vehicle);

    // ---- animacao em curso: verifica se precisa cancelar
    if (S.assoc) {
        bool cancel = blocked || w != S.lastWeapon || hp < S.lastHealth || (S.idleMode && !idle) ;
        if (!strncmp(S.anim,"Idle_rain",9) || !strcmp(S.anim,"Idle_hot")) {
            Weather wx = CurWeather(); if ((wx != W_RAIN) && !(wx == W_SUN && !strcmp(S.anim,"Idle_hot"))) cancel = true; }
        if ((!strcmp(S.anim,"Idle_6") || !strcmp(S.anim,"Walk_1")) && !(C.watch && HasWatch(ped))) cancel = true;
        if (!strcmp(S.anim,"Idle_Bat") && *(float*)((char*)S.assoc + Off::AssocTime) > 0.4f) AssocSetCurrentTime(S.assoc, 0.f);
        if (S.sayAt && real >= S.sayAt) { S.sayAt = 0; if (PedSay) PedSay(ped, 349, 0, 1.f, 0, 0, 0); }
        S.lastHealth = hp;
        if (cancel) StopAnim();
        return true;
    }
    S.lastHealth = hp; S.lastWeapon = w;

    // ---- aguardando o tempo de idle
    if (blocked || heavy) { S.armed = false; return true; }
    if (!S.armed || S.idleMode != idle) {
        S.armed = true; S.idleMode = idle; S.allowed = Pct(C.percent);
        int base = C.timeToIdle * 1000; S.fireAt = now + Rand(base * 10 / 25, base);
    }
    if (C.pause && resumed && idle) { S.allowed = true; S.fireAt = now; }
    if (now >= S.fireAt) {
        S.armed = false;
        Pick p;
        if (S.allowed && EnsureBlock() && Choose(ped, idle, p)) Play(ped, p, idle);
    }
    return true;
}

static void Hook_ProcessControl(void* self)
{
    ProcessControl_orig(self);
    if (S.dead || !S.ready) return;
    void* ped = FindPlayerPed(-1);
    if (ped != self) return;
    if (!S.selfTested) { SelfTest(ped); if (S.dead) return; }
    Frame(ped);
}

// ------------------------------------------------- inicializacao
static bool Exists(const std::string& p){ return access(p.c_str(), F_OK) == 0; }

extern "C" void OnModLoad()
{
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) { LOG("libGTASA.so nao encontrada"); return; }

    std::string base = aml->GetAndroidDataPath();
    const std::string cand[] = { base + "/ImprovedIdles/", base + "/files/ImprovedIdles/",
        "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/ImprovedIdles/" };
    for (auto& c : cand) if (Exists(c + "Improved_Idles.ini")) { g_dir = c; break; }
    if (g_dir.empty()) { g_dir = cand[1]; LOG("pasta ImprovedIdles/ nao achada; usando %s", g_dir.c_str()); }
    LoadIni();

    Sym(h, FindPlayerPed, "_Z13FindPlayerPedi");          Sym(h, FindPlayerWanted, "_Z16FindPlayerWantedi");
    Sym(h, GetAnimationBlockIndex, "_ZN12CAnimManager22GetAnimationBlockIndexEPKc");
    Sym(h, GetAnimationBlock, "_ZN12CAnimManager17GetAnimationBlockEPKc");
    Sym(h, GetAnimation, "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    Sym(h, BlendAnimation, "_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    Sym(h, AddAnimBlockRef, "_ZN12CAnimManager15AddAnimBlockRefEi");
    Sym(h, RequestModel, "_ZN10CStreaming12RequestModelEii");
    Sym(h, LoadAllRequestedModels, "_ZN10CStreaming22LoadAllRequestedModelsEb");
    Sym(h, AssocSetBlend, "_ZN21CAnimBlendAssociation8SetBlendEff");
    Sym(h, AssocSetCurrentTime, "_ZN21CAnimBlendAssociation14SetCurrentTimeEf");
    Sym(h, AssocSetFinishCB, "_ZN21CAnimBlendAssociation17SetFinishCallbackEPFvPS_PvES1_");
    Sym(h, AssocSetDeleteCB, "_ZN21CAnimBlendAssociation17SetDeleteCallbackEPFvPS_PvES1_");
    Sym(h, PedSay, "_ZN4CPed3SayEtjfhhh");                Sym(h, PedInControl, "_ZN4CPed14IsPedInControlEv");
    Sym(h, GetTaskDuck, "_ZNK16CPedIntelligence11GetTaskDuckEb");
    Sym(h, GetStatValue, "_ZN6CStats12GetStatValueEt");   Sym(h, GangWarGoingOn, "_ZN9CGangWars14GangWarGoingOnEv");
    Sym(h, KeyUpper, "_ZN7CKeyGen15GetUppercaseKeyEPKc");
    Sym(h, GetPad, "_ZN4CPad6GetPadEi");                  Sym(h, PadGetTarget, "_ZN4CPad9GetTargetEb");
    Sym(h, PadGetWeapon, "_ZN4CPad9GetWeaponEP4CPedb");   Sym(h, PadGetDuck, "_ZN4CPad7GetDuckEv");
    pNewWeather = (short*)dlsym(h, "_ZN8CWeather14NewWeatherTypeE"); pRain = (float*)dlsym(h, "_ZN8CWeather4RainE");
    pCurrArea = (unsigned*)dlsym(h, "_ZN5CGame8currAreaE"); pCutscene = (bool*)dlsym(h, "_ZN12CCutsceneMgr10ms_runningE");
    pTimeMs = (unsigned*)dlsym(h, "_ZN6CTimer22m_snTimeInMillisecondsE");

    void* pc = dlsym(h, "_ZN10CPlayerPed14ProcessControlEv");
    if (!FindPlayerPed || !BlendAnimation || !GetAnimation || !AssocSetBlend || !PedInControl || !pc || !pNewWeather || !pTimeMs || !GetPad || !PadGetTarget || !PadGetWeapon || !PadGetDuck) {
        LOG("simbolos essenciais faltando - plugin desligado"); return; }
    if (!RequestModel || !LoadAllRequestedModels || !AddAnimBlockRef || !GetAnimationBlock || !AssocSetCurrentTime) { LOG("simbolos de streaming faltando"); return; }

    srand((unsigned)time(nullptr));
    aml->Hook(pc, (void*)Hook_ProcessControl, (void**)&ProcessControl_orig);
    S.ready = true;
    LOG("Improved Idles carregado; pasta: %s", g_dir.c_str());
}
