/* Synthetic checks of the DKCountry telemetry bridge. No ROM is used. */
#include "dkc1_telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

typedef struct {
  char text[16384];
} Capture;

static void Sink(void *context, const char *line) {
  Capture *c = (Capture *)context;
  strncat(c->text, line, sizeof c->text - strlen(c->text) - 2);
  strncat(c->text, "\n", sizeof c->text - strlen(c->text) - 1);
}

static int Count(const Capture *c, const char *needle) {
  int n = 0;
  for (const char *p = c->text; (p = strstr(p, needle)) != NULL; ++p) ++n;
  return n;
}

static void TestSnapshot(void) {
  static uint8_t wram[0x20000];
  memset(wram, 0, sizeof wram);
  wram[0x003E] = 0x16;
  wram[0x0575] = 5;
  wram[0x057B] = 42;
  wram[0x057F] = 0x0B;
  wram[0x056F] = 1;
  wram[0x088B] = 0x34; wram[0x088C] = 0x12;
  Dkc1TelemetrySnapshot s;
  Dkc1TelemetryReadSnapshot(wram, &s);
  CHECK(s.entrance == 0x16);
  CHECK(s.lives == 5);
  CHECK(s.bananas == 42);
  CHECK(s.letters == 0x0B);
  CHECK(s.kong == 1);
  CHECK(s.camera_x == 0x1234);
}

static void TestEvents(void) {
  Capture c;
  memset(&c, 0, sizeof c);
  Dkc1Telemetry t;
  Dkc1TelemetryInit(&t, Sink, &c);
  Dkc1TelemetryHello(&t, "0.0.15", 0);
  CHECK(strstr(c.text, "\"game\":\"dkc1\",\"build\":\"0.0.15\"") != NULL);

  Dkc1TelemetrySnapshot s = {.entrance = 3, .lives = 5, .bananas = 95, .camera_x = 100};
  uint64_t f = 1;
  Dkc1TelemetryInput(&t, 0x80, f);
  Dkc1TelemetryStep(&t, &s, f++);              /* baseline */
  s.bananas = 96;
  s.camera_x = 110;
  Dkc1TelemetryStep(&t, &s, f++);
  s.bananas = 2;                               /* bunch across 100: +6 */
  s.lives = 6;                                 /* the 100-banana life */
  s.camera_x = 130;
  Dkc1TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"bananas\"") == 2);
  CHECK(strstr(c.text, "\"n\":6,\"level\":3") != NULL);
  CHECK(Count(&c, "\"e\":\"lives\",") == 1);
  CHECK(Count(&c, "\"e\":\"death\"") == 0);

  s.lives = 5;                                 /* lost a life */
  Dkc1TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"death\"") == 1);
  CHECK(strstr(c.text, "\"x\":130,\"cause\":\"life_lost\"") != NULL);

  s.letters = 1;
  Dkc1TelemetryStep(&t, &s, f++);
  s.letters = 3;
  Dkc1TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"letters\"") == 2);

  s.entrance = 4;
  s.camera_x = 4000;                           /* room change: not walked */
  Dkc1TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"level\"") == 1);
  s.bananas = 50;                              /* jump: ignored */
  Dkc1TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"bananas\"") == 2);

  while (f <= 60) Dkc1TelemetryStep(&t, &s, f++);
  CHECK(strstr(c.text, "\"e\":\"tick\",\"f\":60,\"demo\":false,\"level\":4") != NULL);
  CHECK(strstr(c.text, "\"e\":\"move\",\"f\":60,\"dx\":30}") != NULL);

  Dkc1TelemetryStateLoaded(&t, f);
  s.bananas = 51;
  Dkc1TelemetryStep(&t, &s, f++);
  CHECK(Count(&c, "\"e\":\"bananas\"") == 2);
  Dkc1TelemetryBye(&t, f);
  CHECK(Count(&c, "\"e\":\"bye\"") == 1);
}

static void TestUnattendedDemoIsSilent(void) {
  Capture c;
  memset(&c, 0, sizeof c);
  Dkc1Telemetry t;
  Dkc1TelemetryInit(&t, Sink, &c);
  Dkc1TelemetrySnapshot s = {.entrance = 1, .lives = 5};
  for (uint64_t f = 1; f <= 240; ++f) {
    s.bananas = (uint16_t)(f % 50);
    s.entrance = (uint16_t)(1 + f / 100);
    s.lives = (uint16_t)(f % 90 ? 5 : 4);
    Dkc1TelemetryStep(&t, &s, f);
  }
  CHECK(Count(&c, "\"e\":\"bananas\"") == 0);
  CHECK(Count(&c, "\"e\":\"death\"") == 0);
  CHECK(Count(&c, "\"e\":\"level\"") == 0);
  CHECK(Count(&c, "\"demo\":true") == 4);

  /* Input opens the window; it closes again after 30 idle seconds. */
  Dkc1TelemetryInput(&t, 0x10, 300);
  CHECK(Dkc1TelemetryPlayerActive(&t, 300));
  CHECK(Dkc1TelemetryPlayerActive(&t, 300 + kDkc1TelemetryInputWindow));
  CHECK(!Dkc1TelemetryPlayerActive(&t, 301 + kDkc1TelemetryInputWindow));
  Dkc1TelemetryInput(&t, 0, 5000);
  CHECK(!Dkc1TelemetryPlayerActive(&t, 5000));
}

static void TestHostInertWithoutVariable(void) {
  static uint8_t wram[0x20000];
  Dkc1TelemetryHostInput(1);
  Dkc1TelemetryHostFrame(wram, 1);
  Dkc1TelemetryHostStateLoaded();
}

static uint32_t Le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t ReadAll(const char *path, uint8_t *out, size_t capacity) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  const size_t n = fread(out, 1, capacity, f);
  fclose(f);
  return n;
}

static void TestBootAudio(void) {
  static uint8_t bytes[1 << 20];
  static int16_t block[2 * 8000];
  const char *path = "dkc1_boot_audio_test.wav";
  enum { kRate = 8000, kHalf = kRate / 2 };
  Dkc1BootAudio b;
  CHECK(!Dkc1BootAudioOpen(&b, "", 2, kRate, 5));
  CHECK(!Dkc1BootAudioOpen(&b, path, 3, kRate, 5));
  Dkc1BootAudioWrite(&b, NULL, 4);         /* closed: harmless */

  /* 20 s limit. A second of silence, then sound: the silence is dropped. */
  CHECK(Dkc1BootAudioOpen(&b, path, 2, kRate, 20));
  Dkc1BootAudioWrite(&b, block, kRate);
  CHECK(b.frames == 0 && !b.heard);
  for (int i = 0; i < kRate; ++i)
    block[2 * i] = block[2 * i + 1] = (int16_t)(i < kHalf ? 0 : 1000 + i);
  Dkc1BootAudioWrite(&b, block, kRate);
  CHECK(b.heard && b.frames == kHalf);
  /* Until the capture ends the header says "no data yet". */
  fflush(b.file);
  size_t n = ReadAll(path, bytes, sizeof bytes);
  CHECK(n == 44 + kHalf * 4 && memcmp(bytes, "RIFF", 4) == 0 && Le32(bytes + 40) == 0);
  CHECK((int16_t)(bytes[44] | bytes[45] << 8) == 1000 + kHalf);

  /* Gameplay before ten seconds of sound does not cut the capture short;
   * past ten seconds it ends it. */
  Dkc1BootAudioGameplay(&b);
  for (int i = 0; i < 9; ++i) Dkc1BootAudioWrite(&b, block, kRate);
  CHECK(!Dkc1BootAudioDone(&b) && b.frames == 9u * kRate + kHalf);
  Dkc1BootAudioWrite(&b, block, kRate);
  CHECK(Dkc1BootAudioDone(&b) && b.file == NULL);
  const uint32_t data = (10u * kRate + kHalf) * 4u;
  n = ReadAll(path, bytes, sizeof bytes);
  CHECK(n == 44 + data);
  CHECK(Le32(bytes + 40) == data && Le32(bytes + 4) == data + 36);
  CHECK(Le32(bytes + 24) == kRate && bytes[22] == 2 && bytes[34] == 16);
  Dkc1BootAudioWrite(&b, block, kRate);    /* finished: ignored */

  /* Without gameplay the limit ends it, exactly. */
  CHECK(Dkc1BootAudioOpen(&b, path, 1, kRate, 1.5));
  for (int i = 0; i < kRate; ++i) block[i] = 2000;
  Dkc1BootAudioWrite(&b, block, kRate);
  Dkc1BootAudioWrite(&b, block, kRate);
  CHECK(Dkc1BootAudioDone(&b));
  n = ReadAll(path, bytes, sizeof bytes);
  CHECK(n == 44 + 3u * kHalf * 2 && Le32(bytes + 40) == 3u * kHalf * 2 && bytes[22] == 1);
  remove(path);
}

int main(void) {
  TestBootAudio();
  TestSnapshot();
  TestEvents();
  TestUnattendedDemoIsSilent();
  if (!getenv("DKCOUNTRY_TELEMETRY")) TestHostInertWithoutVariable();
  if (failures) {
    fprintf(stderr, "%d telemetry check(s) failed\n", failures);
    return 1;
  }
  puts("dkc1 telemetry tests passed");
  return 0;
}
