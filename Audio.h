#pragma once
// Reprodutor simples de mp3 via OpenSL ES (substitui o audio_stream do CLEO)
namespace Audio {
    bool Init();
    void Play(const char* absPath, float volume01);
    void Stop();
}
