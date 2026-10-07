#ifndef _END_CHAT_H_
#define _END_CHAT_H_

// Herramienta MCP "self.chat.end": cuando el usuario se despide o agradece
// ("chau", "gracias", "listo"), el modelo la llama y Kira cierra la charla
// apenas termina de decir su ultima frase, en vez de quedarse escuchando.
//
// Un esp_timer revisa cada 100 ms: cuando ya no queda audio por reproducir
// (Kira termino de hablar) espera 0,6 s mas y cierra la charla con
// Application::CloseChat(), sin depender de que el servidor mande el fin del
// habla (a veces no lo manda y Kira quedaba trabado en "hablando").
// Si en 20 s no pasa, se cancela.

#include "application.h"
#include "mcp_server.h"

#include <esp_timer.h>
#include <esp_log.h>
#include <atomic>

namespace end_chat {

inline std::atomic<bool> pending{false};
inline std::atomic<int64_t> requested_us{0};
inline int64_t quiet_since_us = 0;
inline bool saw_audio = false;      // ya empezo a decir la despedida
inline esp_timer_handle_t timer = nullptr;

inline void Check() {
    if (!pending.load()) return;
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - requested_us.load();
    if (state == kDeviceStateIdle || elapsed > 20 * 1000 * 1000) {
        pending = false;    // ya termino solo o se vencio
        return;
    }
    bool quiet = state == kDeviceStateListening ||
                 (state == kDeviceStateSpeaking && app.GetAudioService().IsPlaybackIdle());
    if (!quiet) {
        if (state == kDeviceStateSpeaking) saw_audio = true;
        quiet_since_us = 0;
        return;
    }
    if (quiet_since_us == 0) quiet_since_us = now;
    // Cerrar 0,6 s despues de terminar la despedida. Si la herramienta llego
    // antes de que empiece a hablar, esperar hasta 3 s de silencio total.
    int64_t need = saw_audio ? 600 * 1000 : 3000 * 1000;
    if (now - quiet_since_us > need) {
        pending = false;
        quiet_since_us = 0;
        ESP_LOGI("EndChat", "Fin de la charla pedido por el modelo");
        app.CloseChat();
    }
}

inline void Register() {
    esp_timer_create_args_t args = {};
    args.callback = [](void*) { Check(); };
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "end_chat";
    args.skip_unhandled_events = true;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, 100 * 1000);
    }

    McpServer::GetInstance().AddTool("self.chat.end",
        "Termina la conversacion y deja de escuchar. Usala SIEMPRE que el usuario se "
        "despida, agradezca para cerrar o diga que no necesita nada mas (por ejemplo: "
        "chau, gracias, listo, nada mas, hasta luego). Primero despedite con una frase "
        "corta y despues llama a esta herramienta. Nunca digas la palabra 'end' en voz alta.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            requested_us = esp_timer_get_time();
            quiet_since_us = 0;
            saw_audio = false;
            pending = true;
            return "{\"ok\": true}";
        });
}

}  // namespace end_chat

#endif  // _END_CHAT_H_
