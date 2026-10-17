#ifndef _HOME_COMMAND_H_
#define _HOME_COMMAND_H_

// Herramienta MCP "self.home.command": las ordenes de la casa (prender,
// apagar, abrir, cerrar) no pasan mas por el modelo adivinando nombres de
// entidades. Kira le manda la frase tal cual a Home Assistant por MQTT
// (kira/sala/comando) y HA la resuelve con su propio Assist local, con sus
// areas, nombres y alias. Si HA no puede, lo avisa por voz (automatizacion
// "Kira - ordenes de la casa").
//
// Corre en la tarea principal: solo encola el texto, no bloquea.

#include "ha_bridge.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <string>

namespace home_command {

inline void Register() {
    McpServer::GetInstance().AddTool(
        "self.home.command",
        "Controla la casa a traves de Home Assistant. Usala SIEMPRE (en lugar de HassTurnOn, "
        "HassTurnOff u otras herramientas de Home Assistant) para prender, apagar, abrir, "
        "cerrar o activar cualquier cosa de la casa: luces, enchufes, la bomba, la valvula, "
        "el aire, la tele, el dispenser, etc. Pasale UNA orden corta en castellano, en "
        "imperativo de tu y con el nombre del aparato o del lugar tal como lo dijo el usuario, "
        "por ejemplo: 'apaga la luz del taller', 'enciende la bomba', 'apaga las luces de la "
        "sala'. Si el usuario pide varias cosas, llamala una vez por cada una. No la uses para "
        "temporizadores, alarmas ni para preguntar estados. Despues de llamarla confirma en "
        "pocas palabras que lo hiciste; si algo falla, Home Assistant avisa solo.",
        PropertyList({Property("orden", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            std::string text = properties["orden"].value<std::string>();
            if (text.empty() || text.size() > 200) {
                return "{\"ok\": false, \"error\": \"orden vacia o demasiado larga\"}";
            }
            if (!HaBridge::Instance().SendCommand(text)) {
                return "{\"ok\": false, \"error\": \"Home Assistant no esta conectado ahora\"}";
            }
            return "{\"ok\": true}";
        });
}

}  // namespace home_command

#endif  // _HOME_COMMAND_H_
