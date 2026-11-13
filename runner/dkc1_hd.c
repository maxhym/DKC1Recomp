#include "dkc1_hd.h"

#include "dkc1_wram_gen.h"
#include "snes/ppu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kHeaderSize = 64,
  kMaxFrames = 1024,
  kMaxSteps = 4096,
  kMaxNativeOnly = 512,
  kMaxMap = 512,
  kFidgetDelay = 300, /* ticks standing still before a fidget */
  kFadeTicks = 9,     /* cross-fade between two poses, about 150 ms */
  kMaxFrameSide = 1024,
  kMaskRows = 240,
  kCaptureHalfWidth = 80,
};

typedef struct HdFrame {
  uint16_t width, height;
  int16_t anchor_x, anchor_y;
  const uint8_t *rgba;
} HdFrame;
typedef struct HdStep { uint16_t frame, duration; } HdStep;
typedef struct HdAnimation { uint16_t first, count, loop; } HdAnimation;

typedef struct HdPack {
  uint8_t *data;
  size_t size;
  uint16_t flags;
  float scale, offset_x, offset_y;
  HdFrame *frames;
  size_t frame_count;
  HdStep *steps;
  size_t step_count;
  HdAnimation animations[kDkc1HdAnimCount];
  uint16_t *native_only;
  size_t native_only_count;
  uint16_t (*map)[2]; /* native animation ID -> slot */
  size_t map_count;
} HdPack;

/* What is shown, beyond the pose the native animation asks for. */
typedef struct HdShow {
  int animation;
  uint32_t since, idle_since;
  unsigned native;
  unsigned fidget_turn; /* next fidget slot to try */
  /* Cross-fade: the frame shown last tick, and the one being faded out. */
  const struct HdFrame *last_frame, *fade_from;
  uint32_t fade_start;
  /* Upright to all fours (+1) or back (-1), while its clip plays. */
  int turn;
  uint32_t turn_start;
  bool valid;
} HdShow;
static HdShow s_show;

typedef struct HdActor {
  bool active;
  Dkc1HdActorInput input;
  bool center;
  const HdFrame *frame;
  const HdFrame *fade_frame; /* previous pose while cross-fading, or NULL */
  float fade;                /* weight of `frame`, 0..1 */
  bool flip;
  unsigned level;
  int rx, ry, rw, rh;        /* drawn rectangle in HD pixels, origin-relative */
  float left, top, width, height;
} HdActor;

static HdPack s_pack;
static bool s_enabled = true;
static char s_status[256] = "No HD character pack loaded.";
static HdActor s_actor;
/* Rows of the main screen where a layer above Donkey's priority is drawn. */
static uint8_t s_mask[kMaskRows][kPpuBufWidth];
static uint8_t s_capture[kPpuBufWidth * kMaskRows * 4];
static bool s_capturing;
static uint8_t *s_out;
static size_t s_out_capacity;
static uint8_t *s_tmp;
static size_t s_tmp_capacity;
/* Presentation style and size, chosen by the player (launcher). */
static int s_style = kDkc1HdStyleHd;
static float s_size = 1.0f;

static float EffectiveScale(void) { return s_pack.scale / s_size; }
static Dkc1OverlaySprite s_sprite;
static int s_sprite_count;
static uint32_t s_serial;
static uint32_t s_tick;
static bool s_trace;

static uint16_t Read16(const uint8_t *p) { return (uint16_t)(p[0] | (unsigned)p[1] << 8); }
static uint32_t Read32(const uint8_t *p) { return Read16(p) | (uint32_t)Read16(p + 2) << 16; }

static void FreePack(HdPack *pack) {
  free(pack->data);
  free(pack->frames);
  free(pack->steps);
  free(pack->native_only);
  free(pack->map);
  memset(pack, 0, sizeof *pack);
}


void Dkc1HdUnload(void) {
  FreePack(&s_pack);
  memset(&s_actor, 0, sizeof s_actor);
  memset(&s_show, 0, sizeof s_show);
  s_sprite_count = 0;
  (void)snprintf(s_status, sizeof s_status, "No HD character pack loaded.");
}

bool Dkc1HdReady(void) { return s_pack.frame_count != 0; }
const char *Dkc1HdStatus(void) { return s_status; }
void Dkc1HdSetEnabled(bool enabled) { s_enabled = enabled; }
bool Dkc1HdToggle(void) {
  s_enabled = !s_enabled;
  return Dkc1HdEnabled();
}
bool Dkc1HdEnabled(void) { return s_enabled && Dkc1HdReady(); }

static bool Invalid(const char *why) {
  (void)snprintf(s_status, sizeof s_status, "HD pack rejected: %s", why);
  return false;
}

bool Dkc1HdLoadBytes(const uint8_t *bytes, size_t size) {
  const bool v2 = bytes && size >= 32 && memcmp(bytes, "DKC2HD02", 8) == 0;
  if (!bytes || size < 32 || size > 256u * 1024u * 1024u ||
      (!v2 && (size < kHeaderSize || memcmp(bytes, "DKC2HD01", 8) != 0)))
    return Invalid("not a DKC2HD01/02 file");
  /* v1: five animation records at 24, tables at 64. v2: animation and map
   * counts at 22/24, records at 32, then steps, native-only list, map. */
  const unsigned anim_count = v2 ? Read16(bytes + 22) : kDkc1HdBasicAnimCount;
  const size_t map_count = v2 ? Read16(bytes + 24) : 0;
  const size_t records = v2 ? 32 : 24;
  if (!anim_count || anim_count > kDkc1HdAnimCount || map_count > kMaxMap ||
      records + (size_t)anim_count * 8 > size)
    return Invalid("bad animation table");
  HdPack pack;
  memset(&pack, 0, sizeof pack);
  pack.flags = Read16(bytes + 8);
  const unsigned scale_q8 = Read16(bytes + 10);
  pack.offset_x = (float)(int16_t)Read16(bytes + 12) / 16.0f;
  pack.offset_y = (float)(int16_t)Read16(bytes + 14) / 16.0f;
  pack.frame_count = Read16(bytes + 16);
  pack.step_count = Read16(bytes + 18);
  pack.native_only_count = Read16(bytes + 20);
  if (scale_q8 < 64 || scale_q8 > 16 * 256) return Invalid("scale out of range");
  pack.scale = (float)scale_q8 / 256.0f;
  if (!pack.frame_count || pack.frame_count > kMaxFrames || !pack.step_count ||
      pack.step_count > kMaxSteps || pack.native_only_count > kMaxNativeOnly)
    return Invalid("bad table sizes");
  for (unsigned a = 0; a < anim_count; ++a) {
    const uint8_t *p = bytes + records + a * 8;
    pack.animations[a].first = Read16(p);
    pack.animations[a].count = Read16(p + 2);
    pack.animations[a].loop = Read16(p + 4);
    if (pack.animations[a].count &&
        (size_t)pack.animations[a].first + pack.animations[a].count > pack.step_count)
      return Invalid("animation outside the step table");
  }
  if (!pack.animations[kDkc1HdIdle].count) return Invalid("the idle animation is required");
  size_t cursor = v2 ? records + (size_t)anim_count * 8 : kHeaderSize;
  pack.map_count = map_count;
  const size_t tables = pack.step_count * 4 + pack.native_only_count * 2 + map_count * 4 +
                        pack.frame_count * 12;
  if (tables > size - cursor) return Invalid("truncated tables");
  pack.steps = (HdStep *)calloc(pack.step_count, sizeof *pack.steps);
  pack.native_only = (uint16_t *)calloc(pack.native_only_count ? pack.native_only_count : 1,
                                        sizeof *pack.native_only);
  pack.frames = (HdFrame *)calloc(pack.frame_count, sizeof *pack.frames);
  pack.map = (uint16_t (*)[2])calloc(map_count ? map_count : 1, sizeof *pack.map);
  pack.data = (uint8_t *)malloc(size);
  if (!pack.steps || !pack.native_only || !pack.frames || !pack.map || !pack.data) {
    FreePack(&pack);
    return Invalid("out of memory");
  }
  memcpy(pack.data, bytes, size);
  pack.size = size;
  for (size_t i = 0; i < pack.step_count; ++i, cursor += 4) {
    pack.steps[i].frame = Read16(bytes + cursor);
    pack.steps[i].duration = Read16(bytes + cursor + 2);
    if (pack.steps[i].frame >= pack.frame_count || !pack.steps[i].duration) {
      FreePack(&pack);
      return Invalid("bad animation step");
    }
  }
  for (size_t i = 0; i < pack.native_only_count; ++i, cursor += 2)
    pack.native_only[i] = Read16(bytes + cursor);
  for (size_t i = 0; i < map_count; ++i, cursor += 4) {
    pack.map[i][0] = Read16(bytes + cursor);
    pack.map[i][1] = Read16(bytes + cursor + 2);
    if (pack.map[i][1] >= kDkc1HdAnimCount) {
      FreePack(&pack);
      return Invalid("map names an unknown animation slot");
    }
  }
  for (size_t i = 0; i < pack.frame_count; ++i, cursor += 12) {
    HdFrame *f = &pack.frames[i];
    f->width = Read16(bytes + cursor);
    f->height = Read16(bytes + cursor + 2);
    f->anchor_x = (int16_t)Read16(bytes + cursor + 4);
    f->anchor_y = (int16_t)Read16(bytes + cursor + 6);
    const uint32_t offset = Read32(bytes + cursor + 8);
    const size_t bytes_needed = (size_t)f->width * f->height * 4;
    if (!f->width || !f->height || f->width > kMaxFrameSide || f->height > kMaxFrameSide ||
        offset < cursor || offset > size || bytes_needed > size - offset) {
      FreePack(&pack);
      return Invalid("frame pixels outside the file");
    }
    f->rgba = pack.data + offset;
  }
  FreePack(&s_pack);
  s_pack = pack;
  memset(&s_actor, 0, sizeof s_actor);
  memset(&s_show, 0, sizeof s_show);
  unsigned slots = 0;
  for (unsigned a = 0; a < kDkc1HdAnimCount; ++a) slots += s_pack.animations[a].count != 0;
  (void)snprintf(s_status, sizeof s_status,
                 "HD pack loaded: %zu frames, %u animations, scale %.2f",
                 pack.frame_count, slots, pack.scale);
  return true;
}

bool Dkc1HdLoad(const char *path) {
  FILE *f = path ? fopen(path, "rb") : NULL;
  if (!f) {
    (void)snprintf(s_status, sizeof s_status, "HD pack not found: %s", path ? path : "(null)");
    return false;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return Invalid("unreadable file"); }
  const long length = ftell(f);
  if (length <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return Invalid("unreadable file"); }
  uint8_t *bytes = (uint8_t *)malloc((size_t)length);
  const bool read = bytes && fread(bytes, 1, (size_t)length, f) == (size_t)length;
  fclose(f);
  const bool ok = read && Dkc1HdLoadBytes(bytes, (size_t)length);
  free(bytes);
  if (!read) return Invalid("unreadable file");
  return ok;
}


void Dkc1HdInitialize(const char *default_pack) {
  const char *toggle = getenv("DKC1_HD");
  if (toggle && toggle[0] == '0') {
    s_enabled = false;
    return;
  }
  const char *path = getenv("DKC1_HD_PACK");
  if (!path || !*path) path = default_pack && *default_pack ? default_pack : "mods/kong-hd.dkc2hd";
  FILE *f = fopen(path, "rb");
  if (!f) return;
  fclose(f);
  if (!Dkc1HdLoad(path)) {
    fprintf(stderr, "%s\n", s_status);
    return;
  }
  fprintf(stderr, "%s\n", s_status);
  const char *offset = getenv("DKC1_HD_OFFSET");
  float dx, dy;
  if (offset && sscanf(offset, "%f,%f", &dx, &dy) == 2) {
    s_pack.offset_x += dx;
    s_pack.offset_y += dy;
  }
  const char *scale = getenv("DKC1_HD_SCALE");
  const float value = scale ? (float)atof(scale) : 0.0f;
  if (value >= 0.25f && value <= 16.0f) s_pack.scale = value;
  const char *style = getenv("DKC1_HD_STYLE");
  if (style && *style) Dkc1HdSetStyle(Dkc1HdStyleFromName(style));
  const char *size = getenv("DKC1_HD_SIZE");
  if (size && *size) Dkc1HdSetSize((float)atof(size) / 100.0f);
  const char *flip = getenv("DKC1_HD_FLIP");
  if (flip && flip[0] == '1') s_pack.flags ^= kDkc1HdFlagFacesLeft;
  s_trace = getenv("DKC1_HD_TRACE") != NULL;
}

static int Resolve(int animation) {
  if (s_pack.animations[animation].count) return animation;
  switch (animation) {
    case kDkc1HdRun: return Resolve(kDkc1HdWalk);
    case kDkc1HdCarryWalk: return Resolve(kDkc1HdCarryIdle);
    case kDkc1HdClimbMove: return Resolve(kDkc1HdClimbIdle);
    case kDkc1HdClimbDown: return Resolve(kDkc1HdClimbMove);
    case kDkc1HdHang: return Resolve(kDkc1HdClimbIdle);
    case kDkc1HdKnockout: return Resolve(kDkc1HdHurt);
    case kDkc1HdFall: return s_pack.animations[kDkc1HdJump].count ? kDkc1HdJump : kDkc1HdIdle;
    default: return kDkc1HdIdle;
  }
}

static const HdFrame *FrameAt(int animation, uint32_t elapsed) {
  const HdAnimation *a = &s_pack.animations[animation];
  uint32_t total = 0;
  for (unsigned i = 0; i < a->count; ++i) total += s_pack.steps[a->first + i].duration;
  if (!total) return &s_pack.frames[s_pack.steps[a->first].frame];
  if (a->loop) elapsed %= total;
  else if (elapsed >= total) return &s_pack.frames[s_pack.steps[a->first + a->count - 1].frame];
  for (unsigned i = 0; i < a->count; ++i) {
    const HdStep *step = &s_pack.steps[a->first + i];
    if (elapsed < step->duration) return &s_pack.frames[step->frame];
    elapsed -= step->duration;
  }
  return &s_pack.frames[s_pack.steps[a->first + a->count - 1].frame];
}

static uint32_t Duration(int animation) {
  const HdAnimation *a = &s_pack.animations[animation];
  uint32_t total = 0;
  for (unsigned i = 0; i < a->count; ++i) total += s_pack.steps[a->first + i].duration;
  return total;
}

static bool IsFidget(int animation) {
  return animation == kDkc1HdFidget || animation == kDkc1HdFidget2 ||
         animation == kDkc1HdFidget3;
}

static bool IsUpright(int animation) {
  return animation == kDkc1HdIdle || IsFidget(animation);
}

static bool IsOnFours(int animation) {
  return animation == kDkc1HdWalk || animation == kDkc1HdRun;
}

/* The fidget slots the pack carries take turns. */
static int NextFidget(HdShow *show) {
  static const int kFidgets[3] = {kDkc1HdFidget, kDkc1HdFidget2, kDkc1HdFidget3};
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned turn = (show->fidget_turn + i) % 3;
    if (s_pack.animations[kFidgets[turn]].count) {
      show->fidget_turn = (turn + 1) % 3;
      return kFidgets[turn];
    }
  }
  return -1;
}

/* ---- Donkey's animation IDs ($0001-$0068) as pose slots. The numeric IDs
 * are stable cartridge metadata; the grouping follows the Baby Kong map. */

enum { kAir = 0xff };
typedef struct PoseMap { uint8_t slot; bool center, idle, set; } PoseMap;
#define FEET(slot_) { slot_, false, false, true }
#define MID(slot_) { slot_, true, false, true }
#define REST { kDkc1HdIdle, false, true, true }
#define AIR { kAir, true, false, true }

static const PoseMap kDonkeyPoses[0x69] = {
  [0x01] = REST,                    /* idle */
  [0x02] = FEET(kDkc1HdRun),
  [0x03] = FEET(kDkc1HdWalk),
  [0x04] = FEET(kDkc1HdIdle),       /* sit down */
  [0x05] = AIR,
  [0x06] = FEET(kDkc1HdWalk),       /* turn */
  [0x07] = AIR,
  [0x08] = AIR,
  [0x09] = FEET(kDkc1HdFidget),     /* beats his chest at the level start */
  [0x0a] = FEET(kDkc1HdIdle),       /* too close to an edge */
  [0x0b] = REST,
  [0x0c] = MID(kDkc1HdHurt),
  [0x0d] = MID(kDkc1HdHurt),
  [0x0e] = FEET(kDkc1HdRun),
  [0x0f] = MID(kDkc1HdSwim),
  [0x10] = FEET(kDkc1HdKnockout),   /* dead */
  [0x11] = FEET(kDkc1HdFidget),     /* pushes against a wall (unused) */
  [0x12] = FEET(kDkc1HdFidget),
  [0x13] = FEET(kDkc1HdFidget),
  [0x14] = FEET(kDkc1HdRun),
  [0x15] = AIR,
  [0x16] = FEET(kDkc1HdIdle),       /* land */
  [0x17] = AIR,
  [0x18] = FEET(kDkc1HdRoll),
  [0x19] = FEET(kDkc1HdIdle),
  [0x1a] = FEET(kDkc1HdIdle),
  /* $1B-$42: on an animal buddy (seated, idle, jump, turn, attack). */
  [0x1b] = FEET(kDkc1HdRide), [0x1c] = FEET(kDkc1HdRide), [0x1d] = FEET(kDkc1HdRide),
  [0x1e] = FEET(kDkc1HdRide), [0x1f] = FEET(kDkc1HdRide), [0x20] = FEET(kDkc1HdRide),
  [0x21] = FEET(kDkc1HdRide), [0x22] = FEET(kDkc1HdRide), [0x23] = FEET(kDkc1HdRide),
  [0x24] = FEET(kDkc1HdRide), [0x25] = FEET(kDkc1HdRide), [0x26] = FEET(kDkc1HdRide),
  [0x27] = FEET(kDkc1HdRide), [0x28] = FEET(kDkc1HdRide), [0x29] = FEET(kDkc1HdRide),
  [0x2a] = FEET(kDkc1HdRide), [0x2b] = FEET(kDkc1HdRide), [0x2c] = FEET(kDkc1HdRide),
  [0x2d] = FEET(kDkc1HdRide), [0x2e] = FEET(kDkc1HdRide), [0x2f] = FEET(kDkc1HdRide),
  [0x30] = FEET(kDkc1HdRide), [0x31] = FEET(kDkc1HdRide), [0x32] = FEET(kDkc1HdRide),
  [0x33] = FEET(kDkc1HdRide), [0x34] = FEET(kDkc1HdRide), [0x35] = FEET(kDkc1HdRide),
  [0x36] = FEET(kDkc1HdRide), [0x37] = FEET(kDkc1HdRide), [0x38] = FEET(kDkc1HdRide),
  [0x39] = FEET(kDkc1HdRide), [0x3a] = FEET(kDkc1HdRide), [0x3b] = FEET(kDkc1HdRide),
  [0x3c] = FEET(kDkc1HdRide), [0x3d] = FEET(kDkc1HdRide), [0x3e] = FEET(kDkc1HdRide),
  [0x3f] = FEET(kDkc1HdRide), [0x40] = FEET(kDkc1HdRide), [0x41] = FEET(kDkc1HdRide),
  [0x42] = FEET(kDkc1HdRide),
  [0x43] = MID(kDkc1HdHang), [0x44] = MID(kDkc1HdHang),
  [0x45] = MID(kDkc1HdHang), [0x46] = MID(kDkc1HdHang),
  [0x47] = FEET(kDkc1HdCarryIdle),  /* pick up */
  [0x48] = FEET(kDkc1HdCarryIdle),
  [0x49] = FEET(kDkc1HdCarryWalk),
  [0x4a] = FEET(kDkc1HdThrow),
  [0x4b] = FEET(kDkc1HdCarryIdle),
  [0x4c] = FEET(kDkc1HdCarryIdle),  /* turn while carrying */
  [0x4d] = FEET(kDkc1HdCarryIdle),  /* jump while carrying */
  [0x4e] = FEET(kDkc1HdRun),        /* on a steel keg */
  [0x4f] = FEET(kDkc1HdIdle),       /* tag */
  [0x50] = FEET(kDkc1HdIdle),
  [0x51] = FEET(kDkc1HdVictory),
  [0x52] = AIR,
  [0x53] = AIR,
  /* $54-$5A: ducking, then entering, crossing and leaving a crawlspace. */
  [0x54] = FEET(kDkc1HdCrouch), [0x55] = FEET(kDkc1HdCrouch), [0x56] = FEET(kDkc1HdCrouch),
  [0x57] = FEET(kDkc1HdCrouch), [0x58] = FEET(kDkc1HdCrouch), [0x59] = FEET(kDkc1HdCrouch),
  [0x5a] = FEET(kDkc1HdCrouch),
  [0x5b] = FEET(kDkc1HdRide),
  [0x5c] = MID(kDkc1HdClimbMove),
  [0x5d] = MID(kDkc1HdClimbMove),
  [0x5e] = MID(kDkc1HdClimbIdle),
  [0x5f] = MID(kDkc1HdClimbIdle),
  [0x60] = MID(kDkc1HdSwim),
  [0x61] = REST,
  [0x62] = REST,
  [0x63] = FEET(kDkc1HdVictory),
  [0x64] = MID(kDkc1HdHurt),
  [0x65] = MID(kDkc1HdSwim),
  [0x66] = MID(kDkc1HdHurt),
  [0x67] = FEET(kDkc1HdFidget),     /* ground slap */
  [0x68] = AIR,
};
#undef FEET
#undef MID
#undef REST
#undef AIR

static Dkc1HdPose Pose(int slot, bool center, bool idle) {
  const Dkc1HdPose pose = { slot, center, idle };
  return pose;
}

Dkc1HdPose Dkc1HdPoseForAnimation(unsigned animation_id, unsigned state,
                                  int x_velocity, int y_velocity) {
  const int airborne = y_velocity < 0 ? kDkc1HdJump : kDkc1HdFall;
  if (animation_id < sizeof kDonkeyPoses / sizeof kDonkeyPoses[0] &&
      kDonkeyPoses[animation_id].set) {
    const PoseMap map = kDonkeyPoses[animation_id];
    if (map.slot == kAir) return Pose(airborne, true, false);
    return Pose(map.slot, map.center, map.idle);
  }
  /* Transitions briefly expose animation ID zero. */
  if (state == 43u) return Pose(kDkc1HdSwim, true, false);
  if (state == 1u) return Pose(airborne, true, false);
  if (x_velocity > 0x0180 || x_velocity < -0x0180) return Pose(kDkc1HdRun, false, false);
  if (x_velocity > 0x0020 || x_velocity < -0x0020) return Pose(kDkc1HdWalk, false, false);
  return Pose(kDkc1HdIdle, false, true);
}

void Dkc1HdBeginFrame(void) {
  if (!s_actor.active) s_show.valid = false;
  s_actor.active = false;
  memset(s_mask, 0, sizeof s_mask);
  ++s_tick;
}

void Dkc1HdSetActor(const Dkc1HdActorInput *input) {
  if (!input || !Dkc1HdEnabled()) return;
  HdActor *actor = &s_actor;
  HdShow *show = &s_show;
  actor->active = true;
  actor->input = *input;
  const int previous = show->valid ? show->animation : -1;
  const unsigned previous_native = show->native;
  if (!show->valid) {
    show->valid = true;
    show->animation = -1;
    show->idle_since = s_tick;
    show->last_frame = show->fade_from = NULL;
  }
  const Dkc1HdPose pose = Dkc1HdPoseForAnimation(
      input->animation, input->state, input->x_velocity, input->y_velocity);
  int animation = pose.slot;
  actor->center = pose.center;
  if (!pose.idle) {
    show->idle_since = s_tick;
  } else {
    /* A finished fidget restarts the wait before the next one. */
    const bool fidgeting = IsFidget(show->animation);
    if (fidgeting && s_tick - show->since >= Duration(show->animation))
      show->idle_since = s_tick;
    if (fidgeting && s_tick - show->since < Duration(show->animation)) {
      animation = show->animation;
    } else if (s_tick - show->idle_since >= kFidgetDelay) {
      const int fidget = NextFidget(show);
      if (fidget >= 0) animation = fidget;
    }
  }
  if (animation != show->animation) {
    const bool strides = (animation == kDkc1HdWalk || animation == kDkc1HdRun) &&
                         (show->animation == kDkc1HdWalk || show->animation == kDkc1HdRun);
    const uint32_t from = strides ? Duration(Resolve(show->animation)) : 0;
    const uint32_t to = strides ? Duration(Resolve(animation)) : 0;
    if (from && to && Resolve(show->animation) != Resolve(animation)) {
      /* Walk and run keep their place in the stride across the change. */
      const uint32_t phase = (s_tick - show->since) % from;
      show->since = s_tick - (uint32_t)((uint64_t)phase * to / from);
    } else {
      show->since = s_tick;
    }
    if (previous >= 0 && show->last_frame) {
      show->fade_from = show->last_frame;
      show->fade_start = s_tick;
    }
    /* Between a standing pose and the four-legged gaits, the body goes
     * down or up through its own clip instead of a plain cross-fade. A
     * change of mind part-way continues from the same point, backwards. */
    const int turn = IsUpright(show->animation) && IsOnFours(animation) ? 1 :
                     IsOnFours(show->animation) && IsUpright(animation) ? -1 : 0;
    const uint32_t turn_length = Duration(kDkc1HdStandToFours);
    if (turn && turn_length && previous >= 0) {
      const uint32_t done = show->turn ? s_tick - show->turn_start : turn_length;
      show->turn_start = done < turn_length ? s_tick - (turn_length - 1 - done) : s_tick;
      show->turn = turn;
    } else if (!IsFidget(animation) || !IsUpright(show->animation)) {
      show->turn = 0;
    }
    show->animation = animation;
  }
  show->native = input->animation;
  const int resolved = Resolve(animation);
  uint32_t elapsed = s_tick - show->since;
  if (resolved == kDkc1HdWalk && animation == kDkc1HdRun)
    elapsed = elapsed * 8 / 5; /* no run clip: a faster walk */
  if (resolved == kDkc1HdJump && animation == kDkc1HdFall)
    elapsed = UINT32_MAX / 2; /* no fall clip: hold the jump's last pose */
  actor->frame = FrameAt(resolved, elapsed);
  if (show->turn) {
    const uint32_t length = Duration(kDkc1HdStandToFours);
    const uint32_t done = s_tick - show->turn_start;
    if (done < length) {
      actor->frame = FrameAt(kDkc1HdStandToFours, show->turn > 0 ? done : length - 1 - done);
      show->since = s_tick; /* the gait or the rest starts when it ends */
    } else {
      show->turn = 0;
      if (show->last_frame) {
        show->fade_from = show->last_frame;
        show->fade_start = s_tick;
      }
    }
  }
  actor->fade_frame = NULL;
  actor->fade = 1.0f;
  if (show->fade_from) {
    const uint32_t age = s_tick - show->fade_start;
    if (age < kFadeTicks && show->fade_from != actor->frame) {
      actor->fade_frame = show->fade_from;
      /* Eased, so the change starts and ends gently. */
      const float t = (float)(age + 1) / (float)(kFadeTicks + 1);
      actor->fade = t * t * (3.0f - 2.0f * t);
    } else {
      show->fade_from = NULL;
    }
  }
  show->last_frame = actor->frame;
  /* OBJ priority p sits at level p * 4 + 2 of the PPU's priority buffer. */
  actor->level = (unsigned)((input->properties >> 4) & 3) * 4 + 2;
  const bool art_left = (s_pack.flags & kDkc1HdFlagFacesLeft) != 0;
  actor->flip = ((input->properties & 0x40) != 0) != art_left;
  if (s_trace && (previous != show->animation || previous_native != input->animation))
    fprintf(stderr, "hd slot=%d native=0x%02X state=%u xv=%d yv=%d x=%d\n",
            show->animation, input->animation, input->state,
            input->x_velocity, input->y_velocity, input->anchor_x);
}

int Dkc1HdActorSlot(void) { return s_actor.active ? s_show.animation : -1; }

static void FrameRect(const HdFrame *f, bool flip, int *x0, int *y0, int *x1, int *y1) {
  *x0 = flip ? -((int)f->width - f->anchor_x) : -f->anchor_x;
  *x1 = *x0 + (int)f->width;
  *y0 = -f->anchor_y;
  *y1 = *y0 + (int)f->height;
}

/* The frame's pixel at an origin-relative HD position, or NULL outside it. */
static const uint8_t *FramePixel(const HdFrame *f, bool flip, int x, int y) {
  const int su = flip ? f->anchor_x - 1 - x : x + f->anchor_x;
  const int sv = y + f->anchor_y;
  if (su < 0 || sv < 0 || su >= (int)f->width || sv >= (int)f->height) return NULL;
  return f->rgba + ((size_t)sv * f->width + (size_t)su) * 4;
}

void Dkc1HdObserveLine(const Ppu *ppu, int line) {
  if (!ppu || !s_actor.active || line < 1 || line > kMaskRows) return;
  uint8_t *row = s_mask[line - 1];
  const PpuZbufType *data = ppu->bgBuffers[0].data;
  const unsigned level = s_actor.level;
  /* Only backgrounds cover Donkey: between sprites the OAM order decides,
   * not the priority bits, and the pose is drawn over them (an animal buddy
   * under its rider). Sprite pixels use the upper half of the palette. */
  for (unsigned i = 0; i < kPpuBufWidth; ++i)
    row[i] = (unsigned)(data[i] >> 12) > level && (data[i] & 0x80) == 0;
}

/* ---- Presentation styles: run on the composited frame, never on the pack. */

static bool Scratch(size_t bytes) {
  if (s_tmp_capacity >= bytes) return true;
  uint8_t *grown = (uint8_t *)realloc(s_tmp, bytes);
  if (!grown) return false;
  s_tmp = grown;
  s_tmp_capacity = bytes;
  return true;
}

/* 1-2-1 blur of premultiplied colour, horizontally then vertically. */
static void Soften(uint8_t *px, unsigned w, unsigned h) {
  if (!Scratch((size_t)w * h * 4)) return;
  for (int pass = 0; pass < 2; ++pass) {
    const uint8_t *src = px;
    uint8_t *dst = s_tmp;
    for (unsigned y = 0; y < h; ++y) {
      for (unsigned x = 0; x < w; ++x) {
        unsigned acc[4] = {0, 0, 0, 0}, total = 0;
        for (int d = -1; d <= 1; ++d) {
          const int sx = pass == 0 ? (int)x + d : (int)x;
          const int sy = pass == 0 ? (int)y : (int)y + d;
          if (sx < 0 || sy < 0 || sx >= (int)w || sy >= (int)h) continue;
          const uint8_t *p = src + ((size_t)sy * w + (size_t)sx) * 4;
          const unsigned weight = d == 0 ? 2 : 1;
          for (int c = 0; c < 3; ++c) acc[c] += (unsigned)p[c] * p[3] / 255 * weight;
          acc[3] += (unsigned)p[3] * weight;
          total += weight;
        }
        uint8_t *o = dst + ((size_t)y * w + x) * 4;
        const unsigned alpha = acc[3] / total;
        for (int c = 0; c < 3; ++c) {
          const unsigned premul = acc[c] / total;
          o[c] = (uint8_t)(alpha ? (premul * 255 / alpha > 255 ? 255 : premul * 255 / alpha) : 0);
        }
        o[3] = (uint8_t)alpha;
      }
    }
    memcpy(px, s_tmp, (size_t)w * h * 4);
  }
}

static void Saturate(uint8_t *px, size_t count, float amount) {
  for (size_t i = 0; i < count; ++i, px += 4) {
    const float grey = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
    for (int c = 0; c < 3; ++c) {
      float v = grey + (px[c] - grey) * amount;
      px[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
}

/* SNES look: blocks of `block` HD pixels, 15-bit colour, hard alpha and a
 * one-block dark outline. */
static void Retro(uint8_t *px, unsigned w, unsigned h, unsigned block) {
  if (block < 1) block = 1;
  const unsigned bw = (w + block - 1) / block, bh = (h + block - 1) / block;
  if (!Scratch((size_t)bw * bh * 4)) return;
  uint8_t *cells = s_tmp;
  for (unsigned by = 0; by < bh; ++by) {
    for (unsigned bx = 0; bx < bw; ++bx) {
      unsigned acc[4] = {0, 0, 0, 0}, n = 0;
      for (unsigned y = by * block; y < (by + 1) * block && y < h; ++y)
        for (unsigned x = bx * block; x < (bx + 1) * block && x < w; ++x) {
          const uint8_t *p = px + ((size_t)y * w + x) * 4;
          for (int c = 0; c < 3; ++c) acc[c] += (unsigned)p[c] * p[3];
          acc[3] += p[3];
          ++n;
        }
      uint8_t *cell = cells + ((size_t)by * bw + bx) * 4;
      const bool solid = n && acc[3] / n >= 128;
      for (int c = 0; c < 3; ++c) {
        const unsigned v = acc[3] ? acc[c] / acc[3] : 0;
        cell[c] = (uint8_t)((v & 0xF8) | (v >> 5)); /* 5 bits per channel */
      }
      cell[3] = solid ? 255 : 0;
    }
  }
  /* Outline: an empty cell touching a solid one becomes dark brown. */
  for (unsigned by = 0; by < bh; ++by)
    for (unsigned bx = 0; bx < bw; ++bx) {
      uint8_t *cell = cells + ((size_t)by * bw + bx) * 4;
      if (cell[3] == 255) continue;
      static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (int k = 0; k < 4; ++k) {
        const int nx = (int)bx + d[k][0], ny = (int)by + d[k][1];
        if (nx < 0 || ny < 0 || nx >= (int)bw || ny >= (int)bh) continue;
        const uint8_t *nb = cells + ((size_t)ny * bw + (size_t)nx) * 4;
        if (nb[3] == 255) {
          cell[0] = 24; cell[1] = 16; cell[2] = 8; cell[3] = 254; /* not a neighbour source */
          break;
        }
      }
    }
  for (unsigned y = 0; y < h; ++y)
    for (unsigned x = 0; x < w; ++x) {
      uint8_t *p = px + ((size_t)y * w + x) * 4;
      const uint8_t *cell = cells + ((size_t)(y / block) * bw + x / block) * 4;
      memcpy(p, cell, 4);
      if (p[3] == 254) p[3] = 255;
    }
}

/* Darkens the HD rows that fall between the native scanlines, so the
 * character shares the CRT pass's line structure. */
static void Scanlines(uint8_t *px, unsigned w, unsigned h, float top, float scale) {
  const float inv = 1.0f / scale;
  for (unsigned y = 0; y < h; ++y) {
    const float ny = top + ((float)y + 0.5f) * inv;
    const float frac = ny - floorf(ny);
    if (frac < 0.55f) continue;
    uint8_t *p = px + (size_t)y * w * 4;
    for (unsigned x = 0; x < w; ++x, p += 4)
      for (int c = 0; c < 3; ++c) p[c] = (uint8_t)(p[c] * 0.68f);
  }
}

static void ApplyStyle(uint8_t *px, unsigned w, unsigned h, float top, float scale) {
  switch (s_style) {
    case kDkc1HdStyleSoft:
      Soften(px, w, h);
      Saturate(px, (size_t)w * h, 0.9f);
      break;
    case kDkc1HdStyleRetro:
      Retro(px, w, h, (unsigned)(scale / 2.0f + 0.5f));
      break;
    case kDkc1HdStyleCrt:
      Soften(px, w, h);
      Scanlines(px, w, h, top, scale);
      break;
    default:
      break;
  }
}

void Dkc1HdSetStyle(int style) {
  s_style = style >= kDkc1HdStyleHd && style < kDkc1HdStyleCount ? style : kDkc1HdStyleHd;
}
int Dkc1HdStyle(void) { return s_style; }
void Dkc1HdSetSize(float size) { s_size = size >= 0.5f && size <= 2.0f ? size : 1.0f; }
float Dkc1HdSize(void) { return s_size; }

int Dkc1HdStyleFromName(const char *name) {
  static const char *const names[kDkc1HdStyleCount] = {"hd", "doux", "retro", "crt"};
  for (int i = 0; name && i < kDkc1HdStyleCount; ++i)
    if (strcmp(name, names[i]) == 0) return i;
  return kDkc1HdStyleHd;
}

/* Places the pose on the native sprite's rows and cuts it against the
 * frame's occluders. */
void Dkc1HdCompose(const Ppu *ppu, int native_top, int native_bottom) {
  s_sprite_count = 0;
  HdActor *actor = &s_actor;
  if (!ppu || !actor->active || !actor->frame || (ppu->inidisp & 0x80)) return;
  const HdFrame *f = actor->frame;
  const HdFrame *g = actor->fade_frame;
  int output_width = (int)(ppu->renderPitch / 4u);
  if (output_width < kPpuXPixels) output_width = kPpuXPixels;
  if (output_width > kPpuBufWidth) output_width = kPpuBufWidth;
  const int output_extra = (output_width - kPpuXPixels) / 2;
  const float scale = EffectiveScale();
  int x0, y0, x1, y1;
  FrameRect(f, actor->flip, &x0, &y0, &x1, &y1);
  /* Feet: the anchor row sits under the native sprite. Centre: the pose's
   * own middle sits on the native sprite's middle (ropes, water, air). */
  float origin_y = (float)(native_bottom + 1);
  if (actor->center)
    origin_y = (float)(native_top + native_bottom + 1) * 0.5f -
               (float)(y0 + y1) * 0.5f / scale;
  origin_y += s_pack.offset_y;
  const float origin_x = (float)(actor->input.anchor_x + output_extra) + s_pack.offset_x;
  if (g) {
    int px0, py0, px1, py1;
    FrameRect(g, actor->flip, &px0, &py0, &px1, &py1);
    if (px0 < x0) x0 = px0;
    if (py0 < y0) y0 = py0;
    if (px1 > x1) x1 = px1;
    if (py1 > y1) y1 = py1;
  }
  actor->rx = x0;
  actor->ry = y0;
  actor->rw = x1 - x0;
  actor->rh = y1 - y0;
  actor->width = (float)actor->rw / scale;
  actor->height = (float)actor->rh / scale;
  actor->left = origin_x + (float)x0 / scale;
  actor->top = origin_y + (float)y0 / scale;

  const unsigned w = (unsigned)actor->rw, h = (unsigned)actor->rh;
  const size_t needed = (size_t)w * h * 4;
  if (s_out_capacity < needed) {
    uint8_t *grown = (uint8_t *)realloc(s_out, needed);
    if (!grown) return;
    s_out = grown;
    s_out_capacity = needed;
  }
  const float inv = 1.0f / scale;
  const float keep = actor->fade, drop = 1.0f - actor->fade;
  for (unsigned v = 0; v < h; ++v) {
    const int ny = (int)floorf(actor->top + ((float)v + 0.5f) * inv);
    const bool row_ok = ny >= 0 && ny < 224;
    uint8_t *dst = s_out + (size_t)v * w * 4;
    for (unsigned u = 0; u < w; ++u, dst += 4) {
      const int x = actor->rx + (int)u, y = actor->ry + (int)v;
      const uint8_t *src = FramePixel(f, actor->flip, x, y);
      const int nx = (int)floorf(actor->left + ((float)u + 0.5f) * inv);
      /* The mask is indexed like the PPU's line buffers. */
      const int index = nx - output_extra + kPpuExtraLeftRight;
      const bool visible = row_ok && nx >= 0 && nx < output_width &&
                           index >= 0 && index < kPpuBufWidth && !s_mask[ny][index];
      if (!g) {
        dst[0] = src ? src[0] : 0;
        dst[1] = src ? src[1] : 0;
        dst[2] = src ? src[2] : 0;
        dst[3] = src && visible ? src[3] : 0;
        continue;
      }
      /* Cross-fade in premultiplied space so edges do not darken. */
      const uint8_t *old = FramePixel(g, actor->flip, x, y);
      const float a_new = src ? (float)src[3] * keep : 0.0f;
      const float a_old = old ? (float)old[3] * drop : 0.0f;
      const float alpha = a_new + a_old;
      if (alpha < 0.5f || !visible) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        continue;
      }
      for (unsigned c = 0; c < 3; ++c)
        dst[c] = (uint8_t)(((src ? (float)src[c] * a_new : 0.0f) +
                            (old ? (float)old[c] * a_old : 0.0f)) / alpha + 0.5f);
      dst[3] = (uint8_t)(alpha + 0.5f);
    }
  }
  ApplyStyle(s_out, w, h, actor->top, scale);
  s_sprite.rgba = s_out;
  s_sprite.width = (int)w;
  s_sprite.height = (int)h;
  s_sprite.x = actor->left;
  s_sprite.y = actor->top;
  s_sprite.w = actor->width;
  s_sprite.h = actor->height;
  s_sprite.brightness = (float)(ppu->inidisp & 15) / 15.0f;
  s_sprite.serial = ++s_serial;
  s_sprite_count = 1;
}

/* ---- Finding Donkey in the frame (the Baby Kong mod's method). */

/* On an animal buddy the game splits Donkey in two: his own actor slot
 * plays the animal's animations ($1E-$46, "ridden by DK") and shows the
 * animal, while a rider actor plays one of four seated or hanging
 * animations. The rider is the one to replace; the animal keeps its art. */
bool Dkc1HdIsRiderAnimation(unsigned animation) {
  return animation == 0x1bu || animation == 0x1cu || animation == 0x1du ||
         animation == 0x5bu;
}

bool Dkc1HdIsAnimalAnimation(unsigned animation) {
  return animation >= 0x1eu && animation <= 0x46u;
}

static uint32_t FindDonkeySlot(const uint8_t *wram) {
  uint32_t donkey = 0;
  for (uint32_t slot = DKC1_ACTOR_SLOT_FIRST;
       slot <= DKC1_ACTOR_SLOT_LAST; slot += DKC1_ACTOR_SLOT_STEP) {
    const unsigned id = Dkc1WramU16(wram, DKC1_WRAM_NorSpr_SpriteIDLo + slot);
    if (id == 0u) continue;
    if (Dkc1HdIsRiderAnimation(Dkc1Actor_SprAnimID(wram, slot))) return slot;
    if (id == 1u && !donkey) donkey = slot;
  }
  if (donkey && Dkc1HdIsAnimalAnimation(Dkc1Actor_SprAnimID(wram, donkey))) return 0;
  return donkey;
}

static int DecodeOamX(const Ppu *ppu, int slot) {
  const uint16_t word = ppu->oam[slot * 2];
  const unsigned shift = (unsigned)(slot & 3) * 2u;
  int x = (word & 0xffu) | (((ppu->highOam[slot >> 2] >> shift) & 1u) << 8);
  if (x >= 256) x -= 512;
  return x;
}

static bool OamMatchesDonkey(const Ppu *ppu, int slot, int anchor_x, uint8_t properties) {
  const uint8_t y = (uint8_t)(ppu->oam[slot * 2] >> 8);
  const uint8_t slot_properties = (uint8_t)(ppu->oam[slot * 2 + 1] >> 8);
  const int dx = DecodeOamX(ppu, slot) - anchor_x;
  return y != 0xf0u && (slot_properties & 0x7eu) == (properties & 0x7eu) &&
         dx >= -72 && dx <= 72;
}

/* Donkey's pieces are the longest contiguous OAM run with his attributes
 * around his position. */
static bool FindDonkeyOamRun(const Ppu *ppu, int anchor_x, uint8_t properties,
                             uint8_t *first, uint8_t *count) {
  int best_first = -1, best_count = 0;
  for (int slot = 0; slot < 128;) {
    if (!OamMatchesDonkey(ppu, slot, anchor_x, properties)) {
      slot++;
      continue;
    }
    const int start = slot;
    while (slot < 128 && OamMatchesDonkey(ppu, slot, anchor_x, properties)) slot++;
    if (slot - start > best_count) {
      best_first = start;
      best_count = slot - start;
    }
  }
  if (best_count < 2) return false;
  *first = (uint8_t)best_first;
  *count = (uint8_t)best_count;
  return true;
}

void Dkc1HdPrepareFrame(Ppu *ppu, const uint8_t *wram, int presentation_bias) {
  Dkc1HdBeginFrame();
  s_capturing = false;
  if (!Dkc1HdEnabled() || !ppu || !wram || PPU_forcedBlank(ppu) ||
      PPU_brightness(ppu) == 0 ||
      ((ppu->screenEnabled[0] | ppu->screenEnabled[1]) & 0x10u) == 0)
    return;
  const uint32_t slot = FindDonkeySlot(wram);
  if (!slot) return;
  const int frame_width = (int)(ppu->renderPitch / 4u);
  if (frame_width < kPpuXPixels || frame_width > kPpuBufWidth) return;
  const int actor_x = (int16_t)(Dkc1Actor_SprXPos(wram, slot) -
                                Dkc1WramU16(wram, DKC1_WRAM_CameraX));
  const int anchor_x = actor_x - presentation_bias;
  const uint8_t properties = (uint8_t)(Dkc1Actor_SprFlags(wram, slot) >> 8);
  uint8_t first = 0, count = 0;
  if (!FindDonkeyOamRun(ppu, anchor_x, properties, &first, &count)) return;
  /* Take Donkey's pieces out of the composite; the host draws the pose. */
  if (!PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, s_capture, ppu->renderPitch) ||
      !PpuSetOverlayCapture(ppu, kPpuOverlaySource_Obj, anchor_x - kCaptureHalfWidth, 0,
                            kCaptureHalfWidth * 2, 224, kPpuOverlayFlag_RemoveFromGame) ||
      !PpuSetOverlayOamRange(ppu, first, count)) {
    (void)PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, NULL, 0);
    return;
  }
  Dkc1HdActorInput input;
  input.anchor_x = anchor_x;
  input.properties = properties;
  input.animation = Dkc1Actor_SprAnimID(wram, slot);
  input.state = Dkc1Actor_RAMTable1029Lo(wram, slot);
  input.x_velocity = (int16_t)Dkc1Actor_SprXVelocity(wram, slot);
  input.y_velocity = (int16_t)Dkc1Actor_SprYVelocity(wram, slot);
  Dkc1HdSetActor(&input);
  s_capturing = true;
}

void Dkc1HdFinishFrame(Ppu *ppu) {
  s_sprite_count = 0;
  if (!s_capturing || !ppu) return;
  s_capturing = false;
  /* The captured native sprite gives the rows the pose stands on. A frame
   * where the game hides Donkey (blinking) hides the pose too. */
  const int output_width = (int)(ppu->renderPitch / 4u);
  const int output_extra = (output_width - kPpuXPixels) / 2;
  int x0 = s_actor.input.anchor_x - kCaptureHalfWidth + output_extra;
  int x1 = s_actor.input.anchor_x + kCaptureHalfWidth + output_extra;
  if (x0 < 0) x0 = 0;
  if (x1 > output_width) x1 = output_width;
  int top = -1, bottom = -1;
  for (int y = 0; y < 224; y++) {
    const uint32_t *row = (const uint32_t *)(s_capture + (size_t)y * ppu->renderPitch);
    for (int x = x0; x < x1; x++) {
      if (row[x] != 0) {
        if (top < 0) top = y;
        bottom = y;
        break;
      }
    }
  }
  if (top < 0) return;
  Dkc1HdCompose(ppu, top, bottom);
}

int Dkc1HdSprites(const Dkc1OverlaySprite **sprites) {
  if (sprites) *sprites = s_sprite_count ? &s_sprite : NULL;
  return s_sprite_count;
}
