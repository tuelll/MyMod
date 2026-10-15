// Improved Idles v2 (port AML 32 bits do script CLEO de X3N-0N284, v2.3)
// GTA SA Android 2.00 armeabi-v7a.
// v2: log em arquivo + handler de crash, hook por vtable, .ifp carregado do disco, ini na pasta configs do AML.
#include <mod/amlmod.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Audio.h"

MYMODCFG(net.x3n.improvedidles, Improved Idles, 2.3, X3N-0N284)

// ---------------------------------------------------------------- log em arquivo
static int         g_fd   = -1;
static const char* g_step = "inicio";          // ultimo passo executado (aparece no log de crash)
static void Log(const char* fmt, ...)
{
    char buf[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf - 2, fmt, ap); va_end(ap);
    if (n < 0) return; if (n > (int)sizeof buf - 2) n = sizeof buf - 2; buf[n++] = '\n';
    __android_log_write(ANDROID_LOG_INFO, "ImprovedIdles", buf);
    if (g_fd >= 0) { ssize_t r = write(g_fd, buf, n); (void)r; }
}
#define LOG(...) Log(__VA_ARGS__)
#define STEP(s)  (g_step = (s))

static struct sigaction g_oldSig[32];
static void CrashHandler(int sig, siginfo_t* si, void* ctx)
{
    uintptr_t pc = 0, lr = 0;
#if defined(__arm__)
    ucontext_t* uc = (ucontext_t*)ctx; pc = uc->uc_mcontext.arm_pc; lr = uc->uc_mcontext.arm_lr;
#endif
    Dl_info a{}, b{}; dladdr((void*)pc, &a); dladdr((void*)lr, &b);
    char buf[640];
    int n = snprintf(buf, sizeof buf, "CRASH sinal=%d fault=%p pc=%s+0x%lx lr=%s+0x%lx ultimo_passo=\"%s\"\n", sig, si ? si->si_addr : nullptr,
        a.dli_fname ? a.dli_fname : "?", a.dli_fbase ? (unsigned long)(pc - (uintptr_t)a.dli_fbase) : (unsigned long)pc,
        b.dli_fname ? b.dli_fname : "?", b.dli_fbase ? (unsigned long)(lr - (uintptr_t)b.dli_fbase) : (unsigned long)lr, g_step);
    if (g_fd >= 0) { ssize_t r = write(g_fd, buf, n); (void)r; }
    __android_log_write(ANDROID_LOG_FATAL, "ImprovedIdles", buf);
    if (sig > 0 && sig < 32) sigaction(sig, &g_oldSig[sig], nullptr);   // devolve ao handler original e refaz a falha
}
static void InstallCrashHandler()
{
    struct sigaction sa{}; sa.sa_sigaction = CrashHandler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (int s : sigs) sigaction(s, &sa, &g_oldSig[s]);
}

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
static CAnimBlock* (*RegisterAnimBlock)(const char*);
static void   (*LoadAnimFileStream)(void*, bool, const void*);
static void*  (*RwStreamOpen)(int, int, const void*);
static int    (*RwStreamClose)(void*, void*);
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
struct Cfg { bool enabled=true; int timeToIdle=12, percent=90; bool pause=true, speak=true, inspect=true, fat=true, watch=true, weather=true; } C;
static std::string g_cfg, g_data;          // g_cfg = .../configs/   g_data = .../configs/ImprovedIdles/

static std::string Trim(std::string s){ size_t a=s.find_first_not_of(" \t\r\n"); if(a==std::string::npos) return ""; size_t b=s.find_last_not_of(" \t\r\n"); return s.substr(a,b-a+1); }
static void LoadIni()
{
    FILE* f = fopen((g_cfg + "ImprovedIdles.ini").c_str(), "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string s = line; size_t c = s.find_first_of("#;"); if (c != std::string::npos) s = s.substr(0, c);
        size_t eq = s.find('='); if (eq == std::string::npos) continue;
        std::string k = Trim(s.substr(0, eq)); int v = atoi(Trim(s.substr(eq + 1)).c_str());
        if (k=="Enabled") C.enabled=v; else if (k=="TimeToIdle") C.timeToIdle=v; else if (k=="RandomPercent") C.percent=v;
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
    bool ready=false, dead=false, selfTested=false, blockOK=false, blockTried=false;
    CAnimBlock* block=nullptr; char blockName[32]="Idle_Anims";
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

// ------------------------------------------------- bloco de animacoes (carregado do disco)
static bool LoadIfpFromDisk()
{
    STEP("ifp: abrir arquivo");
    std::string path = g_data + "Idle_Anims.ifp";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOG("ifp: arquivo nao encontrado: %s", path.c_str()); return false; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size < 64 || size > 8 * 1024 * 1024) { fclose(f); LOG("ifp: tamanho invalido (%ld)", size); return false; }
    std::vector<unsigned char> buf(size);
    size_t rd = fread(buf.data(), 1, size, f); fclose(f);
    if ((long)rd != size) { LOG("ifp: leitura incompleta"); return false; }
    if (memcmp(buf.data(), "ANP3", 4) != 0) { LOG("ifp: formato inesperado (esperado ANP3)"); return false; }
    char name[25] = {0}; memcpy(name, buf.data() + 8, 24); strncpy(S.blockName, name, 31);
    LOG("ifp: %ld bytes, bloco '%s'", size, name);

    STEP("ifp: registrar bloco");
    CAnimBlock* blk = GetAnimationBlock(name);
    if (!blk) blk = RegisterAnimBlock(name);
    LOG("ifp: bloco registrado = %p", blk);

    STEP("ifp: RwStreamOpen");
    struct { unsigned char* start; unsigned int length; } mem = { buf.data(), (unsigned)size };
    void* st = RwStreamOpen(3 /*rwSTREAMMEMORY*/, 1 /*rwSTREAMREAD*/, &mem);
    if (!st) { LOG("ifp: RwStreamOpen falhou"); return false; }

    STEP("ifp: LoadAnimFile");
    LoadAnimFileStream(st, false, nullptr);
    STEP("ifp: RwStreamClose");
    RwStreamClose(st, &mem);

    STEP("ifp: validar");
    blk = GetAnimationBlock(name);
    if (!blk) { LOG("ifp: bloco sumiu apos carregar"); return false; }
    *(bool*)((char*)blk + Off::BlockLoaded) = true;
    CAnimBlendHierarchy* t1 = GetAnimation("Idle_1", blk); CAnimBlendHierarchy* t2 = GetAnimation("Idle_Pistol", blk);
    LOG("ifp: validacao Idle_1=%p Idle_Pistol=%p", t1, t2);
    if (!t1 && !t2) { LOG("ifp: nenhuma animacao encontrada no bloco - desligando animacoes"); return false; }
    S.block = blk; return true;
}

static bool EnsureBlock()
{
    if (S.blockOK) return true;
    if (S.blockTried) return false;           // so tenta uma vez
    S.blockTried = true;
    S.blockOK = LoadIfpFromDisk();
    LOG(S.blockOK ? "ifp: carregado com sucesso" : "ifp: falhou - animacoes desligadas");
    return S.blockOK;
}

static void StopAnim()
{
    if (S.assoc) { AssocSetBlend(S.assoc, 0.f, -4.f); }          // fade out; o callback de delete zera o ponteiro
    Audio::Stop(); S.anim[0] = 0; S.armed = false;
}

static void Play(void* ped, const Pick& p, bool idle)
{
    STEP("play: BlendAnimation");
    void* clump = At<void*>(ped, Off::Clump); if (!clump) return;
    CAnimBlendHierarchy* h = GetAnimation(p.anim, S.block); if (!h) { LOG("anim %s nao encontrada", p.anim); return; }
    LOG("tocando %s (%s) speak=%d som=%s", p.anim, idle ? "parado" : "andando", (int)p.speak, p.sound ? p.sound : "-");
    int flags = idle ? F_STARTED : (F_STARTED | F_PARTIAL | F_WALK);
    CAnimBlendAssociation* a = BlendAnimation(clump, h, flags, 4.0f); if (!a) return;
    if (AssocSetFinishCB) AssocSetFinishCB(a, OnAssocGone, nullptr);
    if (AssocSetDeleteCB) AssocSetDeleteCB(a, OnAssocGone, nullptr);
    S.assoc = a; strcpy(S.anim, p.anim); S.idleMode = idle;
    if (p.sound) Audio::Play((g_data + "cleo_sounds/Improved_Idles/" + p.sound).c_str(), 0.6f);
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
    STEP("ProcessControl original");
    ProcessControl_orig(self);
    if (S.dead || !S.ready) return;
    STEP("FindPlayerPed");
    void* ped = FindPlayerPed(-1);
    if (ped != self) return;
    static bool first = true; if (first) { first = false; LOG("primeiro frame do jogador"); }
    if (!S.selfTested) { STEP("selftest"); SelfTest(ped); if (S.dead) return; }
    STEP("Frame");
    Frame(ped);
    STEP("fim do frame");
}

// ------------------------------------------------- inicializacao
static bool Exists(const std::string& p){ return access(p.c_str(), F_OK) == 0; }
static void MkDir(const std::string& p){ mkdir(p.c_str(), 0777); }

static void SetupPathsAndLog()
{
    const char* c = aml->GetConfigPath();
    std::string cfg = c ? c : "";
    if (cfg.empty()) { const char* d = aml->GetAndroidDataPath(); cfg = std::string(d ? d : "") + "/configs"; }
    if (cfg.back() != '/') cfg += '/';
    g_cfg = cfg; g_data = cfg + "ImprovedIdles/";
    MkDir(g_cfg); MkDir(g_data); MkDir(g_data + "cleo_sounds"); MkDir(g_data + "cleo_sounds/Improved_Idles");
    g_fd = open((g_data + "ImprovedIdles.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
    if (g_fd < 0) { std::string alt = std::string(aml->GetAndroidDataPath()) + "/ImprovedIdles.log"; g_fd = open(alt.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666); }
    LOG("=== Improved Idles v2 iniciando ===");
    LOG("config: %s | dados: %s | log fd=%d", g_cfg.c_str(), g_data.c_str(), g_fd);

    std::string ini = g_cfg + "ImprovedIdles.ini";
    if (!Exists(ini)) {
        FILE* f = fopen(ini.c_str(), "w");
        if (f) { fputs("[settings]\n\nEnabled = 1 # 1 - ligado, 0 - desliga o mod\n\nTimeToIdle = 12 #secs\nRandomPercent = 90 # 100 - always, 0 - disable Idle\nPauseMenu = 1\nIdleSpeaking = 1\nWeaponInspecting = 1\nTiringFatBoy = 1\nWatchingTime = 1\nWeatherReact = 1\nWantedOffset = 32 # offset de CWanted::m_nWantedLevel (ajuste se a reacao a procurado falhar)\n", f); fclose(f); LOG("ini criado com valores padrao"); }
        else LOG("nao consegui criar o ini: %s", ini.c_str());
    }
    LoadIni();
}

static bool HookVtable(void* h)
{
    STEP("hook: vtable");
    void*  pc = dlsym(h, "_ZN10CPlayerPed14ProcessControlEv");
    void** vt = (void**)dlsym(h, "_ZTV10CPlayerPed");
    LOG("hook: ProcessControl=%p vtable=%p", pc, vt);
    if (!pc || !vt) return false;
    uintptr_t target = (uintptr_t)pc & ~1u;
    for (int i = 2; i < 29; i++) {
        if (((uintptr_t)vt[i] & ~1u) != target) continue;
        void** slot = &vt[i]; uintptr_t page = (uintptr_t)slot & ~4095u;
        if (mprotect((void*)page, 4096, PROT_READ | PROT_WRITE) != 0) { LOG("hook: mprotect falhou (errno %d)", errno); return false; }
        ProcessControl_orig = (void(*)(void*))vt[i];
        vt[i] = (void*)Hook_ProcessControl;
        LOG("hook: slot %d da vtable trocado (original=%p)", i - 2, (void*)ProcessControl_orig);
        return true;
    }
    LOG("hook: ProcessControl nao encontrado na vtable do CPlayerPed");
    return false;
}

extern "C" void OnModLoad()
{
    STEP("OnModLoad");
    SetupPathsAndLog();
    InstallCrashHandler();
    if (!C.enabled) { LOG("Enabled=0 no ini - mod desligado"); return; }

    STEP("GetLibHandle");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) { LOG("libGTASA.so nao encontrada"); return; }

    STEP("resolver simbolos");
    Sym(h, FindPlayerPed, "_Z13FindPlayerPedi");          Sym(h, FindPlayerWanted, "_Z16FindPlayerWantedi");
    Sym(h, GetAnimationBlockIndex, "_ZN12CAnimManager22GetAnimationBlockIndexEPKc");
    Sym(h, GetAnimationBlock, "_ZN12CAnimManager17GetAnimationBlockEPKc");
    Sym(h, GetAnimation, "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    Sym(h, RegisterAnimBlock, "_ZN12CAnimManager17RegisterAnimBlockEPKc");
    Sym(h, LoadAnimFileStream, "_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
    Sym(h, RwStreamOpen, "_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
    Sym(h, RwStreamClose, "_Z13RwStreamCloseP8RwStreamPv");
    Sym(h, BlendAnimation, "_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
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

    if (!FindPlayerPed || !BlendAnimation || !GetAnimation || !GetAnimationBlock || !RegisterAnimBlock || !LoadAnimFileStream || !RwStreamOpen || !RwStreamClose ||
        !AssocSetBlend || !AssocSetCurrentTime || !PedInControl || !pNewWeather || !pTimeMs || !GetPad || !PadGetTarget || !PadGetWeapon || !PadGetDuck) {
        LOG("simbolos essenciais faltando - plugin desligado"); return; }
    if (!AssocSetDeleteCB) LOG("aviso: SetDeleteCallback ausente");

    srand((unsigned)time(nullptr));
    if (!HookVtable(h)) { LOG("hook nao instalado - plugin desligado"); return; }
    S.ready = true;
    STEP("OnModLoad concluido");
    LOG("OnModLoad concluido; aguardando o jogador");
}
