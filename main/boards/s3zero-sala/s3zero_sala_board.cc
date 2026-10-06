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

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_sh1106.h>
#include <cmath>

#define TAG "S3ZeroSalaBoard"

// INMP441 con procesamiento de entrada:
//  1) Filtro pasa-altos (~80 Hz): saca la continua del INMP441 y el zumbido
//     grave, que al amplificarse saturaban el audio.
//  2) Ganancia ajustable desde Home Assistant (Sensibilidad microfono).
//  3) Limitador suave: los picos se redondean en vez de cortarse de golpe,
//     asi la voz no suena "rota" aunque se hable fuerte o cerca.
//  4) Silencio total si el microfono esta silenciado desde Home Assistant.
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
        constexpr float kR = 0.969f;          // pasa-altos de 1er orden, ~80 Hz a 16 kHz
        constexpr float kKnee = 16000.0f;     // a partir de aca empieza a comprimir
        constexpr float kMax = 32000.0f;
        constexpr float kRange = kMax - kKnee;
        for (int i = 0; i < n; i++) {
            float x = (float)dest[i];
            float y = x - hp_x_ + kR * hp_y_;
            hp_x_ = x;
            hp_y_ = y;
            float v = y * gain;
            float a = fabsf(v);
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
    float hp_x_ = 0.0f;
    float hp_y_ = 0.0f;
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
        gpio_config_t pir_config = {
            .pin_bit_mask = (1ULL << PIR_GPIO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&pir_config));

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.sala.get_motion",
            "Indica si el sensor de movimiento del living detecta a alguien en este momento",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return gpio_get_level(PIR_GPIO) ? "{\"motion\": true}" : "{\"motion\": false}";
            });
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
