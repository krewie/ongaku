#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ringbuffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    drflac*       flac;         // streaming decoder
    ma_device     device;       // audio device
    RingBuffer    rb;           // frame buffer
    atomic_int    finished;     // 1 when playback fully done
    atomic_int    paused;       // 1 = paused
    drflac_uint64 playedFrames; // for elapsed timer
    pthread_t     th;           // decode thread
    int           running;      // decode thread should run
    int           eof;          // source drained
    float         volume;       // 0.0 .. 1.0
} Player;

/* -------------------- Decode thread (producer) -------------------- */
static void* decode_thread(void* arg) {
    Player* pl = (Player*)arg;
    const size_t CH = pl->rb.channels;
    (void)CH;

    float temp[4096 * 8]; // 4096 frames * up to 8ch

    while (pl->running) {
        // If buffer nearly full, nap briefly.
        if (ring_space(&pl->rb) < 1024) {
            struct timespec ts = {0, 2 * 1000 * 1000}; // 2ms
            nanosleep(&ts, NULL);
            continue;
        }

        size_t want = 4096;
        size_t space = ring_space(&pl->rb);
        if (space < want) want = space;
        if (want == 0) continue;

        drflac_uint64 got = drflac_read_pcm_frames_f32(pl->flac, want, temp);
        if (got == 0) { pl->eof = 1; break; }

        size_t wrote = ring_write(&pl->rb, temp, (size_t)got);
        // If wrote < got, buffer filled mid-chunk; remainder will be read next loop.
    }

    return NULL;
}

/* -------------------- Audio callback (consumer) -------------------- */
static void data_callback(ma_device* dev, void* out, const void* in, ma_uint32 frameCount) {
    (void)in;
    Player* pl = (Player*)dev->pUserData;
    float*  dst = (float*)out;
    ma_uint32 ch = dev->playback.channels;

    // If paused: output silence.
    if (atomic_load(&pl->paused)) {
        memset(dst, 0, (size_t)frameCount * ch * sizeof(float));
        return;
    }

    // Read what we can from the ring buffer.
    size_t got = ring_read(&pl->rb, dst, frameCount);
    pl->playedFrames += (drflac_uint64)got;

    // Apply volume on the frames we got.
    if (pl->volume != 1.0f && got > 0) {
        size_t n = got * ch;
        for (size_t i = 0; i < n; ++i) dst[i] *= pl->volume;
    }

    // If short, pad with silence so the device always gets frameCount frames.
    if (got < frameCount) {
        size_t padFrames = (size_t)frameCount - got;
        memset(dst + got * ch, 0, padFrames * ch * sizeof(float));

        // If source is drained AND buffer empty, mark finished.
        if (pl->eof && ring_available(&pl->rb) == 0) {
            atomic_store(&pl->finished, 1);
        }
    }
}

/* -------------------- Optional: precise seek helper -------------------- */
void ongaku_seek(Player* pl, double seconds) {
    drflac_uint64 target = (drflac_uint64)(seconds * pl->flac->sampleRate);
    if (target > pl->flac->totalPCMFrameCount) target = pl->flac->totalPCMFrameCount;

    // Stop feeding new data while we reposition.
    // In a fuller app you'd pause device and/or lock rb; for this demo it's fine.
    if (!drflac_seek_to_pcm_frame(pl->flac, target)) {
        fprintf(stderr, "Seek failed!\n");
        return;
    }

    // Flush ring so we don't play old samples.
    // Simple approach: clear counters (requires a clear helper or re-init).
    // If your ringbuffer has no clear(), do this:
    ring_free(&pl->rb);
    ring_init(&pl->rb, pl->device.sampleRate * 2, pl->device.playback.channels);

    pl->playedFrames = target;
    pl->eof = 0; // we can read again
}

/* -------------------- Main -------------------- */
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s song.flac [volume 0.0-1.0]\n", argv[0]);
        return 1;
    }

    const char* path  = argv[1];
    float volume = (argc >= 3) ? (float)atof(argv[2]) : 1.0f;

    drflac* flac = drflac_open_file(path, NULL);
    if (!flac) { fprintf(stderr, "Failed to open FLAC: %s\n", path); return 1; }

    /* Print file info */
    double durationSec = (double)flac->totalPCMFrameCount / flac->sampleRate;
    int minutes = (int)(durationSec / 60);
    int seconds = (int)durationSec % 60;

    printf("File: %s\n", path);
    printf("  Channels   : %u\n", flac->channels);
    printf("  SampleRate : %u Hz\n", flac->sampleRate);
    printf("  BitDepth   : %u\n", flac->bitsPerSample);
    printf("  Frames     : %llu\n", (unsigned long long)flac->totalPCMFrameCount);
    printf("  Length     : %d:%02d (%.1f sec)\n", minutes, seconds, durationSec);
    printf("--------------------------------------\n");

    /* Fill player state */
    Player pl = {0};
    pl.flac   = flac;
    pl.volume = (volume < 0.f) ? 0.f : (volume > 1.f ? 1.f : volume);
    atomic_init(&pl.finished, 0);
    atomic_init(&pl.paused,   0);
    pl.running = 1;
    pl.eof     = 0;

    /* Configure device to match the file (or force 48000 and resample if you add a resampler) */
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = (ma_uint32)flac->channels;
    cfg.sampleRate        = (ma_uint32)flac->sampleRate;
    cfg.dataCallback      = data_callback;
    cfg.pUserData         = &pl;

    if (ma_device_init(NULL, &cfg, &pl.device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to init audio device.\n");
        drflac_close(flac);
        return 1;
    }

    /* Init ring buffer with ~2 seconds of audio */
    size_t framesCap = (size_t)pl.device.sampleRate * 2; // 2s cushion
    if (ring_init(&pl.rb, framesCap, pl.device.playback.channels) != 0) {
        fprintf(stderr, "ring buffer init failed\n");
        ma_device_uninit(&pl.device);
        drflac_close(flac);
        return 1;
    }

    /* Start decode thread before audio */
    if (pthread_create(&pl.th, NULL, decode_thread, &pl) != 0) {
        fprintf(stderr, "decode thread create failed\n");
        ring_free(&pl.rb);
        ma_device_uninit(&pl.device);
        drflac_close(flac);
        return 1;
    }

    if (ma_device_start(&pl.device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to start device.\n");
        pl.running = 0;
        pthread_join(pl.th, NULL);
        ring_free(&pl.rb);
        ma_device_uninit(&pl.device);
        drflac_close(flac);
        return 1;
    }

    /* Simple wait loop with elapsed time */
    while (!atomic_load(&pl.finished)) {
        double elapsed = (double)pl.playedFrames / flac->sampleRate;
        size_t avail = ring_available(&pl.rb);
        double secondsBuffered = (double) avail / flac->sampleRate;

        printf("\rElapsed: %02d:%02d Buffer: %.1f sec (%zu frames)", 
               (int)(elapsed/60), 
               (int)elapsed % 60,
               secondsBuffered,
               avail);
        fflush(stdout);
        ma_sleep(250);
    }
    printf("\n");

    /* Cleanup */
    pl.running = 0;
    pthread_join(pl.th, NULL);

    ma_device_uninit(&pl.device);
    ring_free(&pl.rb);
    drflac_close(flac);
    return 0;
}

