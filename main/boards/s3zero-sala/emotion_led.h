#ifndef _EMOTION_LED_H_
#define _EMOTION_LED_H_

// LED WS2812 que sigue el estado del asistente y el color de la emocion,
// igual que en el YAML de ESPHome: escuchando = azul pulsante,
// hablando = color segun emocion, reposo = apagado.
// Modo dormir y modo noche: apagado. Microfono silenciado: rojo tenue.
// En reposo, cuando la cara hace un "momento" (silbar, estrella fugaz,
// mosca...), el LED hace un arcoiris que se prende y se apaga suave.

#include "led/led.h"
#include "application.h"
#include "config.h"
#include "kira_controls.h"

#include <led_strip.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <cmath>
#include <cstring>
#include <mutex>
#include <atomic>

class EmotionLed : public Led {
public:
    explicit EmotionLed(gpio_num_t gpio) {
        led_strip_config_t strip_config = {};
        strip_config.strip_gpio_num = gpio;
        strip_config.max_leds = 1;
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        strip_config.led_model = LED_MODEL_WS2812;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.resolution_hz = 10 * 1000 * 1000;

        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_));
        led_strip_clear(strip_);

        esp_timer_create_args_t args = {
            .callback = [](void* arg) { static_cast<EmotionLed*>(arg)->Tick(); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "emotion_led",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 30 * 1000));
    }

    static EmotionLed* instance;

    // Llamado por la pantalla cuando llega una emocion del servidor
    void SetEmotion(const char* emotion) {
        float r = 0.0f, g = 1.0f, b = 0.0f;  // neutral: verde
        auto is = [&](const char* e) { return strcmp(emotion, e) == 0; };
        if (is("angry")) { r = 1.0f; g = 0.0f; b = 0.0f; }
        else if (is("embarrassed")) { r = 1.0f; g = 0.2f; b = 0.6f; }
        else if (is("laughing") || is("funny") || is("silly")) { r = 1.0f; g = 0.8f; b = 0.0f; }
        else if (is("loving") || is("kissy")) { r = 1.0f; g = 0.0f; b = 0.6f; }
        else if (is("confused") || is("thinking")) { r = 1.0f; g = 0.5f; b = 0.0f; }
        else if (is("sad") || is("crying")) { r = 0.7f; g = 0.0f; b = 1.0f; }
        else if (is("happy") || is("relaxed") || is("confident") || is("delicious") || is("cool")) {
            r = 0.0f; g = 1.0f; b = 0.8f;
        }
        else if (is("surprised") || is("shocked")) { r = 1.0f; g = 1.0f; b = 1.0f; }
        else if (is("sleepy")) { r = 0.0f; g = 0.0f; b = 0.4f; }
        else if (is("winking")) { r = 0.0f; g = 1.0f; b = 0.8f; }

        std::lock_guard<std::mutex> lock(mutex_);
        emo_r_ = r; emo_g_ = g; emo_b_ = b;
    }

    // Arcoiris por un rato (lo pide la cara en los momentos del reposo)
    void Rainbow(int duration_ms) {
        int64_t now = esp_timer_get_time();
        rainbow_start_ = now;
        rainbow_end_ = now + (int64_t)duration_ms * 1000;
    }

    void OnStateChanged() override {
        // El color se recalcula en cada Tick segun el estado actual
    }

private:
    led_strip_handle_t strip_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    std::mutex mutex_;
    float emo_r_ = 0.0f, emo_g_ = 1.0f, emo_b_ = 0.0f;
    uint32_t tick_ = 0;
    uint8_t last_r_ = 255, last_g_ = 255, last_b_ = 255;
    std::atomic<int64_t> rainbow_start_{0};
    std::atomic<int64_t> rainbow_end_{0};

    // Color del arcoiris (h de 0 a 1)
    static void Hue(float h, float& r, float& g, float& b) {
        h = h - floorf(h);
        float x = h * 6.0f;
        int i = (int)x;
        float f = x - i;
        switch (i % 6) {
            case 0: r = 1; g = f; b = 0; break;
            case 1: r = 1 - f; g = 1; b = 0; break;
            case 2: r = 0; g = 1; b = f; break;
            case 3: r = 0; g = 1 - f; b = 1; break;
            case 4: r = f; g = 0; b = 1; break;
            default: r = 1; g = 0; b = 1 - f; break;
        }
    }

    // Devuelve true si esta mostrando el arcoiris
    bool ShowRainbow() {
        int64_t now = esp_timer_get_time();
        int64_t start = rainbow_start_.load(), end = rainbow_end_.load();
        if (now >= end || end <= start) return false;
        float total = (float)(end - start);
        float t = (float)(now - start) / total;          // 0..1
        float env = 1.0f;
        float fade = 400000.0f / total;                   // 0,4 s de fundido
        if (t < fade) env = t / fade;
        else if (t > 1 - fade) env = (1 - t) / fade;
        float r, g, b;
        Hue((float)(now - start) / 2000000.0f, r, g, b);  // una vuelta cada 2 s
        Show(r, g, b, 0.7f * env);
        return true;
    }

    void Show(float r, float g, float b, float level) {
        auto clamp = [](float v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
        uint8_t R = (uint8_t)(clamp(r * level) * LED_MAX_BRIGHTNESS);
        uint8_t G = (uint8_t)(clamp(g * level) * LED_MAX_BRIGHTNESS);
        uint8_t B = (uint8_t)(clamp(b * level) * LED_MAX_BRIGHTNESS);
        if (R == last_r_ && G == last_g_ && B == last_b_) {
            return;
        }
        last_r_ = R; last_g_ = G; last_b_ = B;
        if (R == 0 && G == 0 && B == 0) {
            led_strip_clear(strip_);
        } else {
            led_strip_set_pixel(strip_, 0, R, G, B);
            led_strip_refresh(strip_);
        }
    }

    void Tick() {
        tick_++;
        auto& app = Application::GetInstance();
        float t = tick_ * 0.030f;  // segundos
        float pulse = 0.25f + 0.75f * (0.5f + 0.5f * sinf(t * 6.2831853f));  // 1 s
        bool blink_fast = (tick_ / 4) % 2;   // ~120 ms
        bool blink_slow = (tick_ / 17) % 2;  // ~500 ms

        float r, g, b;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            r = emo_r_; g = emo_g_; b = emo_b_;
        }

        auto& ctl = kira::Controls::Get();
        auto state = app.GetDeviceState();
        bool busy = state == kDeviceStateSpeaking || state == kDeviceStateNotifying ||
                    state == kDeviceStateListening;
        if (ctl.Sleep() || (ctl.Night() && !busy)) {
            Show(0, 0, 0, 0);
            return;
        }
        if (ctl.Mute() && state == kDeviceStateIdle) {
            Show(1, 0, 0, 0.15f);
            return;
        }

        switch (state) {
            case kDeviceStateStarting:
                Show(0, 0, 1, blink_fast ? 1.0f : 0.0f);
                break;
            case kDeviceStateWifiConfiguring:
                Show(0, 0, 1, blink_slow ? 1.0f : 0.0f);
                break;
            case kDeviceStateActivating:
            case kDeviceStateUpgrading:
                Show(0, 1, 0, blink_slow ? 1.0f : 0.0f);
                break;
            case kDeviceStateConnecting:
                Show(0, 0, 1, 1.0f);
                break;
            case kDeviceStateListening:
            case kDeviceStateAudioTesting:
                // Azul pulsante; mas brillante cuando detecta voz
                Show(0, 0, 1, app.IsVoiceDetected() ? 1.0f : pulse * 0.6f);
                break;
            case kDeviceStateSpeaking:
            case kDeviceStateNotifying:
                // Color de la emocion, respirando suave
                Show(r, g, b, 0.55f + 0.45f * pulse);
                break;
            case kDeviceStateFatalError:
                Show(1, 0, 0, blink_slow ? 1.0f : 0.0f);
                break;
            case kDeviceStateIdle:
            default:
                if (!ShowRainbow()) Show(0, 0, 0, 0);
                break;
        }
    }
};

inline EmotionLed* EmotionLed::instance = nullptr;

#endif  // _EMOTION_LED_H_
