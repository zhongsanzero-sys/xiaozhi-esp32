#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

namespace {

constexpr float kPi = 3.14159265f;

// Unified NOMI 03 Palette
constexpr uint32_t kColWhite = 0xFFFFFF;
constexpr uint32_t kColBlack = 0x000000;
constexpr uint32_t kColCoral = 0xFA6B47;   // 03 Coral-Orange
constexpr uint32_t kColRed = 0xEB3847;     // Festive Red
constexpr uint32_t kColCyan = 0x4CEBE0;    // 03 Mint-Cyan
constexpr uint32_t kColBlue = 0x2462EB;    // Stage Soft Blue
constexpr uint32_t kColGold = 0xFFB703;    // Warm Gold
constexpr uint32_t kColGreen = 0x22C55E;   // Charging / Mint Green
constexpr uint32_t kColGray = 0x7E8898;    // Soft Desaturated Gray
constexpr uint32_t kColBlush = 0x4E5460;   // Subtle Dark-Gray Blush

inline float ClampF(float v, float lo = 0.0f, float hi = 1.0f) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float LerpF(float a, float b, float t) { return a + (b - a) * t; }

inline float SmoothStep(float t) {
    t = ClampF(t);
    return t * t * (3.0f - 2.0f * t);
}

inline float EaseInOutCubic(float t) {
    t = ClampF(t);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

inline float EaseOutBack(float t, float c1 = 1.25f) {
    t = ClampF(t);
    float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

inline lv_color_t LerpColorHex(uint32_t c1, uint32_t c2, float t) {
    t = ClampF(t);
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t r = static_cast<uint8_t>(r1 + (r2 - r1) * t);
    uint8_t g = static_cast<uint8_t>(g1 + (g2 - g1) * t);
    uint8_t b = static_cast<uint8_t>(b1 + (b2 - b1) * t);
    return lv_color_make(r, g, b);
}

inline lv_opa_t ToOpa(float alpha) {
    int v = static_cast<int>(ClampF(alpha) * 255.0f);
    return static_cast<lv_opa_t>(v);
}

// =========================================================================
// LVGL v9 ABI-Safe Vector Draw Helpers (ONLY rect / line / arc / label)
// =========================================================================
float g_nomi_scale = 1.0f;
float g_nomi_origin = 120.0f;

inline float Sx(float v) { return g_nomi_origin + (v - g_nomi_origin) * g_nomi_scale; }
inline float Sw(float v) { return v * g_nomi_scale; }

void DrawFillCircle(lv_layer_t* layer, float cx, float cy, float r, lv_color_t col,
                    lv_opa_t opa = LV_OPA_COVER) {
    float sr = Sw(r);
    if (sr < 0.8f || opa < 4) return;
    float scx = Sx(cx), scy = Sx(cy);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = col;
    dsc.bg_opa = opa;
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.border_width = 0;
    lv_area_t a = {static_cast<int32_t>(std::round(scx - sr)),
                   static_cast<int32_t>(std::round(scy - sr)),
                   static_cast<int32_t>(std::round(scx + sr)),
                   static_cast<int32_t>(std::round(scy + sr))};
    lv_draw_rect(layer, &dsc, &a);
}

void DrawFillRoundRect(lv_layer_t* layer, float cx, float cy, float w, float h, float rad,
                       lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    float sw_v = Sw(w), sh_v = Sw(h);
    if (sw_v < 1.0f || sh_v < 1.0f || opa < 4) return;
    float scx = Sx(cx), scy = Sx(cy);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = col;
    dsc.bg_opa = opa;
    dsc.radius = static_cast<int32_t>(std::round(Sw(rad)));
    dsc.border_width = 0;
    lv_area_t a = {static_cast<int32_t>(std::round(scx - sw_v * 0.5f)),
                   static_cast<int32_t>(std::round(scy - sh_v * 0.5f)),
                   static_cast<int32_t>(std::round(scx + sw_v * 0.5f)),
                   static_cast<int32_t>(std::round(scy + sh_v * 0.5f))};
    lv_draw_rect(layer, &dsc, &a);
}

void DrawStrokeRoundRect(lv_layer_t* layer, float cx, float cy, float w, float h, float rad,
                         float sw, lv_color_t col, lv_opa_t opa = LV_OPA_COVER,
                         bool fill_black = false) {
    float sw_v = Sw(w), sh_v = Sw(h);
    if (sw_v < 1.0f || sh_v < 1.0f || opa < 4) return;
    float scx = Sx(cx), scy = Sx(cy);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(kColBlack);
    dsc.bg_opa = fill_black ? opa : LV_OPA_TRANSP;
    dsc.border_color = col;
    dsc.border_width = static_cast<int32_t>(std::max(1.0f, std::round(Sw(sw))));
    dsc.border_opa = opa;
    dsc.radius = static_cast<int32_t>(std::round(Sw(rad)));
    lv_area_t a = {static_cast<int32_t>(std::round(scx - sw_v * 0.5f)),
                   static_cast<int32_t>(std::round(scy - sh_v * 0.5f)),
                   static_cast<int32_t>(std::round(scx + sw_v * 0.5f)),
                   static_cast<int32_t>(std::round(scy + sh_v * 0.5f))};
    lv_draw_rect(layer, &dsc, &a);
}

void DrawLineSeg(lv_layer_t* layer, float x1, float y1, float x2, float y2, float w,
                 lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    if (opa < 4) return;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = col;
    dsc.width = static_cast<int32_t>(std::max(1.0f, std::round(Sw(w))));
    dsc.opa = opa;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = static_cast<lv_value_precise_t>(Sx(x1));
    dsc.p1.y = static_cast<lv_value_precise_t>(Sx(y1));
    dsc.p2.x = static_cast<lv_value_precise_t>(Sx(x2));
    dsc.p2.y = static_cast<lv_value_precise_t>(Sx(y2));
    lv_draw_line(layer, &dsc);
}

void DrawArcSeg(lv_layer_t* layer, float cx, float cy, float rad, float a_start, float a_end,
                float w, lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    float srad = Sw(rad);
    if (srad < 1.5f || opa < 4) return;
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = col;
    dsc.width = static_cast<int32_t>(std::max(1.0f, std::round(Sw(w))));
    dsc.opa = opa;
    dsc.rounded = 1;
    dsc.center.x = static_cast<int32_t>(std::round(Sx(cx)));
    dsc.center.y = static_cast<int32_t>(std::round(Sx(cy)));
    dsc.radius = static_cast<uint16_t>(std::round(srad));
    dsc.start_angle = a_start;
    dsc.end_angle = a_end;
    lv_draw_arc(layer, &dsc);
}

// Large crisp rounded-cap vector digital clock digit (0..9)
void DrawVectorDigit(lv_layer_t* layer, float cx, float cy, float w, float h, float sw, int digit,
                     lv_color_t col, lv_opa_t opa) {
    static const uint8_t kMask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    if (digit < 0 || digit > 9) return;
    uint8_t m = kMask[digit];
    float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
    float y0 = cy - h * 0.5f, ym = cy, y1 = cy + h * 0.5f;
    if (m & 0x01) DrawLineSeg(layer, x0, y0, x1, y0, sw, col, opa);  // A top
    if (m & 0x02) DrawLineSeg(layer, x1, y0, x1, ym, sw, col, opa);  // B top-right
    if (m & 0x04) DrawLineSeg(layer, x1, ym, x1, y1, sw, col, opa);  // C bot-right
    if (m & 0x08) DrawLineSeg(layer, x0, y1, x1, y1, sw, col, opa);  // D bottom
    if (m & 0x10) DrawLineSeg(layer, x0, ym, x0, y1, sw, col, opa);  // E bot-left
    if (m & 0x20) DrawLineSeg(layer, x0, y0, x0, ym, sw, col, opa);  // F top-left
    if (m & 0x40) DrawLineSeg(layer, x0, ym, x1, ym, sw, col, opa);  // G middle
}

// Rotated point helper around pivot (ox, oy)
inline void RotPt(float ox, float oy, float lx, float ly, float ang_rad, float scx, float scy,
                  float& out_x, float& out_y) {
    float sx = lx * scx;
    float sy = ly * scy;
    float c = std::cos(ang_rad);
    float s = std::sin(ang_rad);
    out_x = ox + sx * c - sy * s;
    out_y = oy + sx * s + sy * c;
}

void DrawRotLine(lv_layer_t* layer, float ox, float oy, float ang_rad, float scx, float scy,
                 float x1, float y1, float x2, float y2, float w, lv_color_t col, lv_opa_t opa) {
    float rx1, ry1, rx2, ry2;
    RotPt(ox, oy, x1, y1, ang_rad, scx, scy, rx1, ry1);
    RotPt(ox, oy, x2, y2, ang_rad, scx, scy, rx2, ry2);
    DrawLineSeg(layer, rx1, ry1, rx2, ry2, w, col, opa);
}

void DrawSparkle4(lv_layer_t* layer, float cx, float cy, float s, float rot_deg, lv_color_t col,
                  lv_opa_t opa, float lw = 2.2f) {
    if (s < 1.0f || opa < 8) return;
    if (s < 2.5f) {
        DrawFillCircle(layer, cx, cy, s * 0.7f, col, opa);
        return;
    }
    float rad = rot_deg * kPi / 180.0f;
    float in_r = s * 0.28f;
    float pts[8][2];
    for (int i = 0; i < 8; i++) {
        float r = (i % 2 == 0) ? s : in_r;
        float a = rad + i * (kPi * 0.25f);
        pts[i][0] = cx + std::cos(a) * r;
        pts[i][1] = cy + std::sin(a) * r;
    }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) % 8;
        DrawLineSeg(layer, pts[i][0], pts[i][1], pts[j][0], pts[j][1], lw, col, opa);
    }
}

// Feathered soft radial ambient glow drifting in from bottom-right (ZERO hard cone edges!)
void DrawFeatheredGlow(lv_layer_t* layer, float gx, float gy, float max_r, uint32_t hex_col,
                       float strength) {
    if (strength <= 0.02f) return;
    lv_color_t col = lv_color_hex(hex_col);
    // 4 concentric low-opacity circles simulate ultra-soft feathered radial mist on RGB565
    static const float kRadii[4] = {1.0f, 0.76f, 0.52f, 0.30f};
    static const float kOpa[4] = {0.06f, 0.10f, 0.13f, 0.16f};
    for (int i = 0; i < 4; i++) {
        DrawFillCircle(layer, gx, gy, max_r * kRadii[i], col, ToOpa(kOpa[i] * strength));
    }
}

// Slow dreamy musical note: Vivid 03 color -> smooth gray -> fade out
void DrawColorToGrayNote(lv_layer_t* layer, float x, float y, float sc, int col_idx, float life_p,
                         float env_a) {
    if (env_a <= 0.02f || life_p <= 0.0f || life_p >= 1.0f) return;
    static const uint32_t kNotePalette[5] = {kColCoral, kColCyan, kColGold, kColWhite, 0x82AFFF};
    uint32_t c0 = kNotePalette[col_idx % 5];
    lv_color_t col = lv_color_hex(c0);
    float a = env_a;
    float sp = 1.0f;
    if (life_p < 0.18f) {
        a = SmoothStep(life_p / 0.18f) * env_a;
        sp = LerpF(0.35f, 1.0f, EaseOutBack(life_p / 0.18f, 1.1f));
    } else if (life_p >= 0.52f && life_p < 0.84f) {
        float u = SmoothStep((life_p - 0.52f) / 0.32f);
        col = LerpColorHex(c0, kColGray, u);
        sp = LerpF(1.0f, 0.88f, u);
    } else if (life_p >= 0.84f) {
        float u = SmoothStep((life_p - 0.84f) / 0.16f);
        col = lv_color_hex(kColGray);
        a = (1.0f - u) * env_a;
        sp = LerpF(0.88f, 0.72f, u);
    }
    lv_opa_t opa = ToOpa(a);
    float s = sc * sp;
    DrawFillCircle(layer, x - 3.0f * s, y + 7.0f * s, 4.2f * s, col, opa);
    DrawLineSeg(layer, x + 1.0f * s, y + 6.5f * s, x + 1.0f * s, y - 10.0f * s, 3.0f * s, col, opa);
    DrawLineSeg(layer, x + 1.0f * s, y - 10.0f * s, x + 8.0f * s, y - 4.5f * s, 3.0f * s, col, opa);
}

// Authentic cute scalloped-knuckle NOMI fist
void DrawScallopedFist(lv_layer_t* layer, float cx, float cy, float sc, lv_opa_t opa,
                       bool thumb_up = false, int side = 1) {
    if (sc < 0.1f || opa < 8) return;
    lv_color_t w = lv_color_hex(kColWhite);
    DrawFillRoundRect(layer, cx, cy, 19.0f * sc, 20.0f * sc, 7.5f * sc, w, opa);
    for (int i = 0; i < 4; i++) {
        float ky = (-7.0f + i * 4.7f) * sc;
        DrawFillCircle(layer, cx + side * 9.0f * sc, cy + ky, 3.0f * sc, w, opa);
    }
    if (thumb_up) {
        DrawFillRoundRect(layer, cx - side * 9.5f * sc, cy - 13.0f * sc, 6.0f * sc, 16.0f * sc,
                          3.0f * sc, w, opa);
    }
}

// =========================================================================
// Modular NOMI Eye / Prop / Glow / Motion Enums
// =========================================================================
enum class NomiEyeKind {
    kCapsule,
    kInterest,
    kArcUp,
    kArcUpHigh,
    kSoulful,
    kWink,
    kRound,
    kDotShock,
    kFlat,
    kNarrow,
    kSad,
    kAngry,
    kDoubt,
    kStarry,
    kSleepGray,
    kWindSquint,
    kClockLook,
    kChaseDot,
    kHidden,
};

enum class NomiPropKind {
    kNone,
    kSparkles,
    kDoubleHeart,
    kFloatHeart,
    kFirecracker,
    kRedPacket,
    kLantern,
    kFireworks,
    kXmasHat,
    kXmasTree,
    kXmasGift,
    kRose,
    kZongzi,
    kMidAutumnMoon,
    kMidAutumnCake,
    kNationalFlag,
    kHalloweenPumpkin,
    kHalloweenGhost,
    kBirthdayCake,
    kTrophy,
    kSax,
    kViolin,
    kElecGuitar,
    kAcousticGuitar,
    kHandpan,
    kGlowsticks,
    kHipHop,
    kDJ,
    kMic,
    kMusicNotes,
    kSunny,
    kHotHat,
    kFoldingFan,
    kWindPole,
    kUmbrella,
    kColdScarf,
    kSnowflakes,
    kFaceMask,
    kPinwheel,
    kAutumnLeaf,
    kClockBoard,
    kClockGlasses,
    kChaseDot,
    kTyping,
    kCoffee,
    kBobaTea,
    kMagnifier,
    kHeadphones,
    kPottedPlant,
    kStretchHands,
    kWaveHand,
    kBlush,
    kTears,
    kSweat,
    kAngerMark,
    kVoiceWave,
    kBolt,
    kZzz,
    kSunglasses,
    kCrown,
    kTongue,
};

enum class NomiGlowKind {
    kNone,
    kBlueBR,
    kGoldBR,
    kCyanBR,
    kDualGold,
};

enum class NomiHaloKind {
    kNone,
    kListenPulse,
    kSpinAmber,
    kBlinkRed,
    kSolidCyan,
    kSolidGreen,
    kSolidCoral,
    kSolidGold,
};

struct NomiMoodCfg {
    NomiEyeKind eye;
    NomiPropKind prop;
    NomiGlowKind glow;
    NomiHaloKind halo;
    bool orbit_3d;      // Enable 3D spherical-orbit depth (近大远小)
    bool eye_tilt_sync; // Enable coordinated eye pair-tilt (±8..12 deg)
};

NomiMoodCfg GetNomiMoodCfg(int mood_idx) {
    using E = NomiEyeKind;
    using P = NomiPropKind;
    using G = NomiGlowKind;
    using H = NomiHaloKind;
    switch (mood_idx) {
        // Core emotions (0..23)
        case 0:  return {E::kCapsule,    P::kNone,        G::kNone, H::kNone, false, false};  // kNeutral
        case 1:  return {E::kArcUp,      P::kSparkles,    G::kNone, H::kNone, false, false};  // kHappy
        case 2:  return {E::kArcUp,      P::kBlush,       G::kNone, H::kNone, false, true};   // kLaughing
        case 3:  return {E::kDoubt,      P::kNone,        G::kNone, H::kNone, false, false};  // kGrinning
        case 4:  return {E::kArcUpHigh,  P::kDoubleHeart, G::kNone, H::kNone, false, false};  // kLoving
        case 5:  return {E::kWink,       P::kFloatHeart,  G::kNone, H::kNone, false, false};  // kKissy
        case 6:  return {E::kWink,       P::kSparkles,    G::kNone, H::kNone, false, false};  // kWinking
        case 7:  return {E::kStarry,     P::kSparkles,    G::kNone, H::kNone, false, true};   // kStarry
        case 8:  return {E::kRound,      P::kTears,       G::kNone, H::kNone, false, false};  // kTouched
        case 9:  return {E::kSad,        P::kNone,        G::kNone, H::kNone, false, false};  // kPout
        case 10: return {E::kSad,        P::kNone,        G::kNone, H::kNone, false, false};  // kSad
        case 11: return {E::kSad,        P::kTears,       G::kNone, H::kNone, false, false};  // kCrying
        case 12: return {E::kAngry,      P::kAngerMark,   G::kNone, H::kSolidCoral, false, false}; // kAngry
        case 13: return {E::kAngry,      P::kAngerMark,   G::kNone, H::kBlinkRed, false, true};    // kFurious
        case 14: return {E::kRound,      P::kSparkles,    G::kNone, H::kNone, false, false};  // kSurprised
        case 15: return {E::kDotShock,   P::kSweat,       G::kNone, H::kNone, false, false};  // kShocked
        case 16: return {E::kCapsule,    P::kMagnifier,   G::kNone, H::kSpinAmber, false, false}; // kThinking
        case 17: return {E::kDoubt,      P::kNone,        G::kNone, H::kNone, false, true};   // kConfused
        case 18: return {E::kFlat,       P::kSweat,       G::kNone, H::kNone, false, false};  // kSpeechless
        case 19: return {E::kNarrow,     P::kSweat,       G::kNone, H::kNone, false, false};  // kGuilty
        case 20: return {E::kArcUp,      P::kSweat,       G::kNone, H::kNone, false, false};  // kEmbarrassed
        case 21: return {E::kArcUp,      P::kBlush,       G::kNone, H::kNone, false, false};  // kShy
        case 22: return {E::kWink,       P::kTongue,      G::kNone, H::kNone, false, true};   // kSilly
        case 23: return {E::kDotShock,   P::kNone,        G::kNone, H::kNone, false, true};   // kDazed
        // Device states (24..33)
        case 24: return {E::kInterest,   P::kNone,        G::kNone, H::kListenPulse, false, false}; // kListening
        case 25: return {E::kCapsule,    P::kVoiceWave,   G::kNone, H::kSolidCyan, false, false};   // kSpeaking
        case 26: return {E::kCapsule,    P::kNone,        G::kNone, H::kSpinAmber, false, false};   // kConnecting
        case 27: return {E::kSad,        P::kSweat,       G::kNone, H::kSolidCoral, false, false};  // kWifiError
        case 28: return {E::kFlat,       P::kNone,        G::kNone, H::kNone, false, false};        // kMuted
        case 29: return {E::kNarrow,     P::kSweat,       G::kNone, H::kBlinkRed, false, false};    // kLowBattery
        case 30: return {E::kArcUpHigh,  P::kBolt,        G::kNone, H::kSolidGreen, false, false};  // kCharging
        case 31: return {E::kNarrow,     P::kNone,        G::kNone, H::kNone, false, false};        // kWakingUp
        case 32: return {E::kFlat,       P::kNone,        G::kNone, H::kNone, false, false};        // kYawning
        case 33: return {E::kSleepGray,  P::kZzz,         G::kNone, H::kNone, false, false};        // kSleeping
        // Weather & temperature tiers (34..41)
        case 34: return {E::kArcUp,      P::kSunny,       G::kNone, H::kNone, false, true};   // kSunny (18-26C)
        case 35: return {E::kArcUp,      P::kHotHat,      G::kNone, H::kNone, false, false};  // kWeatherHotHat (>=30C)
        case 36: return {E::kNarrow,     P::kFoldingFan,  G::kNone, H::kNone, false, false};  // kHot (>=35C)
        case 37: return {E::kWindSquint, P::kWindPole,    G::kNone, H::kNone, false, false};  // kWeatherWindy
        case 38: return {E::kCapsule,    P::kUmbrella,    G::kNone, H::kNone, false, false};  // kRainy
        case 39: return {E::kCapsule,    P::kColdScarf,   G::kNone, H::kNone, false, false};  // kCold (<=10C)
        case 40: return {E::kNarrow,     P::kSnowflakes,  G::kNone, H::kNone, false, false};  // kWeatherSnow (<=0C)
        case 41: return {E::kCapsule,    P::kFaceMask,    G::kNone, H::kNone, false, false};  // kWeatherHazeMask
        case 42: return {E::kCapsule,    P::kPinwheel,    G::kNone, H::kNone, false, false};  // kSeasonSpring
        case 43: return {E::kSoulful,    P::kAutumnLeaf,  G::kGoldBR, H::kNone, false, true}; // kSeasonAutumn
        // Year-round split festival easter eggs (42..57)
        case 44: return {E::kArcUpHigh,  P::kFirecracker, G::kNone, H::kNone, false, false};  // kFestFirecracker
        case 45: return {E::kArcUpHigh,  P::kRedPacket,   G::kNone, H::kNone, false, true};   // kFestRedPacket
        case 46: return {E::kArcUp,      P::kLantern,     G::kNone, H::kNone, false, false};  // kFestLantern
        case 47: return {E::kArcUp,      P::kFireworks,   G::kNone, H::kNone, false, false};  // kFestFireworks
        case 48: return {E::kArcUp,      P::kXmasHat,     G::kNone, H::kNone, false, true};   // kFestXmasHat
        case 49: return {E::kArcUpHigh,  P::kXmasTree,    G::kNone, H::kNone, false, false};  // kFestXmasTree
        case 50: return {E::kArcUpHigh,  P::kXmasGift,    G::kNone, H::kNone, false, false};  // kFestXmasGift
        case 51: return {E::kArcUpHigh,  P::kRose,        G::kNone, H::kNone, false, true};   // kFestRose
        case 52: return {E::kArcUpHigh,  P::kZongzi,      G::kNone, H::kNone, false, false};  // kFestZongzi
        case 53: return {E::kArcUp,      P::kMidAutumnMoon, G::kNone, H::kNone, false, false}; // kFestMidAutumnMoon
        case 54: return {E::kArcUpHigh,  P::kMidAutumnCake, G::kNone, H::kNone, false, false}; // kFestMidAutumnCake
        case 55: return {E::kArcUpHigh,  P::kNationalFlag,  G::kNone, H::kNone, false, true};  // kFestNationalFlag
        case 56: return {E::kArcUpHigh,  P::kHalloweenPumpkin, G::kNone, H::kNone, false, false}; // kFestHalloweenPumpkin
        case 57: return {E::kCapsule,    P::kHalloweenGhost,   G::kNone, H::kNone, false, false}; // kFestHalloweenGhost
        case 58: return {E::kArcUpHigh,  P::kBirthdayCake,  G::kNone, H::kNone, false, false}; // kFestBirthday
        case 59: return {E::kArcUpHigh,  P::kTrophy,      G::kGoldBR, H::kNone, true, true};   // kCelebrate
        // Music & stage instruments (58..67)
        case 60: return {E::kSoulful,    P::kAcousticGuitar, G::kGoldBR, H::kNone, true, true};  // kMusic
        case 61: return {E::kSoulful,    P::kSax,            G::kBlueBR, H::kNone, true, true};  // kMusicSax
        case 62: return {E::kSoulful,    P::kViolin,         G::kBlueBR, H::kNone, true, true};  // kMusicViolin
        case 63: return {E::kArcUpHigh,  P::kElecGuitar,     G::kBlueBR, H::kNone, false, true}; // kMusicElecGuitar
        case 64: return {E::kArcUpHigh,  P::kHandpan,        G::kNone,   H::kNone, false, true}; // kMusicHandpan
        case 65: return {E::kArcUpHigh,  P::kGlowsticks,     G::kNone,   H::kNone, false, true}; // kCheer
        case 66: return {E::kHidden,     P::kHipHop,         G::kDualGold, H::kNone, false, true}; // kMusicHipHop
        case 67: return {E::kArcUpHigh,  P::kDJ,             G::kCyanBR, H::kNone, false, true}; // kMusicDJ
        case 68: return {E::kSoulful,    P::kMic,            G::kBlueBR, H::kNone, false, true}; // kMusicMic
        case 69: return {E::kArcUpHigh,  P::kMusicNotes,     G::kCyanBR, H::kNone, false, true}; // kMusicNotes
        // Daily life & idle theater (68..95)
        case 70: return {E::kArcUpHigh,  P::kWaveHand,    G::kNone, H::kNone, false, true};   // kGreeting
        case 71: return {E::kSleepGray,  P::kMidAutumnMoon, G::kNone, H::kNone, false, false}; // kGoodnight
        case 72: return {E::kWink,       P::kWaveHand,    G::kNone, H::kNone, false, true};   // kGoodbye
        case 73: return {E::kArcUp,      P::kSparkles,    G::kNone, H::kNone, false, false};  // kThankYou
        case 74: return {E::kClockLook,  P::kClockBoard,  G::kNone, H::kNone, false, false};  // kClockBoard
        case 75: return {E::kHidden,     P::kClockGlasses, G::kNone, H::kNone, false, false}; // kClockGlasses
        case 76: return {E::kChaseDot,   P::kChaseDot,    G::kNone, H::kNone, false, false};  // kChaseDot
        case 77: return {E::kNarrow,     P::kTyping,      G::kNone, H::kNone, false, false};  // kTyping
        case 78: return {E::kCapsule,    P::kCoffee,      G::kNone, H::kNone, false, false};  // kCoffee
        case 79: return {E::kArcUpHigh,  P::kBobaTea,     G::kNone, H::kNone, false, false};  // kBobaTea
        case 80: return {E::kCapsule,    P::kMagnifier,   G::kNone, H::kNone, false, false};  // kMagnifier
        case 81: return {E::kArcUp,      P::kHeadphones,  G::kNone, H::kNone, false, true};   // kHeadphones
        case 82: return {E::kArcUpHigh,  P::kPottedPlant, G::kNone, H::kNone, false, false};  // kPottedPlant
        case 83: return {E::kArcUp,      P::kStretchHands, G::kNone, H::kNone, false, false}; // kStretch
        case 84: return {E::kArcUp,      P::kTongue,      G::kNone, H::kNone, false, true};   // kDelicious
        case 85: return {E::kRound,      P::kSparkles,    G::kNone, H::kSolidGold, false, false}; // kAlarm
        case 86: return {E::kNarrow,     P::kNone,        G::kNone, H::kNone, false, false};  // kSecret
        case 87: return {E::kHidden,     P::kSunglasses,  G::kNone, H::kNone, false, true};   // kCool
        case 88: return {E::kArcUp,      P::kCrown,       G::kNone, H::kNone, false, false};  // kConfident
        case 89: return {E::kInterest,   P::kNone,        G::kNone, H::kSolidCyan, false, false}; // kWorkFocus
        case 90: return {E::kNarrow,     P::kSweat,       G::kNone, H::kNone, false, false};  // kTired
        case 91: return {E::kStarry,     P::kSparkles,    G::kGoldBR, H::kSolidGold, false, true}; // kMoney
        case 92: return {E::kSad,        P::kSweat,       G::kNone, H::kSolidGreen, false, false}; // kMarketDown
        case 93: return {E::kDotShock,   P::kSweat,       G::kNone, H::kNone, false, false};  // kScared
        case 94: return {E::kSad,        P::kFaceMask,    G::kNone, H::kNone, false, false};  // kSick
        case 95: return {E::kArcUp,      P::kCrown,       G::kNone, H::kNone, false, true};   // kProud
        case 96: return {E::kNarrow,     P::kNone,        G::kNone, H::kNone, false, false};  // kBored
        case 97:
        default: return {E::kDoubt,      P::kNone,        G::kNone, H::kNone, false, true};   // kCurious
    }
}

bool ExtractTemperatureC(const char* text, int& out_temp) {
    if (text == nullptr) return false;
    const char* p = text;
    while (*p != '\0') {
        bool neg = false;
        if (strncmp(p, "零下", 6) == 0) {
            neg = true;
            p += 6;
        } else if (*p == '-' && p[1] >= '0' && p[1] <= '9') {
            neg = true;
            p++;
        }
        if (*p >= '0' && *p <= '9') {
            int val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            if (strncmp(p, "度", 3) == 0 || strncmp(p, "°", 2) == 0 || strncmp(p, "℃", 3) == 0) {
                out_temp = neg ? -val : val;
                return true;
            }
            continue;
        }
        p++;
    }
    return false;
}

}  // namespace

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xF1F5F9));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0xCBD5E1));
    light_theme->set_low_battery_color(lv_color_hex(0xFF3B30));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x14161A));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00C853));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x1E222A));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0x2D333B));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF4757));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;
    hide_subtitle_ = IsRoundDisplay();

    InitializeLcdThemes();

    Settings settings("display", false);
    std::string default_theme = IsRoundDisplay() ? "dark" : "light";
    std::string theme_name = settings.GetString("theme", default_theme);
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    std::vector<uint16_t> buffer(width_, 0x0000);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
#if CONFIG_LV_USE_BUILTIN_MALLOC
        const size_t extra_total = 2560 * 1024;
        const size_t chunk_size = LV_MEM_SIZE;
        size_t added = 0;
        while (added < extra_total) {
            size_t this_chunk = extra_total - added;
            if (this_chunk > chunk_size) this_chunk = chunk_size;
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) heap_caps_free(pool);
                break;
            }
            added += this_chunk;
        }
#endif
        lv_image_cache_resize(2 * 1024 * 1024, true);
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
    }
#endif

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    lvgl_port_display_cfg_t display_cfg = {};
    display_cfg.io_handle = panel_io_;
    display_cfg.panel_handle = panel_;
    display_cfg.buffer_size = static_cast<uint32_t>(width_ * 20);
    display_cfg.double_buffer = true;
    display_cfg.hres = static_cast<uint32_t>(width_);
    display_cfg.vres = static_cast<uint32_t>(height_);
    display_cfg.rotation.swap_xy = swap_xy;
    display_cfg.rotation.mirror_x = mirror_x;
    display_cfg.rotation.mirror_y = mirror_y;
    display_cfg.flags.buff_dma = 1;
    display_cfg.flags.swap_bytes = 0;
    display_cfg.flags.full_refresh = 1;
    display_cfg.flags.direct_mode = 1;
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {.bb_mode = true, .avoid_tearing = true},
    };
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ != nullptr && (offset_x != 0 || offset_y != 0)) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = panel_io;
    disp_cfg.panel_handle = panel;
    disp_cfg.control_handle = nullptr;
    disp_cfg.buffer_size = static_cast<uint32_t>(width_ * 50);
    disp_cfg.double_buffer = false;
    disp_cfg.hres = static_cast<uint32_t>(width_);
    disp_cfg.vres = static_cast<uint32_t>(height_);
    disp_cfg.monochrome = false;
    disp_cfg.rotation.swap_xy = swap_xy;
    disp_cfg.rotation.mirror_x = mirror_x;
    disp_cfg.rotation.mirror_y = mirror_y;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.buff_spiram = false;
    disp_cfg.flags.sw_rotate = true;
    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {.avoid_tearing = false}};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ != nullptr && (offset_x != 0 || offset_y != 0)) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    if (eye_timer_ != nullptr) {
        lv_timer_delete(eye_timer_);
        eye_timer_ = nullptr;
    }
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }
    if (preview_image_ != nullptr) lv_obj_del(preview_image_);
    if (chat_message_label_ != nullptr) lv_obj_del(chat_message_label_);
    if (emoji_label_ != nullptr) lv_obj_del(emoji_label_);
    if (emoji_image_ != nullptr) lv_obj_del(emoji_image_);
    if (emoji_box_ != nullptr) lv_obj_del(emoji_box_);
    if (nomi_canvas_ != nullptr) lv_obj_del(nomi_canvas_);
    if (content_ != nullptr) lv_obj_del(content_);
    if (bottom_bar_ != nullptr) lv_obj_del(bottom_bar_);
    if (status_bar_ != nullptr) lv_obj_del(status_bar_);
    if (top_bar_ != nullptr) lv_obj_del(top_bar_);
    if (side_bar_ != nullptr) lv_obj_del(side_bar_);
    if (container_ != nullptr) lv_obj_del(container_);
    if (display_ != nullptr) lv_display_delete(display_);
    if (panel_ != nullptr) esp_lcd_panel_del(panel_);
    if (panel_io_ != nullptr) esp_lcd_panel_io_del(panel_io_);
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

void LcdDisplay::SetNomiMood(EyeMood mood) {
    if (mood != current_mood_) {
        prev_mood_ = current_mood_;
        current_mood_ = mood;
        mood_enter_tick_ = anim_tick_;
        blink_ticks_left_ = 0;
    }
    if (nomi_canvas_ != nullptr) {
        lv_obj_invalidate(nomi_canvas_);
    }
}

LcdDisplay::EyeMood LcdDisplay::ParseEmotionMood(const char* emotion) const {
    if (emotion == nullptr) return EyeMood::kNeutral;
    uint32_t v = (anim_tick_ / 11);
    if (strcmp(emotion, "happy") == 0) {
        static const EyeMood kPool[5] = {EyeMood::kHappy, EyeMood::kGreeting, EyeMood::kCelebrate,
                                         EyeMood::kCheer, EyeMood::kStarry};
        return kPool[v % 5];
    }
    if (strcmp(emotion, "laughing") == 0 || strcmp(emotion, "funny") == 0) {
        static const EyeMood kPool[3] = {EyeMood::kLaughing, EyeMood::kSilly, EyeMood::kHappy};
        return kPool[v % 3];
    }
    if (strcmp(emotion, "grinning") == 0) return EyeMood::kGrinning;
    if (strcmp(emotion, "loving") == 0) {
        static const EyeMood kPool[3] = {EyeMood::kLoving, EyeMood::kKissy, EyeMood::kFestRose};
        return kPool[v % 3];
    }
    if (strcmp(emotion, "kissy") == 0) return EyeMood::kKissy;
    if (strcmp(emotion, "winking") == 0) return EyeMood::kWinking;
    if (strcmp(emotion, "sad") == 0) return EyeMood::kSad;
    if (strcmp(emotion, "crying") == 0) return EyeMood::kCrying;
    if (strcmp(emotion, "angry") == 0) return EyeMood::kAngry;
    if (strcmp(emotion, "furious") == 0) return EyeMood::kFurious;
    if (strcmp(emotion, "surprised") == 0) return EyeMood::kSurprised;
    if (strcmp(emotion, "shocked") == 0) return EyeMood::kShocked;
    if (strcmp(emotion, "thinking") == 0) return EyeMood::kThinking;
    if (strcmp(emotion, "confused") == 0) return EyeMood::kConfused;
    if (strcmp(emotion, "embarrassed") == 0) return EyeMood::kEmbarrassed;
    if (strcmp(emotion, "shy") == 0) return EyeMood::kShy;
    if (strcmp(emotion, "silly") == 0) return EyeMood::kSilly;
    if (strcmp(emotion, "sleepy") == 0) return EyeMood::kYawning;
    if (strcmp(emotion, "relaxed") == 0) {
        static const EyeMood kPool[3] = {EyeMood::kCoffee, EyeMood::kHeadphones, EyeMood::kStretch};
        return kPool[v % 3];
    }
    if (strcmp(emotion, "delicious") == 0) return EyeMood::kDelicious;
    if (strcmp(emotion, "cool") == 0) {
        static const EyeMood kPool[3] = {EyeMood::kCool, EyeMood::kMusicHipHop, EyeMood::kConfident};
        return kPool[v % 3];
    }
    if (strcmp(emotion, "confident") == 0) return EyeMood::kConfident;
    if (strcmp(emotion, "music") == 0) {
        static const EyeMood kPool[8] = {EyeMood::kMusic,          EyeMood::kMusicSax,
                                         EyeMood::kMusicViolin,    EyeMood::kMusicElecGuitar,
                                         EyeMood::kMusicHandpan,   EyeMood::kMusicDJ,
                                         EyeMood::kMusicMic,       EyeMood::kMusicNotes};
        return kPool[v % 8];
    }
    if (strcmp(emotion, "curious") == 0) return EyeMood::kCurious;
    if (strcmp(emotion, "scared") == 0) return EyeMood::kScared;
    if (strcmp(emotion, "sick") == 0) return EyeMood::kSick;
    if (strcmp(emotion, "proud") == 0) return EyeMood::kProud;
    if (strcmp(emotion, "bored") == 0) return EyeMood::kBored;
    return EyeMood::kNeutral;
}

bool LcdDisplay::MatchContextualMood(const char* text, EyeMood& out_mood) const {
    if (text == nullptr || text[0] == '\0') return false;
    auto has = [text](const char* kw) { return strstr(text, kw) != nullptr; };

    // 1. Weather & temperature-tiered matching
    if (has("刮风") || has("大风") || has("台风") || has("狂风") || has("风很大")) {
        out_mood = EyeMood::kWeatherWindy;
        return true;
    }
    if (has("下雨") || has("暴雨") || has("阵雨") || has("带伞") || has("阴雨")) {
        out_mood = EyeMood::kRainy;
        return true;
    }
    if (has("下雪") || has("暴雪") || has("结冰")) {
        out_mood = EyeMood::kWeatherSnow;
        return true;
    }
    if (has("雾霾") || has("沙尘") || has("口罩") || has("PM2.5")) {
        out_mood = EyeMood::kWeatherHazeMask;
        return true;
    }
    int temp_c = 0;
    if (ExtractTemperatureC(text, temp_c)) {
        if (temp_c <= 0) {
            out_mood = EyeMood::kWeatherSnow;
            return true;
        }
        if (temp_c <= 10) {
            out_mood = EyeMood::kCold;
            return true;
        }
        if (temp_c >= 35) {
            out_mood = EyeMood::kHot;
            return true;
        }
        if (temp_c >= 30) {
            out_mood = EyeMood::kWeatherHotHat;
            return true;
        }
        if (temp_c >= 18 && temp_c <= 26 && (has("晴") || has("天气") || has("温度"))) {
            out_mood = EyeMood::kSunny;
            return true;
        }
    }
    if (has("好冷") || has("降温") || has("寒冷") || has("围巾")) {
        out_mood = EyeMood::kCold;
        return true;
    }
    if (has("好热") || has("高温") || has("防晒") || has("炎热")) {
        out_mood = EyeMood::kWeatherHotHat;
        return true;
    }
    if (has("春天") || has("春风") || has("风车") || has("踏青") || has("樱花")) {
        out_mood = EyeMood::kSeasonSpring;
        return true;
    }
    if (has("秋天") || has("秋风") || has("落叶") || has("枫叶") || has("深秋")) {
        out_mood = EyeMood::kSeasonAutumn;
        return true;
    }
    if (has("晴天") || has("晴朗") || has("阳光")) {
        out_mood = EyeMood::kSunny;
        return true;
    }

    // 2. Year-round split festival easter eggs (1 independent prop per trigger!)
    if (has("鞭炮") || has("除夕") || has("过年")) {
        out_mood = EyeMood::kFestFirecracker;
        return true;
    }
    if (has("红包") || has("拜年") || has("恭喜发财")) {
        out_mood = EyeMood::kFestRedPacket;
        return true;
    }
    if (has("元宵") || has("灯笼")) {
        out_mood = EyeMood::kFestLantern;
        return true;
    }
    if (has("跨年") || has("元旦") || has("烟花") || has("新年快乐")) {
        out_mood = EyeMood::kFestFireworks;
        return true;
    }
    if (has("圣诞")) {
        static const EyeMood kXmasSplit[3] = {EyeMood::kFestXmasHat, EyeMood::kFestXmasTree,
                                              EyeMood::kFestXmasGift};
        out_mood = kXmasSplit[(anim_tick_ / 7) % 3];
        return true;
    }
    if (has("情人节") || has("七夕") || has("520") || has("玫瑰")) {
        out_mood = EyeMood::kFestRose;
        return true;
    }
    if (has("端午") || has("粽子")) {
        out_mood = EyeMood::kFestZongzi;
        return true;
    }
    if (has("月饼")) {
        out_mood = EyeMood::kFestMidAutumnCake;
        return true;
    }
    if (has("中秋") || has("赏月")) {
        out_mood = EyeMood::kFestMidAutumnMoon;
        return true;
    }
    if (has("国庆")) {
        out_mood = EyeMood::kFestNationalFlag;
        return true;
    }
    if (has("万圣")) {
        out_mood = ((anim_tick_ % 2) == 0) ? EyeMood::kFestHalloweenPumpkin
                                           : EyeMood::kFestHalloweenGhost;
        return true;
    }
    if (has("生日")) {
        out_mood = EyeMood::kFestBirthday;
        return true;
    }
    if (has("恭喜") || has("赢了") || has("奖杯") || has("庆祝")) {
        out_mood = EyeMood::kCelebrate;
        return true;
    }

    // 3. Music & stage instruments
    if (has("萨克斯") || has("爵士")) {
        out_mood = EyeMood::kMusicSax;
        return true;
    }
    if (has("小提琴") || has("古典")) {
        out_mood = EyeMood::kMusicViolin;
        return true;
    }
    if (has("摇滚") || has("电吉他")) {
        out_mood = EyeMood::kMusicElecGuitar;
        return true;
    }
    if (has("手碟") || has("敲鼓")) {
        out_mood = EyeMood::kMusicHandpan;
        return true;
    }
    if (has("嘻哈") || has("说唱") || has("rap")) {
        out_mood = EyeMood::kMusicHipHop;
        return true;
    }
    if (has("打碟") || has("DJ") || has("电音")) {
        out_mood = EyeMood::kMusicDJ;
        return true;
    }
    if (has("唱歌") || has("K歌")) {
        out_mood = EyeMood::kMusicMic;
        return true;
    }
    if (has("音乐") || has("听歌") || has("歌曲") || has("吉他")) {
        static const EyeMood kMusicPool[5] = {EyeMood::kMusic, EyeMood::kMusicSax,
                                              EyeMood::kMusicViolin, EyeMood::kMusicElecGuitar,
                                              EyeMood::kMusicNotes};
        out_mood = kMusicPool[(anim_tick_ / 5) % 5];
        return true;
    }

    // 4. Daily scenarios & personality
    if (has("几点") || has("时间") || has("报时")) {
        out_mood = ((anim_tick_ % 2) == 0) ? EyeMood::kClockBoard : EyeMood::kClockGlasses;
        return true;
    }
    if (has("加油") || has("打气") || has("应援") || has("冲鸭")) {
        out_mood = EyeMood::kCheer;
        return true;
    }
    if (has("写代码") || has("上班") || has("搬砖") || has("键盘")) {
        out_mood = EyeMood::kTyping;
        return true;
    }
    if (has("奶茶") || has("摸鱼") || has("下午茶")) {
        out_mood = EyeMood::kBobaTea;
        return true;
    }
    if (has("咖啡") || has("拿铁") || has("美式") || has("提神")) {
        out_mood = EyeMood::kCoffee;
        return true;
    }
    if (has("查查") || has("搜索") || has("找一下")) {
        out_mood = EyeMood::kMagnifier;
        return true;
    }
    if (has("绿植") || has("浇花") || has("植物")) {
        out_mood = EyeMood::kPottedPlant;
        return true;
    }
    if (has("伸懒腰") || has("久坐")) {
        out_mood = EyeMood::kStretch;
        return true;
    }
    if (has("早安") || has("早上好") || has("你好") || has("哈喽")) {
        out_mood = EyeMood::kGreeting;
        return true;
    }
    if (has("晚安") || has("好梦") || has("睡觉")) {
        out_mood = EyeMood::kGoodnight;
        return true;
    }
    if (has("再见") || has("拜拜")) {
        out_mood = EyeMood::kGoodbye;
        return true;
    }
    if (has("谢谢") || has("感谢")) {
        out_mood = EyeMood::kThankYou;
        return true;
    }
    if (has("爱你") || has("喜欢你") || has("比心")) {
        out_mood = EyeMood::kLoving;
        return true;
    }
    if (has("墨镜") || has("好酷") || has("帅气")) {
        out_mood = EyeMood::kCool;
        return true;
    }
    if (has("发财") || has("大涨") || has("涨停")) {
        out_mood = EyeMood::kMoney;
        return true;
    }
    if (has("大跌") || has("跌停") || has("亏钱")) {
        out_mood = EyeMood::kMarketDown;
        return true;
    }
    if (has("哈哈") || has("笑死")) {
        out_mood = EyeMood::kLaughing;
        return true;
    }
    return false;
}

LcdDisplay::EyeMood LcdDisplay::PickIdleTheaterMood() const {
    time_t now = std::time(nullptr);
    struct tm t_info;
    localtime_r(&now, &t_info);
    int month = t_info.tm_mon + 1;
    int day = t_info.tm_mday;

    // Automatic date-based split festival easter eggs if SNTP clock is synced (year >= 2024)
    if (t_info.tm_year + 1900 >= 2024 && ((anim_tick_ / 17) % 3 == 0)) {
        if (month == 12 && day >= 24 && day <= 26) {
            static const EyeMood kXmas[3] = {EyeMood::kFestXmasHat, EyeMood::kFestXmasTree,
                                             EyeMood::kFestXmasGift};
            return kXmas[(anim_tick_ / 29) % 3];
        }
        if (month == 1 && day <= 3) return EyeMood::kFestFireworks;
        if (month == 2 && day == 14) return EyeMood::kFestRose;
        if (month == 10 && day <= 7) return EyeMood::kFestNationalFlag;
        if (month == 10 && day >= 30) {
            return ((anim_tick_ / 29) % 2 == 0) ? EyeMood::kFestHalloweenPumpkin
                                                : EyeMood::kFestHalloweenGhost;
        }
    }

    static const EyeMood kTheater[] = {
        EyeMood::kClockBoard,   EyeMood::kChaseDot,      EyeMood::kHeadphones,
        EyeMood::kMusic,        EyeMood::kStretch,       EyeMood::kClockGlasses,
        EyeMood::kCoffee,       EyeMood::kMusicSax,      EyeMood::kBobaTea,
        EyeMood::kPottedPlant,  EyeMood::kMusicHandpan,  EyeMood::kTyping,
        EyeMood::kSunny,        EyeMood::kCool,          EyeMood::kMusicViolin,
        EyeMood::kSeasonSpring, EyeMood::kSeasonAutumn,  EyeMood::kCheer,
        EyeMood::kMusicDJ,      EyeMood::kMagnifier,     EyeMood::kStarry,
        EyeMood::kWinking,      EyeMood::kMusicElecGuitar, EyeMood::kGreeting,
        EyeMood::kLoving,       EyeMood::kCelebrate,     EyeMood::kMusicHipHop,
        EyeMood::kConfident,    EyeMood::kSilly,         EyeMood::kWeatherWindy,
        EyeMood::kRainy,        EyeMood::kWeatherSnow,
    };
    static uint32_t s_idle_seq = 0;
    s_idle_seq = (s_idle_seq + 1 + (anim_tick_ % 3)) % (sizeof(kTheater) / sizeof(kTheater[0]));
    return kTheater[s_idle_seq];
}

void LcdDisplay::StartMoodShowcase() {
    showcase_active_ = true;
    showcase_index_ = 0;
    showcase_start_tick_ = anim_tick_;
    showcase_next_tick_ = anim_tick_ + 55;  // ~1.8s per expression
    last_activity_tick_ = anim_tick_;
    SetNomiMood(static_cast<EyeMood>(0));
    ShowEmotion();
}

void LcdDisplay::StopMoodShowcase() {
    if (!showcase_active_) return;
    showcase_active_ = false;
    last_activity_tick_ = anim_tick_;
    SetNomiMood(EyeMood::kHappy);
}

bool LcdDisplay::HandleVoiceDisplayCommand(const char* text) {
    if (text == nullptr || text[0] == '\0') return false;
    auto has = [text](const char* kw) { return strstr(text, kw) != nullptr; };

    // 1. Subtitle visibility voice toggle
    if (has("打开字幕") || has("显示字幕") || has("开启字幕")) {
        hide_subtitle_ = false;
        return true;
    }
    if (has("关闭字幕") || has("隐藏字幕") || has("不要字幕")) {
        hide_subtitle_ = true;
        if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        subtitle_visible_ = false;
        return true;
    }

    // 2. Stop showcase command
    if (showcase_active_ &&
        (has("停止展示") || has("停止演示") || has("停止轮播") || has("退出展示") ||
         has("退出表演") || has("别演了") || has("停下来"))) {
        StopMoodShowcase();
        return true;
    }

    // 3. Trigger full expression showcase ("把所有表情显示一遍" / "一边")
    if (has("所有表情") || has("全部表情") || has("表情显示一遍") || has("表情显示一边") ||
        has("表情都显示") || has("展示表情") || has("演示表情") || has("表演表情") ||
        has("表情秀") || has("表情演示") || has("轮播表情") || has("展示全部") ||
        has("show all") || has("SHOW ALL")) {
        if (!showcase_active_) {
            StartMoodShowcase();
        }
        return true;
    }
    return false;
}

void LcdDisplay::CreateEyeUI(lv_obj_t* screen, LvglTheme* lvgl_theme) {
    (void)lvgl_theme;
    nomi_canvas_ = lv_obj_create(screen);
    lv_obj_set_size(nomi_canvas_, width_, height_);
    lv_obj_set_style_bg_color(nomi_canvas_, lv_color_hex(kColBlack), 0);
    lv_obj_set_style_bg_opa(nomi_canvas_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nomi_canvas_, 0, 0);
    lv_obj_set_style_pad_all(nomi_canvas_, 0, 0);
    lv_obj_set_scrollbar_mode(nomi_canvas_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(nomi_canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(nomi_canvas_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(nomi_canvas_, NomiDrawEventCb, LV_EVENT_DRAW_MAIN, this);

    if (eye_timer_ == nullptr) {
        // 33ms (~30 FPS): sweet spot for ESP32-S3 GC9A01 + Xiaozhi voice tasks
        eye_timer_ = lv_timer_create(EyeTimerCallback, 33, this);
    }
}

void LcdDisplay::NomiDrawEventCb(lv_event_t* e) {
    auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    lv_layer_t* layer = lv_event_get_layer(e);
    if (layer != nullptr) {
        self->RenderNomiFrame(layer);
    }
}

void LcdDisplay::RenderNomiFrame(lv_layer_t* layer) {
    const float R = width_ * 0.5f;  // 120.0f on 240x240
    g_nomi_origin = R;
    g_nomi_scale = 1.0f;
    const float t_global = anim_tick_ * 0.033f;
    const float age = (anim_tick_ - mood_enter_tick_) * 0.033f;
    const float sub_shift_y = subtitle_visible_ ? -18.0f : 0.0f;

    NomiMoodCfg cfg = GetNomiMoodCfg(static_cast<int>(current_mood_));

    // 1. 3D spherical-orbit depth parameters (近大远小: 0.76x sides <-> 1.14x front-center)
    float bt = std::max(0.0f, age - 0.36f);
    float orb_ang = std::sin(bt * 2.3f) * 1.12f;
    float lag_ang = std::sin(std::max(0.0f, bt - 0.08f) * 2.3f) * 1.12f;
    float depth_z = std::cos(lag_ang);
    float depth_u = ClampF((depth_z - 0.43f) / 0.57f);
    float persp_s = cfg.orbit_3d ? LerpF(0.76f, 1.14f, SmoothStep(depth_u)) : 1.0f;
    float persp_sx = cfg.orbit_3d ? persp_s * LerpF(0.86f, 1.0f, depth_u) : 1.0f;
    float persp_sy = persp_s;
    float persp_x = cfg.orbit_3d ? std::sin(lag_ang) * 26.0f : 0.0f;
    float persp_y = cfg.orbit_3d ? -(1.0f - depth_u) * 10.0f : 0.0f;

    // 2. Feathered soft ambient stage glow drifting in from bottom-right
    float glow_env = SmoothStep((age - 0.24f) / 0.34f);
    if (cfg.glow == NomiGlowKind::kBlueBR) {
        DrawFeatheredGlow(layer, R + 42.0f + persp_x * 0.4f, R + 64.0f, 88.0f, kColBlue, glow_env);
    } else if (cfg.glow == NomiGlowKind::kGoldBR) {
        DrawFeatheredGlow(layer, R + 42.0f + persp_x * 0.4f, R + 64.0f, 84.0f, kColGold, glow_env);
    } else if (cfg.glow == NomiGlowKind::kCyanBR) {
        DrawFeatheredGlow(layer, R + 42.0f + persp_x * 0.4f, R + 64.0f, 84.0f, kColCyan, glow_env);
    } else if (cfg.glow == NomiGlowKind::kDualGold) {
        DrawFeatheredGlow(layer, R - 52.0f, R + 74.0f, 56.0f, kColGold, glow_env);
        DrawFeatheredGlow(layer, R + 52.0f, R + 74.0f, 56.0f, kColGold, glow_env);
    }

    // 3. Outer circular status halo (R = 114px)
    if (cfg.halo != NomiHaloKind::kNone) {
        if (cfg.halo == NomiHaloKind::kListenPulse) {
            float w = 2.6f + (0.5f + 0.5f * std::sin(t_global * 7.0f)) * 2.4f;
            DrawArcSeg(layer, R, R, 114.0f, 0, 360, w, lv_color_hex(kColCyan), LV_OPA_COVER);
        } else if (cfg.halo == NomiHaloKind::kSpinAmber) {
            float a0 = std::fmod(t_global * 240.0f, 360.0f);
            DrawArcSeg(layer, R, R, 114.0f, a0, a0 + 115.0f, 3.2f, lv_color_hex(kColGold),
                       LV_OPA_COVER);
        } else if (cfg.halo == NomiHaloKind::kBlinkRed) {
            float pulse = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(t_global * 8.0f));
            DrawArcSeg(layer, R, R, 114.0f, 0, 360, 3.2f, lv_color_hex(kColRed), ToOpa(pulse));
        } else {
            uint32_t hc = (cfg.halo == NomiHaloKind::kSolidGreen)   ? kColGreen
                          : (cfg.halo == NomiHaloKind::kSolidCoral) ? kColRed
                          : (cfg.halo == NomiHaloKind::kSolidGold)  ? kColGold
                                                                    : kColCyan;
            float pulse = 0.45f + 0.4f * (0.5f + 0.5f * std::sin(t_global * 4.0f));
            DrawArcSeg(layer, R, R, 114.0f, 0, 360, 2.8f, lv_color_hex(hc), ToOpa(pulse));
        }
    }

    // Showcase progress ring (sleek thin cyan arc at bezel R=114, zero text clutter)
    if (showcase_active_) {
        float prog = ClampF(static_cast<float>(showcase_index_ + 1) /
                            static_cast<float>(static_cast<int>(EyeMood::kMoodCount)));
        DrawArcSeg(layer, R, R, 114.0f, 270.0f, 270.0f + prog * 360.0f, 3.0f,
                   lv_color_hex(kColCyan), ToOpa(0.8f));
    }

    // Enlarge all NOMI eyes & props to fill the 240x240 circular screen cleanly
    g_nomi_scale = subtitle_visible_ ? 1.08f : 1.22f;

    // 4. Coordinated eye tilt & follow offsets
    float eye_x = 0.0f, eye_y = std::sin(t_global * 2.2f) * 1.5f, eye_tilt_deg = 0.0f;
    if (cfg.eye_tilt_sync) {
        if (cfg.orbit_3d) {
            eye_x = std::sin(orb_ang) * 8.5f;
            eye_tilt_deg = std::sin(orb_ang) * 10.0f;
            eye_y = -depth_u * 2.8f;
        } else {
            float s = std::sin(bt * 6.2f);
            eye_x = s * 7.5f;
            eye_tilt_deg = s * 10.5f;
            eye_y = -std::fabs(s) * 3.0f;
        }
    }

    // Blink factor for capsule/interest/round eyes
    float blink_f = 1.0f;
    if (blink_ticks_left_ > 0) {
        float bp = 1.0f - (blink_ticks_left_ / 5.0f);
        blink_f = std::max(0.06f, std::fabs(std::cos(bp * kPi)));
    }

    // 5. Silky Collapse-to-Dot -> Bloom Eye Engine (0..0.26s morph, then active)
    const bool is_high = (cfg.eye == NomiEyeKind::kArcUpHigh || cfg.eye == NomiEyeKind::kSoulful ||
                          cfg.eye == NomiEyeKind::kClockLook ||
                          cfg.prop == NomiPropKind::kFaceMask ||
                          cfg.prop == NomiPropKind::kColdScarf ||
                          cfg.prop == NomiPropKind::kCoffee || cfg.prop == NomiPropKind::kTyping);
    const float base_y = (is_high ? -26.0f : (cfg.eye == NomiEyeKind::kArcUp ? 2.0f : 0.0f)) +
                         sub_shift_y;
    const float shift_x = (cfg.prop == NomiPropKind::kUmbrella || cfg.prop == NomiPropKind::kSunny ||
                           cfg.prop == NomiPropKind::kLantern ||
                           cfg.prop == NomiPropKind::kMidAutumnMoon ||
                           cfg.prop == NomiPropKind::kHalloweenGhost || cfg.prop == NomiPropKind::kPinwheel)
                              ? -22.0f
                              : (cfg.prop == NomiPropKind::kWindPole ? 16.0f : 0.0f);
    const float dx = (cfg.eye == NomiEyeKind::kInterest) ? 35.0f
                     : (cfg.eye == NomiEyeKind::kClockLook ? 26.0f
                                                           : (shift_x != 0.0f ? 30.0f : 42.0f));
    const float trans_dur = (cfg.eye == NomiEyeKind::kHidden || cfg.eye == NomiEyeKind::kChaseDot ||
                             prev_mood_ == current_mood_)
                                ? 0.0f
                                : 0.26f;

    lv_color_t col_w = lv_color_hex(kColWhite);
    lv_color_t col_gray = lv_color_hex(kColGray);

    if (cfg.eye != NomiEyeKind::kHidden) {
        if (age < trans_dur) {
            float u = age / trans_dur;
            float cy = LerpF(R + sub_shift_y, R + base_y, SmoothStep(u));
            float cdx = LerpF(44.0f, dx, SmoothStep(u));
            float csx = LerpF(0.0f, shift_x, SmoothStep(u));
            if (u < 0.45f) {
                float k = EaseInOutCubic(u / 0.45f);
                float h = LerpF(42.0f, 0.0f, k);
                if (h <= 3.0f) {
                    DrawLineSeg(layer, R - cdx + csx - 10, cy, R - cdx + csx + 10, cy, 4.6f, col_w);
                    DrawLineSeg(layer, R + cdx + csx - 10, cy, R + cdx + csx + 10, cy, 4.6f, col_w);
                } else {
                    DrawStrokeRoundRect(layer, R - cdx + csx, cy, 20.0f, h, std::min(10.0f, h * 0.5f),
                                        4.6f, col_w);
                    DrawStrokeRoundRect(layer, R + cdx + csx, cy, 20.0f, h, std::min(10.0f, h * 0.5f),
                                        4.6f, col_w);
                }
            } else if (u < 0.80f) {
                float k = EaseInOutCubic((u - 0.45f) / 0.35f);
                float hw = LerpF(10.0f, 0.0f, k);
                if (hw <= 1.5f) {
                    DrawFillCircle(layer, R - cdx + csx, cy, 2.6f, col_w);
                    DrawFillCircle(layer, R + cdx + csx, cy, 2.6f, col_w);
                } else {
                    DrawLineSeg(layer, R - cdx + csx - hw, cy, R - cdx + csx + hw, cy, 4.6f, col_w);
                    DrawLineSeg(layer, R + cdx + csx - hw, cy, R + cdx + csx + hw, cy, 4.6f, col_w);
                }
            } else {
                DrawFillCircle(layer, R - cdx + csx, cy, 2.6f, col_w);
                DrawFillCircle(layer, R + cdx + csx, cy, 2.6f, col_w);
            }
        } else {
            float bu = ClampF((age - trans_dur) / 0.18f);
            float sc = LerpF(0.18f, 1.0f, EaseOutBack(bu, 1.15f));
            float tilt_rad = eye_tilt_deg * kPi / 180.0f;
            float cx_pair = R + shift_x + eye_x;
            float cy_pair = R + base_y + eye_y;
            float lx, ly, rx, ry;
            RotPt(cx_pair, cy_pair, -dx, 0.0f, tilt_rad, 1.0f, 1.0f, lx, ly);
            RotPt(cx_pair, cy_pair, dx, 0.0f, tilt_rad, 1.0f, 1.0f, rx, ry);

            switch (cfg.eye) {
                case NomiEyeKind::kCapsule:
                case NomiEyeKind::kInterest: {
                    float w = (cfg.eye == NomiEyeKind::kInterest) ? 22.0f : 20.0f;
                    float h = ((cfg.eye == NomiEyeKind::kInterest) ? 46.0f : 42.0f) * blink_f;
                    if (h <= 4.0f) {
                        DrawLineSeg(layer, lx - 10, ly, lx + 10, ly, 4.6f, col_w);
                        DrawLineSeg(layer, rx - 10, ry, rx + 10, ry, 4.6f, col_w);
                    } else {
                        DrawStrokeRoundRect(layer, lx, ly, w, h, w * 0.5f, 4.6f, col_w);
                        DrawStrokeRoundRect(layer, rx, ry, w, h, w * 0.5f, 4.6f, col_w);
                    }
                    break;
                }
                case NomiEyeKind::kArcUp:
                case NomiEyeKind::kArcUpHigh:
                    DrawArcSeg(layer, lx, ly + 6.0f * sc, 16.0f * sc, 195 + eye_tilt_deg,
                               345 + eye_tilt_deg, 4.6f, col_w);
                    DrawArcSeg(layer, rx, ry + 6.0f * sc, 16.0f * sc, 195 + eye_tilt_deg,
                               345 + eye_tilt_deg, 4.6f, col_w);
                    break;
                case NomiEyeKind::kSoulful:
                    DrawArcSeg(layer, lx, ly - 6.0f * sc, 16.5f * sc, 20 + eye_tilt_deg,
                               160 + eye_tilt_deg, 4.6f, col_w);
                    DrawArcSeg(layer, rx, ry - 6.0f * sc, 16.5f * sc, 20 + eye_tilt_deg,
                               160 + eye_tilt_deg, 4.6f, col_w);
                    break;
                case NomiEyeKind::kSleepGray:
                    DrawArcSeg(layer, lx, ly - 5.0f * sc, 15.5f * sc, 20, 160, 4.0f, col_gray);
                    DrawArcSeg(layer, rx, ry - 5.0f * sc, 15.5f * sc, 20, 160, 4.0f, col_gray);
                    break;
                case NomiEyeKind::kWink:
                    DrawStrokeRoundRect(layer, lx, ly, 20.0f, 40.0f * sc, 10.0f, 4.6f, col_w);
                    DrawArcSeg(layer, rx, ry + 6.0f * sc, 16.0f * sc, 195, 345, 4.6f, col_w);
                    break;
                case NomiEyeKind::kRound:
                    DrawStrokeRoundRect(layer, lx, ly, 28.0f * sc, 28.0f * sc * blink_f, 14.0f,
                                        4.6f, col_w);
                    DrawStrokeRoundRect(layer, rx, ry, 28.0f * sc, 28.0f * sc * blink_f, 14.0f,
                                        4.6f, col_w);
                    break;
                case NomiEyeKind::kDotShock:
                    DrawStrokeRoundRect(layer, lx, ly, 14.0f * sc, 14.0f * sc, 7.0f, 4.4f, col_w);
                    DrawStrokeRoundRect(layer, rx, ry, 14.0f * sc, 14.0f * sc, 7.0f, 4.4f, col_w);
                    break;
                case NomiEyeKind::kFlat:
                    DrawLineSeg(layer, lx - 12 * sc, ly, lx + 12 * sc, ly, 4.6f, col_w);
                    DrawLineSeg(layer, rx - 12 * sc, ry, rx + 12 * sc, ry, 4.6f, col_w);
                    break;
                case NomiEyeKind::kNarrow:
                    DrawStrokeRoundRect(layer, lx, ly, 22.0f * sc, 13.0f * sc, 6.5f, 4.4f, col_w);
                    DrawStrokeRoundRect(layer, rx, ry, 22.0f * sc, 13.0f * sc, 6.5f, 4.4f, col_w);
                    break;
                case NomiEyeKind::kSad:
                    DrawLineSeg(layer, lx - 12 * sc, ly + 4, lx + 10 * sc, ly - 4, 4.6f, col_w);
                    DrawLineSeg(layer, rx - 10 * sc, ry - 4, rx + 12 * sc, ry + 4, 4.6f, col_w);
                    break;
                case NomiEyeKind::kAngry:
                    DrawLineSeg(layer, lx - 12 * sc, ly - 5, lx + 11 * sc, ly + 4, 4.8f, col_w);
                    DrawLineSeg(layer, rx - 11 * sc, ry + 4, rx + 12 * sc, ry - 5, 4.8f, col_w);
                    break;
                case NomiEyeKind::kDoubt:
                    DrawStrokeRoundRect(layer, lx, ly + 5, 24.0f * sc, 13.0f * sc, 6.5f, 4.5f, col_w);
                    DrawStrokeRoundRect(layer, rx, ry - 4, 20.0f * sc, 42.0f * sc, 10.0f, 4.5f, col_w);
                    break;
                case NomiEyeKind::kStarry: {
                    lv_color_t sc_col = (current_mood_ == EyeMood::kMoney) ? lv_color_hex(kColGold) : col_w;
                    DrawSparkle4(layer, lx, ly, 16.0f * sc, t_global * 38.0f, sc_col, LV_OPA_COVER, 3.0f);
                    DrawSparkle4(layer, rx, ry, 16.0f * sc, -t_global * 38.0f, sc_col, LV_OPA_COVER, 3.0f);
                    break;
                }
                case NomiEyeKind::kWindSquint:
                    // > < braced against wind
                    DrawLineSeg(layer, lx - 8, ly - 8, lx + 6, ly, 4.5f, col_w);
                    DrawLineSeg(layer, lx + 6, ly, lx - 8, ly + 8, 4.5f, col_w);
                    DrawLineSeg(layer, rx + 8, ry - 8, rx - 6, ry, 4.5f, col_w);
                    DrawLineSeg(layer, rx - 6, ry, rx + 8, ry + 8, 4.5f, col_w);
                    break;
                case NomiEyeKind::kClockLook: {
                    float look_y = (age > 0.35f && age < 0.90f) ? 5.0f : 0.0f;
                    DrawStrokeRoundRect(layer, R - 26, R - 46 + sub_shift_y + look_y, 15,
                                        28 * blink_f, 7.5f, 3.8f, col_w);
                    DrawStrokeRoundRect(layer, R + 26, R - 46 + sub_shift_y + look_y, 15,
                                        28 * blink_f, 7.5f, 3.8f, col_w);
                    break;
                }
                case NomiEyeKind::kChaseDot: {
                    float dot_x = std::sin(t_global * 2.1f) * 56.0f;
                    float dot_y = std::sin(t_global * 4.2f) * 30.0f;
                    DrawStrokeRoundRect(layer, R - 42 + dot_x * 0.25f, R + sub_shift_y + dot_y * 0.25f,
                                        20, 42, 10, 4.6f, col_w);
                    DrawStrokeRoundRect(layer, R + 42 + dot_x * 0.25f, R + sub_shift_y + dot_y * 0.25f,
                                        20, 42, 10, 4.6f, col_w);
                    break;
                }
                case NomiEyeKind::kHidden:
                default:
                    break;
            }
        }
    }

    // 6. Staggered Prop "拿出来 -> 使用" Renderer (120ms stagger + easeOutBack pop)
    const float p_age = std::max(0.0f, age - (trans_dur > 0.0f ? 0.32f : 0.04f));
    const float p_alpha = SmoothStep(p_age / 0.18f);
    const float p_pop = EaseOutBack(ClampF(p_age / 0.24f), 1.25f);
    const lv_opa_t p_opa = ToOpa(p_alpha * (cfg.orbit_3d ? LerpF(0.76f, 1.0f, depth_u) : 1.0f));
    const float py0 = R + sub_shift_y;

    if (p_age > 0.0f && p_opa >= 8) {
        lv_color_t c_coral = lv_color_hex(kColCoral);
        lv_color_t c_red = lv_color_hex(kColRed);
        lv_color_t c_cyan = lv_color_hex(kColCyan);
        lv_color_t c_gold = lv_color_hex(kColGold);
        lv_color_t c_green = lv_color_hex(kColGreen);

        switch (cfg.prop) {
            case NomiPropKind::kSparkles: {
                float s1 = 2.0f + 10.5f * (0.5f + 0.5f * std::sin(p_age * 6.0f - 0.8f));
                float s2 = 2.0f + 12.0f * (0.5f + 0.5f * std::sin(p_age * 6.0f + 1.1f));
                float s3 = 1.5f + 7.0f * (0.5f + 0.5f * std::sin(p_age * 6.0f + 3.0f));
                DrawSparkle4(layer, R + 12, py0 - 26, s1 * p_alpha, 12, col_w, p_opa, 2.2f);
                DrawSparkle4(layer, R + 34, py0 - 42, s2 * p_alpha, -10, col_w, p_opa, 2.3f);
                DrawSparkle4(layer, R + 18, py0 - 56, s3 * p_alpha, 16, col_w, p_opa, 2.0f);
                break;
            }
            case NomiPropKind::kDoubleHeart: {
                float u = ClampF(p_age / 0.34f);
                float gap = LerpF(22.0f, 0.0f, EaseInOutCubic(u));
                float beat = (u >= 1.0f) ? std::pow(std::max(0.0f, std::sin(t_global * 5.2f)), 2.0f) * 0.09f : 0.0f;
                float sc = p_pop + beat;
                // Two cupped solid white paws enclosing a crisp black negative-space heart!
                DrawFillRoundRect(layer, R - gap - 14 * sc, py0 + 34, 28 * sc, 32 * sc, 13 * sc, col_w, p_opa);
                DrawFillRoundRect(layer, R + gap + 14 * sc, py0 + 34, 28 * sc, 32 * sc, 13 * sc, col_w, p_opa);
                if (gap < 6.0f) {
                    // Carve crisp heart negative space in the center
                    lv_color_t blk = lv_color_hex(kColBlack);
                    DrawFillCircle(layer, R - 5.5f * sc, py0 + 30, 6.2f * sc, blk, p_opa);
                    DrawFillCircle(layer, R + 5.5f * sc, py0 + 30, 6.2f * sc, blk, p_opa);
                    DrawLineSeg(layer, R - 9.0f * sc, py0 + 32, R, py0 + 42, 7.0f * sc, blk, p_opa);
                    DrawLineSeg(layer, R + 9.0f * sc, py0 + 32, R, py0 + 42, 7.0f * sc, blk, p_opa);
                }
                break;
            }
            case NomiPropKind::kFloatHeart: {
                float rise = SmoothStep(ClampF(p_age / 0.40f));
                float hy = py0 - 12.0f - rise * 20.0f + std::sin(t_global * 3.0f) * 2.0f;
                DrawArcSeg(layer, R + 50, hy, 5.5f, 180, 360, 2.6f, col_w, p_opa);
                DrawArcSeg(layer, R + 60, hy, 5.5f, 180, 360, 2.6f, col_w, p_opa);
                DrawLineSeg(layer, R + 45, hy + 1, R + 55, hy + 12, 2.6f, col_w, p_opa);
                DrawLineSeg(layer, R + 65, hy + 1, R + 55, hy + 12, 2.6f, col_w, p_opa);
                break;
            }
            case NomiPropKind::kFirecracker: {
                // 1:1 640(7).gif: Hooked white rod + slanted down-left upward-chevron (/ \) hollow-red braid
                float ox = R + 28.0f, oy = py0 + 22.0f;
                DrawLineSeg(layer, ox, oy, ox - 60, oy - 13, 3.6f, col_w, p_opa);
                DrawLineSeg(layer, ox - 60, oy - 13, ox - 63, oy - 2, 3.6f, col_w, p_opa);
                float cyc = std::fmod(p_age, 2.6f) / 2.6f;
                int rem = 5 - static_cast<int>(ClampF(cyc / 0.80f) * 5.0f);
                float sw = std::sin(t_global * 10.0f) * 2.5f;
                for (int s = 0; s < rem; s++) {
                    float px = ox - 63.0f - s * 3.0f + sw * ((s + 1) / 5.0f);
                    float py = oy + 2.0f + s * 9.5f;
                    // Upward-pointing chevron (/ \) pair
                    DrawLineSeg(layer, px - 2, py - 4, px - 10, py + 4, 4.2f, c_red, p_opa);
                    DrawLineSeg(layer, px + 2, py - 4, px + 10, py + 4, 4.2f, c_red, p_opa);
                }
                if (rem > 0) {
                    float px = ox - 63.0f - (rem - 1) * 3.0f + sw;
                    float py = oy + 8.0f + (rem - 1) * 9.5f;
                    for (int k = 0; k < 5; k++) {
                        float a = (20 + k * 35) * kPi / 180.0f;
                        DrawLineSeg(layer, px + std::cos(a) * 5, py + std::sin(a) * 5,
                                    px + std::cos(a) * 13, py + std::sin(a) * 13, 2.2f, c_gold, p_opa);
                    }
                }
                break;
            }
            case NomiPropKind::kRedPacket: {
                float b = std::sin(t_global * 4.5f) * 4.0f;
                DrawFillRoundRect(layer, R - 24, py0 + 32 + b, 24 * p_pop, 34 * p_pop, 4, c_red, p_opa);
                DrawFillCircle(layer, R - 24, py0 + 28 + b, 4.2f * p_pop, c_gold, p_opa);
                DrawFillRoundRect(layer, R + 4, py0 + 38 - b * 0.7f, 24 * p_pop, 34 * p_pop, 4, c_red, p_opa);
                DrawFillCircle(layer, R + 4, py0 + 34 - b * 0.7f, 4.2f * p_pop, c_gold, p_opa);
                break;
            }
            case NomiPropKind::kLantern: {
                float sw = std::sin(t_global * 4.4f) * 6.0f;
                float lx = R + 54.0f + sw, ly = py0 - 10.0f;
                DrawLineSeg(layer, R + 54, py0 - 44, lx, ly - 16, 2.4f, c_gold, p_opa);
                DrawFillRoundRect(layer, lx, ly, 28 * p_pop, 32 * p_pop, 14 * p_pop, c_red, p_opa);
                DrawLineSeg(layer, lx - 8, ly - 16, lx + 8, ly - 16, 2.6f, c_gold, p_opa);
                DrawLineSeg(layer, lx - 8, ly + 16, lx + 8, ly + 16, 2.6f, c_gold, p_opa);
                DrawLineSeg(layer, lx, ly + 16, lx, ly + 32, 2.4f, c_gold, p_opa);
                break;
            }
            case NomiPropKind::kFireworks: {
                static const float kFx[3][3] = {{-46, -42, 0.0f}, {46, -38, 0.35f}, {2, -60, 0.7f}};
                static const uint32_t kFc[3] = {kColCoral, kColCyan, kColGold};
                for (int b = 0; b < 3; b++) {
                    float ph = std::fmod(p_age * 0.9f + kFx[b][2], 1.0f);
                    lv_opa_t fo = ToOpa(std::sin(ph * kPi) * p_alpha);
                    float r1 = ph * 9.0f, r2 = 4.0f + ph * 18.0f;
                    for (int k = 0; k < 8; k++) {
                        float a = k * kPi * 0.25f;
                        DrawLineSeg(layer, R + kFx[b][0] + std::cos(a) * r1,
                                    py0 + kFx[b][1] + std::sin(a) * r1,
                                    R + kFx[b][0] + std::cos(a) * r2,
                                    py0 + kFx[b][1] + std::sin(a) * r2, 2.3f,
                                    lv_color_hex(kFc[b]), fo);
                    }
                }
                break;
            }
            case NomiPropKind::kXmasHat: {
                float drop = (1.0f - p_pop) * -20.0f;
                float hy = py0 - 34.0f + drop;
                DrawLineSeg(layer, R - 40, hy, R, hy - 34, 3.8f, col_w, p_opa);
                DrawLineSeg(layer, R, hy - 34, R + 42, hy - 14, 3.8f, col_w, p_opa);
                DrawLineSeg(layer, R + 42, hy - 14, R + 32, hy, 3.8f, col_w, p_opa);
                DrawStrokeRoundRect(layer, R - 4, hy + 4, 82, 13, 6.5f, 3.6f, col_w, p_opa, true);
                DrawFillCircle(layer, R + 45, hy - 12, 8.0f, c_red, p_opa);
                break;
            }
            case NomiPropKind::kXmasTree: {
                float ty = py0 + 34.0f;
                DrawLineSeg(layer, R, ty - 32, R - 18, ty - 10, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R, ty - 32, R + 18, ty - 10, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R - 18, ty - 10, R + 18, ty - 10, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R - 12, ty - 10, R - 26, ty + 10, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R + 12, ty - 10, R + 26, ty + 10, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R - 26, ty + 10, R + 26, ty + 10, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R, ty + 10, R, ty + 24, 3.6f, col_w, p_opa);
                DrawSparkle4(layer, R, ty - 38, 7.5f, 0, c_gold, p_opa, 2.2f);
                break;
            }
            case NomiPropKind::kXmasGift: {
                float gy = py0 + 38.0f;
                DrawStrokeRoundRect(layer, R, gy + 4, 46 * p_pop, 34 * p_pop, 5, 3.6f, col_w, p_opa);
                DrawStrokeRoundRect(layer, R, gy - 14, 54 * p_pop, 10 * p_pop, 4, 3.6f, col_w, p_opa, true);
                DrawLineSeg(layer, R, gy - 18, R, gy + 20, 3.4f, c_red, p_opa);
                DrawArcSeg(layer, R - 7, gy - 21, 6, 0, 360, 2.8f, c_red, p_opa);
                DrawArcSeg(layer, R + 7, gy - 21, 6, 0, 360, 2.8f, c_red, p_opa);
                break;
            }
            case NomiPropKind::kRose:
                DrawLineSeg(layer, R - 26, py0 + 54, R + 14, py0 + 26, 3.2f, col_w, p_opa);
                DrawFillCircle(layer, R + 18, py0 + 23, 11.0f * p_pop, c_red, p_opa);
                DrawArcSeg(layer, R + 18, py0 + 23, 5.5f * p_pop, 20, 250, 2.0f, lv_color_hex(kColBlack), p_opa);
                break;
            case NomiPropKind::kZongzi:
                DrawLineSeg(layer, R, py0 + 16, R - 24, py0 + 54, 3.6f, col_w, p_opa);
                DrawLineSeg(layer, R, py0 + 16, R + 24, py0 + 54, 3.6f, col_w, p_opa);
                DrawLineSeg(layer, R - 24, py0 + 54, R + 24, py0 + 54, 3.6f, col_w, p_opa);
                DrawLineSeg(layer, R - 12, py0 + 38, R + 14, py0 + 45, 2.8f, c_cyan, p_opa);
                DrawLineSeg(layer, R + 10, py0 + 34, R - 10, py0 + 50, 2.8f, c_cyan, p_opa);
                break;
            case NomiPropKind::kMidAutumnMoon:
                DrawFillCircle(layer, R + 48, py0 - 44, 17 * p_pop, c_gold, p_opa);
                DrawFillCircle(layer, R + 56, py0 - 48, 14.5f * p_pop, lv_color_hex(kColBlack), p_opa);
                DrawSparkle4(layer, R + 18, py0 - 58, 6.5f, 12, col_w, p_opa, 1.8f);
                break;
            case NomiPropKind::kMidAutumnCake:
                DrawArcSeg(layer, R, py0 + 38, 22 * p_pop, 0, 360, 3.6f, col_w, p_opa);
                DrawArcSeg(layer, R, py0 + 38, 14 * p_pop, 0, 360, 2.6f, c_gold, p_opa);
                DrawLineSeg(layer, R - 7, py0 + 38, R + 7, py0 + 38, 2.6f, c_gold, p_opa);
                DrawLineSeg(layer, R, py0 + 31, R, py0 + 45, 2.6f, c_gold, p_opa);
                break;
            case NomiPropKind::kNationalFlag: {
                float wf = std::sin(t_global * 5.0f) * 3.0f;
                DrawLineSeg(layer, R - 14, py0 + 62, R - 14, py0 + 14, 3.6f, col_w, p_opa);
                DrawFillRoundRect(layer, R + 8, py0 + 27 + wf * 0.4f, 40 * p_pop, 26 * p_pop, 3, c_red, p_opa);
                DrawSparkle4(layer, R - 2, py0 + 23 + wf * 0.4f, 5.5f, 0, c_gold, p_opa, 1.8f);
                break;
            }
            case NomiPropKind::kHalloweenPumpkin:
                DrawStrokeRoundRect(layer, R, py0 + 40, 46 * p_pop, 34 * p_pop, 17 * p_pop, 3.4f, c_gold, p_opa);
                DrawStrokeRoundRect(layer, R, py0 + 40, 22 * p_pop, 34 * p_pop, 11 * p_pop, 3.0f, c_gold, p_opa);
                DrawLineSeg(layer, R, py0 + 22, R + 6, py0 + 14, 3.2f, c_gold, p_opa);
                break;
            case NomiPropKind::kHalloweenGhost: {
                float fy = std::sin(t_global * 4.0f) * 6.0f;
                DrawStrokeRoundRect(layer, R + 46, py0 + 26 + fy, 30 * p_pop, 36 * p_pop, 14, 3.4f, col_w, p_opa);
                DrawFillCircle(layer, R + 41, py0 + 20 + fy, 2.4f, col_w, p_opa);
                DrawFillCircle(layer, R + 51, py0 + 20 + fy, 2.4f, col_w, p_opa);
                break;
            }
            case NomiPropKind::kBirthdayCake:
                DrawStrokeRoundRect(layer, R, py0 + 48, 50 * p_pop, 22 * p_pop, 4, 3.4f, col_w, p_opa);
                DrawStrokeRoundRect(layer, R, py0 + 30, 34 * p_pop, 13 * p_pop, 3, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R, py0 + 23, R, py0 + 11, 3.2f, col_w, p_opa);
                DrawFillCircle(layer, R + std::sin(t_global * 14.0f) * 1.5f, py0 + 5, 5.0f, c_gold, p_opa);
                DrawSparkle4(layer, R - 38, py0 + 16, 6.0f, 12, col_w, p_opa, 1.8f);
                DrawSparkle4(layer, R + 38, py0 + 14, 6.0f, -12, c_gold, p_opa, 1.8f);
                break;
            case NomiPropKind::kTrophy: {
                float tx = R + persp_x, ty = py0 + 34 + persp_y;
                float sc = p_pop * persp_sy;
                DrawStrokeRoundRect(layer, tx, ty - 4 * sc, 28 * sc, 24 * sc, 10 * sc, 3.4f, col_w, p_opa);
                DrawArcSeg(layer, tx - 15 * sc, ty - 6 * sc, 7 * sc, 90, 270, 3.0f, col_w, p_opa);
                DrawArcSeg(layer, tx + 15 * sc, ty - 6 * sc, 7 * sc, 270, 90, 3.0f, col_w, p_opa);
                DrawLineSeg(layer, tx, ty + 9 * sc, tx, ty + 20 * sc, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, tx - 11 * sc, ty + 20 * sc, tx + 11 * sc, ty + 20 * sc, 3.4f, col_w, p_opa);
                DrawSparkle4(layer, tx + 22 * sc, ty - 18 * sc, 7.0f, 12, c_gold, p_opa, 2.0f);
                break;
            }
            case NomiPropKind::kSax: {
                float sx = R + persp_x, sy = py0 + 36 + persp_y;
                float ang = std::sin(lag_ang) * 0.24f;
                float scx = p_pop * persp_sx, scy = p_pop * persp_sy;
                DrawRotLine(layer, sx, sy, ang, scx, scy, -18, -24, -6, -22, 3.8f, col_w, p_opa);
                DrawRotLine(layer, sx, sy, ang, scx, scy, -6, -22, 4, 20, 4.2f, col_w, p_opa);
                DrawRotLine(layer, sx, sy, ang, scx, scy, 4, 20, 22, 22, 4.2f, col_w, p_opa);
                DrawRotLine(layer, sx, sy, ang, scx, scy, 22, 22, 26, -2, 4.2f, col_w, p_opa);
                float bx, by;
                RotPt(sx, sy, 25, -4, ang, scx, scy, bx, by);
                DrawStrokeRoundRect(layer, bx, by, 16 * scx, 9 * scy, 4, 3.2f, col_w, p_opa, true);
                float p1 = std::fmod(p_age / 2.1f, 1.0f), p2 = std::fmod((p_age + 1.05f) / 2.1f, 1.0f);
                DrawColorToGrayNote(layer, bx + 12 + p1 * 16, by - 8 - p1 * 28, 0.8f, 1, p1, p_alpha);
                DrawColorToGrayNote(layer, bx + 6 + p2 * 14, by - 4 - p2 * 26, 0.76f, 0, p2, p_alpha);
                break;
            }
            case NomiPropKind::kViolin: {
                float sx = R + 6 + persp_x * 0.8f, sy = py0 + 36 + persp_y;
                float sc = p_pop * persp_sy;
                float bow = std::sin(bt * 6.2f) * 14.0f;
                DrawStrokeRoundRect(layer, sx - 6 * sc, sy + 6 * sc, 24 * sc, 24 * sc, 12 * sc, 3.4f, col_w, p_opa, true);
                DrawStrokeRoundRect(layer, sx + 8 * sc, sy - 6 * sc, 18 * sc, 18 * sc, 9 * sc, 3.4f, col_w, p_opa, true);
                DrawLineSeg(layer, sx + 14 * sc, sy - 12 * sc, sx + 32 * sc, sy - 28 * sc, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, sx - 28 + bow * 0.6f, sy + 14 - bow * 0.4f,
                            sx + 22 + bow * 0.6f, sy - 14 - bow * 0.4f, 2.8f, col_w, p_opa);
                DrawFillRoundRect(layer, sx - 26 + bow * 0.6f, sy + 14 - bow * 0.4f, 12, 10, 4, col_w, p_opa);
                float p1 = std::fmod(p_age / 2.1f, 1.0f);
                DrawColorToGrayNote(layer, sx + 34 + p1 * 12, sy - 14 - p1 * 26, 0.76f, 2, p1, p_alpha);
                break;
            }
            case NomiPropKind::kElecGuitar:
            case NomiPropKind::kAcousticGuitar: {
                float sx = R - 4 + (cfg.orbit_3d ? persp_x * 0.7f : std::sin(bt * 6.2f) * 7.0f);
                float sy = py0 + 36 + persp_y;
                float ang = (-0.28f + std::sin(bt * 4.5f) * 0.14f);
                float scx = p_pop * persp_sx, scy = p_pop * persp_sy;
                DrawStrokeRoundRect(layer, sx - 16 * scx, sy + 6 * scy, 34 * scx, 28 * scy, 13, 4.0f, col_w, p_opa, true);
                DrawRotLine(layer, sx, sy, ang, scx, scy, -4, -4, 48, -10, 4.0f, col_w, p_opa);
                DrawRotLine(layer, sx, sy, ang, scx, scy, -4, 5, 48, -1, 4.0f, col_w, p_opa);
                float hx, hy;
                RotPt(sx, sy, 56, -6, ang, scx, scy, hx, hy);
                DrawStrokeRoundRect(layer, hx, hy, 16 * scx, 12 * scy, 4, 3.6f, col_w, p_opa, true);
                if (cfg.prop == NomiPropKind::kElecGuitar) {
                    float gx, gy;
                    RotPt(sx, sy, 22, 10, ang, scx, scy, gx, gy);
                    DrawScallopedFist(layer, gx, gy, 0.85f * scy, p_opa, false, 1);
                } else {
                    DrawArcSeg(layer, sx - 8 * scx, sy + 4 * scy, 6.5f * scy, 0, 360, 2.8f, col_w, p_opa);
                }
                float p1 = std::fmod(p_age / 2.0f, 1.0f), p2 = std::fmod((p_age + 1.0f) / 2.0f, 1.0f);
                DrawColorToGrayNote(layer, R - 56 - p1 * 8, py0 + 16 - p1 * 24, 0.76f, 1, p1, p_alpha);
                DrawColorToGrayNote(layer, R + 56 + p2 * 8, py0 + 18 - p2 * 24, 0.76f, 0, p2, p_alpha);
                break;
            }
            case NomiPropKind::kHandpan: {
                float w = std::sin(bt * 6.6f);
                float l_pat = std::pow(std::max(0.0f, -w), 2.0f);
                float r_pat = std::pow(std::max(0.0f, w), 2.0f);
                float any_p = std::max(l_pat, r_pat);
                float sx = p_pop * (1.0f + any_p * 0.05f), sy = p_pop * (1.0f - any_p * 0.05f);
                DrawFillRoundRect(layer, R, py0 + 40, 84 * sx, 34 * sy, 17 * sy, col_w, p_opa);
                DrawArcSeg(layer, R, py0 + 44, 7.5f * sy, 0, 360, 4.0f, lv_color_hex(kColBlack), p_opa);
                DrawArcSeg(layer, R, py0 + 31, 5.0f * sy, 0, 360, 3.2f, lv_color_hex(kColBlack), p_opa);
                DrawScallopedFist(layer, R - 34, py0 + 28 + l_pat * 7.0f, 0.95f * p_pop, p_opa, false, 1);
                DrawScallopedFist(layer, R + 34, py0 + 28 + r_pat * 7.0f, 0.95f * p_pop, p_opa, false, -1);
                if (any_p > 0.35f) {
                    DrawArcSeg(layer, (l_pat > r_pat) ? R - 24 : R + 24, py0 + 18, 9 + any_p * 9,
                               205, 335, 2.2f, c_cyan, ToOpa((any_p - 0.35f) * 1.2f));
                }
                break;
            }
            case NomiPropKind::kGlowsticks: {
                float sp = std::sin(std::max(0.0f, bt - 0.09f) * 6.4f);
                float ang = sp * 0.42f;
                float sx = sp * 20.0f;
                for (int side = -1; side <= 1; side += 2) {
                    float bx = R + side * 17.0f + sx, by = py0 + 46.0f;
                    float tx, ty;
                    RotPt(bx, by, 0, -46, ang, p_pop, p_pop, tx, ty);
                    lv_color_t sc = (side < 0) ? c_coral : c_cyan;
                    DrawLineSeg(layer, bx, by, tx, ty, 11.0f * p_pop, sc, p_opa);
                    DrawScallopedFist(layer, bx, by + 4, 0.95f * p_pop, p_opa, false, side);
                }
                if (std::fabs(sp) > 0.72f) {
                    int sgn = (sp < 0) ? -1 : 1;
                    float cx = R + sgn * 36.0f + sx, cy = py0 + 2.0f;
                    DrawFillRoundRect(layer, cx + sgn * 10, cy - 8, 5, 5, 1, c_cyan, p_opa);
                    DrawFillRoundRect(layer, cx + sgn * 18, cy + 4, 5, 5, 1, c_coral, p_opa);
                    DrawFillRoundRect(layer, cx + sgn * 6, cy - 16, 5, 5, 1, c_gold, p_opa);
                }
                break;
            }
            case NomiPropKind::kHipHop:
            case NomiPropKind::kSunglasses: {
                float sx = std::sin(bt * 3.8f) * 5.0f;
                DrawStrokeRoundRect(layer, R - 35 + sx, py0 - 6, 58 * p_pop, 40 * p_pop, 12, 4.2f,
                                    col_w, p_opa, true);
                DrawStrokeRoundRect(layer, R + 35 + sx, py0 - 6, 58 * p_pop, 40 * p_pop, 12, 4.2f,
                                    col_w, p_opa, true);
                DrawLineSeg(layer, R - 6 + sx, py0 - 12, R + 6 + sx, py0 - 12, 4.2f, col_w, p_opa);
                if (cfg.prop == NomiPropKind::kHipHop) {
                    DrawSparkle4(layer, R + 62 + sx, py0 - 30, 8.0f, 12, c_gold, p_opa, 2.2f);
                    DrawLineSeg(layer, R - 26 + sx, py0 + 26, R - 10 + sx, py0 + 40, 2.8f, col_w, p_opa);
                    DrawLineSeg(layer, R + 26 + sx, py0 + 26, R + 10 + sx, py0 + 40, 2.8f, col_w, p_opa);
                    DrawArcSeg(layer, R + sx, py0 + 48, 11, 0, 360, 2.8f, col_w, p_opa);
                    DrawLineSeg(layer, R + sx, py0 + 37, R + sx, py0 + 59, 2.4f, col_w, p_opa);
                    DrawLineSeg(layer, R + sx, py0 + 48, R - 7 + sx, py0 + 56, 2.4f, col_w, p_opa);
                    DrawLineSeg(layer, R + sx, py0 + 48, R + 7 + sx, py0 + 56, 2.4f, col_w, p_opa);
                } else {
                    DrawLineSeg(layer, R - 50 + sx, py0 + 4, R - 36 + sx, py0 - 12, 2.6f, c_cyan, p_opa);
                    DrawLineSeg(layer, R + 20 + sx, py0 + 4, R + 34 + sx, py0 - 12, 2.6f, c_cyan, p_opa);
                }
                break;
            }
            case NomiPropKind::kDJ:
                DrawStrokeRoundRect(layer, R + 4, py0 + 42, 54 * p_pop, 28 * p_pop, 14, 3.2f, col_w, p_opa);
                DrawStrokeRoundRect(layer, R + 4, py0 + 42, 16 * p_pop, 9 * p_pop, 4, 2.6f, col_w, p_opa);
                DrawScallopedFist(layer, R - 24 + std::sin(t_global * 11.0f) * 5.0f, py0 + 28,
                                  0.85f * p_pop, p_opa, false, 1);
                break;
            case NomiPropKind::kMic:
                DrawFillRoundRect(layer, R - 14, py0 + 24, 18 * p_pop, 24 * p_pop, 9, col_w, p_opa);
                DrawLineSeg(layer, R - 14, py0 + 36, R - 14, py0 + 56, 3.6f, col_w, p_opa);
                DrawScallopedFist(layer, R + 18, py0 + 38 + std::sin(bt * 4.5f) * 3.0f, 0.85f * p_pop,
                                  p_opa, true, -1);
                break;
            case NomiPropKind::kMusicNotes: {
                float p1 = std::fmod(p_age / 2.1f, 1.0f), p2 = std::fmod((p_age + 1.05f) / 2.1f, 1.0f);
                DrawColorToGrayNote(layer, R - 24 + p1 * 18, py0 + 44 - p1 * 30, 0.95f, 0, p1, p_alpha);
                DrawColorToGrayNote(layer, R + 12 + p2 * 16, py0 + 42 - p2 * 28, 0.90f, 1, p2, p_alpha);
                break;
            }
            case NomiPropKind::kSunny: {
                float sx = R + 50.0f, sy = py0 - 46.0f;
                DrawArcSeg(layer, sx, sy, 12 * p_pop, 0, 360, 3.2f, c_gold, p_opa);
                for (int k = 0; k < 8; k++) {
                    float a = k * kPi * 0.25f + t_global * 0.8f;
                    DrawLineSeg(layer, sx + std::cos(a) * 18, sy + std::sin(a) * 18,
                                sx + std::cos(a) * 24, sy + std::sin(a) * 24, 2.8f, c_gold, p_opa);
                }
                break;
            }
            case NomiPropKind::kHotHat: {
                float hy = py0 - 40.0f + (1.0f - p_pop) * -18.0f;
                DrawLineSeg(layer, R - 62, hy + 8, R + 62, hy + 8, 4.0f, col_w, p_opa);
                DrawStrokeRoundRect(layer, R, hy - 6, 54, 28, 10, 3.8f, col_w, p_opa, true);
                DrawLineSeg(layer, R - 24, hy + 4, R + 24, hy + 4, 3.2f, c_coral, p_opa);
                DrawFillCircle(layer, R + 58, py0 + 6 + std::fmod(t_global * 14.0f, 12.0f), 4.0f, c_cyan, p_opa);
                break;
            }
            case NomiPropKind::kFoldingFan: {
                float sw = std::sin(t_global * 6.5f) * 12.0f;
                DrawArcSeg(layer, R + 26, py0 + 54, 34 * p_pop, 225 + sw, 315 + sw, 26 * p_pop, col_w, p_opa);
                DrawLineSeg(layer, R - 46, py0 + 22, R - 18, py0 + 20, 2.4f, c_cyan, p_opa);
                break;
            }
            case NomiPropKind::kWindPole: {
                float sh = std::sin(t_global * 26.0f) * 1.8f;
                DrawLineSeg(layer, R - 42, py0 - 60, R - 42, py0 + 66, 4.6f, col_w, p_opa);
                DrawScallopedFist(layer, R - 34 + sh, py0 + 14, 0.9f * p_pop, p_opa, false, 1);
                DrawScallopedFist(layer, R - 34 + sh, py0 + 36, 0.9f * p_pop, p_opa, false, 1);
                float gx = std::fmod(t_global * 70.0f, 44.0f);
                DrawLineSeg(layer, R + 6 + gx, py0 - 48, R + 34 + gx, py0 - 48, 2.4f, c_cyan, p_opa);
                DrawLineSeg(layer, R + 12 + gx * 0.8f, py0 + 32, R + 44 + gx * 0.8f, py0 + 32, 2.4f, c_cyan, p_opa);
                break;
            }
            case NomiPropKind::kUmbrella: {
                float ux = R + 48.0f, uy = py0 - 2.0f;
                DrawArcSeg(layer, ux, uy, 24 * p_pop, 180, 360, 3.6f, col_w, p_opa);
                DrawLineSeg(layer, ux - 24 * p_pop, uy, ux + 24 * p_pop, uy, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, ux, uy, ux, uy + 32 * p_pop, 3.4f, col_w, p_opa);
                DrawArcSeg(layer, ux - 6, uy + 32 * p_pop, 6, 0, 180, 3.4f, col_w, p_opa);
                for (int k = 0; k < 3; k++) {
                    float d = std::fmod(t_global * 42.0f + k * 14.0f, 22.0f);
                    float rx = ux - 14 + k * 18 - d * 0.25f, ry = uy - 48 + d;
                    DrawLineSeg(layer, rx, ry, rx - 4, ry + 10, 2.2f, c_cyan, p_opa);
                }
                break;
            }
            case NomiPropKind::kColdScarf:
                DrawStrokeRoundRect(layer, R, py0 + 28, 96 * p_pop, 22 * p_pop, 11, 3.8f, col_w, p_opa, true);
                DrawStrokeRoundRect(layer, R + 30, py0 + 50, 20 * p_pop, 26 * p_pop, 4, 3.6f, col_w, p_opa, true);
                DrawLineSeg(layer, R - 20, py0 + 20, R - 20, py0 + 36, 3.2f, c_coral, p_opa);
                DrawLineSeg(layer, R - 6, py0 + 20, R - 6, py0 + 36, 3.2f, c_coral, p_opa);
                DrawLineSeg(layer, R + 22, py0 + 52, R + 38, py0 + 52, 3.2f, c_coral, p_opa);
                break;
            case NomiPropKind::kSnowflakes:
                for (int k = 0; k < 3; k++) {
                    float sy = py0 + 24 + std::fmod(t_global * 16.0f + k * 16.0f, 36.0f);
                    float sx = R - 42 + k * 42;
                    DrawSparkle4(layer, sx, sy, 9.0f, t_global * 30.0f + k * 40,
                                 (k == 1 ? col_w : c_cyan), p_opa, 2.0f);
                }
                break;
            case NomiPropKind::kPinwheel: {
                float px = R + 46.0f, py = py0 + 26.0f;
                DrawLineSeg(layer, px, py + 4, px, py + 38 * p_pop, 3.4f, col_w, p_opa);
                float spin = t_global * 5.2f;
                for (int k = 0; k < 4; k++) {
                    float a = spin + k * (kPi * 0.5f);
                    float bx = px + std::cos(a) * 14.0f * p_pop;
                    float by = py + std::sin(a) * 14.0f * p_pop;
                    DrawLineSeg(layer, px, py, bx, by, 7.5f * p_pop, col_w, p_opa);
                }
                DrawFillCircle(layer, px, py, 3.0f, lv_color_hex(kColBlack), p_opa);
                float d = std::fmod(t_global * 18.0f, 10.0f);
                DrawFillCircle(layer, px - 30 + d, py + 8, 2.5f, col_w, p_opa);
                DrawFillCircle(layer, px - 20 + d * 0.7f, py + 14, 2.0f, col_w, p_opa);
                break;
            }
            case NomiPropKind::kAutumnLeaf: {
                float ph = std::fmod(p_age / 2.6f, 1.0f);
                float lx = R + 42.0f - ph * 76.0f + std::sin(ph * 6.28f) * 12.0f;
                float ly = py0 + 4.0f + ph * 58.0f;
                float sc_l = p_pop * (0.82f + 0.24f * std::sin(ph * kPi));
                lv_color_t lc = (ph < 0.65f) ? c_gold : LerpColorHex(kColGold, kColGray, (ph - 0.65f) / 0.35f);
                lv_opa_t lo = ToOpa(std::sin(ph * kPi) * p_alpha);
                DrawFillCircle(layer, lx, ly, 6.5f * sc_l, lc, lo);
                DrawLineSeg(layer, lx - 9 * sc_l, ly + 2, lx + 9 * sc_l, ly - 2, 3.2f * sc_l, lc, lo);
                DrawLineSeg(layer, lx, ly - 9 * sc_l, lx, ly + 9 * sc_l, 3.2f * sc_l, lc, lo);
                break;
            }
            case NomiPropKind::kFaceMask:
                DrawFillRoundRect(layer, R, py0 + 28, 128 * p_pop, 48 * p_pop, 12, col_w, p_opa);
                DrawLineSeg(layer, R - 82, py0 + 6, R - 62, py0 + 18, 4.0f, col_w, p_opa);
                DrawLineSeg(layer, R + 82, py0 + 6, R + 62, py0 + 18, 4.0f, col_w, p_opa);
                DrawArcSeg(layer, R - 38, py0 + 30, 6.5f, 0, 360, 3.0f, lv_color_hex(kColBlack), p_opa);
                break;
            case NomiPropKind::kClockBoard:
            case NomiPropKind::kClockGlasses: {
                time_t now = std::time(nullptr);
                struct tm ti;
                localtime_r(&now, &ti);
                int h1 = (ti.tm_hour / 10) % 10;
                int h2 = ti.tm_hour % 10;
                int m1 = (ti.tm_min / 10) % 10;
                int m2 = ti.tm_min % 10;
                bool col_on = ((anim_tick_ / 15) % 2) == 0;
                if (cfg.prop == NomiPropKind::kClockBoard) {
                    float lift = (1.0f - p_pop) * 28.0f;
                    float cy_b = py0 + 16.0f + lift;
                    DrawStrokeRoundRect(layer, R, cy_b, 148, 60, 15, 3.8f, col_w, p_opa, true);
                    DrawFillRoundRect(layer, R - 44, cy_b - 31, 22, 13, 6.5f, col_w, p_opa);
                    DrawFillRoundRect(layer, R + 44, cy_b - 31, 22, 13, 6.5f, col_w, p_opa);
                    // Large crisp vector digits (at 1.22x scale -> ~36px tall!)
                    DrawVectorDigit(layer, R - 46, cy_b, 16.0f, 30.0f, 4.0f, h1, col_w, p_opa);
                    DrawVectorDigit(layer, R - 20, cy_b, 16.0f, 30.0f, 4.0f, h2, col_w, p_opa);
                    DrawVectorDigit(layer, R + 20, cy_b, 16.0f, 30.0f, 4.0f, m1, col_w, p_opa);
                    DrawVectorDigit(layer, R + 46, cy_b, 16.0f, 30.0f, 4.0f, m2, col_w, p_opa);
                    if (col_on) {
                        DrawFillCircle(layer, R, cy_b - 7.5f, 3.0f, col_w, p_opa);
                        DrawFillCircle(layer, R, cy_b + 7.5f, 3.0f, col_w, p_opa);
                    }
                } else {
                    DrawStrokeRoundRect(layer, R - 38, py0 - 2, 66 * p_pop, 46 * p_pop, 14, 3.6f, col_w, p_opa, true);
                    DrawStrokeRoundRect(layer, R + 38, py0 - 2, 66 * p_pop, 46 * p_pop, 14, 3.6f, col_w, p_opa, true);
                    DrawLineSeg(layer, R - 5, py0 - 10, R + 5, py0 - 10, 3.6f, col_w, p_opa);
                    DrawVectorDigit(layer, R - 50, py0 - 2, 13.0f * p_pop, 24.0f * p_pop, 3.4f, h1, col_w, p_opa);
                    DrawVectorDigit(layer, R - 26, py0 - 2, 13.0f * p_pop, 24.0f * p_pop, 3.4f, h2, col_w, p_opa);
                    DrawVectorDigit(layer, R + 26, py0 - 2, 13.0f * p_pop, 24.0f * p_pop, 3.4f, m1, col_w, p_opa);
                    DrawVectorDigit(layer, R + 50, py0 - 2, 13.0f * p_pop, 24.0f * p_pop, 3.4f, m2, col_w, p_opa);
                    if (col_on) {
                        DrawFillCircle(layer, R, py0 - 7, 2.5f, c_cyan, p_opa);
                        DrawFillCircle(layer, R, py0 + 5, 2.5f, c_cyan, p_opa);
                    }
                }
                break;
            }
            case NomiPropKind::kChaseDot: {
                float dx_d = std::sin(t_global * 2.1f) * 56.0f;
                float dy_d = std::sin(t_global * 4.2f) * 30.0f;
                DrawFillCircle(layer, R + dx_d, py0 + dy_d, 5.0f, c_cyan, LV_OPA_COVER);
                DrawArcSeg(layer, R + dx_d, py0 + dy_d, 9.5f, 0, 360, 2.0f, c_cyan, LV_OPA_50);
                break;
            }
            case NomiPropKind::kTyping: {
                float y_l = std::sin(t_global * 20.0f) * 5.0f;
                DrawStrokeRoundRect(layer, R, py0 + 44, 108, 24, 6, 3.2f, col_w, p_opa);
                DrawScallopedFist(layer, R - 28, py0 + 24 + y_l, 0.85f * p_pop, p_opa, false, 1);
                DrawScallopedFist(layer, R + 28, py0 + 24 - y_l, 0.85f * p_pop, p_opa, false, -1);
                break;
            }
            case NomiPropKind::kCoffee:
                DrawStrokeRoundRect(layer, R - 4, py0 + 42, 34 * p_pop, 28 * p_pop, 6, 3.4f, col_w, p_opa);
                DrawArcSeg(layer, R + 14, py0 + 42, 8 * p_pop, 270, 90, 3.2f, col_w, p_opa);
                DrawLineSeg(layer, R - 10, py0 + 22, R - 10 + std::sin(t_global * 4) * 3, py0 + 10, 2.4f, c_gold, p_opa);
                DrawLineSeg(layer, R + 2, py0 + 22, R + 2 - std::sin(t_global * 4) * 3, py0 + 10, 2.4f, c_gold, p_opa);
                break;
            case NomiPropKind::kBobaTea: {
                float sip = std::sin(t_global * 5.0f) * 3.0f;
                DrawStrokeRoundRect(layer, R, py0 + 42 + sip, 30 * p_pop, 36 * p_pop, 5, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R + 2, py0 + 24 + sip, R + 8, py0 + 8 + sip, 3.2f, col_w, p_opa);
                DrawFillCircle(layer, R - 5, py0 + 52 + sip, 2.5f, c_gold, p_opa);
                DrawFillCircle(layer, R + 4, py0 + 54 + sip, 2.5f, c_gold, p_opa);
                break;
            }
            case NomiPropKind::kMagnifier:
                DrawArcSeg(layer, R - 42, py0, 31 * p_pop, 0, 360, 3.8f, col_w, p_opa);
                DrawLineSeg(layer, R - 64, py0 + 22, R - 82, py0 + 40, 5.2f, col_w, p_opa);
                break;
            case NomiPropKind::kHeadphones:
                DrawArcSeg(layer, R, py0 + 2, 74, 200, 340, 3.8f, col_w, p_opa);
                DrawFillRoundRect(layer, R - 76, py0 + 2, 13, 36, 6.5f, col_w, p_opa);
                DrawFillRoundRect(layer, R + 76, py0 + 2, 13, 36, 6.5f, col_w, p_opa);
                break;
            case NomiPropKind::kPottedPlant:
                DrawStrokeRoundRect(layer, R, py0 + 48, 32 * p_pop, 22 * p_pop, 4, 3.4f, col_w, p_opa);
                DrawLineSeg(layer, R, py0 + 36, R, py0 + 18, 3.0f, c_cyan, p_opa);
                DrawFillCircle(layer, R - 8, py0 + 18, 5.5f * p_pop, c_cyan, p_opa);
                DrawFillCircle(layer, R + 8, py0 + 14, 5.5f * p_pop, c_cyan, p_opa);
                break;
            case NomiPropKind::kStretchHands: {
                float lift = std::sin(t_global * 2.5f) * 8.0f;
                DrawScallopedFist(layer, R - 62, py0 - 46 - lift, 0.95f * p_pop, p_opa, false, 1);
                DrawScallopedFist(layer, R + 62, py0 - 46 - lift, 0.95f * p_pop, p_opa, false, -1);
                break;
            }
            case NomiPropKind::kWaveHand:
                DrawScallopedFist(layer, R + 44 + std::sin(t_global * 11.0f) * 6.0f, py0 + 38,
                                  1.05f * p_pop, p_opa, true, -1);
                break;
            case NomiPropKind::kBlush:
                DrawFillRoundRect(layer, R - 54, py0 + 24, 24, 11, 5.5f, lv_color_hex(kColBlush), p_opa);
                DrawFillRoundRect(layer, R + 54, py0 + 24, 24, 11, 5.5f, lv_color_hex(kColBlush), p_opa);
                break;
            case NomiPropKind::kTears: {
                float dy = std::fmod(t_global * 32.0f, 24.0f);
                DrawFillCircle(layer, R - 42, py0 + 18 + dy, 4.0f, c_cyan, p_opa);
                DrawFillCircle(layer, R + 42, py0 + 18 + std::fmod(dy + 12.0f, 24.0f), 4.0f, c_cyan, p_opa);
                break;
            }
            case NomiPropKind::kSweat:
                DrawFillRoundRect(layer, R + 60, py0 - 24 + std::fmod(t_global * 16.0f, 14.0f), 8, 13, 4,
                                  c_cyan, p_opa);
                break;
            case NomiPropKind::kAngerMark:
                DrawLineSeg(layer, R + 48, py0 - 42, R + 54, py0 - 42, 3.0f, c_coral, p_opa);
                DrawLineSeg(layer, R + 54, py0 - 42, R + 54, py0 - 48, 3.0f, c_coral, p_opa);
                DrawLineSeg(layer, R + 64, py0 - 34, R + 58, py0 - 34, 3.0f, c_coral, p_opa);
                DrawLineSeg(layer, R + 58, py0 - 34, R + 58, py0 - 28, 3.0f, c_coral, p_opa);
                break;
            case NomiPropKind::kVoiceWave:
                for (int k = -2; k <= 2; k++) {
                    float h = 4.0f + std::fabs(std::sin(t_global * 10.0f + k * 0.9f)) * 11.0f;
                    DrawLineSeg(layer, R + k * 9.0f, py0 + 44 - h * 0.5f, R + k * 9.0f,
                                py0 + 44 + h * 0.5f, 3.2f, c_cyan, p_opa);
                }
                break;
            case NomiPropKind::kBolt:
                DrawLineSeg(layer, R + 4, py0 + 20, R - 8, py0 + 38, 4.0f, c_green, p_opa);
                DrawLineSeg(layer, R - 8, py0 + 38, R + 8, py0 + 38, 4.0f, c_green, p_opa);
                DrawLineSeg(layer, R + 8, py0 + 38, R - 4, py0 + 56, 4.0f, c_green, p_opa);
                break;
            case NomiPropKind::kZzz: {
                float p = std::fmod(t_global * 0.45f, 1.0f);
                lv_opa_t zo = ToOpa(std::sin(p * kPi));
                float zx = R + 44 + p * 12, zy = py0 - 28 - p * 20;
                DrawLineSeg(layer, zx, zy, zx + 10, zy, 2.4f, col_gray, zo);
                DrawLineSeg(layer, zx + 10, zy, zx, zy + 10, 2.4f, col_gray, zo);
                DrawLineSeg(layer, zx, zy + 10, zx + 10, zy + 10, 2.4f, col_gray, zo);
                break;
            }
            case NomiPropKind::kCrown:
                DrawLineSeg(layer, R - 18, py0 - 48, R - 14, py0 - 64, 3.2f, c_gold, p_opa);
                DrawLineSeg(layer, R - 14, py0 - 64, R - 5, py0 - 54, 3.2f, c_gold, p_opa);
                DrawLineSeg(layer, R - 5, py0 - 54, R, py0 - 68, 3.2f, c_gold, p_opa);
                DrawLineSeg(layer, R, py0 - 68, R + 5, py0 - 54, 3.2f, c_gold, p_opa);
                DrawLineSeg(layer, R + 5, py0 - 54, R + 14, py0 - 64, 3.2f, c_gold, p_opa);
                DrawLineSeg(layer, R + 14, py0 - 64, R + 18, py0 - 48, 3.2f, c_gold, p_opa);
                DrawLineSeg(layer, R - 18, py0 - 48, R + 18, py0 - 48, 3.2f, c_gold, p_opa);
                break;
            case NomiPropKind::kTongue:
                DrawFillRoundRect(layer, R, py0 + 30, 18 * p_pop, 16 * p_pop, 8, c_coral, p_opa);
                break;
            case NomiPropKind::kNone:
            default:
                break;
        }
    }
    g_nomi_scale = 1.0f;
}

void LcdDisplay::EyeTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnEyeTimerTick();
    }
}

void LcdDisplay::OnEyeTimerTick() {
    anim_tick_++;

    // 1. Smooth subtitle auto-scroll
    if (subtitle_visible_ && subtitle_max_scroll_ > 0 && chat_message_label_ != nullptr) {
        if (subtitle_pause_ticks_ > 0) {
            subtitle_pause_ticks_--;
        } else if ((anim_tick_ % 2) == 0) {
            subtitle_scroll_y_ += 1;
            if (subtitle_scroll_y_ > subtitle_max_scroll_) {
                subtitle_scroll_y_ = 0;
                subtitle_pause_ticks_ = 44;
            } else if (subtitle_scroll_y_ == subtitle_max_scroll_) {
                subtitle_pause_ticks_ = 40;
            }
            lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, -subtitle_scroll_y_);
        }
    }

    // 2. Autonomous natural blinking (every 2.8~5.5s, 15% double-blink)
    if (blink_ticks_left_ > 0) {
        blink_ticks_left_--;
        if (blink_ticks_left_ == 0 && double_blink_pending_) {
            double_blink_pending_ = false;
            blink_ticks_left_ = 5;
        }
    } else if (anim_tick_ >= next_blink_tick_) {
        blink_ticks_left_ = 5;
        double_blink_pending_ = ((anim_tick_ * 17) % 100) < 15;
        next_blink_tick_ = anim_tick_ + 85 + ((anim_tick_ * 37) % 80);
    }

    // 3. Full Expression Showcase Mode (cycles all 98 moods sequentially, ~1.8s each)
    if (showcase_active_) {
        if (anim_tick_ >= showcase_next_tick_) {
            showcase_index_++;
            if (showcase_index_ >= static_cast<int>(EyeMood::kMoodCount)) {
                StopMoodShowcase();
            } else {
                SetNomiMood(static_cast<EyeMood>(showcase_index_));
                showcase_next_tick_ = anim_tick_ + 55;
            }
        }
    } else if (visual_state_ == DeviceVisualState::kIdle && !subtitle_visible_) {
        // 4. Lively Idle NOMI life theater (12s start, switches every 8~12s -> >5min or night sleep)
        uint32_t idle_ticks = anim_tick_ - last_activity_tick_;
        time_t now = std::time(nullptr);
        struct tm ti;
        localtime_r(&now, &ti);
        bool night_quiet = (ti.tm_year + 1900 >= 2024) && (ti.tm_hour >= 23 || ti.tm_hour < 7);

        if ((idle_ticks > 9000 || (night_quiet && idle_ticks > 900)) &&
            current_mood_ != EyeMood::kSleeping) {
            SetNomiMood(EyeMood::kSleeping);
        } else if (!night_quiet && idle_ticks > 360 && anim_tick_ >= next_idle_act_tick_ &&
                   current_mood_ != EyeMood::kSleeping) {
            SetNomiMood(PickIdleTheaterMood());
            next_idle_act_tick_ = anim_tick_ + 240 + ((anim_tick_ * 29) % 130);
        }
    }

    if (nomi_canvas_ != nullptr && !lv_obj_has_flag(nomi_canvas_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_invalidate(nomi_canvas_);
    }
}

void LcdDisplay::UpdateTopBarVisibility() {
    if (!IsRoundDisplay()) return;
    bool show_net = (visual_state_ == DeviceVisualState::kConnecting ||
                     visual_state_ == DeviceVisualState::kWifiError);
    bool show_notif = (notification_label_ != nullptr &&
                       !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN));

    if (top_bar_ != nullptr) {
        if (show_net) {
            lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (network_label_ != nullptr) {
                lv_obj_set_style_text_color(network_label_, lv_color_hex(kColGold), 0);
                lv_obj_set_style_text_opa(network_label_, LV_OPA_COVER, 0);
            }
        } else {
            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Hide top status/time text during normal standby/listening/speaking for a clean full-face NOMI
    if (status_bar_ != nullptr) {
        if (show_net || show_notif) {
            lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, show_net ? 30 : 18);
        } else {
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_opa(status_label_, LV_OPA_90, 0);
    }
}

void LcdDisplay::UpdateSubtitleScrollMetrics() {
    if (!IsRoundDisplay() || bottom_bar_ == nullptr || chat_message_label_ == nullptr) return;
    lv_obj_update_layout(chat_message_label_);
    int label_h = lv_obj_get_height(chat_message_label_);
    subtitle_scroll_y_ = 0;
    subtitle_pause_ticks_ = 36;
    if (label_h > subtitle_box_height_) {
        subtitle_max_scroll_ = label_h - subtitle_box_height_ + 4;
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 0);
    } else {
        subtitle_max_scroll_ = 0;
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, (subtitle_box_height_ - label_h) / 2);
    }
}

void LcdDisplay::ShowEmotion() {
    if (IsRoundDisplay() && nomi_canvas_ != nullptr) {
        lv_obj_remove_flag(nomi_canvas_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(nomi_canvas_);
        return;
    }
    if (emoji_label_ != nullptr) {
        if (emotion_uses_image_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        if (emotion_uses_image_) lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emotion_uses_image_ && gif_controller_ && !gif_controller_->IsPlaying()) {
        gif_controller_->Start();
    }
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    if (setup_ui_called_) return;
    Display::SetupUI();
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);

    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);
    notification_label_ = lv_label_create(status_bar_);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    status_label_ = lv_label_create(status_bar_);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    content_ = lv_obj_create(container_);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_ || content_ == nullptr || content == nullptr || strlen(content) == 0) return;
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    lv_obj_set_width(msg_text, LV_HOR_RES * 80 / 100);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_user_data(msg_bubble, (void*)role);
    chat_message_label_ = msg_text;
    lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    (void)image;
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    (void)role;
    (void)content;
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ != nullptr) lv_obj_clean(content_);
    chat_message_label_ = nullptr;
    ShowEmotion();
}
#else
void LcdDisplay::SetupUI() {
    if (setup_ui_called_) return;

    Display::SetupUI();
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    if (IsRoundDisplay()) {
        /* 1. 30 FPS pure-vector NOMI canvas */
        CreateEyeUI(screen, lvgl_theme);

        /* Hidden compatibility placeholders */
        emoji_box_ = lv_obj_create(screen);
        lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(emoji_box_, 0, 0);
        lv_obj_set_style_border_width(emoji_box_, 0, 0);
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

        emoji_label_ = lv_label_create(emoji_box_);
        lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

        emoji_image_ = lv_img_create(emoji_box_);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

        preview_image_ = lv_image_create(screen);
        lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

        /* 2. Top icon bar (visible only when offline/connecting) */
        top_bar_ = lv_obj_create(screen);
        lv_obj_set_size(top_bar_, 110, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(top_bar_, 0, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top_bar_, 0, 0);
        lv_obj_set_style_pad_all(top_bar_, 0, 0);
        lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(top_bar_, 6, 0);
        lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 12);

        network_label_ = lv_label_create(top_bar_);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lv_color_hex(kColGold), 0);

        mute_label_ = lv_label_create(top_bar_);
        lv_label_set_text(mute_label_, "");
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

        battery_label_ = lv_label_create(top_bar_);
        lv_label_set_text(battery_label_, "");
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

        /* 3. Status bar */
        status_bar_ = lv_obj_create(screen);
        lv_obj_set_width(status_bar_, 126);
        lv_obj_set_height(status_bar_, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(status_bar_, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(status_bar_, 0, 0);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 30);

        notification_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(notification_label_, 124);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(notification_label_, "");
        lv_obj_align(notification_label_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        status_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(status_label_, 124);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 0);

        /* 4. Borderless circular-safe subtitle box */
        subtitle_box_height_ =
            std::max(38, std::min(50, static_cast<int>(text_font->line_height) * 2 + 6));
        bottom_bar_ = lv_obj_create(screen);
        lv_obj_set_size(bottom_bar_, 136, subtitle_box_height_);
        lv_obj_set_style_radius(bottom_bar_, 0, 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bottom_bar_, 0, 0);
        lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -30);

        chat_message_label_ = lv_label_create(bottom_bar_);
        lv_label_set_text(chat_message_label_, "");
        lv_obj_set_width(chat_message_label_, 134);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(chat_message_label_, 2, 0);
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

        /* 5. Low battery popup */
        low_battery_popup_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(low_battery_popup_, 142, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(low_battery_popup_, 6, 0);
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -32);
        lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
        lv_obj_set_style_border_width(low_battery_popup_, 0, 0);
        lv_obj_set_style_radius(low_battery_popup_, 12, 0);

        low_battery_label_ = lv_label_create(low_battery_popup_);
        lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
        lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
        lv_obj_center(low_battery_label_);
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

        UpdateTopBarVisibility();
        return;
    }

    /* Original rectangular display layout preserved intact */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) return;
    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        ShowEmotion();
        return;
    }
    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }
    if (gif_controller_) gif_controller_->Stop();
    if (nomi_canvas_ != nullptr) lv_obj_add_flag(nomi_canvas_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_ != nullptr) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) return;
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) return;

    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, content != nullptr ? content : "");

    bool has_content = (content != nullptr && content[0] != '\0');
    bool is_system = (role != nullptr && strcmp(role, "system") == 0);

    if (IsRoundDisplay() && has_content) {
        last_activity_tick_ = anim_tick_;
        if (HandleVoiceDisplayCommand(content)) {
            if (bottom_bar_ != nullptr && hide_subtitle_ && !is_system) {
                lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
                subtitle_visible_ = false;
            }
            return;
        }
    }

    if (bottom_bar_ != nullptr) {
        if (!has_content || (hide_subtitle_ && !is_system)) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            subtitle_visible_ = false;
        } else {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            subtitle_visible_ = true;
            if (IsRoundDisplay()) {
                lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -26);
                UpdateSubtitleScrollMetrics();
            }
        }
    }

    if (IsRoundDisplay() && has_content && !showcase_active_) {
        EyeMood ctx_mood = current_mood_;
        if (MatchContextualMood(content, ctx_mood)) {
            SetNomiMood(ctx_mood);
        } else if (nomi_canvas_ != nullptr) {
            lv_obj_invalidate(nomi_canvas_);
        }
    }
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    (void)role;
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr ||
        strcmp(lv_label_get_text(chat_message_label_), content) != 0) {
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, "");
    if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    subtitle_visible_ = false;
    if (nomi_canvas_ != nullptr) lv_obj_invalidate(nomi_canvas_);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_message_label_ != nullptr) lv_label_set_text(chat_message_label_, "");
    if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    subtitle_visible_ = false;
    if (nomi_canvas_ != nullptr) lv_obj_invalidate(nomi_canvas_);
}
#endif

void LcdDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);
    if (!IsRoundDisplay()) return;
    DisplayLockGuard lock(this);
    UpdateTopBarVisibility();
}

void LcdDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);
    if (!IsRoundDisplay() || status == nullptr) return;

    DisplayLockGuard lock(this);
    DeviceVisualState prev_state = visual_state_;
    DeviceVisualState new_state = visual_state_;

    if (strstr(status, "聆听") != nullptr || strstr(status, "Listen") != nullptr ||
        strstr(status, "listen") != nullptr) {
        new_state = DeviceVisualState::kListening;
        if (showcase_active_ && (anim_tick_ - showcase_start_tick_) > 75) {
            showcase_active_ = false;
        }
        if (!showcase_active_) {
            SetNomiMood(EyeMood::kListening);
        }
        last_activity_tick_ = anim_tick_;
    } else if (strstr(status, "说话") != nullptr || strstr(status, "Speak") != nullptr ||
               strstr(status, "speak") != nullptr) {
        new_state = DeviceVisualState::kSpeaking;
        if (!showcase_active_ &&
            (current_mood_ == EyeMood::kListening || current_mood_ == EyeMood::kSleeping ||
             current_mood_ == EyeMood::kConnecting)) {
            static const EyeMood kSpeakVar[4] = {EyeMood::kSpeaking, EyeMood::kHappy,
                                                 EyeMood::kGreeting, EyeMood::kCurious};
            SetNomiMood(kSpeakVar[(anim_tick_ / 13) % 4]);
        }
        last_activity_tick_ = anim_tick_;
    } else if (strstr(status, "断开") != nullptr || strstr(status, "失败") != nullptr ||
               strstr(status, "离线") != nullptr || strstr(status, "Error") != nullptr ||
               strstr(status, "Offline") != nullptr) {
        new_state = DeviceVisualState::kWifiError;
        showcase_active_ = false;
        SetNomiMood(EyeMood::kWifiError);
    } else if (strstr(status, "连接") != nullptr || strstr(status, "初始") != nullptr ||
               strstr(status, "配网") != nullptr || strstr(status, "Wi-Fi") != nullptr ||
               strstr(status, "WiFi") != nullptr || strstr(status, "Connect") != nullptr ||
               strstr(status, "Init") != nullptr || strstr(status, "Config") != nullptr) {
        new_state = DeviceVisualState::kConnecting;
        showcase_active_ = false;
        SetNomiMood(EyeMood::kConnecting);
    } else if (strstr(status, "待命") != nullptr || strstr(status, "Standby") != nullptr ||
               strstr(status, "Idle") != nullptr) {
        new_state = DeviceVisualState::kIdle;
        if (!showcase_active_) {
            if (prev_state == DeviceVisualState::kConnecting ||
                prev_state == DeviceVisualState::kWifiError) {
                SetNomiMood(EyeMood::kWakingUp);
            } else {
                SetNomiMood(EyeMood::kNeutral);
            }
            next_idle_act_tick_ = anim_tick_ + 360;
        }
        last_activity_tick_ = anim_tick_;
    }

    visual_state_ = new_state;
    UpdateTopBarVisibility();
    ShowEmotion();
}

bool LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_ || emotion == nullptr) return false;

    if (IsRoundDisplay() && nomi_canvas_ != nullptr) {
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        last_activity_tick_ = anim_tick_;
        if (strcmp(emotion, "demo") == 0 || strcmp(emotion, "showcase") == 0 ||
            strcmp(emotion, "all") == 0) {
            StartMoodShowcase();
        } else if (!showcase_active_) {
            EyeMood parsed = ParseEmotionMood(emotion);
            if (!(parsed == EyeMood::kNeutral && visual_state_ == DeviceVisualState::kSpeaking)) {
                SetNomiMood(parsed);
            }
        }
        emotion_uses_image_ = false;
        ShowEmotion();
        return true;
    }

    if (emoji_image_ == nullptr) return false;
    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 == nullptr || emoji_label_ == nullptr) return false;
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
        lv_label_set_text(emoji_label_, utf8);
        emotion_uses_image_ = false;
        ShowEmotion();
        return true;
    }

    DisplayLockGuard lock(this);
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    bool shown = false;
    if (image->IsGif()) {
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        if (gif_controller_->IsLoaded()) {
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            emotion_uses_image_ = true;
            ShowEmotion();
            shown = true;
        } else {
            gif_controller_.reset();
            emotion_uses_image_ = false;
            ShowEmotion();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        emotion_uses_image_ = true;
        ShowEmotion();
        shown = true;
    }
    return shown;
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    lv_obj_t* screen = lv_screen_active();
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    const lv_font_t* bar_icon_font = (text_font->line_height >= 40) ? large_icon_font : icon_font;
    if (mute_label_ != nullptr) lv_obj_set_style_text_font(mute_label_, bar_icon_font, 0);
    if (battery_label_ != nullptr) lv_obj_set_style_text_font(battery_label_, bar_icon_font, 0);
    if (network_label_ != nullptr) lv_obj_set_style_text_font(network_label_, bar_icon_font, 0);

    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    if (IsRoundDisplay()) {
        subtitle_box_height_ =
            std::max(38, std::min(50, static_cast<int>(text_font->line_height) * 2 + 6));
        if (bottom_bar_ != nullptr) {
            lv_obj_set_size(bottom_bar_, 136, subtitle_box_height_);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bottom_bar_, 0, 0);
        }
        if (chat_message_label_ != nullptr) {
            lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
        }
        UpdateTopBarVisibility();
        UpdateSubtitleScrollMetrics();
        ShowEmotion();
    }
    if (low_battery_popup_ != nullptr) {
        lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    }
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            subtitle_visible_ = false;
        } else {
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
                subtitle_visible_ = true;
                UpdateSubtitleScrollMetrics();
            }
        }
    }
    if (nomi_canvas_ != nullptr) lv_obj_invalidate(nomi_canvas_);
}
