#ifndef _FACE_DISPLAY_H_
#define _FACE_DISPLAY_H_

// Cara animada a pantalla completa para OLED 128x64: dos ojos grandes
// (estilo ZzPet) que cambian segun la emocion que manda el servidor y el
// estado del asistente. Sin texto: el texto original solo aparece cuando
// hace falta (configurar WiFi, codigo de activacion, actualizacion, error).

#include "display/oled_display.h"
#include "application.h"
#include "emotion_led.h"

#include <esp_random.h>
#include <esp_log.h>
#include <cstring>
#include <cstdlib>

class FaceDisplay : public OledDisplay {
public:
    using OledDisplay::OledDisplay;

    void SetupUI() override {
        uint32_t before = lv_obj_get_child_count(lv_screen_active());
        OledDisplay::SetupUI();

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        // Layout original de OledDisplay: [contenedor, barra de estado, popup bateria]
        original_ = lv_obj_get_child(screen, before);
        status_ = lv_obj_get_child(screen, before + 1);

        face_ = lv_obj_create(screen);
        Plain(face_);
        lv_obj_set_style_bg_opa(face_, LV_OPA_TRANSP, 0);
        lv_obj_set_size(face_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(face_, 0, 0);

        for (int i = 0; i < 2; i++) {
            eye_[i] = MakeRect(face_);
            arc_[i] = MakeLine(face_, 5);
            brow_[i] = MakeLine(face_, 4);

            blush_[i] = MakeRect(face_);
            lv_obj_set_style_radius(blush_[i], 2, 0);
            lv_obj_set_size(blush_[i], 10, 3);

            // Corazon: dos circulos + triangulo hecho con barras horizontales
            for (int k = 0; k < 2; k++) {
                lobe_[i][k] = MakeRect(face_);
                lv_obj_set_style_radius(lobe_[i][k], LV_RADIUS_CIRCLE, 0);
            }
            for (int k = 0; k < kHeartRows; k++) {
                heart_bar_[i][k] = MakeRect(face_);
            }
        }

        bridge_ = MakeRect(face_);
        lv_obj_set_size(bridge_, kEyeX[1] - kEyeX[0] - 40, 3);
        lv_obj_set_pos(bridge_, kEyeX[0] + 20, kCy - 5);

        next_blink_ = 60 + esp_random() % 40;
        timer_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<FaceDisplay*>(lv_timer_get_user_data(t))->Tick();
        }, 50, this);

        ApplyMode();
        Render();
    }

    void SetEmotion(const char* emotion) override {
        if (EmotionLed::instance != nullptr) {
            EmotionLed::instance->SetEmotion(emotion);
        }
        DisplayLockGuard lock(this);
        emotion_ = Parse(emotion);
        Render();
    }

private:
    enum Emotion {
        kNeutral, kHappy, kLoving, kSad, kAngry, kSurprised, kThinking,
        kWink, kCool, kSleepy, kSilly, kConfused, kEmbarrassed
    };

    static constexpr int kCy = 32;
    static constexpr int kEyeX[2] = {34, 94};
    static constexpr int kHeartRows = 9;

    lv_obj_t* original_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* face_ = nullptr;
    lv_obj_t* eye_[2] = {};
    lv_obj_t* arc_[2] = {};
    lv_obj_t* brow_[2] = {};
    lv_obj_t* blush_[2] = {};
    lv_obj_t* bridge_ = nullptr;
    lv_obj_t* lobe_[2][2] = {};
    lv_obj_t* heart_bar_[2][kHeartRows] = {};
    lv_timer_t* timer_ = nullptr;

    lv_point_precise_t arc_pts_[2][3] = {};
    lv_point_precise_t brow_pts_[2][2] = {};

    Emotion emotion_ = kNeutral;
    uint32_t tick_ = 0;
    uint32_t next_blink_ = 80;
    int blink_left_ = 0;
    uint32_t idle_ticks_ = 0;
    uint32_t last_key_ = 0xFFFFFFFF;
    int mode_ = -1;  // 0 = cara, 1 = texto informativo

    static lv_point_precise_t P(int x, int y) {
        lv_point_precise_t p;
        p.x = (lv_value_precise_t)x;
        p.y = (lv_value_precise_t)y;
        return p;
    }

    static void Plain(lv_obj_t* o) {
        lv_obj_remove_style_all(o);
        lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }

    static lv_obj_t* MakeRect(lv_obj_t* parent) {
        lv_obj_t* o = lv_obj_create(parent);
        Plain(o);
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    }

    static lv_obj_t* MakeLine(lv_obj_t* parent, int width) {
        lv_obj_t* l = lv_line_create(parent);
        lv_obj_set_style_line_color(l, lv_color_black(), 0);
        lv_obj_set_style_line_width(l, width, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_set_pos(l, 0, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return l;
    }

    static Emotion Parse(const char* e) {
        auto is = [&](const char* s) { return strcmp(e, s) == 0; };
        if (is("happy") || is("laughing") || is("funny") || is("confident") ||
            is("delicious") || is("relaxed")) return kHappy;
        if (is("loving") || is("kissy")) return kLoving;
        if (is("sad") || is("crying")) return kSad;
        if (is("angry")) return kAngry;
        if (is("surprised") || is("shocked")) return kSurprised;
        if (is("thinking")) return kThinking;
        if (is("winking")) return kWink;
        if (is("cool")) return kCool;
        if (is("sleepy")) return kSleepy;
        if (is("silly")) return kSilly;
        if (is("confused")) return kConfused;
        if (is("embarrassed")) return kEmbarrassed;
        return kNeutral;
    }

    static void Show(lv_obj_t* o, bool show) {
        if (o == nullptr) return;
        if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    // Estados donde hace falta leer texto (instrucciones WiFi, codigo, etc.)
    static bool NeedsText(DeviceState s) {
        return s == kDeviceStateStarting || s == kDeviceStateWifiConfiguring ||
               s == kDeviceStateActivating || s == kDeviceStateUpgrading ||
               s == kDeviceStateFatalError;
    }

    void ApplyMode() {
        int mode = NeedsText(Application::GetInstance().GetDeviceState()) ? 1 : 0;
        if (mode == mode_) return;
        mode_ = mode;
        Show(original_, mode == 1);
        Show(status_, mode == 1);
        Show(face_, mode == 0);
        last_key_ = 0xFFFFFFFF;  // forzar redibujo de la cara
    }

    void Tick() {
        tick_++;
        auto state = Application::GetInstance().GetDeviceState();
        idle_ticks_ = (state == kDeviceStateIdle) ? idle_ticks_ + 1 : 0;

        if (blink_left_ > 0) {
            blink_left_--;
        } else if (tick_ >= next_blink_) {
            blink_left_ = 3;  // ~150 ms con el ojo cerrado
            next_blink_ = tick_ + 60 + esp_random() % 50;  // cada 3 a 5,5 s
        }
        ApplyMode();
        Render();
    }

    // Corazon centrado en (cx, cy): dos circulos arriba y un triangulo
    // hecho con barras horizontales (simetrico). grow = latido.
    void DrawHeart(int i, int cx, int cy, int grow) {
        int r = 9 + grow / 2;
        for (int k = 0; k < 2; k++) {
            int lx = cx + (k ? 8 : -8);
            lv_obj_set_size(lobe_[i][k], r * 2, r * 2);
            lv_obj_set_pos(lobe_[i][k], lx - r, cy - 6 - r);
        }
        int top = cy - 2;
        int half = 16 + grow;
        int rows_h = 2;
        for (int k = 0; k < kHeartRows; k++) {
            int y = top + k * rows_h;
            int hw = half - (half * (k + 1)) / (kHeartRows + 1) + 1;
            lv_obj_set_size(heart_bar_[i][k], hw * 2, rows_h);
            lv_obj_set_pos(heart_bar_[i][k], cx - hw, y);
        }
    }

    void Render() {
        if (face_ == nullptr || mode_ != 0) return;
        auto state = Application::GetInstance().GetDeviceState();

        Emotion emo = emotion_;
        // Despues de 3 minutos en reposo, se "duerme"
        if (state == kDeviceStateIdle && idle_ticks_ > 3600) emo = kSleepy;

        bool listening = state == kDeviceStateListening;
        bool speaking = state == kDeviceStateSpeaking;
        bool blinking = blink_left_ > 0;
        int bob = speaking ? (((tick_ / 4) % 2) ? -2 : 2) : 0;
        // Latido: los corazones se agrandan un instante cada ~1 s
        bool beat = emo == kLoving && (tick_ % 20) < 3;

        // Evita redibujar si nada cambio (el I2C de la OLED es lento)
        uint32_t key = emo | (listening << 5) | (speaking << 6) | (blinking << 7) |
                       ((bob + 2) << 8) | (beat << 11);
        if (key == last_key_) return;
        last_key_ = key;

        for (int i = 0; i < 2; i++) {
            int cx = kEyeX[i];
            int w = 30, h = 42, r = 11, dx = 0, dy = bob;
            bool eye = true, arc = false, brow = false, blush = false;

            switch (emo) {
                case kHappy:
                    eye = false; arc = true;
                    break;
                case kLoving:
                    eye = false;
                    break;
                case kSad:
                    h = 30; dy += 5; brow = true;
                    brow_pts_[i][0] = P(cx - 16, kCy - (i ? 26 : 18));
                    brow_pts_[i][1] = P(cx + 16, kCy - (i ? 18 : 26));
                    break;
                case kAngry:
                    h = 30; dy += 5; brow = true;
                    brow_pts_[i][0] = P(cx - 16, kCy - (i ? 17 : 27));
                    brow_pts_[i][1] = P(cx + 16, kCy - (i ? 27 : 17));
                    break;
                case kSurprised:
                    w = 40; h = 40; r = 20;
                    break;
                case kThinking:
                    h = 32; dx = 9; dy -= 6;
                    break;
                case kWink:
                    if (i == 1) { h = 5; r = 2; }
                    break;
                case kCool:
                    w = 40; h = 16; r = 4;
                    break;
                case kSleepy:
                    w = 32; h = 5; r = 2; dy += 8;
                    break;
                case kSilly:
                    if (i == 0) { w = 36; h = 46; r = 14; } else { w = 18; h = 18; r = 9; }
                    break;
                case kConfused:
                    if (i == 1) { w = 20; h = 18; r = 8; dy += 6; }
                    break;
                case kEmbarrassed:
                    h = 30; blush = true;
                    break;
                case kNeutral:
                default:
                    break;
            }

            if (listening && emo == kNeutral) { w += 5; h += 6; r += 2; }
            bool can_blink = eye && emo != kSleepy && !(emo == kWink && i == 1);
            if (blinking && can_blink) { h = 5; r = 2; }

            Show(eye_[i], eye);
            if (eye) {
                lv_obj_set_size(eye_[i], w, h);
                lv_obj_set_style_radius(eye_[i], r, 0);
                lv_obj_set_pos(eye_[i], cx - w / 2 + dx, kCy - h / 2 + dy);
            }

            Show(arc_[i], arc);
            if (arc) {
                arc_pts_[i][0] = P(cx - 16, kCy + 7 + dy);
                arc_pts_[i][1] = P(cx, kCy - 9 + dy);
                arc_pts_[i][2] = P(cx + 16, kCy + 7 + dy);
                lv_line_set_points(arc_[i], arc_pts_[i], 3);
            }

            Show(brow_[i], brow);
            if (brow) {
                lv_line_set_points(brow_[i], brow_pts_[i], 2);
            }

            bool heart = emo == kLoving;
            Show(lobe_[i][0], heart);
            Show(lobe_[i][1], heart);
            for (int k = 0; k < kHeartRows; k++) Show(heart_bar_[i][k], heart);
            if (heart) {
                DrawHeart(i, cx, kCy + dy, beat ? 2 : 0);
            }

            Show(blush_[i], blush);
            lv_obj_set_pos(blush_[i], cx - 5 + (i ? 12 : -12), kCy + 20);
        }
        Show(bridge_, emo == kCool);
    }
};

#endif  // _FACE_DISPLAY_H_
