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
