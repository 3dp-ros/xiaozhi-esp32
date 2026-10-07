#ifndef _WAKE_THRESHOLD_H_
#define _WAKE_THRESHOLD_H_

// Sensibilidad del wake word ajustable en vivo (la usa la placa s3zero-sala
// desde Home Assistant). El motor AFE la aplica en su propia tarea.
//   0      = umbral original del modelo
//   1..10  = umbral de deteccion de 0,72 (exigente) a 0,45 (muy sensible)

#include <atomic>

namespace wake_threshold {

inline std::atomic<int> level{0};
inline std::atomic<bool> dirty{true};

inline int Clamp(int v) { return v < 0 ? 0 : (v > 10 ? 10 : v); }

inline void Set(int v) {
    level = Clamp(v);
    dirty = true;
}

inline int Get() { return level.load(); }

// Umbral para WakeNet (solo vale si level > 0)
inline float Threshold(int v) { return 0.72f - 0.03f * (float)(v - 1); }

}  // namespace wake_threshold

#endif  // _WAKE_THRESHOLD_H_
