/* The single translation unit that compiles miniaudio.
 *
 * The MA_NO_* defines strip the decoders, encoders, resource manager, node
 * graph, engine, and waveform generators before compilation, so --gc-sections
 * has less to remove and the size budget stays reachable. See AGENTS.md 4.
 *
 * ALSA and PulseAudio are resolved with dlopen by miniaudio, so this file does
 * not need their development headers. */

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_MP3
#define MA_NO_FLAC
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "audio/device.h"
#include "core/engine.h"
#include "core/noise.h"

/* One device for the process lifetime. Static rather than heap-allocated: the
 * callback must never allocate, and a fixed address cannot be freed under it. */
static ma_device device;
static ma_context forced_context;
static int device_open;
static int context_open;

/* Set from the miniaudio notification callback, which runs on a miniaudio-owned
 * thread. Only a flag crosses that boundary; the reinitialization itself happens
 * on the main thread (M7). */
static _Atomic int restart_requested;
static int stopping;

/* Audio thread. Calls exactly one function: no allocation, no locks, no I/O. */
static void data_callback(ma_device *dev, void *output, const void *input,
                          ma_uint32 frame_count)
{
    (void)input;

    wn_engine_render((wn_engine *)dev->pUserData, (float *)output, frame_count);
}

/* miniaudio-owned thread. A device that disappears mid-stream reports stopped,
 * interrupted, or rerouted; any of those means the main thread must rebuild it. */
static void notification_callback(const ma_device_notification *notification)
{
    if (stopping) {
        return;
    }

    switch (notification->type) {
        case ma_device_notification_type_stopped:
        case ma_device_notification_type_rerouted:
        case ma_device_notification_type_interruption_began:
            atomic_store_explicit(&restart_requested, 1, memory_order_relaxed);
            break;
        default:
            break;
    }
}

/* TINYAMBIENT_AUDIO=null forces miniaudio's null backend, which drives the
 * callback in real time without touching hardware. That is the only way CI can
 * exercise the callback path; it is also the quickest way to tell a device
 * problem from a DSP problem. */
static ma_context *select_context(void)
{
    const char *forced = getenv("TINYAMBIENT_AUDIO");

    if (forced == NULL || strcmp(forced, "null") != 0) {
        return NULL; /* let miniaudio pick the default backend list */
    }

    {
        ma_backend backends[] = {ma_backend_null};

        if (ma_context_init(backends, 1, NULL, &forced_context) != MA_SUCCESS) {
            return NULL;
        }
        context_open = 1;
        return &forced_context;
    }
}

int wn_audio_start(wn_engine *engine)
{
    ma_device_config config;
    ma_context *context;
    ma_result result;

    if (engine == NULL) {
        return -1;
    }
    if (device_open) {
        return 0;
    }

    stopping = 0;
    config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = WN_CHANNELS;
    config.sampleRate = WN_SAMPLE_RATE;
    config.periodSizeInFrames = 512;
    config.dataCallback = data_callback;
    config.notificationCallback = notification_callback;
    config.pUserData = engine;

    context = select_context();

    result = ma_device_init(context, &config, &device);
    if (result != MA_SUCCESS) {
        if (context_open) {
            ma_context_uninit(&forced_context);
            context_open = 0;
        }
        return -1;
    }

    atomic_store_explicit(&restart_requested, 0, memory_order_relaxed);

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&device);
        if (context_open) {
            ma_context_uninit(&forced_context);
            context_open = 0;
        }
        return -1;
    }

    device_open = 1;
    return 0;
}

void wn_audio_stop(void)
{
    if (!device_open) {
        return;
    }

    /* ma_device_uninit stops the device and waits for an in-flight callback to
     * return, so the engine is safe to destroy afterwards. The `stopping` guard
     * stops our own shutdown from arming the restart flag. */
    stopping = 1;
    ma_device_uninit(&device);
    stopping = 0;
    device_open = 0;

    if (context_open) {
        ma_context_uninit(&forced_context);
        context_open = 0;
    }
}

int wn_audio_needs_restart(void)
{
    return atomic_load_explicit(&restart_requested, memory_order_relaxed);
}
