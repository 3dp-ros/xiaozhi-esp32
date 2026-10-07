#ifndef _HA_BRIDGE_H_
#define _HA_BRIDGE_H_

// Puente MQTT con Home Assistant (autodescubrimiento).
//
// Entidades que aparecen en HA (dispositivo "Kira Sala"):
//   binary_sensor  Movimiento   (PIR)
//   sensor         Estado       (reposo, escuchando, hablando...)
//   sensor         Emocion      (ultima emocion de Kira)
//   number         Volumen      (0-100)
//   number         Sensibilidad microfono (1-12, se guarda en la placa)
//   number         Brillo pantalla (1-100 %)
//   number         Sensibilidad wake word (0 = original, 1-10 mas sensible)
//   switch         Modo dormir  (pantalla/LED apagados; "Ey Kira" lo despierta)
//   switch         Silenciar microfono (no escucha nada; se reactiva desde HA)
//   switch         Modo noche   (pantalla al minimo, LED apagado)
//   sensor         Senal WiFi   (dBm)
//   button         Escuchar     (activa a Kira como el wake word)
//   notify         Anunciar     (publica el texto en kira/sala/decir)
//
// Avisos por voz: Home Assistant genera el audio con su TTS (Ogg Opus) y
// publica {"url": "...", "text": "..."} en kira/sala/audio. El firmware lo
// reproduce con su reproductor de notificaciones. El paso texto -> audio lo
// hace el script "Kira decir" en HA (ver instrucciones).

#include "application.h"
#include "board.h"
#include "config.h"
#include "mic_gain.h"
#include "kira_controls.h"

#include <mqtt.h>
#include <wifi_manager.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <atomic>
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

    // Conexion MQTT con Home Assistant (para el LED de diagnostico)
    bool IsConnected() const { return mqtt_ok_.load(); }

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
    std::atomic<bool> mqtt_ok_{false};

    std::string emotion_ = "neutral";
    std::string pending_url_;
    std::string pending_text_;
    int pending_wait_ = 0;

    std::string last_state_;
    std::string last_emotion_;
    int last_motion_ = -1;
    int last_volume_ = -1;
    int last_mic_gain_ = -1;
    int last_bright_ = -1;
    int last_wake_ = -1;
    int last_sleep_ = -1, last_mute_ = -1, last_night_ = -1;
    int rssi_ticks_ = 0;

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
        pub("number", "microfono",
            "\"name\":\"Sensibilidad microfono\",\"icon\":\"mdi:microphone-settings\",\"min\":1,\"max\":12,\"step\":1,"
            "\"mode\":\"slider\",\"entity_category\":\"config\","
            "\"state_topic\":\"" + T("microfono") + "\",\"command_topic\":\"" + T("microfono/set") + "\"");
        pub("number", "brillo",
            "\"name\":\"Brillo pantalla\",\"icon\":\"mdi:brightness-6\",\"min\":1,\"max\":100,\"step\":1,"
            "\"unit_of_measurement\":\"%\",\"mode\":\"slider\",\"entity_category\":\"config\","
            "\"state_topic\":\"" + T("brillo") + "\",\"command_topic\":\"" + T("brillo/set") + "\"");
        pub("number", "wakeword",
            "\"name\":\"Sensibilidad wake word\",\"icon\":\"mdi:ear-hearing\",\"min\":0,\"max\":10,\"step\":1,"
            "\"mode\":\"slider\",\"entity_category\":\"config\","
            "\"state_topic\":\"" + T("wakeword") + "\",\"command_topic\":\"" + T("wakeword/set") + "\"");
        auto sw = [&](const char* obj, const char* name, const char* icon) {
            pub("switch", obj,
                std::string("\"name\":\"") + name + "\",\"icon\":\"" + icon + "\","
                "\"state_topic\":\"" + T(obj) + "\",\"command_topic\":\"" + T(obj) + "/set\"");
        };
        sw("dormir", "Modo dormir", "mdi:sleep");
        sw("silencio", "Silenciar microfono", "mdi:microphone-off");
        sw("noche", "Modo noche", "mdi:weather-night");
        pub("sensor", "wifi",
            "\"name\":\"Senal WiFi\",\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\","
            "\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\","
            "\"state_topic\":\"" + T("wifi") + "\"");
        pub("button", "escuchar",
            "\"name\":\"Escuchar\",\"icon\":\"mdi:microphone\",\"command_topic\":\"" + T("escuchar") + "\"");
        pub("notify", "anunciar",
            "\"name\":\"Anunciar\",\"icon\":\"mdi:bullhorn\",\"command_topic\":\"" + T("decir") + "\"");
    }

    void OnConnected() {
        ESP_LOGI(kTag, "Conectado al broker de Home Assistant");
        mqtt_->Subscribe(T("audio"), 1);
        mqtt_->Subscribe(T("escuchar"), 1);
        mqtt_->Subscribe(T("volumen/set"), 1);
        mqtt_->Subscribe(T("microfono/set"), 1);
        mqtt_->Subscribe(T("brillo/set"), 1);
        mqtt_->Subscribe(T("wakeword/set"), 1);
        mqtt_->Subscribe(T("dormir/set"), 1);
        mqtt_->Subscribe(T("silencio/set"), 1);
        mqtt_->Subscribe(T("noche/set"), 1);
        mqtt_->Subscribe("homeassistant/status", 1);
        PublishDiscovery();
        std::lock_guard<std::mutex> lock(mutex_);
        resend_all_ = true;
    }

    void OnMessage(const std::string& topic, const std::string& payload) {
        if (topic == T("audio")) {
            cJSON* root = cJSON_Parse(payload.c_str());
            if (root == nullptr) {
                ESP_LOGW(kTag, "Aviso con JSON invalido");
                return;
            }
            cJSON* url = cJSON_GetObjectItem(root, "url");
            cJSON* text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(url) && url->valuestring[0] != '\0') {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_url_ = url->valuestring;
                pending_text_ = cJSON_IsString(text) ? text->valuestring : "";
                pending_wait_ = 0;
                ESP_LOGI(kTag, "Aviso recibido: %s", pending_text_.c_str());
            }
            cJSON_Delete(root);
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
        } else if (topic == T("microfono/set")) {
            int gain = atoi(payload.c_str());
            mic_gain::Set(gain);
            ESP_LOGI(kTag, "Sensibilidad de microfono: %d", mic_gain::Get());
        } else if (topic == T("brillo/set")) {
            kira::Controls::Get().SetBrightness(atoi(payload.c_str()));
        } else if (topic == T("wakeword/set")) {
            kira::Controls::Get().SetWakeLevel(atoi(payload.c_str()));
            ESP_LOGI(kTag, "Sensibilidad wake word: %d", kira::Controls::Get().WakeLevel());
        } else if (topic == T("dormir/set")) {
            kira::Controls::Get().SetSleep(payload == "ON");
        } else if (topic == T("silencio/set")) {
            kira::Controls::Get().SetMute(payload == "ON");
        } else if (topic == T("noche/set")) {
            kira::Controls::Get().SetNight(payload == "ON");
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
        int gain = mic_gain::Get();
        if (all || gain != last_mic_gain_) {
            if (mqtt_->Publish(T("microfono"), std::to_string(gain))) last_mic_gain_ = gain;
        }
        auto& ctl = kira::Controls::Get();
        auto pub_flag = [&](const char* obj, bool v, int& last) {
            if (all || (int)v != last) {
                if (mqtt_->Publish(T(obj), v ? "ON" : "OFF")) last = v;
            }
        };
        pub_flag("dormir", ctl.Sleep(), last_sleep_);
        pub_flag("silencio", ctl.Mute(), last_mute_);
        pub_flag("noche", ctl.Night(), last_night_);
        int bright = ctl.Brightness();
        if (all || bright != last_bright_) {
            if (mqtt_->Publish(T("brillo"), std::to_string(bright))) last_bright_ = bright;
        }
        int wake = ctl.WakeLevel();
        if (all || wake != last_wake_) {
            if (mqtt_->Publish(T("wakeword"), std::to_string(wake))) last_wake_ = wake;
        }
        // Senal WiFi: cada ~30 s
        if (all || ++rssi_ticks_ >= 150) {
            rssi_ticks_ = 0;
            mqtt_->Publish(T("wifi"), std::to_string(WifiManager::GetInstance().GetRssi()));
        }
        int volume = Board::GetInstance().GetAudioCodec()->output_volume();
        if (all || volume != last_volume_) {
            if (mqtt_->Publish(T("volumen"), std::to_string(volume))) last_volume_ = volume;
        }
    }

    void ProcessPendingSay() {
        std::string url, text;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_url_.empty()) return;
            url = pending_url_;
            text = pending_text_;
        }
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            // Esperar a que termine lo que esta haciendo (hasta ~90 s)
            std::lock_guard<std::mutex> lock(mutex_);
            if (++pending_wait_ > 450) {
                ESP_LOGW(kTag, "Aviso descartado, Kira estuvo ocupada: %s", text.c_str());
                pending_url_.clear();
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_url_.clear();
        }
        ESP_LOGI(kTag, "Reproduciendo aviso: %s", text.c_str());
        app.PlayNotification(url, text);
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
            mqtt_ok_ = mqtt_->IsConnected();
            if (!mqtt_ok_) {
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
