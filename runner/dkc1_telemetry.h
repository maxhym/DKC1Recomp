#ifndef DKC1_TELEMETRY_H
#define DKC1_TELEMETRY_H

/* Optional, read-only session telemetry for the DKCountry launcher.
 *
 * When DKCOUNTRY_TELEMETRY names a file, the host appends one JSON object per
 * line describing the session (protocol version 1, specified in the
 * DKCountry repository's docs/TELEMETRY_PROTOCOL.md). Without that variable
 * nothing is opened or written. The bridge only reads WRAM after a completed
 * frame and never writes emulated state, so it cannot change cartridge
 * behaviour, timing, saves or widescreen decisions.
 *
 * DKC1 has no known attract-demo flag, so gameplay events are only counted
 * while the host has seen real controller input recently. An unattended
 * title-screen demo therefore produces ticks but no bananas, deaths or
 * levels. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  kDkc1TelemetryProtocol = 1,
  kDkc1TelemetryFramesPerTick = 60,
  /* Gameplay events count for 30 seconds after the last real input. */
  kDkc1TelemetryInputWindow = 30 * 60,
};

/* WRAM fields, named in runner/dkc1_wram_gen.h. */
typedef struct Dkc1TelemetrySnapshot {
  uint16_t entrance; /* $003E Global_EntranceIDLo */
  uint16_t lives;    /* $0575 Player_CurrentLifeCountLo */
  uint16_t bananas;  /* $057B Player_CurrentBananaCountLo (binary) */
  uint16_t letters;  /* $057F Player_CollectedKONGLettersLo */
  uint16_t kong;     /* $056F Player_CurrentKongLo */
  uint16_t camera_x; /* $088B Global_Layer1XPosLo */
} Dkc1TelemetrySnapshot;

typedef void (*Dkc1TelemetrySink)(void *context, const char *line);

typedef struct Dkc1Telemetry {
  Dkc1TelemetrySink sink;
  void *context;
  Dkc1TelemetrySnapshot previous;
  bool have_previous;
  bool said_hello;
  bool seen_input;
  uint64_t last_input_frame;
  uint32_t frames_since_tick;
  uint32_t move_px;
} Dkc1Telemetry;

void Dkc1TelemetryReadSnapshot(const uint8_t *wram, Dkc1TelemetrySnapshot *out);

void Dkc1TelemetryInit(Dkc1Telemetry *t, Dkc1TelemetrySink sink, void *context);
void Dkc1TelemetryHello(Dkc1Telemetry *t, const char *build, uint64_t frame);
/* Reports the host controller word applied to the next frame. */
void Dkc1TelemetryInput(Dkc1Telemetry *t, uint32_t input, uint64_t frame);
bool Dkc1TelemetryPlayerActive(const Dkc1Telemetry *t, uint64_t frame);
void Dkc1TelemetryStep(Dkc1Telemetry *t, const Dkc1TelemetrySnapshot *now, uint64_t frame);
void Dkc1TelemetryStateLoaded(Dkc1Telemetry *t, uint64_t frame);
void Dkc1TelemetryBye(Dkc1Telemetry *t, uint64_t frame);

/* Process-wide file-backed instance used by the hosts. All calls are no-ops
 * unless DKCOUNTRY_TELEMETRY was set when the first frame ran. */
void Dkc1TelemetryHostInput(uint32_t input);
void Dkc1TelemetryHostFrame(const uint8_t *wram, uint64_t frame);
void Dkc1TelemetryHostStateLoaded(void);

#ifdef __cplusplus
}
#endif

#endif
