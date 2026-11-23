#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LvglTheme;

class LcdDisplay : public LvglDisplay {
protected:
    // NIO NOMI-inspired 240x240 round display mood system (Core + Device + Weather + Split Festivals + Music + Daily)
    enum class EyeMood {
        // Core emotions (24)
        kNeutral = 0,
        kHappy,
        kLaughing,
        kGrinning,
        kLoving,
        kKissy,
        kWinking,
        kStarry,
        kTouched,
        kPout,
        kSad,
        kCrying,
        kAngry,
        kFurious,
        kSurprised,
        kShocked,
        kThinking,
        kConfused,
        kSpeechless,
        kGuilty,
        kEmbarrassed,
        kShy,
        kSilly,
        kDazed,
        // Device states & lifecycle (10)
        kListening,
        kSpeaking,
        kConnecting,
        kWifiError,
        kMuted,
        kLowBattery,
        kCharging,
        kWakingUp,
        kYawning,
        kSleeping,
        // Weather & temperature tiers (8)
        kSunny,
        kWeatherHotHat,
        kHot,
        kWeatherWindy,
        kRainy,
        kCold,
        kWeatherSnow,
        kWeatherHazeMask,
        // Year-round split festival easter eggs (16 - one prop per scene!)
        kFestFirecracker,
        kFestRedPacket,
        kFestLantern,
        kFestFireworks,
        kFestXmasHat,
        kFestXmasTree,
        kFestXmasGift,
        kFestRose,
        kFestZongzi,
        kFestMidAutumnMoon,
        kFestMidAutumnCake,
        kFestNationalFlag,
        kFestHalloweenPumpkin,
        kFestHalloweenGhost,
        kFestBirthday,
        kCelebrate,
        // Music & stage instruments (10)
        kMusic,
        kMusicSax,
        kMusicViolin,
        kMusicElecGuitar,
        kMusicHandpan,
        kCheer,
        kMusicHipHop,
        kMusicDJ,
        kMusicMic,
        kMusicNotes,
        // Daily life, personality & idle theater (18)
        kGreeting,
        kGoodnight,
        kGoodbye,
        kThankYou,
        kClockBoard,
        kClockGlasses,
        kChaseDot,
        kTyping,
        kCoffee,
        kBobaTea,
        kMagnifier,
        kHeadphones,
        kPottedPlant,
        kStretch,
        kDelicious,
        kAlarm,
        kSecret,
        kCool,
        kConfident,
        kWorkFocus,
        kTired,
        kMoney,
        kMarketDown,
        kScared,
        kSick,
        kProud,
        kBored,
        kCurious,
    };

    enum class DeviceVisualState {
        kConnecting,
        kWifiError,
        kIdle,
        kListening,
        kSpeaking,
    };

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    bool emotion_uses_image_ = false;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;

    // Round display NOMI 30 FPS pure-vector LVGL 9 renderer
    lv_obj_t* nomi_canvas_ = nullptr;
    lv_timer_t* eye_timer_ = nullptr;

    EyeMood current_mood_ = EyeMood::kConnecting;
    EyeMood prev_mood_ = EyeMood::kNeutral;
    DeviceVisualState visual_state_ = DeviceVisualState::kConnecting;
    uint32_t anim_tick_ = 0;         // 33ms per tick (~30 FPS)
    uint32_t mood_enter_tick_ = 0;   // Tick when current_mood_ started (drives collapse->bloom & prop pop-in)
    uint32_t last_activity_tick_ = 0;
    uint32_t next_blink_tick_ = 90;
    uint32_t next_idle_act_tick_ = 900;
    int blink_ticks_left_ = 0;
    bool double_blink_pending_ = false;
    bool subtitle_visible_ = false;
    int subtitle_scroll_y_ = 0;
    int subtitle_max_scroll_ = 0;
    int subtitle_pause_ticks_ = 0;
    int subtitle_box_height_ = 46;

    bool IsRoundDisplay() const { return width_ == height_ && width_ <= 240; }
    void InitializeLcdThemes();
    void ShowEmotion();
    void CreateEyeUI(lv_obj_t* screen, LvglTheme* lvgl_theme);
    void SetNomiMood(EyeMood mood);
    void UpdateTopBarVisibility();
    void UpdateSubtitleScrollMetrics();
    void OnEyeTimerTick();
    void RenderNomiFrame(lv_layer_t* layer);
    static void EyeTimerCallback(lv_timer_t* timer);
    static void NomiDrawEventCb(lv_event_t* e);
    EyeMood ParseEmotionMood(const char* emotion) const;
    bool MatchContextualMood(const char* text, EyeMood& out_mood) const;
    EyeMood PickIdleTheaterMood() const;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    virtual void SetTheme(Theme* theme) override;
    virtual void SetHideSubtitle(bool hide) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
