#ifndef _DIAG_LED_H_
#define _DIAG_LED_H_

// LED RGB de la placa (WS2812 en GPIO21) usado para diagnostico.
// No se ve desde afuera de la carcasa: es para cuando esta abierto en el banco.
//
//   Azul parpadeando lento   -> modo configuracion WiFi
//   Rojo parpadeando         -> sin WiFi
//   Amarillo fijo            -> WiFi OK, sin conexion MQTT con Home Assistant
//   Verde tenue              -> todo OK, buena senal WiFi
//   Naranja tenue            -> todo OK, pero senal WiFi debil (< -75 dBm)
//   Rojo fijo                -> error grave del firmware
//   Destello blanco          -> detecto el wake word / empezo una charla

#include "application.h"
#include "ha_bridge.h"

#include <led_strip.h>
#include <esp_timer.h>
#include <wifi_manager.h>

class DiagLed {
public:
    explicit DiagLed(gpio_num_t gpio) {
        led_strip_config_t strip_config = {};
        strip_config.strip_gpio_num = gpio;
        strip_config.max_leds = 1;
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;
        strip_config.led_model = LED_MODEL_WS2812;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.resolution_hz = 10 * 1000 * 1000;

        if (led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_) != ESP_OK) {
            strip_ = nullptr;
            return;
        }
        led_strip_clear(strip_);

        esp_timer_create_args_t args = {
            .callback = [](void* arg) { static_cast<DiagLed*>(arg)->Tick(); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "diag_led",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &timer_);
        esp_timer_start_periodic(timer_, kTickMs * 1000);
    }

private:
    static constexpr int kTickMs = 100;
    static constexpr int kBright = 30;          // 0-255, tenue
    static constexpr int kWeakRssi = -75;

    led_strip_handle_t strip_ = nullptr;
    esp_timer_handle_t timer_ = nullptr;
    uint32_t tick_ = 0;
    int flash_left_ = 0;
    DeviceState last_state_ = kDeviceStateUnknown;
    uint8_t last_r_ = 255, last_g_ = 255, last_b_ = 255;

    void Show(float r, float g, float b) {
        uint8_t R = (uint8_t)(r * kBright), G = (uint8_t)(g * kBright), B = (uint8_t)(b * kBright);
        if (R == last_r_ && G == last_g_ && B == last_b_) return;
        last_r_ = R; last_g_ = G; last_b_ = B;
        led_strip_set_pixel(strip_, 0, R, G, B);
        led_strip_refresh(strip_);
    }

    void Tick() {
        tick_++;
        bool blink = (tick_ / 3) % 2;          // ~300 ms
        bool blink_slow = (tick_ / 8) % 2;     // ~800 ms

        auto state = Application::GetInstance().GetDeviceState();
        if (last_state_ == kDeviceStateIdle && state != kDeviceStateIdle) {
            flash_left_ = 2;                   // ~200 ms
        }
        last_state_ = state;

        if (flash_left_ > 0) {
            flash_left_--;
            Show(1, 1, 1);
            return;
        }

        auto& wifi = WifiManager::GetInstance();
        if (state == kDeviceStateFatalError) {
            Show(1, 0, 0);
        } else if (state == kDeviceStateWifiConfiguring || wifi.IsConfigMode()) {
            Show(0, 0, blink_slow ? 1 : 0);
        } else if (!wifi.IsConnected()) {
            Show(blink ? 1 : 0, 0, 0);
        } else if (!HaBridge::Instance().IsConnected()) {
            Show(1, 0.7f, 0);
        } else if (wifi.GetRssi() < kWeakRssi) {
            Show(1, 0.35f, 0);
        } else {
            Show(0, 1, 0);
        }
    }
};

#endif  // _DIAG_LED_H_
