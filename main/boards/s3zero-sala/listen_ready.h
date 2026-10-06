#ifndef _LISTEN_READY_H_
#define _LISTEN_READY_H_

// Indica cuando Kira REALMENTE esta escuchando.
//
// Al pasar a kDeviceStateListening el firmware todavia no manda audio:
// primero descarta ~120 ms de "calentamiento" del microfono
// (audio_service.cc) y el procesador AFE tarda unos frames mas en entregar
// datos. Si la cara y el LED cambian en el mismo instante que el estado,
// la primera silaba que decis se pierde (~300-400 ms).
//
// La cara y el LED usan EffectiveState(): mientras no pase LISTEN_READY_MS
// desde que empezo a escuchar, informa kDeviceStateConnecting, asi la
// senal de "habla ahora" aparece recien cuando el audio ya esta saliendo.

#include "application.h"

#include <esp_timer.h>
#include <mutex>

#ifndef LISTEN_READY_MS
#define LISTEN_READY_MS 450
#endif

namespace listen_ready {

inline std::mutex mutex_;
inline DeviceState last_ = kDeviceStateUnknown;
inline int64_t since_us_ = 0;

inline DeviceState EffectiveState(DeviceState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = esp_timer_get_time();
    if (state == kDeviceStateListening && last_ != kDeviceStateListening) {
        since_us_ = now;
    }
    last_ = state;
    if (state == kDeviceStateListening &&
        now - since_us_ < (int64_t)LISTEN_READY_MS * 1000) {
        return kDeviceStateConnecting;
    }
    return state;
}

}  // namespace listen_ready

#endif  // _LISTEN_READY_H_
