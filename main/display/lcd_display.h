#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>
#include <string>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LvglTheme;

class LcdDisplay : public LvglDisplay {
protected:
    // 54 NIO NOMI-inspired contextual moods
    enum class EyeMood {
        // Core emotions (18)
        kNeutral = 0,
        kHappy,
        kLaughing,
        kGrinning,
        kLoving,
        kKissy,
        kWinking,
        kSad,
        kCrying,
        kAngry,
        kFurious,
        kSurprised,
        kShocked,
        kThinking,
        kConfused,
        kEmbarrassed,
        kShy,
        kSilly,
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
        // Daily life scenarios (14)
        kGreeting,
        kGoodnight,
        kGoodbye,
        kThankYou,
        kCelebrate,
        kMusic,
        kDelicious,
        kCoffee,
        kSunny,
        kRainy,
        kCold,
        kHot,
        kAlarm,
        kSecret,
        // Personality & fun scenarios (12)
        kCool,
        kConfident,
        kCheer,
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

    // Round display NOMI face rig & emotion halo
    lv_obj_t* emotion_halo_ = nullptr;
    lv_obj_t* eye_container_ = nullptr;
    lv_obj_t* left_eye_wrap_ = nullptr;
    lv_obj_t* right_eye_wrap_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_highlight_ = nullptr;
    lv_obj_t* right_highlight_ = nullptr;
    lv_obj_t* left_top_lid_ = nullptr;
    lv_obj_t* right_top_lid_ = nullptr;
    lv_obj_t* left_bottom_lid_ = nullptr;
    lv_obj_t* right_bottom_lid_ = nullptr;
    lv_obj_t* left_blush_ = nullptr;
    lv_obj_t* right_blush_ = nullptr;
    lv_obj_t* glasses_bridge_ = nullptr;
    lv_obj_t* mouth_arc_ = nullptr;
    lv_obj_t* mouth_shape_ = nullptr;
    lv_obj_t* mouth_tongue_ = nullptr;
    lv_obj_t* prop_drop_ = nullptr;
    lv_obj_t* prop_label_ = nullptr;
    lv_timer_t* eye_timer_ = nullptr;

    EyeMood current_mood_ = EyeMood::kConnecting;
    DeviceVisualState visual_state_ = DeviceVisualState::kConnecting;
    uint32_t anim_tick_ = 0;
    uint32_t last_activity_tick_ = 0;
    uint32_t next_blink_tick_ = 40;
    uint32_t next_gaze_tick_ = 28;
    uint32_t next_idle_act_tick_ = 400;
    int blink_phase_ = 0;
    int gaze_x_ = 0;
    int gaze_y_ = 0;
    int target_gaze_x_ = 0;
    int target_gaze_y_ = 0;
    int halo_angle_ = 270;
    bool subtitle_visible_ = false;
    int subtitle_scroll_y_ = 0;
    int subtitle_max_scroll_ = 0;
    int subtitle_pause_ticks_ = 0;
    int subtitle_box_height_ = 46;

    bool IsRoundDisplay() const { return width_ == height_ && width_ <= 240; }
    void InitializeLcdThemes();
    void ShowEmotion();
    void CreateEyeUI(lv_obj_t* screen, LvglTheme* lvgl_theme);
    void ApplyEyeMood();
    void UpdateEyeLayout();
    void UpdateTopBarVisibility();
    void UpdateSubtitleScrollMetrics();
    void OnEyeTimerTick();
    static void EyeTimerCallback(lv_timer_t* timer);
    EyeMood ParseEmotionMood(const char* emotion) const;
    bool MatchContextualMood(const char* text, EyeMood& out_mood) const;

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
