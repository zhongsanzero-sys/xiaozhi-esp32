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

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0xFF3B30));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme (recommended for round screens)
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

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings (default to dark on round display for seamless black bezel)
    Settings settings("display", false);
    std::string default_theme = IsRoundDisplay() ? "dark" : "light";
    std::string theme_name = settings.GetString("theme", default_theme);
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
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
    // Clear screen with black to avoid bright white flash on boot
    std::vector<uint16_t> buffer(width_, 0x0000);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
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

// RGB LCD implementation
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

    ESP_LOGI(TAG, "Adding LCD display");
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

    ESP_LOGI(TAG, "Adding LCD display");
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

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (eye_container_ != nullptr) {
        lv_obj_del(eye_container_);
    }
    if (emotion_halo_ != nullptr) {
        lv_obj_del(emotion_halo_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

LcdDisplay::EyeMood LcdDisplay::ParseEmotionMood(const char* emotion) const {
    if (emotion == nullptr) {
        return EyeMood::kNeutral;
    }
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "delicious") == 0) {
        return EyeMood::kHappy;
    }
    if (strcmp(emotion, "laughing") == 0 || strcmp(emotion, "funny") == 0 ||
        strcmp(emotion, "silly") == 0) {
        return EyeMood::kLaughing;
    }
    if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0) {
        return EyeMood::kSad;
    }
    if (strcmp(emotion, "angry") == 0) {
        return EyeMood::kAngry;
    }
    if (strcmp(emotion, "surprised") == 0 || strcmp(emotion, "shocked") == 0) {
        return EyeMood::kSurprised;
    }
    if (strcmp(emotion, "loving") == 0 || strcmp(emotion, "kissy") == 0) {
        return EyeMood::kLoving;
    }
    if (strcmp(emotion, "embarrassed") == 0) {
        return EyeMood::kEmbarrassed;
    }
    if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0) {
        return EyeMood::kThinking;
    }
    if (strcmp(emotion, "sleepy") == 0 || strcmp(emotion, "relaxed") == 0) {
        return EyeMood::kSleepy;
    }
    if (strcmp(emotion, "winking") == 0) {
        return EyeMood::kWinking;
    }
    if (strcmp(emotion, "cool") == 0 || strcmp(emotion, "confident") == 0) {
        return EyeMood::kCool;
    }
    return EyeMood::kNeutral;
}

lv_color_t LcdDisplay::GetMoodColor(EyeMood mood) const {
    switch (mood) {
        case EyeMood::kHappy:
        case EyeMood::kLaughing:
            return lv_color_hex(0xFFD166);  // Warm golden yellow
        case EyeMood::kSad:
            return lv_color_hex(0x4A90E2);  // Soft melancholic blue
        case EyeMood::kAngry:
            return lv_color_hex(0xFF4757);  // Energetic crimson red
        case EyeMood::kSurprised:
            return lv_color_hex(0xC084FC);  // Electric violet
        case EyeMood::kLoving:
        case EyeMood::kEmbarrassed:
            return lv_color_hex(0xFF6B9E);  // Warm rose pink
        case EyeMood::kThinking:
            return lv_color_hex(0xF59E0B);  // Amber thinking glow
        case EyeMood::kSleepy:
            return lv_color_hex(0x7C6CE0);  // Calm night indigo
        case EyeMood::kWinking:
            return lv_color_hex(0x34D399);  // Playful mint green
        case EyeMood::kCool:
            return lv_color_hex(0x22D3EE);  // Sharp ice cyan
        case EyeMood::kNeutral:
        default:
            return lv_color_hex(0x38BDF8);  // Crisp companion sky cyan
    }
}

void LcdDisplay::CreateEyeUI(lv_obj_t* screen, LvglTheme* lvgl_theme) {
    lv_color_t bg_color = lvgl_theme->background_color();

    /* Outer circular emotion halo hugging the 240x240 rim */
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
    lv_obj_set_style_arc_opa(emotion_halo_, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_arc_width(emotion_halo_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(emotion_halo_, GetMoodColor(current_mood_), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(emotion_halo_, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(emotion_halo_, true, LV_PART_INDICATOR);

    /* Center expressive eyes container */
    eye_container_ = lv_obj_create(screen);
    lv_obj_set_size(eye_container_, 172, 96);
    lv_obj_set_style_bg_opa(eye_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eye_container_, 0, 0);
    lv_obj_set_style_pad_all(eye_container_, 0, 0);
    lv_obj_set_scrollbar_mode(eye_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(eye_container_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(eye_container_, LV_ALIGN_CENTER, 0, -4);

    auto make_eye_group = [&](int x_offset, lv_obj_t*& wrap, lv_obj_t*& eye, lv_obj_t*& highlight,
                              lv_obj_t*& top_lid, lv_obj_t*& bottom_lid) {
        wrap = lv_obj_create(eye_container_);
        lv_obj_set_size(wrap, 68, 82);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_set_scrollbar_mode(wrap, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(wrap, LV_ALIGN_CENTER, x_offset, 0);

        eye = lv_obj_create(wrap);
        lv_obj_set_size(eye, 48, 60);
        lv_obj_set_style_radius(eye, 20, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_pad_all(eye, 0, 0);
        lv_obj_set_scrollbar_mode(eye, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(eye, GetMoodColor(current_mood_), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_align(eye, LV_ALIGN_CENTER, 0, 0);

        // Specular gloss dot inside eye for cute lively feel
        highlight = lv_obj_create(eye);
        lv_obj_set_size(highlight, 12, 12);
        lv_obj_set_style_radius(highlight, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(highlight, 0, 0);
        lv_obj_set_style_pad_all(highlight, 0, 0);
        lv_obj_set_style_bg_color(highlight, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(highlight, LV_OPA_70, 0);
        lv_obj_align(highlight, LV_ALIGN_TOP_LEFT, 9, 9);

        // Top eyelid mask (used for angry / sad / sleepy / cool)
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

        // Bottom curved eyelid mask (used to form joyful crescent eyes ^ ^)
        bottom_lid = lv_obj_create(wrap);
        lv_obj_set_size(bottom_lid, 68, 54);
        lv_obj_set_style_radius(bottom_lid, 32, 0);
        lv_obj_set_style_border_width(bottom_lid, 0, 0);
        lv_obj_set_style_pad_all(bottom_lid, 0, 0);
        lv_obj_set_style_bg_color(bottom_lid, bg_color, 0);
        lv_obj_set_style_bg_opa(bottom_lid, LV_OPA_COVER, 0);
        lv_obj_align(bottom_lid, LV_ALIGN_BOTTOM_MID, 0, 58);
    };

    make_eye_group(-38, left_eye_wrap_, left_eye_, left_highlight_, left_top_lid_, left_bottom_lid_);
    make_eye_group(38, right_eye_wrap_, right_eye_, right_highlight_, right_top_lid_,
                   right_bottom_lid_);

    ApplyEyeMood();

    // 60ms lightweight animation timer (~16 FPS, friendly to SPI bandwidth)
    if (eye_timer_ == nullptr) {
        eye_timer_ = lv_timer_create(EyeTimerCallback, 60, this);
    }
}

void LcdDisplay::UpdateEyeLayout() {
    if (eye_container_ == nullptr) {
        return;
    }
    int base_y = subtitle_visible_ ? -18 : -4;
    lv_obj_align(eye_container_, LV_ALIGN_CENTER, 0, base_y);
}

void LcdDisplay::ApplyEyeMood() {
    lv_color_t mood_color = GetMoodColor(current_mood_);
    if (visual_state_ == DeviceVisualState::kListening && current_mood_ == EyeMood::kNeutral) {
        mood_color = lv_color_hex(0x00E5FF);  // Brighter cyan when actively listening
    } else if (visual_state_ == DeviceVisualState::kConnecting) {
        mood_color = lv_color_hex(0xF59E0B);  // Warm amber when connecting
    }

    if (emotion_halo_ != nullptr) {
        lv_obj_set_style_arc_color(emotion_halo_, mood_color, LV_PART_INDICATOR);
    }

    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(left_eye_, mood_color, 0);
    lv_obj_set_style_bg_color(right_eye_, mood_color, 0);

    // Base geometry defaults
    int lw = 48, lh = 60, lrad = 20;
    int rw = 48, rh = 60, rrad = 20;
    int lt_x = 0, lt_y = -48, lt_rot = 0;
    int rt_x = 0, rt_y = -48, rt_rot = 0;
    int lb_y = 58, rb_y = 58;
    bool show_l_hi = true, show_r_hi = true;

    switch (current_mood_) {
        case EyeMood::kHappy:
            lw = 52; lh = 58; lrad = 24;
            rw = 52; rh = 58; rrad = 24;
            lb_y = 22; rb_y = 22;  // Smiling crescent arches
            break;

        case EyeMood::kLaughing:
            lw = 54; lh = 56; lrad = 26;
            rw = 54; rh = 56; rrad = 26;
            lb_y = 14; rb_y = 14;  // Deep joyful squint crescents
            break;

        case EyeMood::kLoving:
            lw = 52; lh = 58; lrad = 26;
            rw = 52; rh = 58; rrad = 26;
            lb_y = 20; rb_y = 20;
            break;

        case EyeMood::kEmbarrassed:
            lw = 44; lh = 48; lrad = 20;
            rw = 44; rh = 48; rrad = 20;
            lb_y = 26; rb_y = 26;
            break;

        case EyeMood::kSad:
            lw = 46; lh = 54; lrad = 18;
            rw = 46; rh = 54; rrad = 18;
            lt_x = -6; lt_y = -12; lt_rot = -180;  // Drooping outer corners (/ \)
            rt_x = 6;  rt_y = -12; rt_rot = 180;
            break;

        case EyeMood::kAngry:
            lw = 48; lh = 54; lrad = 14;
            rw = 48; rh = 54; rrad = 14;
            lt_x = 6;  lt_y = -10; lt_rot = 200;   // Sharp inner brow (\ /)
            rt_x = -6; rt_y = -10; rt_rot = -200;
            break;

        case EyeMood::kSurprised:
            lw = 56; lh = 68; lrad = 28;
            rw = 56; rh = 68; rrad = 28;
            break;

        case EyeMood::kThinking:
            lw = 50; lh = 60; lrad = 22;
            rw = 46; rh = 46; rrad = 18;
            rt_y = -24;  // Asymmetric curious squint
            break;

        case EyeMood::kSleepy:
            lw = 48; lh = 46; lrad = 16;
            rw = 48; rh = 46; rrad = 16;
            lt_y = -14; rt_y = -14;  // Heavy relaxed eyelids
            show_l_hi = false;
            show_r_hi = false;
            break;

        case EyeMood::kWinking:
            lw = 50; lh = 60; lrad = 22;
            rw = 50; rh = 10; rrad = 5;  // Right eye playfully closed
            show_r_hi = false;
            break;

        case EyeMood::kCool:
            lw = 52; lh = 42; lrad = 12;
            rw = 52; rh = 42; rrad = 12;
            lt_y = -20; rt_y = -20;
            break;

        case EyeMood::kNeutral:
        default:
            if (visual_state_ == DeviceVisualState::kListening) {
                lw = 52; lh = 64; lrad = 24;
                rw = 52; rh = 64; rrad = 24;
            }
            break;
    }

    // Apply blink squash override
    if (blink_phase_ > 0) {
        lh = 6;
        lrad = 3;
        show_l_hi = false;
        if (current_mood_ != EyeMood::kWinking) {
            rh = 6;
            rrad = 3;
            show_r_hi = false;
        }
    }

    lv_obj_set_size(left_eye_, lw, lh);
    lv_obj_set_style_radius(left_eye_, lrad, 0);
    lv_obj_set_size(right_eye_, rw, rh);
    lv_obj_set_style_radius(right_eye_, rrad, 0);

    if (show_l_hi) {
        lv_obj_remove_flag(left_highlight_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(left_highlight_, LV_OBJ_FLAG_HIDDEN);
    }
    if (show_r_hi) {
        lv_obj_remove_flag(right_highlight_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(right_highlight_, LV_OBJ_FLAG_HIDDEN);
    }

    // Position and tilt eyelids
    lv_obj_set_style_transform_rotation(left_top_lid_, lt_rot, 0);
    lv_obj_align(left_top_lid_, LV_ALIGN_TOP_MID, lt_x, lt_y);
    lv_obj_set_style_transform_rotation(right_top_lid_, rt_rot, 0);
    lv_obj_align(right_top_lid_, LV_ALIGN_TOP_MID, rt_x, rt_y);

    lv_obj_align(left_bottom_lid_, LV_ALIGN_BOTTOM_MID, 0, lb_y);
    lv_obj_align(right_bottom_lid_, LV_ALIGN_BOTTOM_MID, 0, rb_y);
}

void LcdDisplay::EyeTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnEyeTimerTick();
    }
}

void LcdDisplay::OnEyeTimerTick() {
    anim_tick_++;

    // 1. Animate outer circular emotion halo
    if (emotion_halo_ != nullptr) {
        bool spin_mode = (visual_state_ == DeviceVisualState::kListening ||
                          visual_state_ == DeviceVisualState::kConnecting ||
                          current_mood_ == EyeMood::kThinking);
        if (spin_mode) {
            halo_angle_ = (halo_angle_ + 12) % 360;
            lv_arc_set_rotation(emotion_halo_, halo_angle_);
            lv_arc_set_angles(emotion_halo_, 0, 125);
            lv_obj_set_style_arc_opa(emotion_halo_, LV_OPA_COVER, LV_PART_INDICATOR);
        } else {
            lv_arc_set_rotation(emotion_halo_, 270);
            lv_arc_set_angles(emotion_halo_, 0, 360);

            int period = 44;
            int min_opa = LV_OPA_30;
            int max_opa = LV_OPA_70;
            if (visual_state_ == DeviceVisualState::kSpeaking ||
                current_mood_ == EyeMood::kAngry || current_mood_ == EyeMood::kLaughing) {
                period = 18;
                min_opa = LV_OPA_50;
                max_opa = LV_OPA_COVER;
            } else if (current_mood_ == EyeMood::kSleepy) {
                period = 64;
                min_opa = LV_OPA_20;
                max_opa = LV_OPA_40;
            }
            int half = period / 2;
            int step = static_cast<int>(anim_tick_ % period);
            int tri = (step < half) ? step : (period - step);
            lv_opa_t opa = static_cast<lv_opa_t>(min_opa + ((max_opa - min_opa) * tri) / half);
            lv_obj_set_style_arc_opa(emotion_halo_, opa, LV_PART_INDICATOR);
        }
    }

    // 2. Animate eyes when visible
    if (eye_container_ == nullptr || lv_obj_has_flag(eye_container_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    // Natural autonomous blinking
    if (blink_phase_ > 0) {
        blink_phase_--;
        if (blink_phase_ == 0) {
            ApplyEyeMood();
        }
    } else if (anim_tick_ >= next_blink_tick_ && current_mood_ != EyeMood::kLaughing &&
               current_mood_ != EyeMood::kSleepy) {
        blink_phase_ = 2;  // ~120ms quick natural blink
        ApplyEyeMood();
        // Next blink in ~3.0 to ~5.2 seconds
        next_blink_tick_ = anim_tick_ + 50 + ((anim_tick_ * 37) % 38);
    }

    // Subtle natural gaze wandering & playful bounce
    if (anim_tick_ >= next_gaze_tick_) {
        static const int kGazeChoicesX[] = {0, -7, 0, 7, -4, 4, 0};
        static const int kGazeChoicesY[] = {0, -2, 0, -2, 2, 0, 0};
        size_t idx = (anim_tick_ / 7) % 7;
        if (current_mood_ == EyeMood::kThinking) {
            target_gaze_x_ = ((anim_tick_ / 15) % 2 == 0) ? -8 : 8;
            target_gaze_y_ = -5;
        } else if (current_mood_ == EyeMood::kSad || current_mood_ == EyeMood::kSleepy) {
            target_gaze_x_ = 0;
            target_gaze_y_ = 4;
        } else if (current_mood_ == EyeMood::kNeutral || current_mood_ == EyeMood::kCool ||
                   visual_state_ == DeviceVisualState::kListening) {
            target_gaze_x_ = kGazeChoicesX[idx];
            target_gaze_y_ = kGazeChoicesY[idx];
        } else {
            target_gaze_x_ = 0;
            target_gaze_y_ = 0;
        }
        next_gaze_tick_ = anim_tick_ + 36 + ((anim_tick_ * 23) % 32);
    }

    // Smooth step toward target gaze
    if (gaze_x_ < target_gaze_x_) {
        gaze_x_ += 2;
        if (gaze_x_ > target_gaze_x_) gaze_x_ = target_gaze_x_;
    } else if (gaze_x_ > target_gaze_x_) {
        gaze_x_ -= 2;
        if (gaze_x_ < target_gaze_x_) gaze_x_ = target_gaze_x_;
    }

    if (gaze_y_ < target_gaze_y_) {
        gaze_y_ += 1;
    } else if (gaze_y_ > target_gaze_y_) {
        gaze_y_ -= 1;
    }

    int bounce_y = 0;
    if (current_mood_ == EyeMood::kHappy || current_mood_ == EyeMood::kLaughing ||
        current_mood_ == EyeMood::kLoving || visual_state_ == DeviceVisualState::kSpeaking) {
        int b = static_cast<int>(anim_tick_ % 12);
        bounce_y = (b < 6) ? -(b / 2) : -((12 - b) / 2);
    }

    lv_obj_align(left_eye_wrap_, LV_ALIGN_CENTER, -38 + gaze_x_, gaze_y_ + bounce_y);
    lv_obj_align(right_eye_wrap_, LV_ALIGN_CENTER, 38 + gaze_x_, gaze_y_ + bounce_y);
}

void LcdDisplay::ShowEmotion() {
    if (IsRoundDisplay() && eye_container_ != nullptr) {
        if (emotion_uses_image_) {
            lv_obj_add_flag(eye_container_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_label_ != nullptr) {
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_image_ != nullptr) {
                lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_remove_flag(eye_container_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_label_ != nullptr) {
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_image_ != nullptr) {
                lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (emotion_uses_image_ && gif_controller_ && !gif_controller_->IsPlaying()) {
            gif_controller_->Start();
        }
        return;
    }

    // Default behavior for non-round displays
    if (emoji_label_ != nullptr) {
        if (emotion_uses_image_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (emoji_image_ != nullptr) {
        if (emotion_uses_image_) {
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (emotion_uses_image_ && gif_controller_ && !gif_controller_->IsPlaying()) {
        gif_controller_->Start();
    }
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

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
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

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

    if (strlen(content) == 0) {
        return;
    }

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
    if (text_width < min_width) {
        text_width = min_width;
    }

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
    if (content_ == nullptr || image == nullptr) {
        return;
    }

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
            if (img != nullptr) {
                delete img;
            }
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
    if (content_ == nullptr || role == nullptr || content == nullptr) {
        return;
    }
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
        if (strcmp(lv_label_get_text(label), content) != 0) {
            continue;
        }
        if (chat_message_label_ == label) {
            chat_message_label_ = nullptr;
        }
        lv_obj_del(container);
        if (lv_obj_get_child_cnt(content_) == 0) {
            ShowEmotion();
        }
        return;
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    lv_obj_clean(content_);
    chat_message_label_ = nullptr;
    ShowEmotion();
}
#else
void LcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

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

    /* Background container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    if (IsRoundDisplay()) {
        /* 1. Outer emotion halo & animated big eyes */
        CreateEyeUI(screen, lvgl_theme);

        /* Keep emoji_box_ for optional GIF/PNG asset compatibility (hidden by default) */
        emoji_box_ = lv_obj_create(screen);
        lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(emoji_box_, 0, 0);
        lv_obj_set_style_border_width(emoji_box_, 0, 0);
        lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -4);

        emoji_label_ = lv_label_create(emoji_box_);
        lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

        emoji_image_ = lv_img_create(emoji_box_);
        lv_obj_center(emoji_image_);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

        /* Preview image */
        preview_image_ = lv_image_create(screen);
        lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

        /* 2. Top icon bar tucked inside circular top arc safe zone */
        top_bar_ = lv_obj_create(screen);
        lv_obj_set_size(top_bar_, 116, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(top_bar_, 0, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top_bar_, 0, 0);
        lv_obj_set_style_pad_all(top_bar_, 0, 0);
        lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(top_bar_, 8, 0);
        lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 12);

        lv_color_t subtle_text = lv_color_hex(0x94A3B8);

        network_label_ = lv_label_create(top_bar_);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, subtle_text, 0);

        mute_label_ = lv_label_create(top_bar_);
        lv_label_set_text(mute_label_, "");
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, subtle_text, 0);

        battery_label_ = lv_label_create(top_bar_);
        lv_label_set_text(battery_label_, "");
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, subtle_text, 0);

        /* 3. Status text tucked cleanly below top icons */
        status_bar_ = lv_obj_create(screen);
        lv_obj_set_size(status_bar_, 144, text_font->line_height + 4);
        lv_obj_set_style_radius(status_bar_, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(status_bar_, 0, 0);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 32);

        notification_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(notification_label_, 140);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(notification_label_, "");
        lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        status_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(status_label_, 140);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_label_, lv_color_hex(0xCBD5E1), 0);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
        lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

        /* 4. Circular-safe multiline subtitle pill lifted above bottom rim */
        bottom_bar_ = lv_obj_create(screen);
        lv_obj_set_width(bottom_bar_, 168);
        lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(bottom_bar_, 62, 0);
        lv_obj_set_style_radius(bottom_bar_, 14, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0x16181D), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
        lv_obj_set_style_border_width(bottom_bar_, 1, 0);
        lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(0x2D333B), 0);
        lv_obj_set_style_pad_top(bottom_bar_, 5, 0);
        lv_obj_set_style_pad_bottom(bottom_bar_, 5, 0);
        lv_obj_set_style_pad_left(bottom_bar_, 10, 0);
        lv_obj_set_style_pad_right(bottom_bar_, 10, 0);
        lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -20);

        chat_message_label_ = lv_label_create(bottom_bar_);
        lv_label_set_text(chat_message_label_, "");
        lv_obj_set_width(chat_message_label_, 146);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(chat_message_label_, 2, 0);
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
        lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

        /* 5. Low battery popup inside circular safe zone */
        low_battery_popup_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(low_battery_popup_, 160, text_font->line_height + 16);
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -28);
        lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
        lv_obj_set_style_border_width(low_battery_popup_, 0, 0);
        lv_obj_set_style_radius(low_battery_popup_, 14, 0);

        low_battery_label_ = lv_label_create(low_battery_popup_);
        lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
        lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
        lv_obj_center(low_battery_label_);
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
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
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
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

    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (eye_container_ != nullptr) {
        lv_obj_add_flag(eye_container_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }
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
                lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -20);
            }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
            else {
                lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
            }
#endif
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
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    subtitle_visible_ = false;
    UpdateEyeLayout();
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    subtitle_visible_ = false;
    UpdateEyeLayout();
}
#endif

void LcdDisplay::SetStatus(const char* status) {
    // Call parent first before locking to avoid recursive lock issues
    LvglDisplay::SetStatus(status);

    if (!IsRoundDisplay() || status == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    DeviceVisualState new_state = visual_state_;

    if (strstr(status, "聆听") != nullptr || strstr(status, "Listen") != nullptr ||
        strstr(status, "listen") != nullptr) {
        new_state = DeviceVisualState::kListening;
    } else if (strstr(status, "说话") != nullptr || strstr(status, "Speak") != nullptr ||
               strstr(status, "speak") != nullptr) {
        new_state = DeviceVisualState::kSpeaking;
    } else if (strstr(status, "连接") != nullptr || strstr(status, "初始") != nullptr ||
               strstr(status, "配网") != nullptr || strstr(status, "Connect") != nullptr ||
               strstr(status, "Init") != nullptr) {
        new_state = DeviceVisualState::kConnecting;
    } else if (strstr(status, "待命") != nullptr || strstr(status, "Standby") != nullptr ||
               strstr(status, "Idle") != nullptr) {
        new_state = DeviceVisualState::kIdle;
    }

    if (new_state != visual_state_) {
        visual_state_ = new_state;
        ApplyEyeMood();
    }
}

bool LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
    if (emotion == nullptr || emoji_image_ == nullptr) {
        return false;
    }

    // Always update expressive eyes & circular halo mood
    if (IsRoundDisplay()) {
        DisplayLockGuard lock(this);
        current_mood_ = ParseEmotionMood(emotion);
        blink_phase_ = 0;
        ApplyEyeMood();
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        // On round displays without external image assets, render expressive animated eyes
        if (IsRoundDisplay() && eye_container_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            emotion_uses_image_ = false;
            ShowEmotion();
            return true;
        }

        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 == nullptr || emoji_label_ == nullptr) {
            return false;
        }
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
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
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
        if (gif_controller_) {
            gif_controller_->Pause();
        }
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

    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Keep eyelid masks seamlessly matched to active background color
    lv_color_t lid_color = lvgl_theme->background_color();
    if (left_top_lid_ != nullptr) lv_obj_set_style_bg_color(left_top_lid_, lid_color, 0);
    if (right_top_lid_ != nullptr) lv_obj_set_style_bg_color(right_top_lid_, lid_color, 0);
    if (left_bottom_lid_ != nullptr) lv_obj_set_style_bg_color(left_bottom_lid_, lid_color, 0);
    if (right_bottom_lid_ != nullptr) lv_obj_set_style_bg_color(right_bottom_lid_, lid_color, 0);

    if (top_bar_ != nullptr && !IsRoundDisplay()) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    if (!IsRoundDisplay()) {
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    }
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

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
            } else if (strcmp(bubble_type, "system") == 0 || strcmp(bubble_type, "image") == 0) {
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
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    if (bottom_bar_ != nullptr) {
        if (IsRoundDisplay()) {
            bool is_dark = (lvgl_theme->name() == "dark");
            lv_obj_set_style_bg_color(bottom_bar_,
                                      is_dark ? lv_color_hex(0x16181D) : lv_color_hex(0xF1F5F9), 0);
            lv_obj_set_style_border_color(
                bottom_bar_, is_dark ? lv_color_hex(0x2D333B) : lv_color_hex(0xCBD5E1), 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
        } else {
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
            lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
        }
    }
#endif

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
            }
        }
    }
    UpdateEyeLayout();
}
