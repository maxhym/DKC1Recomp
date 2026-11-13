#ifndef DKC1_HD_H
#define DKC1_HD_H

/* High-resolution replacement for Donkey Kong (presentation only).
 *
 * An HD pack (`.dkc2hd`, the format DKC2Recomp's
 * scripts/build_hd_kong_pack.py writes) holds straight-alpha RGBA frames
 * grouped into pose slots. When a pack is loaded and a presenter draws
 * overlay sprites, Donkey's OAM run is captured out of the PPU composite,
 * his semantic animation ID picks a pose slot, the frame is placed on the
 * captured sprite's own bounds, pixels covered by a higher-priority layer are
 * cut out, and the host draws the result at window resolution. WRAM, VRAM,
 * OAM, collision and timing are never modified. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ppu Ppu;

/* A host-resolution sprite drawn by the presenter after the game image. */
typedef struct Dkc1OverlaySprite {
  const uint8_t *rgba;  /* straight-alpha RGBA8, width * height * 4 bytes */
  int width, height;
  /* Destination rectangle in presented-frame pixels (origin top-left). */
  float x, y, w, h;
  float brightness;     /* SNES master brightness, 0..1 */
  uint32_t serial;      /* changes whenever the pixels change */
} Dkc1OverlaySprite;

/* Pose slots of a pack (the same numbering as DKC2Recomp). */
enum {
  kDkc1HdIdle = 0,
  kDkc1HdWalk = 1,
  kDkc1HdRun = 2,
  kDkc1HdJump = 3,
  kDkc1HdFall = 4,
  kDkc1HdFidget = 5,
  kDkc1HdRoll = 6,
  kDkc1HdCarryIdle = 7,
  kDkc1HdCarryWalk = 8,
  kDkc1HdClimbIdle = 9,
  kDkc1HdClimbMove = 10,
  kDkc1HdSwim = 11,
  kDkc1HdHurt = 12,
  kDkc1HdRide = 13,
  kDkc1HdVictory = 14,
  kDkc1HdThrow = 15,
  kDkc1HdFidget2 = 16,
  kDkc1HdFidget3 = 17,
  kDkc1HdHang = 18,
  kDkc1HdClimbDown = 19,
  /* Objects held in front of the chest: DKC2's Kongs. Donkey carries
   * overhead, so these three are not used here. */
  kDkc1HdCarryFrontIdle = 20,
  kDkc1HdCarryFrontWalk = 21,
  kDkc1HdThrowFront = 22,
  kDkc1HdStandToFours = 23, /* upright to all fours; played backwards too */
  /* DKC2 only: hanging still, and spread between two ropes. */
  kDkc1HdHangIdle = 24,
  kDkc1HdClimbWideIdle = 25,
  kDkc1HdClimbWideMove = 26,
  kDkc1HdCrouch = 27,       /* ducked low: ducking and crawlspaces */
  kDkc1HdKnockout = 28,     /* lying knocked out: a lost life */
  kDkc1HdBasicAnimCount = 5,
  kDkc1HdAnimCount = 29,
};

/* Presentation styles, applied to each composited frame at run time. */
enum {
  kDkc1HdStyleHd = 0,
  kDkc1HdStyleSoft = 1,
  kDkc1HdStyleRetro = 2,
  kDkc1HdStyleCrt = 3,
  kDkc1HdStyleCount = 4,
};
void Dkc1HdSetStyle(int style);
int Dkc1HdStyle(void);
int Dkc1HdStyleFromName(const char *name);
void Dkc1HdSetSize(float size);
float Dkc1HdSize(void);

enum { kDkc1HdFlagFacesLeft = 1 };

bool Dkc1HdLoad(const char *path);
bool Dkc1HdLoadBytes(const uint8_t *bytes, size_t size);
void Dkc1HdUnload(void);
bool Dkc1HdReady(void);
const char *Dkc1HdStatus(void);
/* Called by a host whose presenter draws Dkc1HdSprites(). Loads
 * DKC1_HD_PACK, else default_pack, else mods/kong-hd.dkc2hd in the working
 * directory; DKC1_HD=0 disables HD. Tuning:
 * DKC1_HD_OFFSET="dx,dy" (native px), DKC1_HD_SCALE (HD px per native px),
 * DKC1_HD_FLIP=1, DKC1_HD_STYLE=hd|doux|retro|crt, DKC1_HD_SIZE=percent
 * (50-200), DKC1_HD_TRACE=1 (log pose changes with the native IDs). */
void Dkc1HdInitialize(const char *default_pack);
void Dkc1HdSetEnabled(bool enabled);
bool Dkc1HdToggle(void);
bool Dkc1HdEnabled(void);

/* How a native animation is shown. */
typedef struct Dkc1HdPose {
  int slot;     /* kDkc1Hd* */
  bool center;  /* align on the native sprite's centre instead of its feet */
  bool idle;    /* a resting pose that may be relieved by a fidget */
} Dkc1HdPose;
/* Donkey's semantic animation ID is the oracle; state and velocity only
 * cover the frames that briefly expose ID zero. A negative Y velocity is
 * upward. */
Dkc1HdPose Dkc1HdPoseForAnimation(unsigned animation_id, unsigned state,
                                  int x_velocity, int y_velocity);

/* Which of Donkey's animation IDs belong to the rider on an animal buddy,
 * and which to the animal his own actor slot then shows. */
bool Dkc1HdIsRiderAnimation(unsigned animation);
bool Dkc1HdIsAnimalAnimation(unsigned animation);

typedef struct Dkc1HdActorInput {
  int anchor_x;         /* actor centre line in authentic screen px */
  uint8_t properties;   /* OAM attribute byte: flip and priority */
  unsigned animation;   /* native animation ID */
  unsigned state;
  int x_velocity, y_velocity;
} Dkc1HdActorInput;

/* Per-frame protocol, driven by dkc1_game.c. PrepareFrame finds Donkey and
 * captures his OAM run; ObserveLine runs after each PPU line; FinishFrame
 * places and composites the pose. */
void Dkc1HdPrepareFrame(Ppu *ppu, const uint8_t *wram, int presentation_bias);
void Dkc1HdObserveLine(const Ppu *ppu, int line);
void Dkc1HdFinishFrame(Ppu *ppu);
int Dkc1HdSprites(const Dkc1OverlaySprite **sprites);

/* Engine steps behind that protocol, exposed for tests. */
void Dkc1HdBeginFrame(void);
void Dkc1HdSetActor(const Dkc1HdActorInput *input);
/* native_top/native_bottom: first and last output rows of the captured
 * native sprite. */
void Dkc1HdCompose(const Ppu *ppu, int native_top, int native_bottom);
int Dkc1HdActorSlot(void);

#ifdef __cplusplus
}
#endif

#endif
