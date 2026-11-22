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
#include <cstring>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

namespace {

enum class MouthKind {
    kHidden,
    kSmile,
    kBigSmile,
    kSad,
    kSmallO,
    kBigO,
    kFlat,
    kWavy,
    kTongue,
    kZipper,
    kSmirk,
    kTalking,
};

enum class PropKind {
    kNone,
    kGlasses,
    kTear,
    kSweat,
    kBadge,
};

enum class MotionKind {
    kCalm,
    kBounce,
    kSway,
    kShiver,
    kNod,
    kSleep,
    kScan,
    kAlert,
};

struct MoodSpec {
    uint32_t color_hex;
    uint32_t halo_hex;
    int lw, lh, lrad;
    int rw, rh, rrad;
    int lt_x, lt_y, lt_rot;
    int rt_x, rt_y, rt_rot;
    int lb_y, rb_y;
    bool l_hi, r_hi;
    bool blush;
    MouthKind mouth;
    PropKind prop;
    const char* badge_text;
    MotionKind motion;
    bool spin_halo;
};

constexpr MoodSpec MakeSpec(uint32_t color, uint32_t halo, int lw, int lh, int lrad, int rw,
                            int rh, int rrad, int ltx, int lty, int ltrot, int rtx, int rty,
                            int rtrot, int lby, int rby, bool lhi, bool rhi, bool blush,
                            MouthKind mouth, PropKind prop, const char* badge, MotionKind motion,
                            bool spin = false) {
    return MoodSpec{color, halo,  lw,  lh,  lrad,  rw,   rh,    rrad,   ltx,    lty,
                    ltrot, rtx,   rty, rtrot, lby,  rby,  lhi,   rhi,    blush,  mouth,
                    prop,  badge, motion, spin};
}

}  // namespace

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
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

    // dark theme (default for round screens)
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
    // Clear screen with black to avoid white flash on boot
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
            if (this_chunk > chunk_size) {
                this_chunk = chunk_size;
            }
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) {
                    heap_caps_free(pool);
                }
                ESP_LOGE(TAG, "Failed to add %uKB LVGL PSRAM pool (added %uKB)",
                         (unsigned)(this_chunk / 1024), (unsigned)(added / 1024));
                break;
            }
            added += this_chunk;
        }
        if (added == extra_total) {
            ESP_LOGI(TAG, "Added %uKB LVGL pool in PSRAM", (unsigned)(added / 1024));
        }
#endif
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
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

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
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
    if (eye_container_ != nullptr) lv_obj_del(eye_container_);
    if (emotion_halo_ != nullptr) lv_obj_del(emotion_halo_);
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

static MoodSpec GetMoodSpec(int mood_index) {
    // 54 NIO NOMI-inspired expressions
    switch (mood_index) {
        // --- Core Emotions (18) ---
        case 0:  // kNeutral
            return MakeSpec(0x38BDF8, 0x38BDF8, 46, 56, 20, 46, 56, 20, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kHidden, PropKind::kNone, "",
                            MotionKind::kCalm);
        case 1:  // kHappy
            return MakeSpec(0xFFD166, 0xFFD166, 50, 54, 24, 50, 54, 24, 0, -48, 0, 0, -48, 0, 22,
                            22, true, true, true, MouthKind::kSmile, PropKind::kNone, "",
                            MotionKind::kBounce);
        case 2:  // kLaughing
            return MakeSpec(0xFFC43D, 0xFFD166, 52, 52, 26, 52, 52, 26, 0, -48, 0, 0, -48, 0, 14,
                            14, false, false, true, MouthKind::kBigSmile, PropKind::kNone, "",
                            MotionKind::kBounce);
        case 3:  // kGrinning
            return MakeSpec(0xFBBF24, 0xF59E0B, 48, 44, 18, 48, 44, 18, 0, -34, 0, 0, -34, 0, 26,
                            26, true, true, false, MouthKind::kSmirk, PropKind::kNone, "",
                            MotionKind::kSway);
        case 4:  // kLoving
            return MakeSpec(0xFF6B9E, 0xFF6B9E, 52, 56, 26, 52, 56, 26, 0, -48, 0, 0, -48, 0, 20,
                            20, true, true, true, MouthKind::kSmile, PropKind::kBadge, "<3",
                            MotionKind::kBounce);
        case 5:  // kKissy
            return MakeSpec(0xFF7EB6, 0xFF6B9E, 48, 10, 5, 48, 56, 22, 0, -48, 0, 0, -48, 0, 58,
                            24, false, true, true, MouthKind::kSmallO, PropKind::kBadge, "<3",
                            MotionKind::kNod);
        case 6:  // kWinking
            return MakeSpec(0x34D399, 0x34D399, 48, 56, 22, 48, 10, 5, 0, -48, 0, 0, -48, 0, 58,
                            58, true, false, true, MouthKind::kSmile, PropKind::kNone, "",
                            MotionKind::kNod);
        case 7:  // kSad
            return MakeSpec(0x4A90E2, 0x4A90E2, 44, 50, 18, 44, 50, 18, -6, -12, -180, 6, -12,
                            180, 58, 58, true, true, false, MouthKind::kSad, PropKind::kNone, "",
                            MotionKind::kCalm);
        case 8:  // kCrying
            return MakeSpec(0x3B82F6, 0x2563EB, 44, 48, 18, 44, 48, 18, -6, -10, -200, 6, -10,
                            200, 28, 28, true, true, false, MouthKind::kSad, PropKind::kTear, "",
                            MotionKind::kShiver);
        case 9:  // kAngry
            return MakeSpec(0xFF4757, 0xFF4757, 46, 50, 14, 46, 50, 14, 6, -10, 200, -6, -10,
                            -200, 58, 58, true, true, false, MouthKind::kFlat, PropKind::kNone, "",
                            MotionKind::kAlert);
        case 10:  // kFurious
            return MakeSpec(0xEF233C, 0xFF2A2A, 48, 44, 10, 48, 44, 10, 6, -4, 240, -6, -4, -240,
                            58, 58, false, false, false, MouthKind::kBigO, PropKind::kBadge, "!!",
                            MotionKind::kShiver);
        case 11:  // kSurprised
            return MakeSpec(0xC084FC, 0xC084FC, 54, 64, 27, 54, 64, 27, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kBigO, PropKind::kBadge, "!",
                            MotionKind::kBounce);
        case 12:  // kShocked
            return MakeSpec(0xE879F9, 0xD946EF, 26, 26, 13, 26, 26, 13, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kBigO, PropKind::kBadge, "?!",
                            MotionKind::kShiver);
        case 13:  // kThinking
            return MakeSpec(0xF59E0B, 0xF59E0B, 48, 56, 22, 44, 42, 18, 0, -48, 0, 0, -24, 0, 58,
                            58, true, true, false, MouthKind::kFlat, PropKind::kBadge, "?",
                            MotionKind::kScan, true);
        case 14:  // kConfused
            return MakeSpec(0xFB923C, 0xF59E0B, 48, 56, 20, 42, 38, 16, -4, -30, -120, 4, -18,
                            120, 58, 58, true, true, false, MouthKind::kWavy, PropKind::kBadge,
                            "??", MotionKind::kSway);
        case 15:  // kEmbarrassed
            return MakeSpec(0xF472B6, 0xF472B6, 42, 44, 18, 42, 44, 18, 0, -48, 0, 0, -48, 0, 26,
                            26, true, true, true, MouthKind::kWavy, PropKind::kSweat, "",
                            MotionKind::kSway);
        case 16:  // kShy
            return MakeSpec(0xFF85A1, 0xFF85A1, 44, 48, 20, 44, 48, 20, 0, -32, 0, 0, -32, 0, 24,
                            24, true, true, true, MouthKind::kSmile, PropKind::kNone, "",
                            MotionKind::kCalm);
        case 17:  // kSilly
            return MakeSpec(0xA3E635, 0x84CC16, 52, 60, 26, 36, 36, 18, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, true, MouthKind::kTongue, PropKind::kNone, "",
                            MotionKind::kSway);

        // --- Device States & Lifecycle (10) ---
        case 18:  // kListening
            return MakeSpec(0x00E5FF, 0x00E5FF, 50, 62, 24, 50, 62, 24, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kSmallO, PropKind::kNone, "",
                            MotionKind::kCalm, true);
        case 19:  // kSpeaking
            return MakeSpec(0x38BDF8, 0x38BDF8, 48, 56, 22, 48, 56, 22, 0, -48, 0, 0, -48, 0, 28,
                            28, true, true, false, MouthKind::kTalking, PropKind::kNone, "",
                            MotionKind::kBounce);
        case 20:  // kConnecting
            return MakeSpec(0x38BDF8, 0xF59E0B, 46, 54, 20, 46, 54, 20, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kHidden, PropKind::kBadge, "...",
                            MotionKind::kScan, true);
        case 21:  // kWifiError
            return MakeSpec(0xFB7185, 0xF97316, 44, 48, 18, 44, 48, 18, -6, -12, -160, 6, -12,
                            160, 58, 58, true, true, false, MouthKind::kSad, PropKind::kSweat, "",
                            MotionKind::kAlert);
        case 22:  // kMuted
            return MakeSpec(0x94A3B8, 0x64748B, 46, 46, 18, 46, 46, 18, 0, -30, 0, 0, -30, 0, 58,
                            58, true, true, false, MouthKind::kZipper, PropKind::kNone, "",
                            MotionKind::kCalm);
        case 23:  // kLowBattery
            return MakeSpec(0xEF4444, 0xEF4444, 46, 32, 12, 46, 32, 12, 0, -14, 0, 0, -14, 0, 58,
                            58, false, false, false, MouthKind::kWavy, PropKind::kBadge, "!",
                            MotionKind::kShiver);
        case 24:  // kCharging
            return MakeSpec(0x22C55E, 0x22C55E, 50, 58, 24, 50, 58, 24, 0, -48, 0, 0, -48, 0, 22,
                            22, true, true, true, MouthKind::kSmile, PropKind::kBadge, "+",
                            MotionKind::kBounce);
        case 25:  // kWakingUp
            return MakeSpec(0x7DD3FC, 0x38BDF8, 48, 34, 16, 48, 34, 16, 0, -24, 0, 0, -24, 0, 58,
                            58, true, true, false, MouthKind::kSmallO, PropKind::kNone, "",
                            MotionKind::kNod);
        case 26:  // kYawning
            return MakeSpec(0x818CF8, 0x6366F1, 46, 36, 16, 46, 36, 16, 0, -18, 0, 0, -18, 0, 24,
                            24, false, false, false, MouthKind::kBigO, PropKind::kNone, "",
                            MotionKind::kCalm);
        case 27:  // kSleeping
            return MakeSpec(0x6366F1, 0x4338CA, 48, 10, 5, 48, 10, 5, 0, -48, 0, 0, -48, 0, 58,
                            58, false, false, false, MouthKind::kSmallO, PropKind::kBadge, "Zzz",
                            MotionKind::kSleep);

        // --- Daily Life Scenarios (14) ---
        case 28:  // kGreeting
            return MakeSpec(0xFDE047, 0xFACC15, 50, 56, 24, 50, 56, 24, 0, -48, 0, 0, -48, 0, 20,
                            20, true, true, true, MouthKind::kBigSmile, PropKind::kBadge, "Hi",
                            MotionKind::kBounce);
        case 29:  // kGoodnight
            return MakeSpec(0x818CF8, 0x6366F1, 48, 46, 22, 48, 46, 22, 0, -48, 0, 0, -48, 0, 18,
                            18, false, false, true, MouthKind::kSmile, PropKind::kBadge, "Zzz",
                            MotionKind::kSleep);
        case 30:  // kGoodbye
            return MakeSpec(0x2DD4BF, 0x14B8A6, 48, 54, 22, 48, 10, 5, 0, -48, 0, 0, -48, 0, 22,
                            58, true, false, true, MouthKind::kSmile, PropKind::kBadge, "Bye",
                            MotionKind::kSway);
        case 31:  // kThankYou
            return MakeSpec(0xFB7185, 0xF43F5E, 50, 52, 24, 50, 52, 24, 0, -48, 0, 0, -48, 0, 16,
                            16, false, false, true, MouthKind::kSmile, PropKind::kNone, "",
                            MotionKind::kNod);
        case 32:  // kCelebrate
            return MakeSpec(0xFCD34D, 0xFF6B9E, 54, 58, 27, 54, 58, 27, 0, -48, 0, 0, -48, 0, 16,
                            16, true, true, true, MouthKind::kBigSmile, PropKind::kBadge, "!",
                            MotionKind::kBounce);
        case 33:  // kMusic
            return MakeSpec(0xA78BFA, 0x38BDF8, 48, 44, 22, 48, 44, 22, 0, -48, 0, 0, -48, 0, 18,
                            18, true, true, true, MouthKind::kSmallO, PropKind::kBadge, "o/",
                            MotionKind::kSway);
        case 34:  // kDelicious
            return MakeSpec(0xFB923C, 0xF97316, 50, 54, 24, 50, 54, 24, 0, -48, 0, 0, -48, 0, 20,
                            20, true, true, true, MouthKind::kTongue, PropKind::kNone, "",
                            MotionKind::kBounce);
        case 35:  // kCoffee
            return MakeSpec(0xD97706, 0xF59E0B, 52, 60, 24, 52, 60, 24, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kSmile, PropKind::kBadge, "OK",
                            MotionKind::kNod);
        case 36:  // kSunny
            return MakeSpec(0xFACC15, 0xF59E0B, 52, 58, 26, 52, 58, 26, 0, -48, 0, 0, -48, 0, 22,
                            22, true, true, true, MouthKind::kBigSmile, PropKind::kNone, "",
                            MotionKind::kBounce);
        case 37:  // kRainy
            return MakeSpec(0x60A5FA, 0x3B82F6, 46, 52, 20, 46, 52, 20, -4, -18, -120, 4, -18,
                            120, 58, 58, true, true, false, MouthKind::kFlat, PropKind::kTear, "",
                            MotionKind::kCalm);
        case 38:  // kCold
            return MakeSpec(0x67E8F9, 0x06B6D4, 42, 44, 16, 42, 44, 16, 0, -24, 0, 0, -24, 0, 58,
                            58, true, true, false, MouthKind::kWavy, PropKind::kNone, "",
                            MotionKind::kShiver);
        case 39:  // kHot
            return MakeSpec(0xF97316, 0xEF4444, 48, 40, 16, 48, 40, 16, 0, -20, 0, 0, -20, 0, 58,
                            58, false, false, true, MouthKind::kTongue, PropKind::kSweat, "",
                            MotionKind::kSway);
        case 40:  // kAlarm
            return MakeSpec(0xFF595E, 0xFFCA3A, 54, 62, 26, 54, 62, 26, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kBigO, PropKind::kBadge, "!!",
                            MotionKind::kAlert);
        case 41:  // kSecret
            return MakeSpec(0x38BDF8, 0x6366F1, 46, 46, 18, 46, 46, 18, 0, -24, 0, 0, -24, 0, 58,
                            58, true, true, false, MouthKind::kSmallO, PropKind::kBadge, "...",
                            MotionKind::kScan);

        // --- Personality & Fun Scenarios (12) ---
        case 42:  // kCool (NOMI Sunglasses)
            return MakeSpec(0x22D3EE, 0x06B6D4, 54, 34, 8, 54, 34, 8, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kSmirk, PropKind::kGlasses, "",
                            MotionKind::kSway);
        case 43:  // kConfident
            return MakeSpec(0x0EA5E9, 0x38BDF8, 48, 52, 18, 48, 40, 14, 0, -34, 0, -4, -18, -140,
                            58, 58, true, true, false, MouthKind::kSmirk, PropKind::kNone, "",
                            MotionKind::kNod);
        case 44:  // kCheer
            return MakeSpec(0xFB923C, 0xFACC15, 52, 60, 24, 52, 60, 24, 0, -48, 0, 0, -48, 0, 22,
                            22, true, true, true, MouthKind::kBigSmile, PropKind::kBadge, "GO!",
                            MotionKind::kBounce);
        case 45:  // kWorkFocus
            return MakeSpec(0x3B82F6, 0x1D4ED8, 48, 46, 14, 48, 46, 14, 0, -24, 0, 0, -24, 0, 58,
                            58, true, true, false, MouthKind::kFlat, PropKind::kNone, "",
                            MotionKind::kCalm);
        case 46:  // kTired
            return MakeSpec(0x64748B, 0x475569, 46, 36, 14, 46, 36, 14, -4, -14, -100, 4, -14,
                            100, 58, 58, false, false, false, MouthKind::kFlat, PropKind::kSweat,
                            "", MotionKind::kCalm);
        case 47:  // kMoney
            return MakeSpec(0xFACC15, 0x22C55E, 52, 60, 24, 52, 60, 24, 0, -48, 0, 0, -48, 0, 22,
                            22, true, true, true, MouthKind::kBigSmile, PropKind::kBadge, "$$",
                            MotionKind::kBounce);
        case 48:  // kMarketDown
            return MakeSpec(0x10B981, 0x059669, 46, 26, 8, 46, 26, 8, -4, -18, -140, 4, -18, 140,
                            58, 58, false, false, false, MouthKind::kWavy, PropKind::kSweat, "",
                            MotionKind::kShiver);
        case 49:  // kScared
            return MakeSpec(0xA5B4FC, 0x818CF8, 34, 42, 16, 34, 42, 16, -6, -16, -180, 6, -16,
                            180, 58, 58, true, true, false, MouthKind::kBigO, PropKind::kSweat, "",
                            MotionKind::kShiver);
        case 50:  // kSick
            return MakeSpec(0x86EFAC, 0x4ADE80, 44, 34, 14, 44, 34, 14, -6, -12, -160, 6, -12,
                            160, 58, 58, false, false, false, MouthKind::kSad, PropKind::kSweat,
                            "", MotionKind::kCalm);
        case 51:  // kProud
            return MakeSpec(0xF472B6, 0xFBBF24, 48, 38, 18, 48, 38, 18, 0, -48, 0, 0, -48, 0, 20,
                            20, false, false, true, MouthKind::kSmirk, PropKind::kNone, "",
                            MotionKind::kNod);
        case 52:  // kBored
            return MakeSpec(0x7DD3FC, 0x38BDF8, 46, 36, 12, 46, 36, 12, 0, -18, 0, 0, -18, 0, 58,
                            58, true, true, false, MouthKind::kFlat, PropKind::kNone, "",
                            MotionKind::kScan);
        case 53:  // kCurious
        default:
            return MakeSpec(0x38BDF8, 0x22D3EE, 54, 64, 26, 48, 54, 22, 0, -48, 0, 0, -48, 0, 58,
                            58, true, true, false, MouthKind::kSmallO, PropKind::kBadge, "?",
                            MotionKind::kSway);
    }
}

LcdDisplay::EyeMood LcdDisplay::ParseEmotionMood(const char* emotion) const {
    if (emotion == nullptr) return EyeMood::kNeutral;
    if (strcmp(emotion, "happy") == 0) return EyeMood::kHappy;
    if (strcmp(emotion, "laughing") == 0 || strcmp(emotion, "funny") == 0)
        return EyeMood::kLaughing;
    if (strcmp(emotion, "grinning") == 0) return EyeMood::kGrinning;
    if (strcmp(emotion, "loving") == 0) return EyeMood::kLoving;
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
    if (strcmp(emotion, "relaxed") == 0) return EyeMood::kSleeping;
    if (strcmp(emotion, "delicious") == 0) return EyeMood::kDelicious;
    if (strcmp(emotion, "cool") == 0) return EyeMood::kCool;
    if (strcmp(emotion, "confident") == 0) return EyeMood::kConfident;
    if (strcmp(emotion, "music") == 0) return EyeMood::kMusic;
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

    if (has("生日") || has("恭喜") || has("新年") || has("快乐") || has("庆祝")) {
        out_mood = EyeMood::kCelebrate;
        return true;
    }
    if (has("晚安") || has("好梦") || has("睡觉") || has("good night")) {
        out_mood = EyeMood::kGoodnight;
        return true;
    }
    if (has("早安") || has("早上好") || has("你好") || has("哈喽") || has("hello")) {
        out_mood = EyeMood::kGreeting;
        return true;
    }
    if (has("再见") || has("拜拜") || has("bye")) {
        out_mood = EyeMood::kGoodbye;
        return true;
    }
    if (has("谢谢") || has("感谢") || has("多谢")) {
        out_mood = EyeMood::kThankYou;
        return true;
    }
    if (has("下雨") || has("暴雨") || has("阵雨") || has("带伞") || has("阴雨")) {
        out_mood = EyeMood::kRainy;
        return true;
    }
    if (has("晴天") || has("阳光") || has("出太阳") || has("晴朗")) {
        out_mood = EyeMood::kSunny;
        return true;
    }
    if (has("降温") || has("好冷") || has("寒冷") || has("下雪") || has("穿衣")) {
        out_mood = EyeMood::kCold;
        return true;
    }
    if (has("好热") || has("高温") || has("炎热") || has("中暑")) {
        out_mood = EyeMood::kHot;
        return true;
    }
    if (has("音乐") || has("唱歌") || has("歌曲") || has("听歌") || has("旋律")) {
        out_mood = EyeMood::kMusic;
        return true;
    }
    if (has("咖啡") || has("拿铁") || has("美式") || has("提神") || has("奶茶")) {
        out_mood = EyeMood::kCoffee;
        return true;
    }
    if (has("好吃") || has("美食") || has("火锅") || has("干饭") || has("晚餐") || has("午餐")) {
        out_mood = EyeMood::kDelicious;
        return true;
    }
    if (has("闹钟") || has("提醒") || has("日程") || has("别忘了")) {
        out_mood = EyeMood::kAlarm;
        return true;
    }
    if (has("秘密") || has("悄悄") || has("嘘")) {
        out_mood = EyeMood::kSecret;
        return true;
    }
    if (has("发财") || has("大涨") || has("涨停") || has("赚钱") || has("暴涨") || has("盈利")) {
        out_mood = EyeMood::kMoney;
        return true;
    }
    if (has("大跌") || has("跌停") || has("亏钱") || has("暴跌") || has("绿了")) {
        out_mood = EyeMood::kMarketDown;
        return true;
    }
    if (has("加油") || has("打气") || has("你可以") || has("冲鸭") || has("必胜")) {
        out_mood = EyeMood::kCheer;
        return true;
    }
    if (has("加班") || has("好累") || has("疲惫") || has("辛苦了")) {
        out_mood = EyeMood::kTired;
        return true;
    }
    if (has("工作") || has("学习") || has("专注") || has("开会") || has("计划")) {
        out_mood = EyeMood::kWorkFocus;
        return true;
    }
    if (has("生病") || has("发烧") || has("感冒") || has("头疼") || has("难受")) {
        out_mood = EyeMood::kSick;
        return true;
    }
    if (has("害怕") || has("吓人") || has("恐怖") || has("吓死")) {
        out_mood = EyeMood::kScared;
        return true;
    }
    if (has("好酷") || has("墨镜") || has("帅气") || has("潇洒")) {
        out_mood = EyeMood::kCool;
        return true;
    }
    if (has("得意") || has("厉害") || has("夸我") || has("满分")) {
        out_mood = EyeMood::kProud;
        return true;
    }
    if (has("哈哈") || has("笑死") || has("太搞笑")) {
        out_mood = EyeMood::kLaughing;
        return true;
    }
    if (has("爱你") || has("喜欢你") || has("比心") || has("么么")) {
        out_mood = EyeMood::kLoving;
        return true;
    }
    if (has("害羞") || has("不好意思") || has("脸红")) {
        out_mood = EyeMood::kShy;
        return true;
    }
    if (has("好奇") || has("为什么") || has("查查") || has("天气怎么样")) {
        out_mood = EyeMood::kCurious;
        return true;
    }
    return false;
}

void LcdDisplay::CreateEyeUI(lv_obj_t* screen, LvglTheme* lvgl_theme) {
    lv_color_t bg_color = lvgl_theme->background_color();

    /* Outer circular emotion halo */
    emotion_halo_ = lv_arc_create(screen);
    lv_obj_set_size(emotion_halo_, width_ - 4, height_ - 4);
    lv_obj_align(emotion_halo_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(emotion_halo_, 270);
    lv_arc_set_bg_angles(emotion_halo_, 0, 360);
    lv_arc_set_angles(emotion_halo_, 0, 360);
    lv_obj_remove_style(emotion_halo_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(emotion_halo_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(emotion_halo_, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(emotion_halo_, lv_color_hex(0x161B22), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(emotion_halo_, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_width(emotion_halo_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(emotion_halo_, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(emotion_halo_, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(emotion_halo_, true, LV_PART_INDICATOR);

    /* Center NOMI face container */
    eye_container_ = lv_obj_create(screen);
    lv_obj_set_size(eye_container_, 176, 118);
    lv_obj_set_style_bg_opa(eye_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eye_container_, 0, 0);
    lv_obj_set_style_pad_all(eye_container_, 0, 0);
    lv_obj_set_scrollbar_mode(eye_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(eye_container_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(eye_container_, LV_ALIGN_CENTER, 0, 0);

    auto make_eye_group = [&](int x_offset, lv_obj_t*& wrap, lv_obj_t*& eye, lv_obj_t*& highlight,
                              lv_obj_t*& top_lid, lv_obj_t*& bottom_lid) {
        wrap = lv_obj_create(eye_container_);
        lv_obj_set_size(wrap, 68, 76);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_set_scrollbar_mode(wrap, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(wrap, LV_ALIGN_CENTER, x_offset, -10);

        eye = lv_obj_create(wrap);
        lv_obj_set_size(eye, 46, 56);
        lv_obj_set_style_radius(eye, 20, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_pad_all(eye, 0, 0);
        lv_obj_set_scrollbar_mode(eye, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(eye, lv_color_hex(0x38BDF8), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_align(eye, LV_ALIGN_CENTER, 0, 0);

        highlight = lv_obj_create(eye);
        lv_obj_set_size(highlight, 11, 11);
        lv_obj_set_style_radius(highlight, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(highlight, 0, 0);
        lv_obj_set_style_pad_all(highlight, 0, 0);
        lv_obj_set_style_bg_color(highlight, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(highlight, LV_OPA_70, 0);
        lv_obj_align(highlight, LV_ALIGN_TOP_LEFT, 8, 8);

        top_lid = lv_obj_create(wrap);
        lv_obj_set_size(top_lid, 76, 44);
        lv_obj_set_style_radius(top_lid, 8, 0);
        lv_obj_set_style_border_width(top_lid, 0, 0);
        lv_obj_set_style_pad_all(top_lid, 0, 0);
        lv_obj_set_style_bg_color(top_lid, bg_color, 0);
        lv_obj_set_style_bg_opa(top_lid, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_pivot_x(top_lid, 38, 0);
        lv_obj_set_style_transform_pivot_y(top_lid, 22, 0);
        lv_obj_align(top_lid, LV_ALIGN_TOP_MID, 0, -48);

        bottom_lid = lv_obj_create(wrap);
        lv_obj_set_size(bottom_lid, 68, 54);
        lv_obj_set_style_radius(bottom_lid, 32, 0);
        lv_obj_set_style_border_width(bottom_lid, 0, 0);
        lv_obj_set_style_pad_all(bottom_lid, 0, 0);
        lv_obj_set_style_bg_color(bottom_lid, bg_color, 0);
        lv_obj_set_style_bg_opa(bottom_lid, LV_OPA_COVER, 0);
        lv_obj_align(bottom_lid, LV_ALIGN_BOTTOM_MID, 0, 58);
    };

    make_eye_group(-36, left_eye_wrap_, left_eye_, left_highlight_, left_top_lid_, left_bottom_lid_);
    make_eye_group(36, right_eye_wrap_, right_eye_, right_highlight_, right_top_lid_,
                   right_bottom_lid_);

    /* Sunglasses bridge bar (for kCool) */
    glasses_bridge_ = lv_obj_create(eye_container_);
    lv_obj_set_size(glasses_bridge_, 24, 7);
    lv_obj_set_style_radius(glasses_bridge_, 3, 0);
    lv_obj_set_style_border_width(glasses_bridge_, 0, 0);
    lv_obj_set_style_bg_color(glasses_bridge_, lv_color_hex(0x22D3EE), 0);
    lv_obj_set_style_bg_opa(glasses_bridge_, LV_OPA_COVER, 0);
    lv_obj_align(glasses_bridge_, LV_ALIGN_CENTER, 0, -14);
    lv_obj_add_flag(glasses_bridge_, LV_OBJ_FLAG_HIDDEN);

    /* Cute rosy cheeks */
    auto make_blush = [&](int x_offset, lv_obj_t*& blush) {
        blush = lv_obj_create(eye_container_);
        lv_obj_set_size(blush, 20, 10);
        lv_obj_set_style_radius(blush, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(blush, 0, 0);
        lv_obj_set_style_pad_all(blush, 0, 0);
        lv_obj_set_style_bg_color(blush, lv_color_hex(0xFF6B9E), 0);
        lv_obj_set_style_bg_opa(blush, LV_OPA_50, 0);
        lv_obj_align(blush, LV_ALIGN_CENTER, x_offset, 24);
        lv_obj_add_flag(blush, LV_OBJ_FLAG_HIDDEN);
    };
    make_blush(-54, left_blush_);
    make_blush(54, right_blush_);

    /* NOMI curved arc mouth */
    mouth_arc_ = lv_arc_create(eye_container_);
    lv_obj_set_size(mouth_arc_, 34, 24);
    lv_obj_align(mouth_arc_, LV_ALIGN_CENTER, 0, 32);
    lv_arc_set_rotation(mouth_arc_, 0);
    lv_arc_set_bg_angles(mouth_arc_, 25, 155);
    lv_arc_set_angles(mouth_arc_, 25, 155);
    lv_obj_remove_style(mouth_arc_, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(mouth_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(mouth_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mouth_arc_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mouth_arc_, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(mouth_arc_, true, LV_PART_INDICATOR);
    lv_obj_add_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);

    /* NOMI pill/circle mouth (O mouth, flat mouth, talking mouth, zipper) */
    mouth_shape_ = lv_obj_create(eye_container_);
    lv_obj_set_size(mouth_shape_, 18, 12);
    lv_obj_set_style_radius(mouth_shape_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mouth_shape_, 0, 0);
    lv_obj_set_style_pad_all(mouth_shape_, 0, 0);
    lv_obj_set_style_bg_color(mouth_shape_, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_bg_opa(mouth_shape_, LV_OPA_COVER, 0);
    lv_obj_align(mouth_shape_, LV_ALIGN_CENTER, 0, 34);
    lv_obj_add_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN);

    /* Cute pink tongue dot (for delicious / silly / hot) */
    mouth_tongue_ = lv_obj_create(eye_container_);
    lv_obj_set_size(mouth_tongue_, 12, 10);
    lv_obj_set_style_radius(mouth_tongue_, 5, 0);
    lv_obj_set_style_border_width(mouth_tongue_, 0, 0);
    lv_obj_set_style_pad_all(mouth_tongue_, 0, 0);
    lv_obj_set_style_bg_color(mouth_tongue_, lv_color_hex(0xFF6B9E), 0);
    lv_obj_set_style_bg_opa(mouth_tongue_, LV_OPA_COVER, 0);
    lv_obj_align(mouth_tongue_, LV_ALIGN_CENTER, 6, 40);
    lv_obj_add_flag(mouth_tongue_, LV_OBJ_FLAG_HIDDEN);

    /* Tear / sweat drop prop */
    prop_drop_ = lv_obj_create(eye_container_);
    lv_obj_set_size(prop_drop_, 9, 14);
    lv_obj_set_style_radius(prop_drop_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(prop_drop_, 0, 0);
    lv_obj_set_style_pad_all(prop_drop_, 0, 0);
    lv_obj_set_style_bg_color(prop_drop_, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_bg_opa(prop_drop_, LV_OPA_90, 0);
    lv_obj_align(prop_drop_, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_obj_add_flag(prop_drop_, LV_OBJ_FLAG_HIDDEN);

    /* Floating contextual ASCII badge (?, !, Zzz, Hi, $$, etc.) */
    prop_label_ = lv_label_create(eye_container_);
    lv_label_set_text(prop_label_, "");
    lv_obj_set_style_text_color(prop_label_, lv_color_hex(0xFFD166), 0);
    lv_obj_align(prop_label_, LV_ALIGN_TOP_RIGHT, -6, 0);
    lv_obj_add_flag(prop_label_, LV_OBJ_FLAG_HIDDEN);

    ApplyEyeMood();

    if (eye_timer_ == nullptr) {
        eye_timer_ = lv_timer_create(EyeTimerCallback, 60, this);
    }
}

void LcdDisplay::UpdateEyeLayout() {
    if (eye_container_ == nullptr) return;
    int base_y = subtitle_visible_ ? -18 : 2;
    lv_obj_align(eye_container_, LV_ALIGN_CENTER, 0, base_y);
}

void LcdDisplay::UpdateTopBarVisibility() {
    if (!IsRoundDisplay()) return;
    bool show_net = (visual_state_ == DeviceVisualState::kConnecting ||
                     visual_state_ == DeviceVisualState::kWifiError);

    if (top_bar_ != nullptr) {
        if (show_net) {
            lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (network_label_ != nullptr) {
                lv_obj_set_style_text_color(network_label_, lv_color_hex(0xF59E0B), 0);
                lv_obj_set_style_text_opa(network_label_, LV_OPA_COVER, 0);
            }
        } else {
            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (status_bar_ != nullptr) {
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, show_net ? 30 : 18);
    }

    if (status_label_ != nullptr) {
        lv_opa_t opa = (visual_state_ == DeviceVisualState::kIdle) ? LV_OPA_40 : LV_OPA_90;
        lv_obj_set_style_text_opa(status_label_, opa, 0);
    }
}

void LcdDisplay::UpdateSubtitleScrollMetrics() {
    if (!IsRoundDisplay() || bottom_bar_ == nullptr || chat_message_label_ == nullptr) {
        return;
    }
    lv_obj_update_layout(chat_message_label_);
    int label_h = lv_obj_get_height(chat_message_label_);
    subtitle_scroll_y_ = 0;
    subtitle_pause_ticks_ = 22;  // Pause ~1.3s so first lines are easy to read

    if (label_h > subtitle_box_height_) {
        subtitle_max_scroll_ = label_h - subtitle_box_height_ + 4;
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 0);
    } else {
        subtitle_max_scroll_ = 0;
        int v_center = (subtitle_box_height_ - label_h) / 2;
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, v_center);
    }
}

void LcdDisplay::ApplyEyeMood() {
    if (left_eye_ == nullptr || right_eye_ == nullptr) return;

    MoodSpec spec = GetMoodSpec(static_cast<int>(current_mood_));
    lv_color_t eye_color = lv_color_hex(spec.color_hex);
    lv_color_t halo_color = lv_color_hex(spec.halo_hex);

    if (emotion_halo_ != nullptr) {
        lv_obj_set_style_arc_color(emotion_halo_, halo_color, LV_PART_INDICATOR);
    }

    lv_obj_set_style_bg_color(left_eye_, eye_color, 0);
    lv_obj_set_style_bg_color(right_eye_, eye_color, 0);

    int lw = spec.lw, lh = spec.lh, lrad = spec.lrad;
    int rw = spec.rw, rh = spec.rh, rrad = spec.rrad;
    bool l_hi = spec.l_hi, r_hi = spec.r_hi;

    if (blink_phase_ > 0) {
        lh = 6;
        lrad = 3;
        l_hi = false;
        if (current_mood_ != EyeMood::kWinking && current_mood_ != EyeMood::kGoodbye) {
            rh = 6;
            rrad = 3;
            r_hi = false;
        }
    }

    lv_obj_set_size(left_eye_, lw, lh);
    lv_obj_set_style_radius(left_eye_, lrad, 0);
    lv_obj_set_size(right_eye_, rw, rh);
    lv_obj_set_style_radius(right_eye_, rrad, 0);

    if (l_hi) lv_obj_remove_flag(left_highlight_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(left_highlight_, LV_OBJ_FLAG_HIDDEN);

    if (r_hi) lv_obj_remove_flag(right_highlight_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(right_highlight_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_transform_rotation(left_top_lid_, spec.lt_rot, 0);
    lv_obj_align(left_top_lid_, LV_ALIGN_TOP_MID, spec.lt_x, spec.lt_y);
    lv_obj_set_style_transform_rotation(right_top_lid_, spec.rt_rot, 0);
    lv_obj_align(right_top_lid_, LV_ALIGN_TOP_MID, spec.rt_x, spec.rt_y);
    lv_obj_align(left_bottom_lid_, LV_ALIGN_BOTTOM_MID, 0, spec.lb_y);
    lv_obj_align(right_bottom_lid_, LV_ALIGN_BOTTOM_MID, 0, spec.rb_y);

    // Blush cheeks
    if (left_blush_ != nullptr && right_blush_ != nullptr) {
        if (spec.blush) {
            lv_obj_remove_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(left_blush_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(right_blush_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Mouth rendering
    if (mouth_arc_ != nullptr) lv_obj_add_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);
    if (mouth_shape_ != nullptr) lv_obj_add_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN);
    if (mouth_tongue_ != nullptr) lv_obj_add_flag(mouth_tongue_, LV_OBJ_FLAG_HIDDEN);

    MouthKind active_mouth =
        (visual_state_ == DeviceVisualState::kSpeaking && spec.mouth == MouthKind::kHidden)
            ? MouthKind::kTalking
            : spec.mouth;

    switch (active_mouth) {
        case MouthKind::kSmile:
            lv_obj_set_size(mouth_arc_, 32, 20);
            lv_arc_set_bg_angles(mouth_arc_, 25, 155);
            lv_arc_set_angles(mouth_arc_, 25, 155);
            lv_obj_set_style_arc_color(mouth_arc_, eye_color, LV_PART_INDICATOR);
            lv_obj_align(mouth_arc_, LV_ALIGN_CENTER, 0, 32);
            lv_obj_remove_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kBigSmile:
            lv_obj_set_size(mouth_arc_, 42, 26);
            lv_arc_set_bg_angles(mouth_arc_, 15, 165);
            lv_arc_set_angles(mouth_arc_, 15, 165);
            lv_obj_set_style_arc_color(mouth_arc_, eye_color, LV_PART_INDICATOR);
            lv_obj_align(mouth_arc_, LV_ALIGN_CENTER, 0, 30);
            lv_obj_remove_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kSmirk:
            lv_obj_set_size(mouth_arc_, 30, 18);
            lv_arc_set_bg_angles(mouth_arc_, 10, 110);
            lv_arc_set_angles(mouth_arc_, 10, 110);
            lv_obj_set_style_arc_color(mouth_arc_, eye_color, LV_PART_INDICATOR);
            lv_obj_align(mouth_arc_, LV_ALIGN_CENTER, 4, 32);
            lv_obj_remove_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kSad:
            lv_obj_set_size(mouth_arc_, 30, 20);
            lv_arc_set_bg_angles(mouth_arc_, 205, 335);
            lv_arc_set_angles(mouth_arc_, 205, 335);
            lv_obj_set_style_arc_color(mouth_arc_, eye_color, LV_PART_INDICATOR);
            lv_obj_align(mouth_arc_, LV_ALIGN_CENTER, 0, 38);
            lv_obj_remove_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kSmallO:
            lv_obj_set_size(mouth_shape_, 12, 12);
            lv_obj_set_style_radius(mouth_shape_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(mouth_shape_, eye_color, 0);
            lv_obj_align(mouth_shape_, LV_ALIGN_CENTER, 0, 35);
            lv_obj_remove_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kBigO:
            lv_obj_set_size(mouth_shape_, 18, 22);
            lv_obj_set_style_radius(mouth_shape_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(mouth_shape_, eye_color, 0);
            lv_obj_align(mouth_shape_, LV_ALIGN_CENTER, 0, 36);
            lv_obj_remove_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kFlat:
        case MouthKind::kWavy:
            lv_obj_set_size(mouth_shape_, 26, 4);
            lv_obj_set_style_radius(mouth_shape_, 2, 0);
            lv_obj_set_style_bg_color(mouth_shape_, eye_color, 0);
            lv_obj_align(mouth_shape_, LV_ALIGN_CENTER, 0, 36);
            lv_obj_remove_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kZipper:
            lv_obj_set_size(mouth_shape_, 32, 6);
            lv_obj_set_style_radius(mouth_shape_, 2, 0);
            lv_obj_set_style_bg_color(mouth_shape_, lv_color_hex(0x94A3B8), 0);
            lv_obj_align(mouth_shape_, LV_ALIGN_CENTER, 0, 36);
            lv_obj_remove_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kTongue:
            lv_obj_set_size(mouth_arc_, 34, 20);
            lv_arc_set_bg_angles(mouth_arc_, 20, 160);
            lv_arc_set_angles(mouth_arc_, 20, 160);
            lv_obj_set_style_arc_color(mouth_arc_, eye_color, LV_PART_INDICATOR);
            lv_obj_align(mouth_arc_, LV_ALIGN_CENTER, 0, 31);
            lv_obj_remove_flag(mouth_arc_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(mouth_tongue_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kTalking:
            lv_obj_set_size(mouth_shape_, 22, 10);
            lv_obj_set_style_radius(mouth_shape_, 6, 0);
            lv_obj_set_style_bg_color(mouth_shape_, eye_color, 0);
            lv_obj_align(mouth_shape_, LV_ALIGN_CENTER, 0, 35);
            lv_obj_remove_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MouthKind::kHidden:
        default:
            break;
    }

    // Props rendering
    if (glasses_bridge_ != nullptr) lv_obj_add_flag(glasses_bridge_, LV_OBJ_FLAG_HIDDEN);
    if (prop_drop_ != nullptr) lv_obj_add_flag(prop_drop_, LV_OBJ_FLAG_HIDDEN);
    if (prop_label_ != nullptr) lv_obj_add_flag(prop_label_, LV_OBJ_FLAG_HIDDEN);

    switch (spec.prop) {
        case PropKind::kGlasses:
            if (glasses_bridge_ != nullptr) {
                lv_obj_set_style_bg_color(glasses_bridge_, eye_color, 0);
                lv_obj_remove_flag(glasses_bridge_, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case PropKind::kTear:
            if (prop_drop_ != nullptr) {
                lv_obj_set_style_bg_color(prop_drop_, lv_color_hex(0x60A5FA), 0);
                lv_obj_align(prop_drop_, LV_ALIGN_CENTER, -36, 22);
                lv_obj_remove_flag(prop_drop_, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case PropKind::kSweat:
            if (prop_drop_ != nullptr) {
                lv_obj_set_style_bg_color(prop_drop_, lv_color_hex(0x38BDF8), 0);
                lv_obj_align(prop_drop_, LV_ALIGN_TOP_RIGHT, -12, 8);
                lv_obj_remove_flag(prop_drop_, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case PropKind::kBadge:
            if (prop_label_ != nullptr && spec.badge_text != nullptr && spec.badge_text[0] != '\0') {
                lv_label_set_text(prop_label_, spec.badge_text);
                lv_obj_set_style_text_color(prop_label_, eye_color, 0);
                lv_obj_align(prop_label_, LV_ALIGN_TOP_RIGHT, -6, 2);
                lv_obj_remove_flag(prop_label_, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case PropKind::kNone:
        default:
            break;
    }
}

void LcdDisplay::EyeTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnEyeTimerTick();
    }
}

void LcdDisplay::OnEyeTimerTick() {
    anim_tick_++;

    // 1. Smooth subtitle auto-scroll for long replies (prevents clipping)
    if (subtitle_visible_ && subtitle_max_scroll_ > 0 && chat_message_label_ != nullptr) {
        if (subtitle_pause_ticks_ > 0) {
            subtitle_pause_ticks_--;
        } else {
            subtitle_scroll_y_ += 1;
            if (subtitle_scroll_y_ > subtitle_max_scroll_) {
                subtitle_scroll_y_ = 0;
                subtitle_pause_ticks_ = 28;  // Pause at top before next scroll cycle
            } else if (subtitle_scroll_y_ == subtitle_max_scroll_) {
                subtitle_pause_ticks_ = 26;  // Pause at bottom to read last line
            }
            lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, -subtitle_scroll_y_);
        }
    }

    // 2. Idle NOMI life theater (bored -> yawn -> sleeping Zzz)
    if (visual_state_ == DeviceVisualState::kIdle && !subtitle_visible_) {
        uint32_t idle_ticks = anim_tick_ - last_activity_tick_;
        if (idle_ticks > 1250 && current_mood_ != EyeMood::kSleeping) {
            // ~75 seconds idle -> drift into peaceful sleep
            current_mood_ = EyeMood::kSleeping;
            ApplyEyeMood();
        } else if (idle_ticks > 420 && anim_tick_ >= next_idle_act_tick_ &&
                   current_mood_ != EyeMood::kSleeping) {
            static const EyeMood kIdlePlay[] = {
                EyeMood::kNeutral, EyeMood::kCurious, EyeMood::kWinking,
                EyeMood::kBored,   EyeMood::kYawning, EyeMood::kHappy,
            };
            current_mood_ = kIdlePlay[(anim_tick_ / 13) % 6];
            ApplyEyeMood();
            next_idle_act_tick_ = anim_tick_ + 180 + ((anim_tick_ * 29) % 140);
        }
    }

    MoodSpec spec = GetMoodSpec(static_cast<int>(current_mood_));

    // 3. Animate outer circular halo
    if (emotion_halo_ != nullptr) {
        bool spin_mode = spec.spin_halo || visual_state_ == DeviceVisualState::kListening ||
                         visual_state_ == DeviceVisualState::kConnecting;
        if (spin_mode) {
            halo_angle_ = (halo_angle_ + 12) % 360;
            lv_arc_set_rotation(emotion_halo_, halo_angle_);
            lv_arc_set_angles(emotion_halo_, 0, 120);
            lv_obj_set_style_arc_opa(emotion_halo_, LV_OPA_COVER, LV_PART_INDICATOR);
        } else {
            lv_arc_set_rotation(emotion_halo_, 270);
            lv_arc_set_angles(emotion_halo_, 0, 360);

            int period = 44;
            int min_opa = LV_OPA_30;
            int max_opa = LV_OPA_80;
            if (spec.motion == MotionKind::kAlert || visual_state_ == DeviceVisualState::kSpeaking) {
                period = 16;
                min_opa = LV_OPA_50;
                max_opa = LV_OPA_COVER;
            } else if (spec.motion == MotionKind::kSleep) {
                period = 68;
                min_opa = LV_OPA_10;
                max_opa = LV_OPA_40;
            }
            int half = period / 2;
            int step = static_cast<int>(anim_tick_ % period);
            int tri = (step < half) ? step : (period - step);
            lv_opa_t opa = static_cast<lv_opa_t>(min_opa + ((max_opa - min_opa) * tri) / half);
            lv_obj_set_style_arc_opa(emotion_halo_, opa, LV_PART_INDICATOR);
        }
    }

    if (eye_container_ == nullptr || left_eye_wrap_ == nullptr || right_eye_wrap_ == nullptr ||
        lv_obj_has_flag(eye_container_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    // 4. Autonomous natural blinking
    if (blink_phase_ > 0) {
        blink_phase_--;
        if (blink_phase_ == 0) {
            ApplyEyeMood();
        }
    } else if (anim_tick_ >= next_blink_tick_ && current_mood_ != EyeMood::kSleeping &&
               current_mood_ != EyeMood::kLaughing && current_mood_ != EyeMood::kYawning) {
        blink_phase_ = 2;
        ApplyEyeMood();
        next_blink_tick_ = anim_tick_ + 48 + ((anim_tick_ * 37) % 42);
    }

    // 5. Dynamic talking mouth animation
    if (visual_state_ == DeviceVisualState::kSpeaking && mouth_shape_ != nullptr &&
        !lv_obj_has_flag(mouth_shape_, LV_OBJ_FLAG_HIDDEN)) {
        static const int kTalkH[] = {6, 13, 9, 16, 8, 12};
        static const int kTalkW[] = {24, 18, 22, 16, 24, 20};
        size_t idx = (anim_tick_ / 2) % 6;
        lv_obj_set_size(mouth_shape_, kTalkW[idx], kTalkH[idx]);
    }

    // 6. Floating prop micro-animation (tear falling, Zzz drifting)
    if (prop_drop_ != nullptr && !lv_obj_has_flag(prop_drop_, LV_OBJ_FLAG_HIDDEN)) {
        int dy = static_cast<int>((anim_tick_ % 18) / 2);
        if (spec.prop == PropKind::kTear) {
            lv_obj_align(prop_drop_, LV_ALIGN_CENTER, -36, 18 + dy);
        } else {
            lv_obj_align(prop_drop_, LV_ALIGN_TOP_RIGHT, -12, 6 + (dy / 2));
        }
    }
    if (prop_label_ != nullptr && !lv_obj_has_flag(prop_label_, LV_OBJ_FLAG_HIDDEN)) {
        int bob = static_cast<int>((anim_tick_ / 3) % 6);
        int dy = (bob < 3) ? -bob : -(6 - bob);
        lv_obj_align(prop_label_, LV_ALIGN_TOP_RIGHT, -6, 4 + dy);
    }

    // 7. Gaze wandering & motion expression
    if (anim_tick_ >= next_gaze_tick_) {
        static const int kGazeX[] = {0, -8, 0, 8, -4, 4, 0};
        static const int kGazeY[] = {0, -2, 0, -2, 2, 0, 0};
        size_t idx = (anim_tick_ / 7) % 7;

        if (spec.motion == MotionKind::kScan) {
            target_gaze_x_ = ((anim_tick_ / 14) % 2 == 0) ? -9 : 9;
            target_gaze_y_ = (current_mood_ == EyeMood::kThinking) ? -4 : 0;
        } else if (current_mood_ == EyeMood::kSad || current_mood_ == EyeMood::kCrying ||
                   current_mood_ == EyeMood::kTired) {
            target_gaze_x_ = 0;
            target_gaze_y_ = 4;
        } else if (current_mood_ == EyeMood::kShy) {
            target_gaze_x_ = 7;
            target_gaze_y_ = 3;
        } else if (current_mood_ == EyeMood::kProud) {
            target_gaze_x_ = 0;
            target_gaze_y_ = -4;
        } else if (spec.motion == MotionKind::kCalm ||
                   visual_state_ == DeviceVisualState::kListening) {
            target_gaze_x_ = kGazeX[idx];
            target_gaze_y_ = kGazeY[idx];
        } else {
            target_gaze_x_ = 0;
            target_gaze_y_ = 0;
        }
        next_gaze_tick_ = anim_tick_ + 34 + ((anim_tick_ * 23) % 30);
    }

    if (gaze_x_ < target_gaze_x_) {
        gaze_x_ += 2;
        if (gaze_x_ > target_gaze_x_) gaze_x_ = target_gaze_x_;
    } else if (gaze_x_ > target_gaze_x_) {
        gaze_x_ -= 2;
        if (gaze_x_ < target_gaze_x_) gaze_x_ = target_gaze_x_;
    }

    if (gaze_y_ < target_gaze_y_) gaze_y_ += 1;
    else if (gaze_y_ > target_gaze_y_) gaze_y_ -= 1;

    int motion_x = 0;
    int motion_y = 0;
    switch (spec.motion) {
        case MotionKind::kBounce: {
            int b = static_cast<int>(anim_tick_ % 10);
            motion_y = (b < 5) ? -b : -(10 - b);
            break;
        }
        case MotionKind::kSway: {
            int s = static_cast<int>(anim_tick_ % 16);
            motion_x = (s < 8) ? (s - 4) : (12 - s);
            break;
        }
        case MotionKind::kShiver:
            motion_x = (anim_tick_ % 2 == 0) ? -2 : 2;
            break;
        case MotionKind::kNod: {
            int n = static_cast<int>(anim_tick_ % 14);
            motion_y = (n < 7) ? (n / 2) : ((14 - n) / 2);
            break;
        }
        case MotionKind::kAlert:
            motion_y = (anim_tick_ % 4 < 2) ? -2 : 1;
            break;
        case MotionKind::kSleep: {
            int s = static_cast<int>((anim_tick_ / 4) % 8);
            motion_y = (s < 4) ? (s / 2) : ((8 - s) / 2);
            break;
        }
        default:
            break;
    }

    lv_obj_align(left_eye_wrap_, LV_ALIGN_CENTER, -36 + gaze_x_ + motion_x,
                 -10 + gaze_y_ + motion_y);
    lv_obj_align(right_eye_wrap_, LV_ALIGN_CENTER, 36 + gaze_x_ + motion_x,
                 -10 + gaze_y_ + motion_y);
    if (glasses_bridge_ != nullptr && !lv_obj_has_flag(glasses_bridge_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align(glasses_bridge_, LV_ALIGN_CENTER, gaze_x_ + motion_x,
                     -14 + gaze_y_ + motion_y);
    }
}

void LcdDisplay::ShowEmotion() {
    // On round displays, ALWAYS showcase our animated NOMI eyes
    if (IsRoundDisplay() && eye_container_ != nullptr) {
        lv_obj_remove_flag(eye_container_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);

    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}

#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) return;
    DisplayLockGuard lock(this);
    if (content_ == nullptr) return;

    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            child_count = lv_obj_get_child_cnt(content_);
        }
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    if (strcmp(role, "system") == 0) {
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    if (strlen(content) == 0) return;

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;
    lv_coord_t min_width = 20;
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    if (text_width < min_width) text_width = min_width;

    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    if (strcmp(role, "user") == 0) {
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    if (strcmp(role, "user") == 0) {
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr || image == nullptr) return;

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    lv_obj_set_user_data(img_bubble, (void*)"image");

    lv_obj_t* preview_image = lv_image_create(img_bubble);
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;
    lv_coord_t max_height = LV_VER_RES * 50 / 100;

    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    if (zoom > 256) zoom = 256;

    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    LvglImage* raw_image = image.release();
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) delete img;
        },
        LV_EVENT_DELETE, (void*)raw_image);

    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    lv_obj_center(preview_image);
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr || role == nullptr || content == nullptr) return;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(content_); ++i) {
        auto container = lv_obj_get_child(content_, i);
        auto bubble = container;
        auto bubble_role = static_cast<const char*>(lv_obj_get_user_data(bubble));
        if (bubble_role == nullptr && lv_obj_get_child_cnt(bubble) > 0) {
            bubble = lv_obj_get_child(bubble, 0);
            bubble_role = static_cast<const char*>(lv_obj_get_user_data(bubble));
        }
        if (bubble_role == nullptr || strcmp(bubble_role, role) != 0 ||
            lv_obj_get_child_cnt(bubble) == 0) {
            continue;
        }
        auto label = lv_obj_get_child(bubble, 0);
        if (strcmp(lv_label_get_text(label), content) != 0) continue;
        if (chat_message_label_ == label) chat_message_label_ = nullptr;
        lv_obj_del(container);
        if (lv_obj_get_child_cnt(content_) == 0) ShowEmotion();
        return;
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) return;
    lv_obj_clean(content_);
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
        /* 1. Outer emotion halo & 54-mood NOMI face rig */
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

        /* Preview image */
        preview_image_ = lv_image_create(screen);
        lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

        /* 2. Top icon bar: auto-hidden when online, visible only when offline/provisioning */
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
        lv_obj_set_style_text_color(network_label_, lv_color_hex(0xF59E0B), 0);

        mute_label_ = lv_label_create(top_bar_);
        lv_label_set_text(mute_label_, "");
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

        battery_label_ = lv_label_create(top_bar_);
        lv_label_set_text(battery_label_, "");
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

        /* 3. Status bar: auto-height so text is NEVER vertically clipped */
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

        /* 4. Borderless circular-safe subtitle window with smooth auto-scroll */
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

        /* 5. Low battery popup inside circular safe zone */
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

    /* Original rectangular display layout */
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
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
#else
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
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
    if (eye_container_ != nullptr) lv_obj_add_flag(eye_container_, LV_OBJ_FLAG_HIDDEN);
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
    if (bottom_bar_ != nullptr) {
        if (!has_content) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            subtitle_visible_ = false;
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            subtitle_visible_ = true;
            if (IsRoundDisplay()) {
                lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -30);
                UpdateSubtitleScrollMetrics();
            }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
            else {
                lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
            }
#endif
        }
    }

    if (IsRoundDisplay() && has_content) {
        last_activity_tick_ = anim_tick_;
        EyeMood ctx_mood = current_mood_;
        if (MatchContextualMood(content, ctx_mood)) {
            current_mood_ = ctx_mood;
            blink_phase_ = 0;
            ApplyEyeMood();
        }
    }

    UpdateEyeLayout();
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr ||
        strcmp(lv_label_get_text(chat_message_label_), content) != 0) {
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, "");
    if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    subtitle_visible_ = false;
    UpdateEyeLayout();
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_message_label_ != nullptr) lv_label_set_text(chat_message_label_, "");
    if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    subtitle_visible_ = false;
    UpdateEyeLayout();
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
        current_mood_ = EyeMood::kListening;
        last_activity_tick_ = anim_tick_;
    } else if (strstr(status, "说话") != nullptr || strstr(status, "Speak") != nullptr ||
               strstr(status, "speak") != nullptr) {
        new_state = DeviceVisualState::kSpeaking;
        if (current_mood_ == EyeMood::kListening || current_mood_ == EyeMood::kSleeping ||
            current_mood_ == EyeMood::kConnecting) {
            current_mood_ = EyeMood::kSpeaking;
        }
        last_activity_tick_ = anim_tick_;
    } else if (strstr(status, "断开") != nullptr || strstr(status, "失败") != nullptr ||
               strstr(status, "离线") != nullptr || strstr(status, "Error") != nullptr ||
               strstr(status, "Offline") != nullptr) {
        new_state = DeviceVisualState::kWifiError;
        current_mood_ = EyeMood::kWifiError;
    } else if (strstr(status, "连接") != nullptr || strstr(status, "初始") != nullptr ||
               strstr(status, "配网") != nullptr || strstr(status, "Wi-Fi") != nullptr ||
               strstr(status, "WiFi") != nullptr || strstr(status, "Connect") != nullptr ||
               strstr(status, "Init") != nullptr || strstr(status, "Config") != nullptr) {
        new_state = DeviceVisualState::kConnecting;
        current_mood_ = EyeMood::kConnecting;
    } else if (strstr(status, "待命") != nullptr || strstr(status, "Standby") != nullptr ||
               strstr(status, "Idle") != nullptr) {
        new_state = DeviceVisualState::kIdle;
        if (prev_state == DeviceVisualState::kConnecting ||
            prev_state == DeviceVisualState::kWifiError) {
            current_mood_ = EyeMood::kWakingUp;
        } else {
            current_mood_ = EyeMood::kNeutral;
        }
        last_activity_tick_ = anim_tick_;
    }

    visual_state_ = new_state;
    UpdateTopBarVisibility();
    ApplyEyeMood();
    ShowEmotion();
}

bool LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_ || emotion == nullptr) return false;

    // On round displays, ALWAYS render our 54-mood animated NOMI face (never fallback to tiny icons)
    if (IsRoundDisplay() && eye_container_ != nullptr) {
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        last_activity_tick_ = anim_tick_;
        EyeMood parsed = ParseEmotionMood(emotion);
        // Keep speaking / contextual mood vivid if server only sends generic "neutral" while talking
        if (!(parsed == EyeMood::kNeutral && visual_state_ == DeviceVisualState::kSpeaking)) {
            current_mood_ = parsed;
        }
        blink_phase_ = 0;
        emotion_uses_image_ = false;
        ApplyEyeMood();
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

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        if (gif_controller_) gif_controller_->Pause();
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    return shown;
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    lv_obj_t* screen = lv_screen_active();

    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    if (lvgl_theme->background_image() != nullptr && !IsRoundDisplay()) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Keep eyelid masks matched to active background
    lv_color_t lid_color = lvgl_theme->background_color();
    if (left_top_lid_ != nullptr) lv_obj_set_style_bg_color(left_top_lid_, lid_color, 0);
    if (right_top_lid_ != nullptr) lv_obj_set_style_bg_color(right_top_lid_, lid_color, 0);
    if (left_bottom_lid_ != nullptr) lv_obj_set_style_bg_color(left_bottom_lid_, lid_color, 0);
    if (right_bottom_lid_ != nullptr) lv_obj_set_style_bg_color(right_bottom_lid_, lid_color, 0);

    if (top_bar_ != nullptr && !IsRoundDisplay()) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    if (IsRoundDisplay()) {
        // Refresh safe subtitle box height in case a larger cloud font was just loaded
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
    } else {
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
        lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
        uint32_t child_count = lv_obj_get_child_cnt(content_);
        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* obj = lv_obj_get_child(content_, i);
            if (obj == nullptr) continue;
            lv_obj_t* bubble = nullptr;
            if (lv_obj_get_child_cnt(obj) > 0) {
                lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
                bubble = (bg_opa == LV_OPA_TRANSP) ? lv_obj_get_child(obj, 0) : obj;
            } else {
                continue;
            }
            if (bubble == nullptr) continue;
            void* bubble_type_ptr = lv_obj_get_user_data(bubble);
            if (bubble_type_ptr != nullptr) {
                const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
                if (strcmp(bubble_type, "user") == 0) {
                    lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
                } else if (strcmp(bubble_type, "assistant") == 0) {
                    lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
                } else if (strcmp(bubble_type, "system") == 0 ||
                           strcmp(bubble_type, "image") == 0) {
                    lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
                }
                lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
                if (lv_obj_get_child_cnt(bubble) > 0) {
                    lv_obj_t* text = lv_obj_get_child(bubble, 0);
                    if (text != nullptr) {
                        lv_obj_set_style_text_color(
                            text,
                            (strcmp(bubble_type, "system") == 0) ? lvgl_theme->system_text_color()
                                                                 : lvgl_theme->text_color(),
                            0);
                    }
                }
            }
        }
#else
        if (chat_message_label_ != nullptr) {
            lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
            lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
        }
#endif
    }

    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
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
    UpdateEyeLayout();
}
