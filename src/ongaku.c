#include "ongaku.h"
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
#include <termios.h>
#include <unistd.h>

static struct termios g_orig;
static void enable_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig);
    raw = g_orig;
    raw.c_lflag &= ~(ICANON | ECHO);      // no line buffering, no echo
    raw.c_cc[VMIN]  = 0;                  // non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
static void disable_raw(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
}

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
    atomic_bool   seek_pending;
    atomic_uint_fast64_t  seek_target;
    atomic_bool   muted;
    size_t        prebuffer_frames;
} Player;

/* -------------------- Decode thread (producer) -------------------- */
static void* decode_thread(void* arg) {
  Player* pl = (Player*)arg;
  float temp[8192 * 8];                     // 8192 frames x up to 8ch

  while (pl->running) {
    // Handle pending seek
    if (atomic_load(&pl->seek_pending)) {
      drflac_uint64 tgt = (drflac_uint64)atomic_load(&pl->seek_target);
      if (drflac_seek_to_pcm_frame(pl->flac, tgt)) {
        ring_clear(&pl->rb);                // drop stale data
        pl->eof = 0;
        pl->playedFrames = tgt;
        atomic_store(&pl->seek_pending, false);
        // stay muted; we’ll unmute after prebuffer
      } else {
        atomic_store(&pl->seek_pending, false);
        atomic_store(&pl->muted, false);    // don’t stay silent forever
      }
    }

    size_t space = ring_space(&pl->rb);
    if (space == 0) {                       // near-full → nap a bit
      struct timespec ts = {0, 3*1000*1000}; nanosleep(&ts, NULL);
      continue;
    }

    size_t want = space < 8192 ? space : 8192;
    drflac_uint64 got = drflac_read_pcm_frames_f32(pl->flac, want, temp);
    if (got == 0) { pl->eof = 1; break; }

    ring_write(&pl->rb, temp, (size_t)got);

    // Prebuffer gate after seek
    if (atomic_load(&pl->muted) && ring_available(&pl->rb) >= pl->prebuffer_frames) {
      atomic_store(&pl->muted, false);
    }
  }
  return NULL;
}

/* -------------------- Audio callback (consumer) -------------------- */
static void data_callback(ma_device* dev, void* out, const void* in, ma_uint32 frameCount) {
  (void)in;
  Player* pl = (Player*)dev->pUserData;
  float*  dst = (float*)out;
  ma_uint32 ch = dev->playback.channels;

  if (atomic_load(&pl->paused) || atomic_load(&pl->muted)) {
    memset(dst, 0, (size_t)frameCount * ch * sizeof(float));
    return;
  }

  size_t got = ring_read(&pl->rb, dst, frameCount);
  pl->playedFrames += (drflac_uint64)got;

  if (pl->volume != 1.0f) {
    size_t n = got * ch; for (size_t i=0;i<n;i++) dst[i] *= pl->volume;
  }

  if (got < frameCount) {
    memset(dst + got * ch, 0, (frameCount - got) * ch * sizeof(float));
    if (pl->eof && ring_available(&pl->rb) == 0) {
      atomic_store(&pl->finished, true);
    }
  }
}

/* -------------------- Ongaku Player -------------------- */
int ongaku_play(const char* path, float volume) {
    // Open FLAC
    drflac* flac = drflac_open_file(path, NULL);
    if (!flac) { fprintf(stderr, "Failed to open FLAC: %s\n", path); return 1; }

    // print file info
    double durationSec = (double)flac->totalPCMFrameCount / flac->sampleRate;
    int minutes = (int)(durationSec / 60), seconds = (int)durationSec % 60;
    printf("\nFile: %s\n  Channels: %u  SampleRate: %u Hz  BitDepth: %u\n  Frames: %llu  Length: %d:%02d (%.1f s)\n",
           path, flac->channels, flac->sampleRate, flac->bitsPerSample,
           (unsigned long long)flac->totalPCMFrameCount, minutes, seconds, durationSec);
    puts("--------------------------------------");

    // ==== PLAYER STATE ====
    Player pl = {0};
    pl.flac   = flac;
    pl.volume = (volume < 0.f) ? 0.f : (volume > 1.f ? 1.f : volume);
    atomic_init(&pl.finished, 0);
    atomic_init(&pl.paused,   0);
    atomic_init(&pl.seek_pending, 0);
    atomic_init(&pl.muted,        0);
    pl.running = 1; pl.eof = 0;

    // ==== DEVICE ====
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

    // ==== RING BUFFER ====
    size_t framesCap = (size_t)pl.device.sampleRate * 2; // ~2s
    if (ring_init(&pl.rb, framesCap, pl.device.playback.channels) != 0) {
      fprintf(stderr, "ring buffer init failed\n");
      ma_device_uninit(&pl.device); drflac_close(flac);
      return 1;
    }
    pl.prebuffer_frames = pl.device.sampleRate / 2; // ~0.5s before unmute

    // ==== DECODE THREAD ====
    if (pthread_create(&pl.th, NULL, decode_thread, &pl) != 0) {
        fprintf(stderr, "decode thread create failed\n");
        ring_free(&pl.rb); ma_device_uninit(&pl.device); drflac_close(flac);
        return 1;
    }

    // ==== PREFILL & START ====
    atomic_store(&pl.muted, 1);
    while (ring_available(&pl.rb) < pl.prebuffer_frames) {
        struct timespec ts = {0, 3*1000*1000}; nanosleep(&ts, NULL);
    }
    atomic_store(&pl.muted, 0);

    if (ma_device_start(&pl.device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to start device.\n");
        pl.running = 0; pthread_join(pl.th, NULL);
        ring_free(&pl.rb); ma_device_uninit(&pl.device); drflac_close(flac);
        return 1;
    }

    // UI LOOP (j/l/k/q)
    enable_raw();
    while (!atomic_load(&pl.finished)) {
        // status
        double elapsed = (double)pl.playedFrames / flac->sampleRate;
        size_t avail   = ring_available(&pl.rb);
        double pct     = 100.0 * (double)avail / (double)pl.rb.capacity;

        printf("\r%02d:%02d  Buf:%5.1f%%\tVol: %5.2f\t%s",
            (int)(elapsed/60), (int)elapsed % 60, pct, pl.volume,
            atomic_load(&pl.paused) ? "[PAUSED]" :
            (atomic_load(&pl.muted)  ? "[SEEK]"   : "[PLAY]"));
        fflush(stdout);

        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q') { atomic_store(&pl.finished, 1); break; }
            if (c == 'k') { int p = atomic_load(&pl.paused); atomic_store(&pl.paused, !p); }
            if (c == 'n') { pl.volume -= 0.01; }
            if (c == 'm') { pl.volume += 0.01; }
            if (c == 'j' || c == 'l') {
                double now = (double)pl.playedFrames / flac->sampleRate;
                double to  = now + (c=='l' ? +5.0 : -5.0);
                if (to < 0) to = 0.0;
                drflac_uint64 tgt = (drflac_uint64)(to * flac->sampleRate);
                atomic_store(&pl.seek_target, (uint_fast64_t)tgt);
                atomic_store(&pl.seek_pending, 1);
                atomic_store(&pl.muted, 1); // mute until prebuffered
            }
        }
        ma_sleep(100);
    }
    disable_raw();
    puts("");

    // Player shutdown and cleanup
    pl.running = 0;
    pthread_join(pl.th, NULL);
    ma_device_uninit(&pl.device);
    ring_free(&pl.rb);
    drflac_close(flac);
    return 0;
}
