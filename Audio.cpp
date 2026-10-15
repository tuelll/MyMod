#include "Audio.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <string>
#include <cmath>

static SLObjectItf s_engObj = nullptr, s_mixObj = nullptr, s_plObj = nullptr;
static SLEngineItf s_eng = nullptr;
static SLPlayItf   s_play = nullptr;
static SLVolumeItf s_vol = nullptr;

bool Audio::Init()
{
    if (s_eng) return true;
    if (slCreateEngine(&s_engObj, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return false;
    if ((*s_engObj)->Realize(s_engObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return false;
    if ((*s_engObj)->GetInterface(s_engObj, SL_IID_ENGINE, &s_eng) != SL_RESULT_SUCCESS) return false;
    if ((*s_eng)->CreateOutputMix(s_eng, &s_mixObj, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return false;
    return (*s_mixObj)->Realize(s_mixObj, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS;
}

void Audio::Stop()
{
    if (s_play) (*s_play)->SetPlayState(s_play, SL_PLAYSTATE_STOPPED);
    if (s_plObj) (*s_plObj)->Destroy(s_plObj);
    s_plObj = nullptr; s_play = nullptr; s_vol = nullptr;
}

void Audio::Play(const char* absPath, float volume01)
{
    if (!Init()) return;
    Stop();
    std::string uri = std::string("file://") + absPath;

    SLDataLocator_URI loc = { SL_DATALOCATOR_URI, (SLchar*)uri.c_str() };
    SLDataFormat_MIME fmt = { SL_DATAFORMAT_MIME, nullptr, SL_CONTAINERTYPE_UNSPECIFIED };
    SLDataSource src = { &loc, &fmt };
    SLDataLocator_OutputMix lo = { SL_DATALOCATOR_OUTPUTMIX, s_mixObj };
    SLDataSink snk = { &lo, nullptr };
    const SLInterfaceID ids[1] = { SL_IID_VOLUME };
    const SLboolean req[1] = { SL_BOOLEAN_TRUE };

    if ((*s_eng)->CreateAudioPlayer(s_eng, &s_plObj, &src, &snk, 1, ids, req) != SL_RESULT_SUCCESS) { s_plObj = nullptr; return; }
    if ((*s_plObj)->Realize(s_plObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { Stop(); return; }
    (*s_plObj)->GetInterface(s_plObj, SL_IID_PLAY, &s_play);
    (*s_plObj)->GetInterface(s_plObj, SL_IID_VOLUME, &s_vol);
    if (s_vol) {
        float v = volume01 < 0.01f ? 0.01f : (volume01 > 1.f ? 1.f : volume01);
        (*s_vol)->SetVolumeLevel(s_vol, (SLmillibel)(2000.0f * log10f(v)));
    }
    if (s_play) (*s_play)->SetPlayState(s_play, SL_PLAYSTATE_PLAYING);
}
