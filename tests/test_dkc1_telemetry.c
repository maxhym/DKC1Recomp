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

int main(void) {
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
