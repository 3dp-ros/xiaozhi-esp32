#ifndef _HA_BRIDGE_H_
#define _HA_BRIDGE_H_

// Puente MQTT con Home Assistant (autodescubrimiento).
//
// Entidades que aparecen en HA (dispositivo "Kira Sala"):
//   binary_sensor  Movimiento   (PIR)
//   sensor         Estado       (reposo, escuchando, hablando...)
//   sensor         Emocion      (ultima emocion de Kira)
//   number         Volumen      (0-100)
//   button         Escuchar     (activa a Kira como el wake word)
//   notify         Anunciar     (Kira dice en voz alta el texto recibido)
//
// Para que Kira diga un aviso desde una automatizacion:
//   action: notify.send_message
//   target: { entity_id: notify.kira_sala_anunciar }
//   data:   { message: "Saca la jarra" }

#include "application.h"
#include "board.h"
#include "config.h"

#include <mqtt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

class HaBridge {
public:
    static HaBridge& Instance() {
        static HaBridge instance;
        return instance;
    }

    void Start() {
        if (started_) return;
        started_ = true;
        xTaskCreate([](void* arg) { static_cast<HaBridge*>(arg)->Loop(); },
                    "ha_bridge", 6144, this, 2, nullptr);
    }

    // Llamado por la pantalla cuando cambia la emocion
    void OnEmotion(const char* emotion) {
        std::lock_guard<std::mutex> lock(mutex_);
        emotion_ = emotion ? emotion : "neutral";
    }

private:
    static constexpr const char* kTag = "HaBridge";
    static constexpr const char* kBase = "kira/sala";

    std::unique_ptr<Mqtt> mqtt_;
    std::mutex mutex_;
    bool started_ = false;
    bool resend_all_ = true;

    std::string emotion_ = "neutral";
    std::string pending_say_;
    int pending_wait_ = 0;

    std::string last_state_;
    std::string last_emotion_;
    int last_motion_ = -1;
    int last_volume_ = -1;

    static std::string T(const char* suffix) { return std::string(kBase) + "/" + suffix; }

    static std::string Device() {
        return "\"device\":{\"identifiers\":[\"kira_sala\"],\"name\":\"Kira Sala\","
               "\"manufacturer\":\"DIY\",\"model\":\"ESP32-S3-Zero XiaoZhi\"}";
    }

    void PublishDiscovery() {
        auto pub = [&](const char* comp, const char* obj, const std::string& body) {
            std::string topic = std::string("homeassistant/") + comp + "/kira_sala/" + obj + "/config";
            std::string payload = "{" + body + ",\"unique_id\":\"kira_sala_" + obj + "\"," + Device() + "}";
            mqtt_->Publish(topic, payload, 1);
        };
        pub("binary_sensor", "movimiento",
            "\"name\":\"Movimiento\",\"device_class\":\"motion\",\"state_topic\":\"" + T("movimiento") + "\"");
        pub("sensor", "estado",
            "\"name\":\"Estado\",\"icon\":\"mdi:robot\",\"state_topic\":\"" + T("estado") + "\"");
        pub("sensor", "emocion",
            "\"name\":\"Emocion\",\"icon\":\"mdi:emoticon-outline\",\"state_topic\":\"" + T("emocion") + "\"");
        pub("number", "volumen",
            "\"name\":\"Volumen\",\"icon\":\"mdi:volume-high\",\"min\":0,\"max\":100,\"step\":5,"
            "\"state_topic\":\"" + T("volumen") + "\",\"command_topic\":\"" + T("volumen/set") + "\"");
        pub("button", "escuchar",
            "\"name\":\"Escuchar\",\"icon\":\"mdi:microphone\",\"command_topic\":\"" + T("escuchar") + "\"");
        pub("notify", "anunciar",
            "\"name\":\"Anunciar\",\"icon\":\"mdi:bullhorn\",\"command_topic\":\"" + T("decir") + "\"");
    }

    void OnConnected() {
        ESP_LOGI(kTag, "Conectado al broker de Home Assistant");
        mqtt_->Subscribe(T("decir"), 1);
        mqtt_->Subscribe(T("escuchar"), 1);
        mqtt_->Subscribe(T("volumen/set"), 1);
        mqtt_->Subscribe("homeassistant/status", 1);
        PublishDiscovery();
        std::lock_guard<std::mutex> lock(mutex_);
        resend_all_ = true;
    }

    static std::string Sanitize(const std::string& in) {
        // El texto viaja dentro de un JSON armado a mano: sin comillas,
        // barras ni saltos de linea, y con un largo razonable.
        std::string out;
        for (char c : in) {
            if (c == '"') out += '\'';
            else if (c == '\\' || c == '\n' || c == '\r' || c == '\t') out += ' ';
            else out += c;
            if (out.size() >= 220) break;
        }
        return out;
    }

    void OnMessage(const std::string& topic, const std::string& payload) {
        if (topic == T("decir")) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_say_ = Sanitize(payload);
            pending_wait_ = 0;
            ESP_LOGI(kTag, "Aviso recibido: %s", pending_say_.c_str());
        } else if (topic == T("escuchar")) {
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.ToggleChatState();
                }
            });
        } else if (topic == T("volumen/set")) {
            int vol = atoi(payload.c_str());
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            Application::GetInstance().Schedule([vol]() {
                Board::GetInstance().GetAudioCodec()->SetOutputVolume(vol);
            });
        } else if (topic == "homeassistant/status" && payload == "online") {
            // HA se reinicio: volver a anunciar las entidades y sus estados
            PublishDiscovery();
            std::lock_guard<std::mutex> lock(mutex_);
            resend_all_ = true;
        }
    }

    static const char* StateName(DeviceState s) {
        switch (s) {
            case kDeviceStateIdle: return "reposo";
            case kDeviceStateConnecting: return "conectando";
            case kDeviceStateListening: return "escuchando";
            case kDeviceStateSpeaking: return "hablando";
            case kDeviceStateNotifying: return "avisando";
            case kDeviceStateWifiConfiguring: return "configurando wifi";
            case kDeviceStateActivating: return "activando";
            case kDeviceStateUpgrading: return "actualizando";
            case kDeviceStateFatalError: return "error";
            default: return "iniciando";
        }
    }

    static bool Online(DeviceState s) {
        return s == kDeviceStateIdle || s == kDeviceStateConnecting ||
               s == kDeviceStateListening || s == kDeviceStateSpeaking ||
               s == kDeviceStateNotifying;
    }

    void PublishStates() {
        auto& app = Application::GetInstance();
        bool all;
        std::string emotion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            all = resend_all_;
            resend_all_ = false;
            emotion = emotion_;
        }

        std::string state = StateName(app.GetDeviceState());
        if (all || state != last_state_) {
            if (mqtt_->Publish(T("estado"), state)) last_state_ = state;
        }
        int motion = gpio_get_level(PIR_GPIO);
        if (all || motion != last_motion_) {
            if (mqtt_->Publish(T("movimiento"), motion ? "ON" : "OFF")) last_motion_ = motion;
        }
        if (all || emotion != last_emotion_) {
            if (mqtt_->Publish(T("emocion"), emotion)) last_emotion_ = emotion;
        }
        int volume = Board::GetInstance().GetAudioCodec()->output_volume();
        if (all || volume != last_volume_) {
            if (mqtt_->Publish(T("volumen"), std::to_string(volume))) last_volume_ = volume;
        }
    }

    void ProcessPendingSay() {
        std::string text;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_say_.empty()) return;
            text = pending_say_;
        }
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            // Esperar a que termine lo que esta haciendo (hasta ~90 s)
            std::lock_guard<std::mutex> lock(mutex_);
            if (++pending_wait_ > 450) {
                ESP_LOGW(kTag, "Aviso descartado, Kira estuvo ocupada: %s", text.c_str());
                pending_say_.clear();
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_say_.clear();
        }
        std::string prompt =
            "Aviso automatico de la casa, no es una pregunta de David. "
            "Decile en voz alta, en una sola frase breve y sin preguntar nada: " + text;
        ESP_LOGI(kTag, "Anunciando: %s", text.c_str());
        app.WakeWordInvoke(prompt);
    }

    void Loop() {
        if (std::string(HA_MQTT_HOST).find('X') != std::string::npos) {
            ESP_LOGW(kTag, "HA_MQTT_HOST sin configurar en config.h, puente MQTT desactivado");
            vTaskDelete(nullptr);
            return;
        }

        auto& app = Application::GetInstance();
        // Esperar a que el asistente este en linea (WiFi + activacion listos)
        while (!Online(app.GetDeviceState())) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        mqtt_ = Board::GetInstance().GetNetwork()->CreateMqtt(1);
        mqtt_->SetKeepAlive(60);
        mqtt_->OnConnected([this]() { OnConnected(); });
        mqtt_->OnMessage([this](const std::string& t, const std::string& p) { OnMessage(t, p); });

        while (true) {
            if (!mqtt_->IsConnected()) {
                auto result = mqtt_->Connect(HA_MQTT_HOST, HA_MQTT_PORT, "kira_sala",
                                             HA_MQTT_USER, HA_MQTT_PASS);
                if (!result) {
                    ESP_LOGW(kTag, "No se pudo conectar al broker %s:%d, reintento en 15 s",
                             HA_MQTT_HOST, HA_MQTT_PORT);
                    vTaskDelay(pdMS_TO_TICKS(15000));
                    continue;
                }
            }
            PublishStates();
            ProcessPendingSay();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
};

#endif  // _HA_BRIDGE_H_
