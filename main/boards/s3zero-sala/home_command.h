#ifndef _HOME_COMMAND_H_
#define _HOME_COMMAND_H_

// Herramienta MCP "self.home.command": las ordenes de la casa (prender,
// apagar, abrir, cerrar) no pasan por el modelo adivinando nombres de
// entidades. Kira le manda la frase tal cual a Home Assistant por MQTT
// (kira/sala/comando), HA la resuelve con su propio Assist local (areas,
// nombres y alias) y contesta en kira/sala/comando/respuesta.
//
// La herramienta es asincrona: no bloquea la tarea principal y recien
// contesta al modelo cuando HA respondio (o a los 8 s si no responde). Asi
// Kira solo dice "listo" si de verdad se hizo, y dice que no pudo solo si
// HA de verdad fallo.

#include "ha_bridge.h"
#include "mcp_server.h"

#include <cJSON.h>
#include <string>

namespace home_command {

inline std::string Result(bool ok, const std::string& reply) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, ok ? "respuesta_de_home_assistant" : "error", reply.c_str());
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json != nullptr ? json : (ok ? "{\"ok\":true}" : "{\"ok\":false}");
    if (json != nullptr) cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

inline void Register() {
    McpServer::GetInstance().AddAsyncTool(
        "self.home.command",
        "Controla la casa a traves de Home Assistant. Usala SIEMPRE (en lugar de HassTurnOn, "
        "HassTurnOff u otras herramientas de Home Assistant) para prender, apagar, abrir, "
        "cerrar o activar cualquier cosa de la casa: luces, enchufes, la bomba, la valvula, "
        "el aire, la tele, el dispenser, etc. Pasale UNA orden corta en castellano, en "
        "imperativo de tu y con el nombre del aparato o del lugar tal como lo dijo el usuario, "
        "por ejemplo: 'apaga la luz del taller', 'enciende la bomba', 'enciende las luces del "
        "bar'. Si el usuario pide varias cosas, llamala una vez por cada una. No la uses para "
        "temporizadores, alarmas ni para preguntar estados. La herramienta espera a Home "
        "Assistant: si devuelve ok true, decile al usuario en pocas palabras que lo hiciste; "
        "si devuelve ok false, decile que no se pudo y por que, usando el error, sin inventar.",
        PropertyList({Property("orden", kPropertyTypeString)}),
        [](const PropertyList& properties, AsyncToolDone done) {
            std::string text = properties["orden"].value<std::string>();
            if (text.empty() || text.size() > 200) {
                done(Result(false, "orden vacia o demasiado larga"));
                return;
            }
            HaBridge::Instance().SendCommand(
                text, [done](bool ok, const std::string& reply) { done(Result(ok, reply)); });
        });
}

}  // namespace home_command

#endif  // _HOME_COMMAND_H_
