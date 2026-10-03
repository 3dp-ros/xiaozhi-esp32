#ifndef _FACE_DISPLAY_H_
#define _FACE_DISPLAY_H_

// Cara animada de Kira para OLED 128x64 (estilo anime, masculino, simpatico
// y picaro). Cada emocion tiene su propia animacion. En reposo: respira,
// parpadea, mira alrededor y cada tanto tiene "momentos": una sonrisa con
// saltito o un gesto picaro (ceja levantada, mirada de reojo, sonrisa de costado).
// El texto original solo aparece cuando hace falta (WiFi, activacion, error).
// Tambien maneja el brillo (Home Assistant), el modo noche (brillo minimo),
// el modo dormir (pantalla apagada) y un corrimiento lento de toda la cara
// cada 10 minutos para que la OLED no se "queme".

#include "display/oled_display.h"
#include "application.h"
#include "emotion_led.h"
#include "ha_bridge.h"
#include "config.h"
#include "kira_controls.h"

#include <esp_random.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <cmath>
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
            eye_[i] = MakeRect(face_, true);
            for (int k = 0; k < 2; k++) {
                lobe_[i][k] = MakeRect(face_, true);
                lv_obj_set_style_radius(lobe_[i][k], LV_RADIUS_CIRCLE, 0);
            }
            for (int k = 0; k < kHeartRows; k++) heart_bar_[i][k] = MakeRect(face_, true);
            hl_big_[i] = MakeRect(face_, false);
            hl_small_[i] = MakeRect(face_, false);
            lv_obj_set_style_radius(hl_big_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_radius(hl_small_[i], LV_RADIUS_CIRCLE, 0);
            lid_[i] = MakeLine(face_, 10, false);    // parpado enojado (corta el ojo)
            arc_[i] = MakeLine(face_, 4, true);      // ^ ^, > <, ojos cerrados
            brow_[i] = MakeLine(face_, 3, true);     // cejas
            tear_[i] = MakeRect(face_, true);
            for (int k = 0; k < 3; k++) blush_[i][k] = MakeLine(face_, 2, true);
        }
        bridge_ = MakeRect(face_, true);
        glint_ = MakeLine(face_, 2, false);
        for (int k = 0; k < 3; k++) dot_[k] = MakeRect(face_, true);
        mouth_fill_ = MakeRect(face_, true);
        mouth_cover_ = MakeRect(face_, false);
        mouth_ring_ = lv_obj_create(face_);
        Plain(mouth_ring_);
        lv_obj_set_style_bg_opa(mouth_ring_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(mouth_ring_, lv_color_black(), 0);
        lv_obj_set_style_border_width(mouth_ring_, 2, 0);
        lv_obj_set_style_radius(mouth_ring_, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(mouth_ring_, LV_OBJ_FLAG_HIDDEN);
        mouth_line_ = MakeLine(face_, 2, true);
        z_line_ = MakeLine(face_, 2, true);

        next_blink_ = 60 + Rand(40);
        next_moment_ = 200 + Rand(200);
        timer_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<FaceDisplay*>(lv_timer_get_user_data(t))->Tick();
        }, 50, this);

        ApplyMode();
        Render();
    }

    // La placa pasa los handles para poder cambiar el brillo y apagar la OLED
    void SetPanel(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel) {
        io_ = io;
        panel_ = panel;
    }

    void SetEmotion(const char* emotion) override {
        if (EmotionLed::instance != nullptr) {
            EmotionLed::instance->SetEmotion(emotion);
        }
        HaBridge::Instance().OnEmotion(emotion);
        DisplayLockGuard lock(this);
        Emotion e = Parse(emotion);
        if (e != emotion_) {
            emotion_ = e;
            emo_ticks_ = 0;
            moment_ = kMomNone;
            moment_left_ = 0;
        }
        Render();
    }

private:
    enum Emotion {
        kNeutral, kHappy, kLaugh, kLoving, kSad, kAngry, kSurprised, kThinking,
        kWink, kCool, kSleepy, kSilly, kConfused, kEmbarrassed
    };
    enum Mouth { kMNone, kMSmile, kMSmileBig, kMOpen, kMBig, kMO, kMOBig, kMFrown, kMFlat,
                 kMWavy, kMSmirk, kMSmirkBig, kMAngry, kMTalk, kMTalkWide };
    enum EyeShape { kEOpen, kEHappy, kEClosed, kEGreater, kELess, kEHeart, kEGlasses, kENone };
    enum Moment { kMomNone, kMomSmile, kMomPicaro };

    static constexpr int kCy = 26;
    static constexpr int kEyeX[2] = {36, 92};
    static constexpr int kMouthX = 64;
    static constexpr int kMouthY = 54;
    static constexpr int kHeartRows = 9;

    // ---- objetos ----
    lv_obj_t* original_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* face_ = nullptr;
    lv_obj_t* eye_[2] = {};
    lv_obj_t* hl_big_[2] = {};
    lv_obj_t* hl_small_[2] = {};
    lv_obj_t* lid_[2] = {};
    lv_obj_t* arc_[2] = {};
    lv_obj_t* brow_[2] = {};
    lv_obj_t* tear_[2] = {};
    lv_obj_t* blush_[2][3] = {};
    lv_obj_t* lobe_[2][2] = {};
    lv_obj_t* heart_bar_[2][kHeartRows] = {};
    lv_obj_t* bridge_ = nullptr;
    lv_obj_t* glint_ = nullptr;
    lv_obj_t* dot_[3] = {};
    lv_obj_t* mouth_fill_ = nullptr;
    lv_obj_t* mouth_cover_ = nullptr;
    lv_obj_t* mouth_ring_ = nullptr;
    lv_obj_t* mouth_line_ = nullptr;
    lv_obj_t* z_line_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    lv_point_precise_t arc_pts_[2][5] = {};
    lv_point_precise_t lid_pts_[2][2] = {};
    lv_point_precise_t brow_pts_[2][3] = {};
    lv_point_precise_t blush_pts_[2][3][2] = {};
    lv_point_precise_t mouth_pts_[8] = {};
    lv_point_precise_t z_pts_[4] = {};
    lv_point_precise_t glint_pts_[2] = {};

    // ---- estado ----
    Emotion emotion_ = kNeutral;
    uint32_t tick_ = 0;
    uint32_t emo_ticks_ = 0;
    uint32_t idle_ticks_ = 0;
    int mode_ = -1;  // 0 = cara, 1 = texto informativo
    DeviceState prev_state_ = kDeviceStateUnknown;

    uint32_t next_blink_ = 80;
    int blink_left_ = 0;
    int pending_blink_ = 0;
    int look_x_ = 0, look_y_ = 0, look_tx_ = 0, look_ty_ = 0;
    uint32_t next_look_ = 100, look_hold_until_ = 0;
    int bounce_left_ = 0;
    int twinkle_left_ = 0;
    uint32_t next_twinkle_ = 120;
    int open_anim_ = 0;
    int last_pir_ = 0;
    Moment moment_ = kMomNone;
    int moment_left_ = 0;
    uint32_t next_moment_ = 300;

    int last_frame_[48] = {-1};

    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    int applied_contrast_ = -1;
    int applied_on_ = -1;
    int shift_x_ = 0, shift_y_ = 0;   // anti-quemado

    // ---------- utilidades ----------
    static int Rand(int n) { return (int)(esp_random() % (uint32_t)n); }

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

    // En esta OLED "negro" = pixel encendido, "blanco" = apagado
    static lv_color_t Ink(bool on) { return on ? lv_color_black() : lv_color_white(); }

    static lv_obj_t* MakeRect(lv_obj_t* parent, bool on) {
        lv_obj_t* o = lv_obj_create(parent);
        Plain(o);
        lv_obj_set_style_bg_color(o, Ink(on), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return o;
    }

    static lv_obj_t* MakeLine(lv_obj_t* parent, int width, bool on) {
        lv_obj_t* l = lv_line_create(parent);
        lv_obj_set_style_line_color(l, Ink(on), 0);
        lv_obj_set_style_line_width(l, width, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_set_pos(l, 0, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return l;
    }

    static void Show(lv_obj_t* o, bool show) {
        if (o == nullptr) return;
        if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    static void Box(lv_obj_t* o, int x, int y, int w, int h, int r) {
        lv_obj_set_size(o, w, h);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_style_radius(o, r, 0);
        Show(o, true);
    }

    static void Circle(lv_obj_t* o, int cx, int cy, int d) {
        lv_obj_set_size(o, d, d);
        lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
        Show(o, true);
    }

    static Emotion Parse(const char* e) {
        auto is = [&](const char* s) { return strcmp(e, s) == 0; };
        if (is("happy") || is("confident") || is("delicious") || is("relaxed")) return kHappy;
        if (is("laughing") || is("funny")) return kLaugh;
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
        if (mode == 0) open_anim_ = 10;   // abre los ojos de a poco
        last_frame_[0] = -1;
    }

    // ---------- reloj de animacion (cada 50 ms) ----------
    // Brillo y encendido de la OLED segun los controles de Home Assistant
    void ApplyPanel() {
        if (io_ == nullptr || panel_ == nullptr) return;
        auto& ctl = kira::Controls::Get();
        int on = ctl.Sleep() ? 0 : 1;
        if (on != applied_on_) {
            esp_lcd_panel_disp_on_off(panel_, on);
            applied_on_ = on;
            if (on) { open_anim_ = 10; last_frame_[0] = -1; }
        }
        int contrast = ctl.Contrast();
        if (contrast != applied_contrast_) {
            uint8_t v = (uint8_t)contrast;
            esp_lcd_panel_io_tx_param(io_, 0x81, &v, 1);   // comando de contraste SH1106
            applied_contrast_ = contrast;
        }
    }

    void Tick() {
        tick_++;
        emo_ticks_++;
        ApplyPanel();
        // Anti-quemado: cada 10 minutos corre toda la cara 1 px
        static const int kShift[5][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
        int slot = (int)((tick_ / 12000) % 5);
        shift_x_ = kShift[slot][0];
        shift_y_ = kShift[slot][1];
        if (kira::Controls::Get().Sleep()) {
            // Durmiendo: si lo llaman con "Ey Kira" (empieza a escuchar o a
            // conectarse) se despierta solo. Los avisos de HA no lo despiertan.
            auto st = Application::GetInstance().GetDeviceState();
            if (st == kDeviceStateListening || st == kDeviceStateConnecting) {
                kira::Controls::Get().SetSleep(false);
            } else {
                ApplyMode();
                return;   // pantalla apagada, no hace falta animar
            }
        }
        auto state = Application::GetInstance().GetDeviceState();
        idle_ticks_ = (state == kDeviceStateIdle) ? idle_ticks_ + 1 : 0;

        // Saltito cuando empieza a escuchar (wake word o boton)
        if (state == kDeviceStateListening && prev_state_ != kDeviceStateListening) {
            bounce_left_ = 8;
            look_tx_ = look_ty_ = 0;
            moment_ = kMomNone;
            moment_left_ = 0;
        }
        // Al terminar una charla vuelve a neutral (la emocion quedaba pegada)
        if (state == kDeviceStateIdle && prev_state_ != kDeviceStateIdle &&
            prev_state_ != kDeviceStateUnknown && emotion_ != kNeutral) {
            emotion_ = kNeutral;
            emo_ticks_ = 0;
        }
        prev_state_ = state;

        // Sensor de movimiento: si dormia se despierta; si no, "te mira" y sonrie
        int pir = gpio_get_level(PIR_GPIO);
        if (pir && !last_pir_ && state == kDeviceStateIdle) {
            idle_ticks_ = 0;
            bounce_left_ = 8;
            look_tx_ = Rand(2) ? 6 : -6;
            look_ty_ = 0;
            look_hold_until_ = tick_ + 30;
            if (emotion_ == kNeutral) { moment_ = kMomSmile; moment_left_ = 30; }
        }
        last_pir_ = pir;

        // Parpadeo (a veces doble)
        if (blink_left_ > 0) {
            blink_left_--;
        } else if (pending_blink_ > 0) {
            if (--pending_blink_ == 0) blink_left_ = 2;
        } else if (tick_ >= next_blink_) {
            blink_left_ = 3;
            if (Rand(5) == 0) pending_blink_ = 3;
            next_blink_ = tick_ + 60 + Rand(50);
        }

        // Momentos simpaticos en reposo (cada 10 a 25 s)
        if (moment_left_ > 0) {
            if (--moment_left_ == 0) {
                moment_ = kMomNone;
                look_tx_ = look_ty_ = 0;
            }
        } else if (state == kDeviceStateIdle && idle_ticks_ < 3600 &&
                   emotion_ == kNeutral && tick_ >= next_moment_) {
            if (Rand(10) < 6) {
                moment_ = kMomSmile;           // sonrisa con saltito
                moment_left_ = 30;
                bounce_left_ = 8;
            } else {
                moment_ = kMomPicaro;          // ceja levantada + reojo
                moment_left_ = 40;
                look_tx_ = Rand(2) ? 7 : -7;
                look_ty_ = 0;
                look_hold_until_ = tick_ + 40;
            }
            next_moment_ = tick_ + 200 + Rand(300);
        }

        // Mirar alrededor (solo en reposo)
        if (state == kDeviceStateIdle && moment_ == kMomNone) {
            if (tick_ >= next_look_ && tick_ >= look_hold_until_) {
                if (look_tx_ == 0 && look_ty_ == 0) {
                    static const int xs[] = {-7, -5, 5, 7};
                    look_tx_ = xs[Rand(4)];
                    look_ty_ = Rand(3) - 1;
                    look_hold_until_ = tick_ + 20 + Rand(30);
                } else {
                    look_tx_ = look_ty_ = 0;
                }
                next_look_ = tick_ + 40 + Rand(80);
            }
        } else if (state != kDeviceStateIdle) {
            look_tx_ = look_ty_ = 0;
        }
        if (look_x_ < look_tx_) look_x_ += (look_tx_ - look_x_ > 1) ? 2 : 1;
        else if (look_x_ > look_tx_) look_x_ -= (look_x_ - look_tx_ > 1) ? 2 : 1;
        if (look_y_ < look_ty_) look_y_++;
        else if (look_y_ > look_ty_) look_y_--;

        // Brillitos que titilan
        if (twinkle_left_ > 0) twinkle_left_--;
        else if (tick_ >= next_twinkle_) {
            twinkle_left_ = 3;
            next_twinkle_ = tick_ + 100 + Rand(100);
        }

        if (bounce_left_ > 0) bounce_left_--;
        if (open_anim_ > 0) open_anim_--;

        ApplyMode();
        Render();
    }

    // ---------- piezas ----------
    void HideAll() {
        for (int i = 0; i < 2; i++) {
            Show(eye_[i], false);
            Show(hl_big_[i], false);
            Show(hl_small_[i], false);
            Show(lid_[i], false);
            Show(arc_[i], false);
            Show(brow_[i], false);
            Show(tear_[i], false);
            Show(lobe_[i][0], false);
            Show(lobe_[i][1], false);
            for (int k = 0; k < kHeartRows; k++) Show(heart_bar_[i][k], false);
            for (int k = 0; k < 3; k++) Show(blush_[i][k], false);
        }
        Show(bridge_, false);
        Show(glint_, false);
        for (int k = 0; k < 3; k++) Show(dot_[k], false);
        Show(z_line_, false);
    }

    void OpenEye(int i, int cx, int cy, int w, int h, int look_x, int look_y) {
        int r = w / 2 < h / 2 ? w / 2 : h / 2;
        Box(eye_[i], cx - w / 2, cy - h / 2, w, h, r);
        if (h < 10) return;
        int big = w / 3 + 1;
        bool tw = twinkle_left_ > 0;
        Circle(hl_big_[i], cx - w / 5 + look_x, cy - h / 4 + look_y + 1, tw ? big - 2 : big);
        if (h >= 16 && !tw) Circle(hl_small_[i], cx + w / 5 + look_x, cy + h / 5 + look_y, big / 2 + 1);
    }

    void SetArc(int i, int n) {
        lv_line_set_points(arc_[i], arc_pts_[i], n);
        Show(arc_[i], true);
    }

    void HappyEye(int i, int cx, int cy) {
        arc_pts_[i][0] = P(cx - 12, cy + 5);
        arc_pts_[i][1] = P(cx - 6, cy - 2);
        arc_pts_[i][2] = P(cx, cy - 5);
        arc_pts_[i][3] = P(cx + 6, cy - 2);
        arc_pts_[i][4] = P(cx + 12, cy + 5);
        SetArc(i, 5);
    }

    void ClosedEye(int i, int cx, int cy) {
        arc_pts_[i][0] = P(cx - 12, cy - 2);
        arc_pts_[i][1] = P(cx - 6, cy + 3);
        arc_pts_[i][2] = P(cx, cy + 4);
        arc_pts_[i][3] = P(cx + 6, cy + 3);
        arc_pts_[i][4] = P(cx + 12, cy - 2);
        SetArc(i, 5);
    }

    void ChevronEye(int i, int cx, int cy, bool greater) {
        int s = greater ? 1 : -1;
        arc_pts_[i][0] = P(cx - 9 * s, cy - 9);
        arc_pts_[i][1] = P(cx + 9 * s, cy);
        arc_pts_[i][2] = P(cx - 9 * s, cy + 9);
        SetArc(i, 3);
    }

    void HeartEye(int i, int cx, int cy, int grow) {
        int r = 9 + grow / 2;
        for (int k = 0; k < 2; k++) {
            int lx = cx + (k ? 8 : -8);
            lv_obj_set_size(lobe_[i][k], r * 2, r * 2);
            lv_obj_set_pos(lobe_[i][k], lx - r, cy - 6 - r);
            Show(lobe_[i][k], true);
        }
        int top = cy - 2, half = 16 + grow;
        for (int k = 0; k < kHeartRows; k++) {
            int hw = half - (half * (k + 1)) / (kHeartRows + 1) + 1;
            lv_obj_set_size(heart_bar_[i][k], hw * 2, 2);
            lv_obj_set_pos(heart_bar_[i][k], cx - hw, top + k * 2);
            Show(heart_bar_[i][k], true);
        }
    }

    void Brow(int i, int x0, int y0, int x1, int y1, int x2, int y2) {
        brow_pts_[i][0] = P(x0, y0);
        brow_pts_[i][1] = P(x1, y1);
        brow_pts_[i][2] = P(x2, y2);
        lv_line_set_points(brow_[i], brow_pts_[i], 3);
        Show(brow_[i], true);
    }

    void Blush(int i, int cx, int dy, int dx) {
        int base = (i == 0) ? cx - 16 : cx + 4;
        for (int k = 0; k < 3; k++) {
            int x = base + k * 5 + dx;
            blush_pts_[i][k][0] = P(x, kCy + 22 + dy);
            blush_pts_[i][k][1] = P(x + 3, kCy + 17 + dy);
            lv_line_set_points(blush_[i][k], blush_pts_[i][k], 2);
            Show(blush_[i][k], true);
        }
    }

    void SetMouth(Mouth m, int dx, int dy) {
        Show(mouth_fill_, false);
        Show(mouth_cover_, false);
        Show(mouth_ring_, false);
        Show(mouth_line_, false);
        int x = kMouthX + dx, y = kMouthY + dy, n = 0;
        auto pt = [&](int ax, int ay) { mouth_pts_[n++] = P(x + ax, y + ay); };
        switch (m) {
            case kMSmile:    pt(-7, -1); pt(-4, 2); pt(0, 3); pt(4, 2); pt(7, -1); break;
            case kMSmileBig: pt(-9, -2); pt(-5, 2); pt(0, 4); pt(5, 2); pt(9, -2); break;
            case kMFrown:    pt(-6, 3); pt(-3, 0); pt(0, -1); pt(3, 0); pt(6, 3); break;
            case kMFlat:     pt(-4, 0); pt(4, 0); break;
            case kMWavy:     pt(-8, 0); pt(-5, -2); pt(-2, 1); pt(1, -2); pt(4, 1); pt(7, -1); break;
            case kMSmirk:    pt(-6, 1); pt(2, 1); pt(7, -2); break;
            case kMSmirkBig: pt(-5, 1); pt(4, 1); pt(10, -4); break;
            case kMAngry:    pt(-6, 2); pt(-2, -2); pt(6, 3); break;
            case kMOpen:
                Box(mouth_fill_, x - 7, y - 4, 14, 9, 5);
                Box(mouth_cover_, x - 8, y - 5, 16, 4, 0);
                return;
            case kMBig:
                Box(mouth_fill_, x - 9, y - 6, 18, 12, 6);
                Box(mouth_cover_, x - 10, y - 7, 20, 5, 0);
                return;
            case kMTalk:
                Box(mouth_fill_, x - 5, y - 3, 10, 7, 3);
                return;
            case kMTalkWide:
                Box(mouth_fill_, x - 7, y - 2, 14, 5, 2);
                return;
            case kMO:
            case kMOBig: {
                int d = (m == kMOBig) ? 12 : 9;
                lv_obj_set_size(mouth_ring_, d, d + 1);
                lv_obj_set_pos(mouth_ring_, x - d / 2, y - d / 2);
                Show(mouth_ring_, true);
                return;
            }
            case kMNone:
            default:
                return;
        }
        lv_line_set_points(mouth_line_, mouth_pts_, n);
        Show(mouth_line_, true);
    }

    // ---------- cuadro ----------
    struct EyeSpec {
        EyeShape shape = kEOpen;
        int cx = 0, cy = 0, w = 26, h = 34, lx = 0, ly = 0;
    };

    void Render() {
        if (face_ == nullptr || mode_ != 0) return;
        auto state = Application::GetInstance().GetDeviceState();

        Emotion emo = emotion_;
        bool asleep = state == kDeviceStateIdle && idle_ticks_ > 3600;  // 3 min sin uso
        if (asleep) emo = kSleepy;

        const int et = (int)emo_ticks_;
        bool listening = state == kDeviceStateListening;
        bool speaking = state == kDeviceStateSpeaking || state == kDeviceStateNotifying;
        bool talk = speaking && ((tick_ / 3) % 2 == 0);
        bool blinking = blink_left_ > 0;

        // Respiracion
        int breath = 0;
        if (state == kDeviceStateIdle) {
            int period = asleep ? 80 : 60;
            breath = (sinf(tick_ * 6.2831853f / period) > 0.3f) ? -1 : 0;
        }
        // Saltito (aplasta y estira)
        int bdh = 0, bdy = 0;
        if (bounce_left_ >= 7)      { bdh = -8; bdy = 3; }
        else if (bounce_left_ >= 5) { bdh = 5;  bdy = -3; }
        else if (bounce_left_ >= 3) { bdh = 2;  bdy = -1; }

        EyeSpec eye[2];
        for (int i = 0; i < 2; i++) {
            eye[i].cx = kEyeX[i] + look_x_ + shift_x_;
            eye[i].cy = kCy + look_y_ + breath + bdy + shift_y_;
        }
        Mouth mouth = kMSmile;
        int mdx = shift_x_, mdy = breath + shift_y_;
        // extras
        int brow_mode = 0;       // 0 nada, 1 triste, 2 sorpresa, 3 picaro (izq levantada)
        int brow_shake = 0;
        int lid_angry = 0;
        int tear_y = -1;
        int blush = 0, blush_dx = 0;
        int dots = 0;
        int glasses_y = 0, glint = -1;
        int heart_grow = 0;
        int zphase = -1;
        bool talk_override = true;

        switch (emo) {
            case kNeutral:
            default:
                mouth = kMSmile;
                if (moment_ == kMomSmile) {
                    for (auto& e : eye) e.shape = kEHappy;
                    mouth = kMOpen;
                } else if (moment_ == kMomPicaro) {
                    brow_mode = 3;
                    for (auto& e : eye) e.cy += 3;
                    eye[0].h = 34;                      // ojo de la ceja levantada
                    eye[1].h = 28;
                    mouth = kMSmirkBig;
                }
                if (listening) { for (auto& e : eye) { e.w += 2; e.h += 4; } mouth = kMSmileBig; }
                break;

            case kHappy: {
                // Rebota contento
                int hop = ((et / 4) % 2) ? -2 : 0;
                for (auto& e : eye) { e.shape = kEHappy; e.cy += hop; }
                mouth = ((et / 6) % 2) ? kMOpen : kMSmileBig;
                mdy += hop / 2;
                talk_override = false;
                if (speaking) mouth = talk ? kMOpen : kMSmileBig;
                break;
            }
            case kLaugh: {
                // Se sacude de la risa
                int shake = ((et / 2) % 2) ? -2 : 1;
                for (auto& e : eye) { e.shape = kEHappy; e.cy += shake; }
                mouth = ((et / 3) % 2) ? kMBig : kMOpen;
                mdy += shake;
                talk_override = false;
                break;
            }
            case kLoving: {
                // Corazones que laten y se mecen
                heart_grow = ((et % 20) < 3) ? 2 : 0;
                int sway = (int)lroundf(2.0f * sinf(et * 6.2831853f / 30));
                for (auto& e : eye) { e.shape = kEHeart; e.cx += sway; }
                mouth = kMSmile;
                break;
            }
            case kSad: {
                // Los ojos se caen de a poco, cejas que tiemblan, lagrima que cae
                int droop = et < 10 ? et / 2 : 5;
                for (auto& e : eye) { e.h = 34 - droop; e.cy += 2 + droop / 2; e.ly = 2; }
                brow_mode = 1;
                brow_shake = ((et / 3) % 2);
                tear_y = (et / 2) % 14;
                mouth = kMFrown;
                break;
            }
            case kAngry: {
                // Tiembla de bronca: rafagas de temblor
                int shake = 0;
                if (et < 16 || (et % 30) < 6) shake = (et % 2) ? 1 : -1;
                for (auto& e : eye) { e.h = 30; e.cy += 2; e.cx += shake; e.ly = 2; }
                lid_angry = 1;
                mouth = kMAngry;
                mdx += shake;
                break;
            }
            case kSurprised: {
                // Los ojos "explotan" y se acomodan; la boca late
                int pop = et < 3 ? 8 : (et < 6 ? 4 : 0);
                for (auto& e : eye) { e.w = 30 + pop; e.h = 30 + pop; e.cy -= pop / 2; }
                brow_mode = 2;
                for (auto& e : eye) e.cy += 3;
                mouth = ((et / 8) % 2) ? kMOBig : kMO;
                break;
            }
            case kThinking: {
                // Mira arriba a un costado y va moviendo la mirada; puntitos "..."
                float s = sinf(et * 6.2831853f / 40);
                for (auto& e : eye) {
                    e.h = 30;
                    e.cx += 2 + (int)lroundf(2.0f * s);
                    e.cy -= 2;
                    e.lx = 3;
                    e.ly = -2;
                }
                dots = (et / 6) % 4;
                mouth = kMFlat;
                mdx += (int)lroundf(3.0f * s);
                break;
            }
            case kWink: {
                // Guino que se repite + sonrisa de costado + ceja
                bool wink = (et % 50) < 18;
                if (wink) eye[1].shape = kEHappy;
                mouth = kMSmirkBig;
                if (wink) brow_mode = 3;
                break;
            }
            case kCool: {
                // Anteojos que bajan de golpe y un brillo que los cruza
                for (auto& e : eye) e.shape = kEGlasses;
                if (et < 6) glasses_y = -30 + et * 5;
                else if (et < 8) glasses_y = 2 - (et - 6);
                int g = (et - 8) % 45;
                if (et >= 8 && g < 16) glint = g;
                mouth = kMSmirkBig;
                break;
            }
            case kSleepy:
                for (auto& e : eye) { e.shape = kEClosed; e.cy += 4; }
                zphase = (int)((tick_ / 3) % 14);
                mouth = kMFlat;
                talk_override = false;
                break;

            case kSilly: {
                // > < saltando alternados
                bool alt = (et / 4) % 2;
                eye[0].shape = kEGreater;
                eye[1].shape = kELess;
                eye[0].cy += alt ? -2 : 1;
                eye[1].cy += alt ? 1 : -2;
                mouth = alt ? kMOpen : kMBig;
                talk_override = false;
                break;
            }
            case kConfused: {
                // Un ojo grande y otro chico que se van alternando
                bool swap = (et / 30) % 2;
                int small = swap ? 0 : 1;
                eye[small].w = 18;
                eye[small].h = 22;
                eye[small].cy += 3;
                eye[1 - small].cy -= 1;
                mouth = kMWavy;
                mdx += ((et / 5) % 2) ? 1 : -1;
                break;
            }
            case kEmbarrassed: {
                // Mira para abajo a un costado, cambia de lado; sonrojo que vibra
                int side = ((et / 25) % 2) ? 4 : -4;
                for (auto& e : eye) { e.h = 27; e.cx += side; e.cy -= 1; e.lx = side / 4; e.ly = 1; }
                blush = 1;
                blush_dx = ((et / 4) % 2);
                mouth = kMWavy;
                mdx += side / 2;
                break;
            }
        }

        // Saltito y apertura de ojos
        for (auto& e : eye) {
            if (e.shape == kEOpen) {
                e.h += bdh;
                if (open_anim_ > 6) e.shape = kEClosed;
                else if (open_anim_ > 0) e.h = e.h * (7 - open_anim_) / 7 + 4;
                if (blinking && !(emo == kSurprised && et < 30)) e.shape = kEClosed;
            }
        }
        // Hablando: ojos acompanan la boca
        if (talk) for (auto& e : eye) e.cy -= 1;
        if (talk_override && speaking) mouth = talk ? kMTalk : (emo == kNeutral ? kMSmile : mouth);

        // ---- clave del cuadro: si nada cambio, no se redibuja ----
        int key[48];
        int n = 0;
        key[n++] = emo;
        for (auto& e : eye) {
            key[n++] = e.shape; key[n++] = e.cx; key[n++] = e.cy; key[n++] = e.w;
            key[n++] = e.h; key[n++] = e.lx; key[n++] = e.ly;
        }
        key[n++] = mouth; key[n++] = mdx; key[n++] = mdy; key[n++] = brow_mode;
        key[n++] = brow_shake; key[n++] = lid_angry; key[n++] = tear_y; key[n++] = blush;
        key[n++] = blush_dx; key[n++] = dots; key[n++] = glasses_y; key[n++] = glint;
        key[n++] = heart_grow; key[n++] = zphase; key[n++] = twinkle_left_ > 0;
        key[n++] = breath;
        while (n < 48) key[n++] = 0;
        if (memcmp(key, last_frame_, sizeof(key)) == 0) return;
        memcpy(last_frame_, key, sizeof(key));

        // ---- dibujo ----
        HideAll();
        for (int i = 0; i < 2; i++) {
            const EyeSpec& e = eye[i];
            switch (e.shape) {
                case kEOpen:    OpenEye(i, e.cx, e.cy, e.w, e.h, e.lx, e.ly); break;
                case kEHappy:   HappyEye(i, e.cx, e.cy); break;
                case kEClosed:  ClosedEye(i, e.cx, e.cy + 4); break;
                case kEGreater: ChevronEye(i, e.cx, e.cy, true); break;
                case kELess:    ChevronEye(i, e.cx, e.cy, false); break;
                case kEHeart:   HeartEye(i, e.cx, e.cy, heart_grow); break;
                case kEGlasses: Box(eye_[i], e.cx - 17, e.cy - 7 + glasses_y, 34, 14, 4); break;
                default: break;
            }

            if (lid_angry && e.shape == kEOpen) {
                lid_pts_[i][0] = P(e.cx - 16, e.cy - 20 + (i ? 10 : 0));
                lid_pts_[i][1] = P(e.cx + 16, e.cy - 20 + (i ? 0 : 10));
                lv_line_set_points(lid_[i], lid_pts_[i], 2);
                Show(lid_[i], true);
            }
            int top = e.cy - e.h / 2;
            if (brow_mode == 1) {
                int s = brow_shake;
                if (i == 0) Brow(i, e.cx - 13, top - 3 + s, e.cx, top - 5 + s, e.cx + 13, top - 9 + s);
                else        Brow(i, e.cx - 13, top - 9 + s, e.cx, top - 5 + s, e.cx + 13, top - 3 + s);
            } else if (brow_mode == 2) {
                Brow(i, e.cx - 10, top - 4, e.cx, top - 7, e.cx + 10, top - 4);
            } else if (brow_mode == 3 && i == 0) {
                // Ceja picara levantada (arqueada)
                Brow(i, e.cx - 12, top - 3, e.cx - 2, top - 8, e.cx + 11, top - 5);
            }
            if (tear_y >= 0 && i == 1 && e.shape == kEOpen) {
                int ty = e.cy + 15 + tear_y * 3;
                if (ty < 60) Box(tear_[i], e.cx + 9, ty, 4, 7, 2);
            }
            if (blush) Blush(i, kEyeX[i] + shift_x_, breath + shift_y_, blush_dx);
        }

        if (emo == kCool) {
            Box(bridge_, eye[0].cx + 17, eye[0].cy - 4 + glasses_y, eye[1].cx - eye[0].cx - 34, 3, 0);
            if (glint >= 0) {
                // Brillo diagonal que cruza el lente izquierdo y despues el derecho
                int lens = glint < 8 ? 0 : 1;
                int gx = eye[lens].cx - 14 + (glint % 8) * 4;
                glint_pts_[0] = P(gx, eye[lens].cy + 5);
                glint_pts_[1] = P(gx + 5, eye[lens].cy - 5);
                lv_line_set_points(glint_, glint_pts_, 2);
                Show(glint_, true);
            }
        }
        for (int k = 0; k < 3; k++) {
            if (k < dots) Box(dot_[k], 112 + k * 5, 2, 3, 3, 1);
        }
        if (zphase >= 0 && zphase < 12) {
            int zx = 100 + zphase / 3, zy = 16 - zphase, sz = 5 + zphase / 4;
            z_pts_[0] = P(zx, zy);
            z_pts_[1] = P(zx + sz, zy);
            z_pts_[2] = P(zx, zy + sz);
            z_pts_[3] = P(zx + sz, zy + sz);
            lv_line_set_points(z_line_, z_pts_, 4);
            Show(z_line_, true);
        }
        SetMouth(mouth, mdx, mdy);
    }
};

#endif  // _FACE_DISPLAY_H_
