#ifndef _FACE_DISPLAY_H_
#define _FACE_DISPLAY_H_

// Cara animada de Kira para OLED 128x64, estilo "companion": ojos solidos
// tipo pastilla que se deforman con suavidad (se aplastan, se estiran,
// parpadean, miran), boca chiquita en forma de medialuna y un detalle propio
// para cada emocion (nube con lluvia, vena de enojo, globo de pensamiento,
// "JA JA", corazones, ondas de sonido al escuchar, luna y zzz...).
//
// En reposo tiene detalles al azar: estrellitas que titilan, una estrella
// fugaz que sigue con la mirada, una mosca que lo distrae, silba con notas
// musicales, ojitos de gato, mirada picara...
//
// Todo se dibuja pixel a pixel en un canvas de 128x64 y cada forma pasa a la
// siguiente con una transicion (los ojos se cierran, cambian de forma y se
// vuelven a abrir), asi los gestos se ven fluidos.
//
// El texto original solo aparece cuando hace falta (WiFi, activacion, error).
// Tambien maneja el brillo (Home Assistant), el modo noche (brillo minimo),
// el modo dormir (pantalla apagada) y un corrimiento de 1 px cada 10 minutos
// para que la OLED no se "queme".

#include "display/oled_display.h"
#include "application.h"
#include "emotion_led.h"
#include "ha_bridge.h"
#include "config.h"
#include "kira_controls.h"
#include "listen_ready.h"

#include <esp_random.h>
#include <driver/gpio.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <utility>

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

        memset(out_, 255, sizeof(out_));
        canvas_ = lv_canvas_create(screen);
        lv_canvas_set_buffer(canvas_, out_, kW, kH, LV_COLOR_FORMAT_L8);
        lv_obj_set_pos(canvas_, 0, 0);
        lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);

        for (int i = 0; i < 2; i++) {
            cur_[i] = Base(i);
            cur_[i].h = 2;
            kind_[i] = want_[i] = kPill;
            scale_[i] = 1.0f;
        }
        mouth_ = {kMouthX, kMouthY, 12, 5};
        next_blink_ = 50 + Rand(40);
        next_moment_ = 250 + Rand(250);
        next_look_ = 60;

        timer_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<FaceDisplay*>(lv_timer_get_user_data(t))->Tick();
        }, kTickMs, this);

        ApplyMode();
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
            emo_t_ = 0;
            EndMoment();
            ClearParticles();
            if (e == kHappy || e == kSurprised || e == kLaugh || e == kSilly) bounce_t_ = 0;
        }
    }

private:
    // ---------------- tipos ----------------
    enum Emotion {
        kNeutral, kHappy, kLaugh, kLoving, kSad, kCrying, kAngry, kSurprised, kShocked,
        kThinking, kWink, kCool, kSleepy, kSilly, kConfused, kEmbarrassed
    };
    enum EyeKind { kPill, kClosed, kArcUp, kCat, kHeart, kChevron, kGlasses };
    enum MouthKind { kMNone, kMSmile, kMSmileBig, kMUwu, kMO, kMOpen, kMFlat, kMTeeth,
                     kMWavy, kMSmirk, kMFrown, kMTrap };
    enum Moment { kMomNone, kMomContent, kMomCat, kMomPicaro, kMomWow, kMomWhistle,
                  kMomStar, kMomFly };
    enum PartKind { kPartNone, kPartHeart, kPartTear, kPartZ, kPartSpark, kPartStreak,
                    kPartSpeck, kPartNote, kPartDrop, kPartPuff, kPartBang, kPartJa,
                    kPartSweat };

    struct Eye { float x, y, w, h, r, lin, lout; };
    struct Mouth { float x, y, w, h; };
    struct Particle { PartKind kind; float x, y, vx, vy; int life, age, size; };

    static constexpr int kW = 128;
    static constexpr int kH = 64;
    static constexpr int kTickMs = 40;          // 25 cuadros por segundo
    // La cara se disena en "unidades" y se agranda al dibujar (kS) para
    // ocupar mas pantalla, dejando los costados y arriba para los detalles
    static constexpr float kS = 1.25f;
    static constexpr float kCx = 64.0f;
    static constexpr float kCy = 30.0f;
    static constexpr float kEyeX[2] = {40.0f, 88.0f};
    static constexpr float kEyeY = 27.0f;
    static constexpr float kEyeW = 22.0f;
    static constexpr float kEyeH = 30.0f;
    static constexpr float kMouthX = 64.0f;
    static constexpr float kMouthY = 38.0f;
    static constexpr int kMaxParticles = 18;

    // ---------------- objetos LVGL ----------------
    lv_obj_t* original_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    alignas(4) uint8_t out_[kW * kH];   // L8: 0 = pixel encendido, 255 = apagado
    uint8_t fb_[kH][kW];                // 1 = encendido

    // ---------------- estado ----------------
    Emotion emotion_ = kNeutral;
    uint32_t tick_ = 0;
    uint32_t emo_t_ = 0;
    uint32_t idle_ticks_ = 0;
    int mode_ = -1;                     // 0 = cara, 1 = texto informativo
    DeviceState prev_state_ = kDeviceStateUnknown;

    Eye cur_[2] = {};
    Eye tgt_[2] = {};
    EyeKind kind_[2] = {kPill, kPill};
    EyeKind want_[2] = {kPill, kPill};
    float scale_[2] = {1.0f, 1.0f};
    float scale_tgt_[2] = {1.0f, 1.0f};
    Mouth mouth_ = {};
    Mouth mouth_tgt_ = {};
    MouthKind mouth_kind_ = kMSmile;
    float shake_x_ = 0, shake_y_ = 0;

    uint32_t next_blink_ = 60;
    int blink_left_ = 0;
    int pending_blink_ = 0;
    float look_x_ = 0, look_y_ = 0;
    float look_tx_ = 0, look_ty_ = 0;
    uint32_t next_look_ = 60, look_hold_until_ = 0;
    int bounce_t_ = -1;
    // Pirueta al sonar el "pop": sacude la cabeza inclinandose y queda contento
    static constexpr int kShakeTicks = 18;  // sacudida (~720 ms)
    static constexpr int kHappyTicks = 8;   // ojos ^^ al final (~320 ms)
    int spin_t_ = -1;
    uint32_t last_cue_ = 0;
    Moment moment_ = kMomNone;
    int moment_left_ = 0;
    int moment_t_ = 0;
    uint32_t next_moment_ = 300;
    uint32_t next_twinkle_ = 80;
    int last_pir_ = 0;
    int talk_left_ = 0;
    float talk_h_ = 4;
    float fly_x_ = 0, fly_y_ = 0;       // mosca (momento)
    float star_x_ = 0, star_y_ = 0;     // estrella fugaz (momento)

    // detalles pedidos por la pose actual
    int fx_hearts_ = 0, fx_tears_ = 0, fx_z_ = 0, fx_sparks_ = 0, fx_streaks_ = 0;
    int fx_specks_ = 0, fx_blush_ = 0, fx_question_ = 0, fx_glint_ = 0;
    int fx_cloud_ = 0, fx_vein_ = 0, fx_waves_ = 0, fx_moon_ = 0, fx_thought_ = 0;
    int fx_sweat_ = 0, fx_tongue_ = 0, fx_bang_ = 0, fx_ja_ = 0, fx_notes_ = 0;
    int fx_winkstar_ = 0;
    float glasses_drop_ = 0;
    Particle parts_[kMaxParticles] = {};

    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    int applied_contrast_ = -1;
    int applied_on_ = -1;
    int shift_x_ = 0, shift_y_ = 0;    // anti-quemado

    // ---------------- utilidades ----------------
    static int Rand(int n) { return (int)(esp_random() % (uint32_t)n); }
    static float RandF(float a, float b) { return a + (b - a) * (float)Rand(1000) / 1000.0f; }
    static float Clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
    static float Approach(float c, float t, float k) {
        float d = t - c;
        if (fabsf(d) < 0.05f) return t;
        return c + d * k;
    }
    static Eye Base(int i) { return {kEyeX[i], kEyeY, kEyeW, kEyeH, 8.0f, 0.0f, 0.0f}; }
    // de unidades de diseno a pixeles de pantalla
    static float TX(float x) { return kCx + (x - kCx) * kS; }
    static float TY(float y) { return kCy + (y - kCy) * kS; }

    static Emotion Parse(const char* e) {
        auto is = [&](const char* s) { return strcmp(e, s) == 0; };
        if (is("happy") || is("confident") || is("delicious") || is("relaxed")) return kHappy;
        if (is("laughing") || is("funny")) return kLaugh;
        if (is("loving") || is("kissy")) return kLoving;
        if (is("sad")) return kSad;
        if (is("crying")) return kCrying;
        if (is("angry")) return kAngry;
        if (is("surprised")) return kSurprised;
        if (is("shocked")) return kShocked;
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

    static void ShowObj(lv_obj_t* o, bool show) {
        if (o == nullptr) return;
        if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    void ApplyMode() {
        int mode = NeedsText(Application::GetInstance().GetDeviceState()) ? 1 : 0;
        if (mode == mode_) return;
        mode_ = mode;
        ShowObj(original_, mode == 1);
        ShowObj(status_, mode == 1);
        ShowObj(canvas_, mode == 0);
        if (mode == 0) OpenEyes();
    }

    // Los ojos arrancan cerrados y se abren solos con la animacion
    void OpenEyes() {
        for (int i = 0; i < 2; i++) {
            kind_[i] = kPill;
            cur_[i].h = 2;
        }
    }

    void EndMoment() {
        if (moment_ == kMomWhistle) {
            for (auto& p : parts_) if (p.kind == kPartNote) p.kind = kPartNone;
        }
        moment_ = kMomNone;
        moment_left_ = 0;
        look_tx_ = look_ty_ = 0;
    }

    void StartMoment(Moment m, int ticks) {
        moment_ = m;
        moment_left_ = ticks;
        moment_t_ = 0;
        // el LED acompana con un arcoiris y despues se apaga
        if (EmotionLed::instance != nullptr) {
            EmotionLed::instance->Rainbow(ticks * kTickMs);
        }
    }

    // Brillo y encendido de la OLED segun los controles de Home Assistant
    void ApplyPanel() {
        if (io_ == nullptr || panel_ == nullptr) return;
        auto& ctl = kira::Controls::Get();
        int on = ctl.Sleep() ? 0 : 1;
        if (on != applied_on_) {
            esp_lcd_panel_disp_on_off(panel_, on);
            applied_on_ = on;
            if (on) OpenEyes();
        }
        int contrast = ctl.Contrast();
        if (contrast != applied_contrast_) {
            uint8_t v = (uint8_t)contrast;
            esp_lcd_panel_io_tx_param(io_, 0x81, &v, 1);   // comando de contraste SH1106
            applied_contrast_ = contrast;
        }
    }

    // ================= reloj de animacion =================
    void Tick() {
        tick_++;
        emo_t_++;
        ApplyPanel();

        // Anti-quemado: cada 10 minutos corre toda la cara 1 px
        static const int kShift[5][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
        int slot = (int)((tick_ / 15000) % 5);
        shift_x_ = kShift[slot][0];
        shift_y_ = kShift[slot][1];

        auto& app = Application::GetInstance();
        if (kira::Controls::Get().Sleep()) {
            // Durmiendo: si lo llaman con "Ey Kira" se despierta solo.
            // Los avisos de Home Assistant no lo despiertan.
            auto st = app.GetDeviceState();
            if (st == kDeviceStateListening || st == kDeviceStateConnecting) {
                kira::Controls::Get().SetSleep(false);
            } else {
                ApplyMode();
                return;   // pantalla apagada, no hace falta animar
            }
        }

        // "Escuchando" recien cuando el audio ya sale (ver listen_ready.h)
        auto state = listen_ready::EffectiveState(app.GetDeviceState());
        idle_ticks_ = (state == kDeviceStateIdle) ? idle_ticks_ + 1 : 0;

        // Pirueta cuando suena el "pop" de "ya podes hablar"
        uint32_t cue = app.GetListenCue();
        if (cue != last_cue_) {
            last_cue_ = cue;
            spin_t_ = 0;
            bounce_t_ = -1;
            EndMoment();
        }

        // Saltito cuando empieza a escuchar (si no esta haciendo la pirueta)
        if (state == kDeviceStateListening && prev_state_ != kDeviceStateListening &&
            spin_t_ < 0) {
            bounce_t_ = 0;
            EndMoment();
        }
        // Al terminar una charla vuelve a neutral (la emocion quedaba pegada)
        if (state == kDeviceStateIdle && prev_state_ != kDeviceStateIdle &&
            prev_state_ != kDeviceStateUnknown && emotion_ != kNeutral) {
            emotion_ = kNeutral;
            emo_t_ = 0;
            ClearParticles();
        }
        prev_state_ = state;

        // Sensor de movimiento: te mira y se pone contento
        int pir = gpio_get_level(PIR_GPIO);
        if (pir && !last_pir_ && state == kDeviceStateIdle) {
            idle_ticks_ = 0;
            if (emotion_ == kNeutral) {
                StartMoment(kMomCat, 45);
                bounce_t_ = 0;
            }
            look_tx_ = Rand(2) ? 8 : -8;
            look_ty_ = 0;
            look_hold_until_ = tick_ + 25;
        }
        last_pir_ = pir;

        UpdateTimers(state);
        ApplyMode();
        if (mode_ != 0) return;

        Pose(state);
        Animate(state);
        UpdateParticles(state);
        Draw();
        Present();
    }

    void UpdateTimers(DeviceState state) {
        // Parpadeo (a veces doble)
        if (blink_left_ > 0) {
            blink_left_--;
        } else if (pending_blink_ > 0) {
            if (--pending_blink_ == 0) blink_left_ = 3;
        } else if (tick_ >= next_blink_) {
            blink_left_ = 3;
            if (Rand(5) == 0) pending_blink_ = 4;
            next_blink_ = tick_ + 70 + Rand(80);
        }

        // Momentos en reposo (cada 10 a 25 s)
        if (moment_left_ > 0) {
            moment_t_++;
            if (--moment_left_ == 0) EndMoment();
        } else if (state == kDeviceStateIdle && idle_ticks_ < 4500 &&
                   emotion_ == kNeutral && tick_ >= next_moment_) {
            int r = Rand(14);
            if (r < 2) {
                StartMoment(kMomContent, 50);      // -‿- contento
            } else if (r < 4) {
                StartMoment(kMomCat, 45);          // ojitos de gato con saltito
                bounce_t_ = 0;
            } else if (r < 6) {
                StartMoment(kMomPicaro, 55);       // reojo y sonrisa ladeada
                look_tx_ = Rand(2) ? 8 : -8;
                look_ty_ = 0;
                look_hold_until_ = tick_ + 55;
            } else if (r < 7) {
                StartMoment(kMomWow, 30);          // "¡uh!" ojos grandes
                bounce_t_ = 0;
            } else if (r < 9) {
                StartMoment(kMomWhistle, 90);      // silba con notitas
            } else if (r < 11) {
                StartMoment(kMomStar, 60);         // estrella fugaz
                star_x_ = -10;
                star_y_ = RandF(1, 2.5f);
            } else {
                StartMoment(kMomFly, 150);         // una mosca lo distrae
                fly_x_ = Rand(2) ? -4 : 132;
                fly_y_ = RandF(10, 50);
            }
            next_moment_ = tick_ + 250 + Rand(375);
        }

        // Mirar alrededor (solo en reposo y sin momento que mande la mirada)
        bool gaze_free = moment_ != kMomPicaro && moment_ != kMomStar && moment_ != kMomFly;
        if (state == kDeviceStateIdle && gaze_free) {
            if (tick_ >= next_look_ && tick_ >= look_hold_until_) {
                if (look_tx_ == 0 && look_ty_ == 0 && Rand(3) > 0) {
                    static const float xs[] = {-9, -6, 6, 9};
                    look_tx_ = xs[Rand(4)];
                    look_ty_ = (float)(Rand(3) - 1) * 3.0f;
                    look_hold_until_ = tick_ + 25 + Rand(40);
                } else {
                    look_tx_ = look_ty_ = 0;
                }
                next_look_ = tick_ + 40 + Rand(90);
            }
        } else if (state != kDeviceStateIdle) {
            look_tx_ = look_ty_ = 0;
        }

        // Estrella fugaz: cruza arriba y los ojos la siguen
        if (moment_ == kMomStar) {
            star_x_ += 3.2f;
            star_y_ += 0.05f;
            look_tx_ = Clamp((star_x_ - 64) / 6, -9, 9);
            look_ty_ = 0;
            if (star_x_ > 135 && moment_left_ > 12) moment_left_ = 12;
        }
        // Mosca: vuela dando vueltas y los ojos la persiguen
        if (moment_ == kMomFly) {
            float t = moment_t_ * 0.07f;
            float tx = 64 + 54 * sinf(t * 1.3f) * cosf(t * 0.4f);
            float ty = 30 + 24 * sinf(t * 2.1f + 1.0f);
            if (moment_left_ < 30) { tx = 140; ty = 4; }      // se va
            fly_x_ = Approach(fly_x_, tx + RandF(-2, 2), 0.18f);
            fly_y_ = Approach(fly_y_, ty + RandF(-2, 2), 0.18f);
            look_tx_ = Clamp((fly_x_ - 64) / 6, -9, 9);
            look_ty_ = Clamp((fly_y_ - 30) / 6, -4, 4);
        }

        // Movimiento de boca al hablar
        if (talk_left_ > 0) {
            talk_left_--;
        } else {
            static const float hs[] = {2, 4, 6, 8, 9, 5, 3};
            talk_h_ = hs[Rand(7)];
            talk_left_ = 2 + Rand(2);
        }

        if (bounce_t_ >= 0 && ++bounce_t_ > 12) bounce_t_ = -1;
        if (spin_t_ >= 0 && ++spin_t_ >= kShakeTicks + kHappyTicks) spin_t_ = -1;
    }

    // ================= pose: a donde tiene que ir cada parte =================
    void Pose(DeviceState state) {
        bool listening = state == kDeviceStateListening;
        bool speaking = state == kDeviceStateSpeaking || state == kDeviceStateNotifying;
        bool asleep = state == kDeviceStateIdle && idle_ticks_ > 4500;   // 3 min sin uso
        const int et = (int)emo_t_;

        Emotion emo = asleep ? kSleepy : emotion_;

        for (int i = 0; i < 2; i++) {
            tgt_[i] = Base(i);
            want_[i] = kPill;
            scale_tgt_[i] = 1.0f;
        }
        MouthKind mk = kMSmile;
        float mw = 12, mh = 5, mdx = 0, mdy = 0;
        float lx = look_tx_, ly = look_ty_;
        bool talk_ok = true;
        shake_x_ = shake_y_ = 0;
        fx_hearts_ = fx_tears_ = fx_z_ = fx_sparks_ = fx_streaks_ = 0;
        fx_specks_ = fx_blush_ = fx_question_ = fx_glint_ = 0;
        fx_cloud_ = fx_vein_ = fx_waves_ = fx_moon_ = fx_thought_ = 0;
        fx_sweat_ = fx_tongue_ = fx_bang_ = fx_ja_ = fx_notes_ = fx_winkstar_ = 0;
        float target_drop = 0;

        switch (emo) {
            case kNeutral:
            default:
                switch (moment_) {
                    case kMomContent:
                        want_[0] = want_[1] = kClosed;
                        mk = kMUwu; mw = 12; mh = 4;
                        break;
                    case kMomCat:
                        want_[0] = want_[1] = kCat;
                        mk = kMO; mw = 5; mh = 6;
                        fx_sparks_ = 1;
                        break;
                    case kMomPicaro: {
                        int far = look_tx_ > 0 ? 0 : 1;
                        tgt_[far].h = 24; tgt_[far].y += 3;
                        tgt_[1 - far].h = 31; tgt_[1 - far].y -= 1;
                        mk = kMSmirk; mw = 15; mh = 5;
                        break;
                    }
                    case kMomWow:
                        for (auto& e : tgt_) { e.h = 36; e.w = 23; }
                        mk = kMO; mw = 6; mh = 7;
                        break;
                    case kMomWhistle:
                        want_[0] = want_[1] = kClosed;
                        mk = kMO; mw = 4; mh = 4; mdx = 2;
                        fx_notes_ = 1;
                        break;
                    case kMomStar:
                        for (auto& e : tgt_) { e.h = 28; e.y += 1; }
                        mk = kMO; mw = 5; mh = 5;
                        break;
                    case kMomFly:
                        mk = (moment_left_ < 30) ? kMSmirk : kMFlat;
                        mw = (moment_left_ < 30) ? 15 : 6;
                        mh = 3;
                        break;
                    default:
                        break;
                }
                if (listening) {
                    for (auto& e : tgt_) { e.h = 34; e.w = 23; }
                    mk = kMO; mw = 6; mh = 6;
                    lx = ly = 0;
                    fx_waves_ = 1;
                }
                break;

            case kHappy: {
                // Ojitos de gato, saltitos y estrellitas
                want_[0] = want_[1] = kCat;
                float hop = ((et / 8) % 2) ? -2.0f : 0.0f;
                for (auto& e : tgt_) e.y += hop;
                mdy += hop;
                mk = kMSmileBig; mw = 16; mh = 7;
                fx_sparks_ = 2;
                lx = ly = 0;
                break;
            }
            case kLaugh: {
                // ^ ^, se sacude y salen "JA" por los costados
                want_[0] = want_[1] = kArcUp;
                shake_y_ = ((et / 2) % 2) ? -2.0f : 1.0f;
                mk = kMOpen; mw = 14; mh = ((et / 3) % 2) ? 12 : 9;
                talk_ok = false;
                fx_ja_ = 1;
                lx = ly = 0;
                break;
            }
            case kLoving: {
                // Corazones que laten y otros que salen flotando
                want_[0] = want_[1] = kHeart;
                float beat = ((et % 20) < 3) ? 1.15f : 1.0f;
                scale_tgt_[0] = scale_tgt_[1] = beat;
                float sway = 2.0f * sinf(et * 6.2831853f / 35);
                for (auto& e : tgt_) e.x += sway;
                mk = kMSmile; mw = 12; mh = 5;
                fx_hearts_ = 1;
                lx = ly = 0;
                break;
            }
            case kSad:
            case kCrying: {
                // Ojos caidos, nubecita con lluvia arriba, lagrimas
                for (auto& e : tgt_) { e.h = 24; e.w = 21; e.y += 4; e.lout = 0.45f; }
                lx = 0; ly = 3;
                mk = kMFrown; mw = 12; mh = 4; mdy = 3;
                fx_tears_ = emo == kCrying ? 2 : 1;
                fx_cloud_ = 1;
                break;
            }
            case kAngry: {
                fx_vein_ = 1;
                if (et < 18) {
                    // Primero aprieta los ojos > <
                    want_[0] = want_[1] = kChevron;
                    mk = kMFlat; mw = 8; mh = 3;
                    fx_specks_ = 1;
                    shake_x_ = (et % 2) ? 1.0f : -1.0f;
                } else {
                    // Ojos enojados, vapor, temblor y glitch a los costados
                    for (auto& e : tgt_) { e.h = 26; e.w = 24; e.y += 2; e.lin = 0.6f; }
                    bool rage = (et % 50) < 14;
                    mk = rage ? kMTeeth : kMTrap;
                    mw = rage ? 14 : 12; mh = rage ? 7 : 6;
                    if (rage || (et % 30) < 4) {
                        shake_x_ = (et % 2) ? 1.5f : -1.5f;
                        shake_y_ = (et % 3 == 0) ? 1.0f : 0.0f;
                    }
                    fx_streaks_ = rage ? 3 : 1;
                }
                lx = ly = 0;
                break;
            }
            case kSurprised:
            case kShocked: {
                // Ojos que "explotan" y se acomodan, "!" a los costados
                float pop = et < 4 ? 4.0f : 0.0f;
                for (auto& e : tgt_) { e.h = 36 + pop; e.w = 24 + pop / 2; }
                mk = kMOpen; mw = 10; mh = ((et / 8) % 2) ? 11 : 9; mdy = 2;
                if (emo == kShocked) {
                    if ((et % 20) < 8) shake_x_ = (et % 2) ? 1.0f : -1.0f;
                    fx_sweat_ = 1;
                }
                fx_bang_ = 1;
                lx = ly = 0;
                break;
            }
            case kThinking: {
                // Mira para arriba, globo de pensamiento con puntitos
                float s = sinf(et * 6.2831853f / 50);
                lx = -6 + 2 * s; ly = -3;
                tgt_[0].h = 28; tgt_[1].h = 26;
                mk = kMFlat; mw = 7; mh = 3; mdx = -4;
                fx_thought_ = 1;
                break;
            }
            case kWink: {
                // Guino que se repite con una estrellita
                bool wink = (et % 55) < 22;
                if (wink) want_[1] = kArcUp;
                fx_winkstar_ = wink && (et % 55) < 12;
                tgt_[0].h = 31;
                mk = kMSmirk; mw = 15; mh = 5;
                lx = ly = 0;
                break;
            }
            case kCool: {
                // Anteojos que caen de arriba con un brillo que los cruza
                want_[0] = want_[1] = kGlasses;
                mk = kMSmirk; mw = 15; mh = 5;
                fx_glint_ = 1;
                fx_sparks_ = 1;
                lx = ly = 0;
                break;
            }
            case kSleepy: {
                want_[0] = want_[1] = kClosed;
                for (auto& e : tgt_) e.y += 4;
                mk = kMO; mw = 4; mh = ((tick_ / 25) % 2) ? 5 : 4; mdy = 3;
                fx_z_ = 1;
                fx_moon_ = 1;
                talk_ok = false;
                lx = ly = 0;
                break;
            }
            case kSilly: {
                // > < saltando alternados y la lengua afuera
                want_[0] = want_[1] = kChevron;
                bool alt = (et / 5) % 2;
                tgt_[0].y += alt ? -3 : 1;
                tgt_[1].y += alt ? 1 : -3;
                mk = kMSmileBig; mw = 16; mh = 6;
                fx_tongue_ = 1;
                talk_ok = false;
                lx = ly = 0;
                break;
            }
            case kConfused: {
                // Un ojo grande y otro chico que se alternan, signos de pregunta
                int small = ((et / 30) % 2) ? 0 : 1;
                tgt_[small].h = 20; tgt_[small].w = 18; tgt_[small].y += 3;
                tgt_[1 - small].h = 32; tgt_[1 - small].y -= 1;
                mk = kMWavy; mw = 12; mh = 3;
                fx_question_ = 1;
                lx = ly = 0;
                break;
            }
            case kEmbarrassed: {
                // Mira abajo a un costado, sonrojo y gotita de transpiracion
                float side = ((et / 30) % 2) ? 5.0f : -5.0f;
                lx = side; ly = 3;
                for (auto& e : tgt_) { e.h = 25; }
                mk = kMWavy; mw = 9; mh = 2;
                fx_blush_ = 1;
                fx_sweat_ = 1;
                break;
            }
        }

        // Escuchando (con cualquier emocion): ondas de sonido a los costados
        if (listening) fx_waves_ = 1;

        if (want_[0] == kGlasses) target_drop = 1;
        glasses_drop_ = Approach(glasses_drop_, target_drop, 0.3f);

        // Hablando: la boca se mueve
        if (speaking && talk_ok) {
            if (mk == kMTeeth || mk == kMTrap) {
                mh = 3 + talk_h_ * 0.6f;
            } else {
                mk = kMOpen;
                mh = talk_h_ + 2;
                mw = 9 + talk_h_ * 0.35f;
            }
        }

        // Mirada: los ojos y la boca se corren; el ojo del lado al que mira
        // se agranda un poco (da sensacion de giro)
        look_x_ = Approach(look_x_, lx, 0.35f);
        look_y_ = Approach(look_y_, ly, 0.35f);
        for (int i = 0; i < 2; i++) {
            tgt_[i].x += look_x_;
            tgt_[i].y += look_y_;
            float side = (i == 0) ? -look_x_ : look_x_;   // >0 si mira hacia este ojo
            tgt_[i].h += side * 0.3f;
            tgt_[i].w += side * 0.12f;
        }
        mdx += look_x_ * 0.7f;
        mdy += look_y_ * 0.6f;

        // Respiracion lenta en reposo
        if (state == kDeviceStateIdle) {
            float period = asleep ? 100.0f : 75.0f;
            float b = sinf(tick_ * 6.2831853f / period);
            for (auto& e : tgt_) e.y += b * 0.8f;
            mdy += b * 0.8f;
        }

        // Saltito: se aplasta, se estira y se acomoda
        if (bounce_t_ >= 0) {
            float dh = 0, dw = 0, dy = 0;
            if (bounce_t_ < 3)      { dh = -8; dw = 4; dy = 3; }
            else if (bounce_t_ < 7) { dh = 4;  dw = -2; dy = -3; }
            for (auto& e : tgt_) { e.h += dh; e.w += dw; e.y += dy; }
            mdy += dy;
        }

        // Pirueta "sacudida": la cabeza va y viene de lado a lado inclinandose
        // (un ojo sube y el otro baja), cada vez mas suave, y termina con ^^
        if (spin_t_ >= 0) {
            const int t = spin_t_;
            if (t < kShakeTicks) {
                float decay = 1.0f - t / (float)kShakeTicks;
                float sw = sinf(t * 1.3f) * decay;
                for (int i = 0; i < 2; i++) {
                    tgt_[i].x += 7.0f * sw;
                    tgt_[i].y += (i == 0 ? 4.0f : -4.0f) * sw;
                }
                mdx += 7.0f * sw;
            } else {
                want_[0] = want_[1] = kArcUp;
                mk = kMSmile; mw = 15; mh = 6;
            }
        }

        // Parpadeo (solo con ojos normales)
        if (blink_left_ > 0) {
            for (int i = 0; i < 2; i++) {
                if (kind_[i] == kPill && want_[i] == kPill && emo != kSurprised &&
                    emo != kShocked) {
                    tgt_[i].y += tgt_[i].h * 0.25f;
                    tgt_[i].h = 2;
                }
            }
        }

        mouth_kind_ = mk;
        mouth_tgt_ = {kMouthX + mdx, kMouthY + mdy, mw, mh};
    }

    // ================= interpolacion y cambios de forma =================
    void Animate(DeviceState state) {
        for (int i = 0; i < 2; i++) {
            Eye& c = cur_[i];
            const Eye& t = tgt_[i];
            bool closing = want_[i] != kind_[i] && kind_[i] == kPill;
            float kh = (blink_left_ > 0 || c.h < 6) ? 0.6f : 0.38f;
            c.x = Approach(c.x, t.x, 0.38f);
            c.w = Approach(c.w, t.w, 0.38f);
            if (!closing) {
                c.y = Approach(c.y, t.y, 0.38f);
                c.h = Approach(c.h, t.h, kh);
            }
            c.r = Approach(c.r, t.r, 0.38f);
            c.lin = Approach(c.lin, t.lin, 0.3f);
            c.lout = Approach(c.lout, t.lout, 0.3f);

            // Cambio de forma: el ojo se cierra, cambia y se vuelve a abrir
            if (want_[i] != kind_[i]) {
                if (kind_[i] == kPill) {
                    c.h = Approach(c.h, 1.0f, 0.55f);
                    c.y = Approach(c.y, t.y + 4, 0.4f);
                    if (c.h < 5) {
                        kind_[i] = want_[i];
                        scale_[i] = 0.35f;
                    }
                } else {
                    scale_[i] = Approach(scale_[i], 0.0f, 0.5f);
                    if (scale_[i] < 0.4f) {
                        kind_[i] = want_[i];
                        if (kind_[i] == kPill) c.h = 3;
                        else scale_[i] = 0.35f;
                    }
                }
            } else if (kind_[i] != kPill) {
                scale_[i] = Approach(scale_[i], scale_tgt_[i], 0.4f);
            }
        }
        Mouth& m = mouth_;
        float km = (state == kDeviceStateSpeaking) ? 0.6f : 0.4f;
        m.x = Approach(m.x, mouth_tgt_.x, 0.38f);
        m.y = Approach(m.y, mouth_tgt_.y, 0.38f);
        m.w = Approach(m.w, mouth_tgt_.w, km);
        m.h = Approach(m.h, mouth_tgt_.h, km);
    }

    // ================= particulas =================
    void ClearParticles() {
        for (auto& p : parts_) p.kind = kPartNone;
    }

    void Spawn(PartKind k, float x, float y, float vx, float vy, int life, int size) {
        for (auto& p : parts_) {
            if (p.kind == kPartNone) {
                p = {k, x, y, vx, vy, life, 0, size};
                return;
            }
        }
    }

    bool Has(PartKind k) const {
        for (auto& p : parts_) if (p.kind == k) return true;
        return false;
    }

    // Lugar libre al azar en los bordes (afuera de los ojos)
    void EdgeSpot(float& x, float& y) {
        int r = Rand(3);
        if (r == 0)      { x = RandF(3, 13);   y = RandF(5, 58); }
        else if (r == 1) { x = RandF(115, 125); y = RandF(5, 58); }
        else             { x = RandF(52, 76);  y = RandF(2, 7); }
    }

    void UpdateParticles(DeviceState state) {
        const int et = (int)emo_t_;
        if (fx_hearts_ && (et % 9) == 0) {
            // salen de abajo por los costados y entre los ojos
            static const float xs[] = {6, 13, 64, 115, 122};
            int k = Rand(5);
            float x = xs[k] + RandF(-2, 2);
            float y = (k == 2) ? 26.0f : 62.0f;
            Spawn(kPartHeart, x, y, RandF(-0.2f, 0.2f), RandF(-1.3f, -0.8f), 60, 4 + Rand(4));
        }
        if (fx_tears_ && (et % (fx_tears_ > 1 ? 10 : 28)) == 0) {
            int i = (fx_tears_ > 1) ? Rand(2) : 1;
            const Eye& e = cur_[i];
            float tx = TX(e.x + (i == 0 ? -e.w * 0.3f : e.w * 0.3f));
            Spawn(kPartTear, tx, TY(e.y + e.h * 0.5f) + 2, 0, 0.6f, 40, 3);
        }
        if (fx_cloud_ && (et % 5) == 0) {
            Spawn(kPartDrop, RandF(54, 74), 12, 0, 1.4f, 7, 3);
        }
        if (fx_z_ && (tick_ % 30) == 0) {
            Spawn(kPartZ, 108, 18, 0.35f, -0.45f, 40, 4);
        }
        if (fx_sparks_ && (et % (fx_sparks_ > 1 ? 7 : 14)) == 0) {
            float x, y;
            EdgeSpot(x, y);
            Spawn(kPartSpark, x, y, 0, 0, 12, 2 + Rand(2));
        }
        if (fx_streaks_ && (et % 2) == 0) {
            for (int k = 0; k < fx_streaks_; k++) {
                float x = Rand(2) ? RandF(1, 11) : RandF(117, 127);
                Spawn(kPartStreak, x, RandF(0, 40), 0, 0, 2, 8 + Rand(24));
            }
        }
        if (fx_specks_ && (et % 3) == 0) {
            float x = Rand(2) ? RandF(6, 16) : RandF(112, 122);
            Spawn(kPartSpeck, x, RandF(34, 48), 0, 0.3f, 6, 2);
        }
        if (fx_vein_ && et >= 18 && (et % 14) == 0) {
            // vapor que sale de la cabeza
            Spawn(kPartPuff, Rand(2) ? 8.0f : 120.0f, 22, 0, -0.9f, 16, 2);
        }
        if (fx_bang_ && (et % 22) == 0) {
            bool left = (et / 22) % 2;
            Spawn(kPartBang, left ? 9.0f : 119.0f, 14, 0, 0, 18, 6);
        }
        if (fx_ja_ && (et % 10) == 0) {
            bool left = (et / 10) % 2;
            Spawn(kPartJa, left ? RandF(2, 6) : RandF(110, 114), RandF(36, 50), 0, -0.7f, 22, 4);
        }
        if (fx_notes_ && (moment_t_ % 18) == 6) {
            Spawn(kPartNote, TX(mouth_.x) + 10, TY(mouth_.y) + 5, 1.3f, -0.25f, 40, 4);
        }
        if (fx_sweat_ && !Has(kPartSweat)) {
            Spawn(kPartSweat, 118, 8, 0, 0.12f, 70, 3);
        }
        // Reposo: estrellitas al azar que titilan en los bordes
        if (state == kDeviceStateIdle && emotion_ == kNeutral && moment_ == kMomNone &&
            tick_ >= next_twinkle_) {
            float x, y;
            EdgeSpot(x, y);
            Spawn(kPartSpark, x, y, 0, 0, 14, 2 + Rand(2));
            next_twinkle_ = tick_ + 60 + Rand(140);
        }
        // Estrella fugaz: deja chispitas en la cola
        if (moment_ == kMomStar && star_x_ < 128 && (moment_t_ % 3) == 0) {
            Spawn(kPartSpark, star_x_ - 6, star_y_, 0, 0.2f, 8, 1);
        }

        for (auto& p : parts_) {
            if (p.kind == kPartNone) continue;
            p.age++;
            if (p.age >= p.life) { p.kind = kPartNone; continue; }
            if (p.kind == kPartTear) p.vy += 0.12f;
            if (p.kind == kPartHeart) p.vx = 0.4f * sinf((p.age + p.size) * 0.3f);
            if (p.kind == kPartNote) p.vy = -0.25f + 0.45f * sinf(p.age * 0.35f);
            if (p.kind == kPartSweat && p.age > 20) p.vy = 0.35f;
            p.x += p.vx;
            p.y += p.vy;
            if (p.y > kH + 8 || p.y < -12 || p.x < -10 || p.x > kW + 10) p.kind = kPartNone;
        }
    }

    // ================= dibujo pixel a pixel =================
    void Px(int x, int y, uint8_t v = 1) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        fb_[y][x] = v;
    }

    void Span(int y, float xa, float xb, uint8_t v = 1) {
        if (y < 0 || y >= kH) return;
        int a = (int)lroundf(xa), b = (int)lroundf(xb);
        if (a < 0) a = 0;
        if (b > kW - 1) b = kW - 1;
        for (int x = a; x <= b; x++) fb_[y][x] = v;
    }

    // Rectangulo con esquinas redondeadas, centrado en (cx, cy)
    void RoundRect(float cx, float cy, float w, float h, float r, uint8_t v = 1) {
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        float top = cy - h / 2, bot = cy + h / 2;
        int y0 = (int)floorf(top + 0.5f), y1 = (int)ceilf(bot - 0.5f) - 1;
        if (y1 < y0) y1 = y0;
        for (int y = y0; y <= y1; y++) {
            float yc = y + 0.5f;
            float d = fminf(yc - top, bot - yc);   // distancia al borde de arriba/abajo
            float inset = 0;
            if (d < r) {
                float q = r - d;
                inset = r - sqrtf(fmaxf(r * r - q * q, 0));
            }
            Span(y, cx - w / 2 + inset, cx + w / 2 - inset - 1, v);
        }
    }

    void Ellipse(float cx, float cy, float rx, float ry, uint8_t v = 1) {
        if (rx < 0.5f || ry < 0.5f) { Px((int)lroundf(cx), (int)lroundf(cy), v); return; }
        int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
        for (int y = y0; y <= y1; y++) {
            float dy = (y - cy) / ry;
            if (fabsf(dy) > 1) continue;
            float dx = rx * sqrtf(1 - dy * dy);
            Span(y, cx - dx, cx + dx, v);
        }
    }

    void Ring(float cx, float cy, float rx, float ry, float t) {
        Ellipse(cx, cy, rx, ry, 1);
        if (rx > t && ry > t) Ellipse(cx, cy, rx - t, ry - t, 0);
    }

    // Linea gruesa (sellando circulitos a lo largo)
    void Line(float x0, float y0, float x1, float y1, float rad, uint8_t v = 1) {
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        int steps = (int)(len * 2) + 1;
        for (int s = 0; s <= steps; s++) {
            float t = (float)s / steps;
            Ellipse(x0 + dx * t, y0 + dy * t, rad, rad, v);
        }
    }

    // Borra todo lo que quede por encima de la recta (x0,y0)-(x1,y1) entre x0 y x1
    void ClearAbove(float x0, float y0, float x1, float y1) {
        if (x1 < x0) { std::swap(x0, x1); std::swap(y0, y1); }
        for (int x = (int)floorf(x0) - 1; x <= (int)ceilf(x1) + 1; x++) {
            if (x < 0 || x >= kW) continue;
            float t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0;
            t = fminf(fmaxf(t, 0), 1);
            int ylim = (int)lroundf(y0 + (y1 - y0) * t);
            for (int y = 0; y < ylim && y < kH; y++) fb_[y][x] = 0;
        }
    }

    void Heart(float cx, float cy, float size, uint8_t v = 1) {
        float s = size / 2.0f;
        if (s < 1) return;
        int x0 = (int)floorf(cx - s * 1.2f), x1 = (int)ceilf(cx + s * 1.2f);
        int y0 = (int)floorf(cy - s * 1.1f), y1 = (int)ceilf(cy + s * 1.2f);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float X = (x + 0.5f - cx) / s * 1.15f;
                float Y = -(y + 0.5f - cy) / s * 1.15f + 0.3f;
                float a = X * X + Y * Y - 1;
                if (a * a * a - X * X * Y * Y * Y <= 0) Px(x, y, v);
            }
        }
    }

    // Curva tipo arco: y = cy + depth * (1 - u^2); depth > 0 = forma de U
    void Arc(float cx, float cy, float hw, float depth, float rad) {
        float px = 0, py = 0;
        for (int k = 0; k <= 12; k++) {
            float u = -1.0f + k / 6.0f;
            float x = cx + u * hw;
            float y = cy + depth * (1 - u * u);
            if (k > 0) {
                float r = rad * (0.65f + 0.35f * (1 - u * u));
                Line(px, py, x, y, r);
            }
            px = x; py = y;
        }
    }

    // Arco de circunferencia (ondas de sonido)
    void CircleArc(float cx, float cy, float r, float a0, float a1, float rad) {
        float px = 0, py = 0;
        for (int k = 0; k <= 8; k++) {
            float a = a0 + (a1 - a0) * k / 8.0f;
            float x = cx + r * cosf(a), y = cy + r * sinf(a);
            if (k > 0) Line(px, py, x, y, rad);
            px = x; py = y;
        }
    }

    // Estrellita de 4 puntas
    void Sparkle(float x, float y, float s) {
        Line(x - s, y, x + s, y, 0.5f);
        Line(x, y - s, x, y + s, 0.5f);
        if (s >= 2.5f) Ellipse(x, y, 1, 1);
    }

    void Cloud(float cx, float cy, float w) {
        Ellipse(cx - w * 0.22f, cy + 1, w * 0.26f, w * 0.18f);
        Ellipse(cx + w * 0.2f, cy + 1, w * 0.28f, w * 0.2f);
        Ellipse(cx - w * 0.02f, cy - w * 0.08f, w * 0.26f, w * 0.24f);
        RoundRect(cx, cy + w * 0.12f, w * 0.9f, w * 0.18f, 2);
    }

    // Letras chiquitas hechas con lineas
    void Glyph(char c, float x, float y, float s) {
        float r = 0.55f;
        switch (c) {
            case 'J':
                Line(x, y, x + s, y, r);
                Line(x + s * 0.7f, y, x + s * 0.7f, y + s * 1.5f, r);
                Line(x + s * 0.7f, y + s * 1.5f, x, y + s * 1.5f, r);
                Line(x, y + s * 1.5f, x, y + s * 1.1f, r);
                break;
            case 'A':
                Line(x, y + s * 1.6f, x + s * 0.55f, y, r);
                Line(x + s * 0.55f, y, x + s * 1.1f, y + s * 1.6f, r);
                Line(x + s * 0.25f, y + s * 0.95f, x + s * 0.85f, y + s * 0.95f, r);
                break;
            case '!':
                Line(x, y, x, y + s * 1.2f, 0.8f);
                Ellipse(x, y + s * 1.8f, 0.9f, 0.9f);
                break;
            case '?':
                Line(x - s * 0.5f, y + s * 0.2f, x - s * 0.3f, y - s * 0.4f, r + 0.15f);
                Line(x - s * 0.3f, y - s * 0.4f, x + s * 0.3f, y - s * 0.4f, r + 0.15f);
                Line(x + s * 0.3f, y - s * 0.4f, x + s * 0.5f, y + s * 0.1f, r + 0.15f);
                Line(x + s * 0.5f, y + s * 0.1f, x, y + s * 0.6f, r + 0.15f);
                Line(x, y + s * 0.6f, x, y + s * 0.9f, r + 0.15f);
                Ellipse(x, y + s * 1.4f, 0.9f, 0.9f);
                break;
            default:
                break;
        }
    }

    void DrawEye(int i) {
        const Eye& d = cur_[i];
        // a pixeles de pantalla
        Eye e = {TX(d.x), TY(d.y), d.w * kS, d.h * kS, d.r * kS, d.lin, d.lout};
        float s = scale_[i] * kS;
        float inner = (i == 0) ? 1.0f : -1.0f;   // hacia el centro de la cara
        switch (kind_[i]) {
            case kPill: {
                RoundRect(e.x, e.y, e.w, e.h, e.r);
                float top = e.y - e.h / 2;
                if (e.lin > 0.02f) {
                    // Parpado enojado: el lado de adentro baja
                    float xi = e.x + inner * (e.w / 2 + 1), xo = e.x - inner * (e.w / 2 + 1);
                    ClearAbove(xi, top + e.lin * e.h * 0.55f, xo, top - 1);
                }
                if (e.lout > 0.02f) {
                    // Triste: el lado de afuera baja
                    float xi = e.x + inner * (e.w / 2 + 1), xo = e.x - inner * (e.w / 2 + 1);
                    ClearAbove(xi, top - 1, xo, top + e.lout * e.h * 0.55f);
                }
                break;
            }
            case kClosed:   // ︶ contento / dormido
                Arc(e.x, e.y - 1, 11 * s, 4 * s, 2.0f);
                break;
            case kArcUp:    // ^ risa / guino
                Arc(e.x, e.y + 4 * kS, 11 * s, -7 * s, 2.0f);
                break;
            case kCat: {
                // Ojito de gato: cuenco con dos puntitas arriba
                float hw = 11.5f * s;
                for (int x = (int)floorf(e.x - hw); x <= (int)ceilf(e.x + hw); x++) {
                    float u = (x + 0.5f - e.x) / hw;
                    if (fabsf(u) > 1) continue;
                    float au = fabsf(u);
                    float top = e.y + 1 - 9 * s * powf(au, 1.6f);
                    float bot = e.y + 1 + 8 * s * sqrtf(fmaxf(1 - au * au, 0));
                    for (int y = (int)lroundf(top); y <= (int)lroundf(bot); y++) Px(x, y);
                }
                break;
            }
            case kHeart:
                Heart(e.x, e.y + 1, 25 * s);
                break;
            case kChevron: {
                float w = 7 * s, h = 8 * s;
                float dd = inner;   // la punta apunta hacia el centro
                Line(e.x - dd * w, e.y - h, e.x + dd * w, e.y, 1.9f);
                Line(e.x + dd * w, e.y, e.x - dd * w, e.y + h, 1.9f);
                break;
            }
            case kGlasses: {
                float drop = (1 - glasses_drop_) * -50;
                float gy = e.y + drop;
                RoundRect(e.x, gy, 30 * s, 15 * s, 5);
                if (fx_glint_ && i == 0 && glasses_drop_ > 0.95f) {
                    int g = (int)(emo_t_ % 50);
                    if (g < 20) {
                        int lens = g < 10 ? 0 : 1;
                        float gx = TX(cur_[lens].x) - 17 + (g % 10) * 3.6f;
                        Line(gx, gy + 6, gx + 6, gy - 6, 0.9f, 0);
                    }
                }
                if (i == 1 && kind_[0] == kGlasses) {
                    // puente entre los lentes
                    Line(TX(cur_[0].x) + 17, gy - 5, e.x - 17, gy - 5, 1.2f);
                }
                break;
            }
        }
    }

    void DrawMouth() {
        Mouth m = {TX(mouth_.x), TY(mouth_.y), mouth_.w * kS, mouth_.h * kS};
        float hw = m.w / 2;
        switch (mouth_kind_) {
            case kMSmile:
            case kMSmileBig:
                // Medialuna: gruesa al medio y finita en las puntas
                for (int x = (int)floorf(m.x - hw); x <= (int)ceilf(m.x + hw); x++) {
                    float u = (x + 0.5f - m.x) / hw;
                    if (fabsf(u) > 1) continue;
                    float k = sqrtf(fmaxf(1 - u * u, 0));
                    float top = m.y + m.h * 0.42f * (1 - u * u);
                    float bot = m.y + m.h * k;
                    int a = (int)lroundf(top), b = (int)lroundf(bot);
                    if (b < a) b = a;
                    for (int y = a; y <= b; y++) Px(x, y);
                }
                break;
            case kMFrown:
                for (int x = (int)floorf(m.x - hw); x <= (int)ceilf(m.x + hw); x++) {
                    float u = (x + 0.5f - m.x) / hw;
                    if (fabsf(u) > 1) continue;
                    float k = sqrtf(fmaxf(1 - u * u, 0));
                    float bot = m.y + m.h - m.h * 0.42f * (1 - u * u);
                    float top = m.y + m.h - m.h * k;
                    int a = (int)lroundf(top), b = (int)lroundf(bot);
                    if (b < a) b = a;
                    for (int y = a; y <= b; y++) Px(x, y);
                }
                break;
            case kMUwu:
                // -‿- con las puntitas levantadas
                Arc(m.x, m.y, hw, m.h * 0.7f, 1.1f);
                Line(m.x - hw, m.y, m.x - hw - 1, m.y - 2, 0.7f);
                Line(m.x + hw, m.y, m.x + hw + 1, m.y - 2, 0.7f);
                break;
            case kMO:
                Ring(m.x, m.y + m.h / 2, m.w / 2, m.h / 2, 1.8f);
                break;
            case kMOpen: {
                // Boca abierta: contorno con la lengua abajo
                float top = m.y - 1;
                RoundRect(m.x, top + m.h / 2, m.w, m.h, 3);
                if (m.h > 6 && m.w > 7) {
                    int y0 = (int)lroundf(top + 2), y1 = (int)lroundf(top + m.h * 0.5f);
                    for (int y = y0; y <= y1; y++) Span(y, m.x - hw + 2, m.x + hw - 3, 0);
                }
                break;
            }
            case kMFlat:
                RoundRect(m.x, m.y + 1, m.w, 3, 1);
                break;
            case kMTeeth:
                // "=" dientes apretados
                RoundRect(m.x, m.y + 1, m.w, 3, 1);
                RoundRect(m.x, m.y + 2 + m.h * 0.5f, m.w, 3, 1);
                break;
            case kMTrap: {
                // Trapecio: mas angosto arriba
                int y0 = (int)lroundf(m.y), y1 = (int)lroundf(m.y + m.h);
                for (int y = y0; y <= y1; y++) {
                    float t = (y1 > y0) ? (float)(y - y0) / (y1 - y0) : 1;
                    float w = m.w * (0.55f + 0.45f * t);
                    Span(y, m.x - w / 2, m.x + w / 2 - 1);
                }
                break;
            }
            case kMWavy: {
                float px = 0, py = 0;
                for (int k = 0; k <= 10; k++) {
                    float x = m.x - hw + m.w * k / 10.0f;
                    float y = m.y + 2 + sinf(k * 3.14159f / 2.5f + emo_t_ * 0.25f) * 1.5f;
                    if (k > 0) Line(px, py, x, y, 0.8f);
                    px = x; py = y;
                }
                break;
            }
            case kMSmirk:
                // Sonrisa ladeada: plana a la izquierda, sube a la derecha
                Line(m.x - hw, m.y + 2, m.x + hw * 0.2f, m.y + 4, 1.3f);
                Line(m.x + hw * 0.2f, m.y + 4, m.x + hw, m.y - 1.5f, 1.3f);
                break;
            case kMNone:
            default:
                break;
        }
        // Lengua afuera (payaso)
        if (fx_tongue_) {
            float tx = m.x + 3, ty = m.y + m.h * 0.75f;
            Ellipse(tx, ty + 3, 4, 4);
            Line(tx, ty + 1, tx, ty + 5, 0.4f, 0);
        }
    }

    void DrawParticles() {
        for (auto& p : parts_) {
            switch (p.kind) {
                case kPartHeart:
                    if (p.life - p.age < 8 && (p.age % 2)) break;   // titila al final
                    Heart(p.x, p.y, (float)p.size);
                    break;
                case kPartTear:
                    Ellipse(p.x, p.y + 1, 1.8f, 2.2f);
                    Px((int)lroundf(p.x), (int)lroundf(p.y - 2));
                    break;
                case kPartDrop:
                    Line(p.x, p.y, p.x - 0.5f, p.y + 2, 0.5f);
                    break;
                case kPartZ: {
                    float s = p.size + p.age / 10.0f;
                    Line(p.x, p.y, p.x + s, p.y, 0.55f);
                    Line(p.x + s, p.y, p.x, p.y + s, 0.55f);
                    Line(p.x, p.y + s, p.x + s, p.y + s, 0.55f);
                    break;
                }
                case kPartSpark: {
                    // estrellita que crece y se achica
                    int a = p.age < p.life / 2 ? p.age : p.life - p.age;
                    Sparkle(p.x, p.y, p.size * (0.4f + a / (float)(p.life / 2)));
                    break;
                }
                case kPartStreak: {
                    int x = (int)p.x;
                    for (int y = (int)p.y; y < (int)p.y + p.size; y++) {
                        Px(x, y);
                        if ((y % 7) < 3) Px(x + 1, y);
                    }
                    break;
                }
                case kPartSpeck:
                    Line(p.x, p.y, p.x + 2, p.y, 0.5f);
                    break;
                case kPartPuff: {
                    float r = 1.5f + p.age * 0.25f;
                    Ring(p.x, p.y, r, r, 1.0f);
                    break;
                }
                case kPartBang: {
                    // "!" que aparece con un saltito
                    float s = p.age < 3 ? 6.0f : 4.5f;
                    Glyph('!', p.x, p.y - s, s);
                    break;
                }
                case kPartJa:
                    if (p.life - p.age < 5 && (p.age % 2)) break;
                    Glyph('J', p.x, p.y, (float)p.size);
                    Glyph('A', p.x + p.size + 2, p.y - 0.4f, (float)p.size);
                    break;
                case kPartNote: {
                    // corchea: cabeza, palito y banderita
                    Ellipse(p.x, p.y + 4, 1.8f, 1.4f);
                    Line(p.x + 1.5f, p.y + 4, p.x + 1.5f, p.y - 2, 0.5f);
                    Line(p.x + 1.5f, p.y - 2, p.x + 4, p.y, 0.5f);
                    break;
                }
                case kPartSweat:
                    Ellipse(p.x, p.y + 2, 2.0f, 2.4f);
                    Line(p.x, p.y + 1, p.x, p.y - 2, 0.6f);
                    Px((int)p.x, (int)p.y + 1, 0);   // brillito
                    break;
                case kPartNone:
                default:
                    break;
            }
        }
    }

    void DrawOverlays(DeviceState state) {
        const int et = (int)emo_t_;
        // Nubecita triste arriba (de ahi sale la lluvia)
        if (fx_cloud_) {
            float dx = 2.5f * sinf(et * 0.05f);
            Cloud(64 + dx, 6, 26);
        }
        // Vena de enojo que late en la esquina
        if (fx_vein_) {
            float k = ((et / 6) % 2) ? 1.3f : 1.0f;
            float cx = 116, cy = 9;
            for (int sx = -1; sx <= 1; sx += 2) {
                for (int sy = -1; sy <= 1; sy += 2) {
                    Line(cx + sx * 1.5f, cy + sy * 5 * k, cx + sx * 1.5f, cy + sy * 1.5f, 0.7f);
                    Line(cx + sx * 1.5f, cy + sy * 1.5f, cx + sx * 5 * k, cy + sy * 1.5f, 0.7f);
                }
            }
        }
        // Ondas de sonido a los costados mientras escucha
        if (fx_waves_) {
            bool voice = Application::GetInstance().IsVoiceDetected();
            int n = voice ? 3 : 1 + (int)((tick_ / 6) % 3);
            for (int k = 0; k < n; k++) {
                float r = 9 + k * 4.5f;
                CircleArc(105, 30, r, -0.7f, 0.7f, 0.6f);
                CircleArc(23, 30, r, 3.14159f - 0.7f, 3.14159f + 0.7f, 0.6f);
            }
        }
        // Luna y estrellitas cuando duerme
        if (fx_moon_) {
            Ellipse(12, 10, 6, 6);
            Ellipse(15, 8, 5.5f, 5.5f, 0);
            if ((tick_ / 20) % 3 != 0) Sparkle(24, 5, 2);
            if ((tick_ / 27) % 3 != 1) Sparkle(5, 22, 1.5f);
        }
        // Globo de pensamiento con puntitos que aparecen de a uno
        if (fx_thought_) {
            if (et > 4) Ellipse(112, 24, 1.5f, 1.5f);
            if (et > 9) Ellipse(116, 17, 2.2f, 2.2f);
            if (et > 14) {
                Cloud(116, 6, 22);
                int dots = (et / 7) % 4;
                for (int k = 0; k < dots; k++) Ellipse(110.0f + k * 5, 7, 1.2f, 1.2f, 0);
            }
        }
        // Sonrojo /// debajo de los ojos
        if (fx_blush_) {
            int jit = (et / 4) % 2;
            for (int i = 0; i < 2; i++) {
                float bx = TX(cur_[i].x) - 9 + jit;
                float by = TY(cur_[i].y + cur_[i].h / 2) + 5;
                for (int k = 0; k < 3; k++) {
                    Line(bx + k * 6, by + 3, bx + k * 6 + 3, by - 1, 0.55f);
                }
            }
        }
        // Signos de pregunta que van y vienen
        if (fx_question_) {
            int ph = (et / 12) % 4;
            if (ph != 3) Glyph('?', 116, 8, 6);
            if (ph != 1) Glyph('?', 11, 12, 5);
        }
        // Estrellita del guino
        if (fx_winkstar_) {
            float s = 2.0f + (et % 55) * 0.25f;
            if (s > 4.5f) s = 4.5f;
            Sparkle(TX(cur_[1].x) + 16, TY(cur_[1].y) - 14, s);
        }
        // Momentos del reposo
        if (moment_ == kMomStar && star_x_ < 134) {
            // estrella fugaz con cola
            Line(star_x_ - 14, star_y_ - 1.1f, star_x_, star_y_, 0.5f);
            Sparkle(star_x_, star_y_, 2.5f);
        }
        if (moment_ == kMomFly) {
            // mosca: cuerpito y alitas que aletean (con un borde apagado
            // alrededor para que se vea aunque pase por encima de un ojo)
            Ellipse(fly_x_, fly_y_ - 0.5f, 3.5f, 3.2f, 0);
            Ellipse(fly_x_, fly_y_, 1.5f, 1.2f);
            if ((tick_ % 2) == 0) {
                Px((int)fly_x_ - 1, (int)fly_y_ - 2);
                Px((int)fly_x_ + 1, (int)fly_y_ - 2);
            } else {
                Px((int)fly_x_ - 2, (int)fly_y_ - 1);
                Px((int)fly_x_ + 2, (int)fly_y_ - 1);
            }
        }
        (void)state;
    }

    void Draw(DeviceState state = kDeviceStateIdle) {
        memset(fb_, 0, sizeof(fb_));
        // sacudida (enojo, risa) mueve toda la cara
        float sx = shake_x_ / kS, sy = shake_y_ / kS;
        for (auto& e : cur_) { e.x += sx; e.y += sy; }
        mouth_.x += sx; mouth_.y += sy;

        DrawEye(0);
        DrawEye(1);
        DrawMouth();

        for (auto& e : cur_) { e.x -= sx; e.y -= sy; }
        mouth_.x -= sx; mouth_.y -= sy;

        DrawParticles();
        DrawOverlays(state);
    }

    // Pasa el cuadro al canvas y solo invalida la zona que cambio
    void Present() {
        int x0 = kW, y0 = kH, x1 = -1, y1 = -1;
        for (int y = 0; y < kH; y++) {
            int sy = y - shift_y_;
            for (int x = 0; x < kW; x++) {
                int sx = x - shift_x_;
                bool on = sx >= 0 && sy >= 0 && sx < kW && sy < kH && fb_[sy][sx];
                uint8_t v = on ? 0 : 255;
                uint8_t& o = out_[y * kW + x];
                if (o != v) {
                    o = v;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
        }
        if (x1 < 0 || canvas_ == nullptr) return;
        lv_area_t area = {(int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1};
        lv_obj_invalidate_area(canvas_, &area);
    }
};

#endif  // _FACE_DISPLAY_H_
