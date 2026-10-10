#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "face_display.h"
#include "emotion_led.h"
#include "ha_bridge.h"
#include "mic_gain.h"
#include "kira_controls.h"
#include "diag_led.h"
#include "end_chat.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_sh1106.h>
#include <cmath>

#define TAG "S3ZeroSalaBoard"

// INMP441 con procesamiento de entrada (16 kHz):
//  1) Pasa-altos de 2do orden (~120 Hz): saca la continua del INMP441 y el
//     exceso de graves (voces graves saturaban antes que las agudas).
//  2) Pasa-bajos eliptico (corte 4,8 kHz, -55 dB desde 5,5 kHz): elimina el
//     pitido de ~7 kHz (interferencia electrica) sin tocar la voz (< 4 kHz).
//  3) Ganancia ajustable desde Home Assistant (Sensibilidad microfono).
//  4) Control automatico de nivel: si la voz llega muy fuerte baja la
//     ganancia sola (ataque rapido, liberacion lenta), asi no satura aunque
//     la sensibilidad este alta o se hable cerca.
//  5) Limitador suave de respaldo para picos sueltos.
//  6) Silencio total si el microfono esta silenciado desde Home Assistant.
class BoostedMicCodec : public NoAudioCodecSimplex {
public:
    using NoAudioCodecSimplex::NoAudioCodecSimplex;

protected:
    int Read(int16_t* dest, int samples) override {
        int n = NoAudioCodecSimplex::Read(dest, samples);
        if (kira::Controls::Get().MicOff()) {
            for (int i = 0; i < n; i++) dest[i] = 0;
            return n;
        }
        const float gain = (float)mic_gain::Get();
        constexpr float kTarget = 10000.0f;   // pico maximo deseado
        constexpr float kAttack = 0.02f;      // ~3 ms
        constexpr float kRelease = 0.0002f;   // ~300 ms
        constexpr float kKnee = 16000.0f;
        constexpr float kMax = 32000.0f;
        constexpr float kRange = kMax - kKnee;
        for (int i = 0; i < n; i++) {
            float v = hp_.Process((float)dest[i]);
            for (auto& s : lp_) v = s.Process(v);
            v *= gain;

            float a = fabsf(v);
            env_ += (a > env_ ? kAttack : kRelease) * (a - env_);
            if (env_ > kTarget) v *= kTarget / env_;

            a = fabsf(v);
            if (a > kKnee) {
                float over = (a - kKnee) / kRange;
                a = kKnee + kRange * tanhf(over);
                v = (v < 0) ? -a : a;
            }
            dest[i] = (int16_t)v;
        }
        return n;
    }

private:
    // Biquad en forma directa II transpuesta
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float z1 = 0.0f, z2 = 0.0f;
        float Process(float x) {
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    // Butterworth 2do orden, 120 Hz @ 16 kHz
    Biquad hp_{0.96722728f, -1.93445457f, 0.96722728f, -1.93338023f, 0.9355289f};
    // Eliptico 6to orden, 4,8 kHz @ 16 kHz (0,5 dB ripple, 55 dB rechazo)
    Biquad lp_[3] = {
        {0.08324422f, 0.15684002f, 0.08324422f, -0.45987479f, 0.20966396f},
        {1.0f, 1.37536923f, 1.0f, 0.24528041f, 0.64996963f},
        {1.0f, 1.0962251f, 1.0f, 0.60636534f, 0.91242537f},
    };
    float env_ = 0.0f;
};

class S3ZeroSalaBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSh1106Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo inicializar la OLED");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        auto face = new FaceDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        face->SetPanel(panel_io_, panel_);
        display_ = face;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // Si dormia o tenia el microfono silenciado, el boton lo despierta
            auto& ctl = kira::Controls::Get();
            if (ctl.Sleep() || ctl.Mute()) {
                ctl.SetSleep(false);
                ctl.SetMute(false);
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        end_chat::Register();
    }

public:
    S3ZeroSalaBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        mic_gain::Load();
        kira::Controls::Get().Load();
        InitializeDisplayI2c();
        InitializeSh1106Display();
        InitializeButtons();
        InitializeTools();
        HaBridge::Instance().Start();
        static DiagLed diag_led(DIAG_LED_GPIO);
    }

    // Siempre enchufado: el WiFi nunca entra en ahorro de energia.
    // El ahorro sumaba demora a la primera respuesta y cortes de audio.
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }

    virtual Led* GetLed() override {
        static EmotionLed led(BUILTIN_LED_GPIO);
        EmotionLed::instance = &led;
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoostedMicCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(S3ZeroSalaBoard);
