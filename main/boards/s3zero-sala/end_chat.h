#ifndef _END_CHAT_H_
#define _END_CHAT_H_

// Herramienta MCP "self.chat.end": cuando el usuario se despide o agradece
// ("chau", "gracias", "listo"), el modelo la llama y Kira cierra la charla
// apenas termina de decir su ultima frase, en vez de quedarse escuchando.
//
// El cierre se hace desde un esp_timer: espera a que Kira deje de hablar
// (estado escuchando) y ahi hace ToggleChatState(), que en ese estado cierra
// el canal de audio y vuelve al reposo. Si en 20 s no pasa, se cancela.

#include "application.h"
#include "mcp_server.h"

#include <esp_timer.h>
#include <esp_log.h>
#include <atomic>

namespace end_chat {

inline std::atomic<bool> pending{false};
inline std::atomic<int64_t> requested_us{0};
inline esp_timer_handle_t timer = nullptr;

inline void Check() {
    if (!pending.load()) return;
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    int64_t elapsed = esp_timer_get_time() - requested_us.load();
    if (state == kDeviceStateIdle || elapsed > 20 * 1000 * 1000) {
        pending = false;    // ya termino solo o se vencio
        return;
    }
    // Cerrar cuando termino de hablar (paso a escuchar), con 300 ms de margen
    if (state == kDeviceStateListening && elapsed > 300 * 1000) {
        pending = false;
        ESP_LOGI("EndChat", "Fin de la charla pedido por el modelo");
        app.ToggleChatState();
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
            pending = true;
            return "{\"ok\": true}";
        });
}

}  // namespace end_chat

#endif  // _END_CHAT_H_
