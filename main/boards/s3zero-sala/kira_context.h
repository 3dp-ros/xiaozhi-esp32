#ifndef _KIRA_CONTEXT_H_
#define _KIRA_CONTEXT_H_

// Contexto de la casa que manda Home Assistant por MQTT (kira/sala/contexto)
// para que la cara reaccione en reposo:
//   clima:  "lluvia" | "calor" | "frio" | "normal"
//   musica: true mientras suena algo en un parlante de la casa
//   hora:   "HH:MM" (la placa la sigue contando sola entre mensajes)
//
// Lo escribe la tarea MQTT y lo lee la pantalla: todo atomico.

#include <esp_timer.h>
#include <atomic>
#include <cstdint>

namespace kira_ctx {

enum Weather { kWeatherNormal = 0, kWeatherRain, kWeatherHot, kWeatherCold };

inline std::atomic<int> weather{kWeatherNormal};
inline std::atomic<bool> music{false};
inline std::atomic<int> clock_min{-1};          // minuto del dia recibido
inline std::atomic<int64_t> clock_at_us{0};     // cuando se recibio

inline void SetClock(int h, int m) {
    if (h < 0 || h > 23 || m < 0 || m > 59) return;
    clock_at_us = esp_timer_get_time();
    clock_min = h * 60 + m;
}

// Minuto del dia actual, o -1 si HA no mando la hora en las ultimas 3 h
inline int NowMinutes() {
    int m = clock_min.load();
    if (m < 0) return -1;
    int64_t elapsed = (esp_timer_get_time() - clock_at_us.load()) / 60000000LL;
    if (elapsed < 0 || elapsed > 180) return -1;
    return (int)((m + elapsed) % 1440);
}

}  // namespace kira_ctx

#endif  // _KIRA_CONTEXT_H_
