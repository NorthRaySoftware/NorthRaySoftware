/* NorthRay Capture audio-effect plugins: the C interface (version 1).
 *
 * A plugin is a Windows DLL (64-bit) that exports one function:
 *
 *     __declspec(dllexport) const NrAudioPlugin* nr_audio_plugin(void);
 *
 * returning a description that stays valid while the DLL is loaded. Capture calls it once, when the
 * user switches the plugin on, then makes an instance for each place it's used (a source's filters).
 *
 * The rules a plugin must keep:
 *   - create() may allocate; process() is called on Capture's audio thread every 10 ms and must not
 *     block, wait, allocate, do file or network work, or take longer than a small part of that time.
 *   - Sound is 32-bit float, interleaved, `channels` channels (always 2 today) at `sample_rate` (always
 *     48000 today), processed in place. Keep it within -1..1 unless you mean to be loud.
 *   - set_param() is called before the first process() and whenever the user changes a value, on the
 *     same thread as process(), never at the same time. Values arrive within the ranges you declared.
 *   - Report your delay in frames with latency() (or leave it NULL for none).
 *   - Each instance is independent: no shared state between instances unless it's read-only.
 *   - No windows, no dialogs, no network, no threads that outlive destroy().
 *
 * Plugins are never switched on automatically: the user enables each one in Settings › Plugins after
 * seeing its file's SHA-256 and signature. If the file changes, it's switched off again. A plugin runs
 * inside Capture, so a crash in it closes Capture; Capture then switches it off.
 */
#ifndef NORTHRAY_AUDIO_PLUGIN_H
#define NORTHRAY_AUDIO_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_AUDIO_PLUGIN_API 1
#define NR_AUDIO_PLUGIN_MAX_PARAMS 16

/* One adjustable value, shown as a slider. */
typedef struct NrAudioParam {
    const char* key;     /* stable, short, ASCII: saved in the user's settings ("depth") */
    const char* label;   /* shown to the user ("Depth") */
    float min, max, def;
    const char* unit;    /* "dB", "ms", "Hz", "%", ":1" or "" */
} NrAudioParam;

typedef struct NrAudioPlugin {
    uint32_t api;            /* NR_AUDIO_PLUGIN_API */
    const char* name;        /* "Tremolo" */
    const char* author;      /* "NorthRay" */
    const char* version;     /* "1.0" */
    uint32_t param_count;    /* up to NR_AUDIO_PLUGIN_MAX_PARAMS */
    const NrAudioParam* params;

    void* (*create)(uint32_t sample_rate, uint32_t channels);   /* NULL if it can't */
    void (*destroy)(void* instance);
    void (*set_param)(void* instance, uint32_t index, float value);
    void (*process)(void* instance, float* frames, uint32_t frame_count);
    uint32_t (*latency)(void* instance);                        /* may be NULL */
} NrAudioPlugin;

typedef const NrAudioPlugin* (*NrAudioPluginEntry)(void);
#define NR_AUDIO_PLUGIN_ENTRY "nr_audio_plugin"

#ifdef __cplusplus
}
#endif

#endif
