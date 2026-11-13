/* Synthetic checks for the HD Donkey Kong layer: pack loading, the pose map,
 * placement on the native sprite, occlusion, fidgets and the cross-fade. */
#include "dkc1_hd.h"
#include "snes/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) \
  do { \
    if (!(condition)) { \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      ++failures; \
    } \
  } while (0)

/* The engine only needs these when it finds Donkey in real WRAM. */
bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source, uint8_t *pixels, size_t pitch) {
  (void)ppu; (void)source; (void)pixels; (void)pitch;
  return false;
}
bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source, int x, int y, int width,
                          int height, uint8_t flags) {
  (void)ppu; (void)source; (void)x; (void)y; (void)width; (void)height; (void)flags;
  return false;
}
bool PpuSetOverlayOamRange(Ppu *ppu, uint8_t first, uint8_t count) {
  (void)ppu; (void)first; (void)count;
  return false;
}

static void Put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t *p, uint32_t v) { Put16(p, v & 0xffff); Put16(p + 2, v >> 16); }

/* Two 8x8 frames at 4 HD pixels per native pixel, anchored bottom centre:
 * frame 0 opaque red, frame 1 blue with a transparent left half.
 * idle = [0], walk = [1], swim = [1], fidget = [1] for 10 ticks, once. */
static size_t BuildPack(uint8_t *out) {
  const unsigned frames = 2, anims = kDkc1HdAnimCount, steps = 4;
  memset(out, 0, 4096);
  memcpy(out, "DKC2HD02", 8);
  Put16(out + 10, 4 * 256);
  Put16(out + 16, frames);
  Put16(out + 18, steps);
  Put16(out + 22, anims);
  const unsigned slot_step[][3] = {{kDkc1HdIdle, 0, 1}, {kDkc1HdWalk, 1, 1},
                                   {kDkc1HdSwim, 2, 1}, {kDkc1HdFidget, 3, 0}};
  for (unsigned i = 0; i < 4; ++i) {
    uint8_t *r = out + 32 + slot_step[i][0] * 8;
    Put16(r, slot_step[i][1]);
    Put16(r + 2, 1);
    Put16(r + 4, slot_step[i][2]);
  }
  size_t c = 32 + anims * 8;
  for (unsigned i = 0; i < steps; ++i, c += 4) {
    Put16(out + c, i ? 1 : 0);
    Put16(out + c + 2, i == 3 ? 10 : 4);
  }
  const size_t table = c;
  size_t pixels = table + frames * 12;
  for (unsigned f = 0; f < frames; ++f) {
    uint8_t *e = out + table + f * 12;
    Put16(e, 8); Put16(e + 2, 8); Put16(e + 4, 4); Put16(e + 6, 8);
    Put32(e + 8, (uint32_t)pixels);
    for (unsigned i = 0; i < 64; ++i) {
      uint8_t *p = out + pixels + i * 4;
      p[0] = f ? 0 : 255;
      p[2] = f ? 255 : 0;
      p[3] = (f && (i % 8) < 4) ? 0 : 255;
    }
    pixels += 64 * 4;
  }
  return pixels;
}

static Ppu *NewPpu(unsigned width) {
  Ppu *ppu = (Ppu *)calloc(1, sizeof(Ppu));
  ppu->inidisp = 0x0f;
  ppu->renderPitch = width * 4;
  for (unsigned i = 0; i < kPpuBufWidth; ++i) ppu->bgBuffers[0].data[i] = 0x7100;
  return ppu;
}

static Dkc1HdActorInput Actor(unsigned animation) {
  Dkc1HdActorInput in;
  memset(&in, 0, sizeof in);
  in.anchor_x = 100;
  in.properties = 2u << 4; /* OBJ priority 2: level 10 */
  in.animation = animation;
  return in;
}

static const Dkc1OverlaySprite *Render(Ppu *ppu, const Dkc1HdActorInput *in, int top, int bottom) {
  Dkc1HdBeginFrame();
  Dkc1HdSetActor(in);
  for (int line = 0; line <= 224; ++line) Dkc1HdObserveLine(ppu, line);
  Dkc1HdCompose(ppu, top, bottom);
  const Dkc1OverlaySprite *sprites = NULL;
  return Dkc1HdSprites(&sprites) == 1 ? sprites : NULL;
}

static void TestPoses(void) {
  Dkc1HdPose p = Dkc1HdPoseForAnimation(0x03, 0, 0, 0);
  CHECK(p.slot == kDkc1HdWalk && !p.center && !p.idle);
  p = Dkc1HdPoseForAnimation(0x01, 0, 0, -0x300);
  CHECK(p.slot == kDkc1HdIdle && p.idle);
  CHECK(Dkc1HdPoseForAnimation(0x18, 0, 0, 0).slot == kDkc1HdRoll);
  p = Dkc1HdPoseForAnimation(0x0f, 0, 0, 0);
  CHECK(p.slot == kDkc1HdSwim && p.center);
  p = Dkc1HdPoseForAnimation(0x05, 1, 0, -0x400);     /* upward */
  CHECK(p.slot == kDkc1HdJump && p.center);
  CHECK(Dkc1HdPoseForAnimation(0x05, 1, 0, 0x200).slot == kDkc1HdFall);
  CHECK(Dkc1HdPoseForAnimation(0x1e, 0, 0, 0).slot == kDkc1HdRide);
  CHECK(Dkc1HdPoseForAnimation(0x49, 0, 0, 0).slot == kDkc1HdCarryWalk);
  CHECK(Dkc1HdPoseForAnimation(0x4a, 0, 0, 0).slot == kDkc1HdThrow);
  CHECK(Dkc1HdPoseForAnimation(0x5e, 0, 0, 0).slot == kDkc1HdClimbIdle);
  CHECK(Dkc1HdPoseForAnimation(0x09, 0, 0, 0).slot == kDkc1HdVictory);
  CHECK(Dkc1HdPoseForAnimation(0x0c, 0, 0, 0).slot == kDkc1HdHurt);
  /* Animation ID zero falls back to the state, then the velocity. */
  CHECK(Dkc1HdPoseForAnimation(0, 43, 0, 0).slot == kDkc1HdSwim);
  CHECK(Dkc1HdPoseForAnimation(0, 1, 0, -1).slot == kDkc1HdJump);
  CHECK(Dkc1HdPoseForAnimation(0, 0, 0x200, 0).slot == kDkc1HdRun);
  CHECK(Dkc1HdPoseForAnimation(0, 0, -0x40, 0).slot == kDkc1HdWalk);
  CHECK(Dkc1HdPoseForAnimation(0x7f, 0, 0, 0).idle);
}

static void TestPlacementAndOcclusion(void) {
  static uint8_t pack[4096];
  CHECK(!Dkc1HdReady());
  CHECK(!Dkc1HdLoadBytes(pack, 40));
  CHECK(Dkc1HdLoadBytes(pack, BuildPack(pack)));
  CHECK(strstr(Dkc1HdStatus(), "4 animations") != NULL);
  Ppu *ppu = NewPpu(256);
  /* A foreground tree covers screen column 100 on every line. */
  ppu->bgBuffers[0].data[100 + kPpuExtraLeftRight] = 0xc000;
  Dkc1HdActorInput in = Actor(0x04);
  /* Feet: the anchor row sits under the native sprite's last row (99). */
  const Dkc1OverlaySprite *s = Render(ppu, &in, 60, 99);
  CHECK(s != NULL);
  if (!s) return;
  CHECK(s->width == 8 && s->height == 8);
  CHECK(s->w == 2.0f && s->h == 2.0f);
  CHECK(s->x == 99.0f && s->y == 98.0f);
  CHECK(s->brightness == 1.0f);
  /* HD columns 0-3 are native column 99, 4-7 are column 100 (covered). */
  CHECK(s->rgba[(2 * 8 + 1) * 4 + 3] == 255 && s->rgba[(2 * 8 + 1) * 4 + 0] == 255);
  CHECK(s->rgba[(2 * 8 + 6) * 4 + 3] == 0);
  /* A higher OBJ priority stands in front of the tree. */
  in.properties = 3u << 4;
  s = Render(ppu, &in, 60, 99);
  CHECK(s && s->rgba[(2 * 8 + 6) * 4 + 3] == 255);
  /* Facing left mirrors around the anchor. */
  in.properties = (2u << 4) | 0x40;
  s = Render(ppu, &in, 60, 99);
  CHECK(s && s->x == 99.0f);
  /* Master brightness dims; forced blank hides the sprite. */
  ppu->inidisp = 0x05;
  s = Render(ppu, &in, 60, 99);
  CHECK(s && s->brightness > 0.33f && s->brightness < 0.34f);
  ppu->inidisp = 0x8f;
  CHECK(Render(ppu, &in, 60, 99) == NULL);
  free(ppu);

  /* Centre: the pose's middle sits on the native sprite's middle, and a
   * widescreen frame shifts the sprite by its margin. */
  Dkc1HdBeginFrame();
  Dkc1HdBeginFrame();
  ppu = NewPpu(256 + 2 * 40);
  in = Actor(0x0f);
  s = Render(ppu, &in, 90, 109);
  CHECK(s && s->y == 99.0f && s->x == 139.0f);
  CHECK(Dkc1HdActorSlot() == kDkc1HdSwim);
  free(ppu);
}

static void TestFidgetAndFade(void) {
  static uint8_t pack[4096];
  CHECK(Dkc1HdLoadBytes(pack, BuildPack(pack)));
  Ppu *ppu = NewPpu(256);
  Dkc1HdActorInput in = Actor(0x01);
  Dkc1HdBeginFrame();
  Dkc1HdBeginFrame();
  int fidget = 0, after = -1;
  for (unsigned i = 0; i < 340 && after < 0; ++i) {
    Dkc1HdBeginFrame();
    Dkc1HdSetActor(&in);
    const int slot = Dkc1HdActorSlot();
    if (slot == kDkc1HdFidget) ++fidget;
    else if (fidget) after = slot;
  }
  CHECK(fidget == 10);
  CHECK(after == kDkc1HdIdle);
  /* Idle (red) to walk (blue, left half clear): the old frame fades out. */
  const Dkc1OverlaySprite *s = NULL;
  for (unsigned i = 0; i < 8; ++i) s = Render(ppu, &in, 60, 99); /* past the fidget's fade */
  CHECK(s && s->rgba[0] == 255 && s->rgba[3] == 255);
  in.animation = 0x03;
  s = Render(ppu, &in, 60, 99);
  CHECK(s && s->rgba[0] == 255 && s->rgba[3] > 200 && s->rgba[3] < 255);
  CHECK(s && s->rgba[6 * 4 + 3] == 255 && s->rgba[6 * 4 + 2] > 20);
  for (unsigned i = 0; i < 5; ++i) s = Render(ppu, &in, 60, 99);
  CHECK(s && s->rgba[3] == 0 && s->rgba[6 * 4 + 2] == 255 && s->rgba[6 * 4 + 0] == 0);
  /* Disabled or unloaded: nothing is drawn. */
  Dkc1HdSetEnabled(false);
  CHECK(Render(ppu, &in, 60, 99) == NULL);
  Dkc1HdSetEnabled(true);
  Dkc1HdUnload();
  CHECK(Render(ppu, &in, 60, 99) == NULL);
  free(ppu);
}

int main(int argc, char **argv) {
  TestPoses();
  TestPlacementAndOcclusion();
  TestFidgetAndFade();
  if (argc > 1) {
    CHECK(Dkc1HdLoad(argv[1]));
    fprintf(stdout, "%s\n", Dkc1HdStatus());
  }
  if (failures) {
    fprintf(stderr, "%d HD check(s) failed\n", failures);
    return 1;
  }
  puts("dkc1 hd tests passed");
  return 0;
}
