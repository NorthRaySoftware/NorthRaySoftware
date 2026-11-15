/* An example NorthRay Capture audio plugin: tremolo (the volume rising and falling at a steady rate).
 * Build (MinGW):  x86_64-w64-mingw32-gcc -O2 -shared -I.. tremolo.c -o tremolo.dll
 * Build (MSVC):   cl /O2 /LD /I.. tremolo.c /Fe:tremolo.dll
 * Then put tremolo.dll in %LOCALAPPDATA%\NorthRay\Capture\plugins and switch it on in Settings › Plugins. */
#include "northray_audio_plugin.h"

#include <math.h>
#include <stdlib.h>

typedef struct {
    float rate;    /* Hz */
    float depth;   /* 0..1 */
    double phase;  /* 0..1 */
    uint32_t sample_rate, channels;
} Tremolo;

static const NrAudioParam params[] = {
    {"rate", "Rate", 0.5f, 20.0f, 5.0f, "Hz"},
    {"depth", "Depth", 0.0f, 100.0f, 50.0f, "%"},
};

static void* create(uint32_t sample_rate, uint32_t channels) {
    Tremolo* t = (Tremolo*)calloc(1, sizeof(Tremolo));
    if (!t) return NULL;
    t->rate = 5.0f;
    t->depth = 0.5f;
    t->sample_rate = sample_rate;
    t->channels = channels;
    return t;
}

static void destroy(void* p) { free(p); }

static void set_param(void* p, uint32_t index, float v) {
    Tremolo* t = (Tremolo*)p;
    if (index == 0) t->rate = v;
    else if (index == 1) t->depth = v / 100.0f;
}

static void process(void* p, float* x, uint32_t frames) {
    Tremolo* t = (Tremolo*)p;
    const double step = t->rate / (double)t->sample_rate;
    for (uint32_t i = 0; i < frames; ++i) {
        /* A gain between 1 - depth and 1, following a sine. */
        const float g = 1.0f - t->depth * 0.5f * (1.0f - (float)cos(2.0 * 3.14159265358979 * t->phase));
        for (uint32_t c = 0; c < t->channels; ++c) x[i * t->channels + c] *= g;
        t->phase += step;
        if (t->phase >= 1.0) t->phase -= 1.0;
    }
}

static const NrAudioPlugin plugin = {
    NR_AUDIO_PLUGIN_API, "Tremolo", "NorthRay (example)", "1.0", 2, params, create, destroy, set_param, process, NULL,
};

__declspec(dllexport) const NrAudioPlugin* nr_audio_plugin(void) { return &plugin; }
