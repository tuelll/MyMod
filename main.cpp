#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <algorithm>

MYMODCFG(net.example.hudopacity, HUD Opacity, 1.0.0, YourName)

struct CRGBA { unsigned char r, g, b, a; };
struct CRect { float left, bottom, right, top; };

static float g_opacity = 0.5f;
static bool  g_inHud   = false;

static inline CRGBA Fade(const CRGBA& c) {
    CRGBA o = c;
    o.a = (unsigned char)std::clamp((int)(c.a * g_opacity), 0, 255);
    return o;
}

// Ponteiros das funcoes originais
DECL_HOOKv(CHud_Draw);
DECL_HOOKv(CSprite2d_DrawRect, void* rect, CRGBA* col);
DECL_HOOKv(CFont_SetColor, CRGBA* col);

// Liga o fade somente enquanto o HUD e desenhado
DECL_HOOKv(CHud_Draw) {
    g_inHud = true;
    CHud_Draw();
    g_inHud = false;
}

DECL_HOOKv(CSprite2d_DrawRect, void* rect, CRGBA* col) {
    if (g_inHud && col) { CRGBA c = Fade(*col); CSprite2d_DrawRect(rect, &c); }
    else CSprite2d_DrawRect(rect, col);
}

DECL_HOOKv(CFont_SetColor, CRGBA* col) {
    if (g_inHud && col) { CRGBA c = Fade(*col); CFont_SetColor(&c); }
    else CFont_SetColor(col);
}

extern "C" void OnModLoad() {
    logger->SetTag("HUDOpacity");

    cfg->Bind("Author", "", "About")->SetString("YourName");
    g_opacity = cfg->Bind("Opacity", 0.5f, "Settings")->GetFloat();
    g_opacity = std::clamp(g_opacity, 0.0f, 1.0f);
    cfg->Save();

    void* hGTASA = aml->GetLibHandle("libGTASA.so");

    // Confira estes simbolos com: nm -D libGTASA.so | grep -i "CHud|Sprite2d|CFont"
    HOOKSYM(CHud_Draw,          hGTASA, "_ZN4CHud4DrawEv");
    HOOKSYM(CSprite2d_DrawRect, hGTASA, "_ZN8CSprite2d8DrawRectERK5CRectRK5CRGBA");
    HOOKSYM(CFont_SetColor,     hGTASA, "_ZN5CFont8SetColorE5CRGBA");

    logger->Info("HUD opacity = %.2f", g_opacity);
}