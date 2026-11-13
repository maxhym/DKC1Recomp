#include "dkc1_telemetry.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  /* A banana bunch is the largest ordinary award in one frame; larger jumps
   * are file loads or resets. */
  kMaxBananaStep = 10,
  /* Larger camera jumps are room changes, not walking. */
  kMaxMoveStep = 48,
};

static uint16_t Read16(const uint8_t *wram, uint32_t address) {
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
}

void Dkc1TelemetryReadSnapshot(const uint8_t *wram, Dkc1TelemetrySnapshot *out) {
  memset(out, 0, sizeof *out);
  out->entrance = Read16(wram, 0x003E);
  out->lives = Read16(wram, 0x0575);
  out->bananas = Read16(wram, 0x057B);
  out->letters = Read16(wram, 0x057F);
  out->kong = Read16(wram, 0x056F);
  out->camera_x = Read16(wram, 0x088B);
}

static void Emit(Dkc1Telemetry *t, const char *line) {
  if (t->sink) t->sink(t->context, line);
}

void Dkc1TelemetryInit(Dkc1Telemetry *t, Dkc1TelemetrySink sink, void *context) {
  memset(t, 0, sizeof *t);
  t->sink = sink;
  t->context = context;
}

void Dkc1TelemetryHello(Dkc1Telemetry *t, const char *build, uint64_t frame) {
  char line[160];
  char safe[48];
  size_t n = 0;
  for (const char *p = build ? build : ""; *p && n + 1 < sizeof safe; ++p)
    if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) safe[n++] = *p;
  safe[n] = 0;
  snprintf(line, sizeof line,
           "{\"e\":\"hello\",\"proto\":%d,\"game\":\"dkc1\",\"build\":\"%s\",\"f\":%" PRIu64 "}",
           kDkc1TelemetryProtocol, safe, frame);
  Emit(t, line);
  t->said_hello = true;
}

void Dkc1TelemetryInput(Dkc1Telemetry *t, uint32_t input, uint64_t frame) {
  if (input) {
    t->seen_input = true;
    t->last_input_frame = frame;
  }
}

bool Dkc1TelemetryPlayerActive(const Dkc1Telemetry *t, uint64_t frame) {
  return t->seen_input && frame >= t->last_input_frame &&
         frame - t->last_input_frame <= kDkc1TelemetryInputWindow;
}

static int BananaDelta(uint16_t before, uint16_t after) {
  if (before > 99 || after > 99 || before == after) return 0;
  int delta = after > before ? after - before : after + 100 - before;
  return delta <= kMaxBananaStep ? delta : 0;
}

void Dkc1TelemetryStep(Dkc1Telemetry *t, const Dkc1TelemetrySnapshot *now, uint64_t frame) {
  char line[192];
  const Dkc1TelemetrySnapshot *was = &t->previous;
  const bool active = Dkc1TelemetryPlayerActive(t, frame);

  if (t->have_previous && active) {
    if (now->entrance != was->entrance) {
      snprintf(line, sizeof line, "{\"e\":\"level\",\"f\":%" PRIu64 ",\"level\":%u}",
               frame, (unsigned)now->entrance);
      Emit(t, line);
    }
    int bananas = BananaDelta(was->bananas, now->bananas);
    if (bananas > 0) {
      snprintf(line, sizeof line, "{\"e\":\"bananas\",\"f\":%" PRIu64 ",\"n\":%d,\"level\":%u}",
               frame, bananas, (unsigned)now->entrance);
      Emit(t, line);
    }
    if (now->lives != was->lives && now->lives < 100 && was->lives < 100) {
      if (now->lives + 1 == was->lives) {
        snprintf(line, sizeof line,
                 "{\"e\":\"death\",\"f\":%" PRIu64 ",\"level\":%u,\"x\":%u,\"cause\":\"life_lost\"}",
                 frame, (unsigned)now->entrance, (unsigned)now->camera_x);
        Emit(t, line);
      }
      snprintf(line, sizeof line, "{\"e\":\"lives\",\"f\":%" PRIu64 ",\"lives\":%u}",
               frame, (unsigned)now->lives);
      Emit(t, line);
    }
    if (now->letters != was->letters) {
      snprintf(line, sizeof line, "{\"e\":\"letters\",\"f\":%" PRIu64 ",\"mask\":%u,\"level\":%u}",
               frame, (unsigned)now->letters, (unsigned)now->entrance);
      Emit(t, line);
    }
    if (now->entrance == was->entrance) {
      int dx = (int)now->camera_x - (int)was->camera_x;
      if (dx < 0) dx = -dx;
      if (dx <= kMaxMoveStep) t->move_px += (uint32_t)dx;
    }
  }

  if (++t->frames_since_tick >= kDkc1TelemetryFramesPerTick) {
    t->frames_since_tick = 0;
    snprintf(line, sizeof line, "{\"e\":\"tick\",\"f\":%" PRIu64 ",\"demo\":%s,\"level\":%u}",
             frame, active ? "false" : "true", (unsigned)now->entrance);
    Emit(t, line);
    if (t->move_px) {
      snprintf(line, sizeof line, "{\"e\":\"move\",\"f\":%" PRIu64 ",\"dx\":%u}", frame, (unsigned)t->move_px);
      Emit(t, line);
      t->move_px = 0;
    }
  }
  t->previous = *now;
  t->have_previous = true;
}

void Dkc1TelemetryStateLoaded(Dkc1Telemetry *t, uint64_t frame) {
  char line[64];
  snprintf(line, sizeof line, "{\"e\":\"state_load\",\"f\":%" PRIu64 "}", frame);
  Emit(t, line);
  /* A loaded state is a new baseline rather than progress. */
  t->have_previous = false;
}

void Dkc1TelemetryBye(Dkc1Telemetry *t, uint64_t frame) {
  char line[64];
  if (t->move_px) {
    snprintf(line, sizeof line, "{\"e\":\"move\",\"f\":%" PRIu64 ",\"dx\":%u}", frame, (unsigned)t->move_px);
    Emit(t, line);
    t->move_px = 0;
  }
  snprintf(line, sizeof line, "{\"e\":\"bye\",\"f\":%" PRIu64 "}", frame);
  Emit(t, line);
}

/* ---- File-backed host instance ---- */

#ifndef DKC1_TELEMETRY_BUILD
#define DKC1_TELEMETRY_BUILD "0.0.14"
#endif

static Dkc1Telemetry s_host;
static FILE *s_file;
static bool s_checked;
static uint64_t s_last_frame;
static uint32_t s_pending_input;

/* ------------------------------------------------- boot sound capture --- */

enum { kBootAudioMinSeconds = 10 };

static void BootAudioPut32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool BootAudioHeader(Dkc1BootAudio *b, uint32_t data_bytes) {
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                   16, 0, 0, 0, 1, 0};
  const uint32_t align = (uint32_t)b->channels * 2u;
  BootAudioPut32(h + 4, data_bytes ? data_bytes + 36u : 0u);
  h[22] = (uint8_t)b->channels;
  BootAudioPut32(h + 24, (uint32_t)b->rate);
  BootAudioPut32(h + 28, (uint32_t)b->rate * align);
  h[32] = (uint8_t)align;
  h[34] = 16;
  memcpy(h + 36, "data", 4);
  BootAudioPut32(h + 40, data_bytes);
  return fseek(b->file, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof h, b->file) == sizeof h;
}

bool Dkc1BootAudioOpen(Dkc1BootAudio *b, const char *path, int channels,
                       int rate, double max_seconds) {
  memset(b, 0, sizeof *b);
  if (!path || !path[0] || channels < 1 || channels > 2 || rate < 8000 || rate > 96000 ||
      !(max_seconds >= 1.0 && max_seconds <= 600.0))
    return false;
  b->file = fopen(path, "wb");
  if (!b->file) return false;
  b->channels = channels;
  b->rate = rate;
  b->max_frames = (uint32_t)(max_seconds * rate);
  if (!BootAudioHeader(b, 0)) {
    fclose(b->file);
    b->file = NULL;
    return false;
  }
  return true;
}

static void BootAudioFinish(Dkc1BootAudio *b) {
  if (!b->file) return;
  (void)BootAudioHeader(b, b->frames * (uint32_t)b->channels * 2u);
  fclose(b->file);
  b->file = NULL;
  b->done = true;
}

void Dkc1BootAudioWrite(Dkc1BootAudio *b, const int16_t *samples, int frames) {
  if (!b->file || !samples || frames <= 0) return;
  if (!b->heard) {
    /* Skip the silence before the first sound. */
    int first = 0;
    for (; first < frames * b->channels; ++first)
      if (samples[first] > 16 || samples[first] < -16) break;
    first /= b->channels;
    if (first >= frames) return;
    b->heard = true;
    samples += first * b->channels;
    frames -= first;
  }
  if ((uint32_t)frames > b->max_frames - b->frames) frames = (int)(b->max_frames - b->frames);
  for (int i = 0; i < frames * b->channels; ++i) {
    const uint16_t v = (uint16_t)samples[i];
    uint8_t le[2];
    le[0] = (uint8_t)v;
    le[1] = (uint8_t)(v >> 8);
    if (fwrite(le, 1, 2, b->file) != 2) {
      BootAudioFinish(b);
      return;
    }
  }
  b->frames += (uint32_t)frames;
  if (b->frames >= b->max_frames ||
      (b->stop && b->frames >= (uint32_t)kBootAudioMinSeconds * (uint32_t)b->rate))
    BootAudioFinish(b);
}

void Dkc1BootAudioGameplay(Dkc1BootAudio *b) {
  if (b->file) b->stop = true;
}

bool Dkc1BootAudioDone(const Dkc1BootAudio *b) { return b->done; }

static Dkc1BootAudio s_boot_audio;
static bool s_boot_audio_checked;

static void BootAudioClose(void) { BootAudioFinish(&s_boot_audio); }

void Dkc1TelemetryHostAudio(const int16_t *samples, int frames, int channels,
                            int rate) {
  if (!s_boot_audio_checked) {
    s_boot_audio_checked = true;
    const char *path = getenv("DKCOUNTRY_BOOT_AUDIO");
    if (path && path[0]) {
      const char *limit = getenv("DKCOUNTRY_BOOT_AUDIO_SECONDS");
      const double seconds = limit && limit[0] ? atof(limit) : 70.0;
      if (Dkc1BootAudioOpen(&s_boot_audio, path, channels, rate, seconds))
        atexit(BootAudioClose);
      else
        fprintf(stderr, "boot audio: cannot write %s; continuing without it\n", path);
    }
  }
  Dkc1BootAudioWrite(&s_boot_audio, samples, frames);
}

static void FileSink(void *context, const char *line) {
  FILE *file = (FILE *)context;
  fputs(line, file);
  fputc('\n', file);
}

static void HostClose(void) {
  if (!s_file) return;
  Dkc1TelemetryBye(&s_host, s_last_frame);
  fclose(s_file);
  s_file = NULL;
}

static bool HostReady(void) {
  if (s_checked) return s_file != NULL;
  s_checked = true;
  const char *path = getenv("DKCOUNTRY_TELEMETRY");
  if (!path || !path[0]) return false;
  s_file = fopen(path, "ab");
  if (!s_file) {
    fprintf(stderr, "telemetry: cannot open %s; continuing without it\n", path);
    return false;
  }
  Dkc1TelemetryInit(&s_host, FileSink, s_file);
  atexit(HostClose);
  return true;
}

void Dkc1TelemetryHostInput(uint32_t input) {
  s_pending_input |= input;
}

void Dkc1TelemetryHostFrame(const uint8_t *wram, uint64_t frame) {
  /* The boot sound capture has no gameplay signal in this port: the
   * launcher's time limit ends it. */
  if (!wram || !HostReady()) return;
  if (!s_host.said_hello) Dkc1TelemetryHello(&s_host, DKC1_TELEMETRY_BUILD, frame);
  Dkc1TelemetryInput(&s_host, s_pending_input, frame);
  s_pending_input = 0;
  Dkc1TelemetrySnapshot snapshot;
  Dkc1TelemetryReadSnapshot(wram, &snapshot);
  Dkc1TelemetryStep(&s_host, &snapshot, frame);
  s_last_frame = frame;
  if (s_host.frames_since_tick == 0) fflush(s_file);
}

void Dkc1TelemetryHostStateLoaded(void) {
  if (s_file) Dkc1TelemetryStateLoaded(&s_host, s_last_frame);
}
