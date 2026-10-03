#ifndef _MIC_GAIN_H_
#define _MIC_GAIN_H_

// Ganancia digital del microfono ajustable en vivo (desde Home Assistant)
// y guardada en la memoria de la placa. MIC_GAIN (config.h) es el valor
// por defecto la primera vez.

#include "config.h"
#include "settings.h"

#include <atomic>

namespace mic_gain {

constexpr int kMin = 1;
constexpr int kMax = 12;

inline std::atomic<int> value{MIC_GAIN};

inline int Clamp(int v) { return v < kMin ? kMin : (v > kMax ? kMax : v); }

inline void Load() {
    Settings settings("kira", false);
    value = Clamp(settings.GetInt("mic_gain", MIC_GAIN));
}

inline void Set(int v) {
    v = Clamp(v);
    value = v;
    Settings settings("kira", true);
    settings.SetInt("mic_gain", v);
}

inline int Get() { return value.load(); }

}  // namespace mic_gain

#endif  // _MIC_GAIN_H_
