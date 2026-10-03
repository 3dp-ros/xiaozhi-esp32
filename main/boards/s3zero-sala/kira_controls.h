#ifndef _KIRA_CONTROLS_H_
#define _KIRA_CONTROLS_H_

// Controles de Kira manejados desde Home Assistant y guardados en la placa:
//   - Modo dormir:   pantalla y LED apagados. Sigue escuchando el wake word:
//                    al decir "Ey Kira" se despierta solo y atiende.
//   - Silenciar mic: no escucha nada (ni el wake word). LED rojo tenue.
//                    Se reactiva desde Home Assistant.
//   - Modo noche:    pantalla al minimo y LED apagado (sigue funcionando).
//   - Brillo:        brillo de la pantalla (1-100 %).

#include "settings.h"

#include <atomic>

namespace kira {

class Controls {
public:
    static Controls& Get() {
        static Controls instance;
        return instance;
    }

    void Load() {
        Settings s("kira", false);
        sleep_ = s.GetBool("sleep", false);
        mute_ = s.GetBool("mute", false);
        night_ = s.GetBool("night", false);
        brightness_ = Clamp(s.GetInt("bright", 100), 1, 100);
    }

    bool Sleep() const { return sleep_.load(); }
    bool Mute() const { return mute_.load(); }
    bool Night() const { return night_.load(); }
    int Brightness() const { return brightness_.load(); }

    // El microfono solo se corta si esta silenciado (durmiendo sigue
    // escuchando el wake word para poder despertarlo con la voz)
    bool MicOff() const { return mute_.load(); }

    void SetSleep(bool v) { sleep_ = v; Save("sleep", v); }
    void SetMute(bool v) { mute_ = v; Save("mute", v); }
    void SetNight(bool v) { night_ = v; Save("night", v); }
    void SetBrightness(int v) {
        v = Clamp(v, 1, 100);
        brightness_ = v;
        Settings s("kira", true);
        s.SetInt("bright", v);
    }

    // Contraste efectivo para la OLED (0-255)
    int Contrast() const {
        if (night_.load()) return 1;
        return 1 + (brightness_.load() * 254) / 100;
    }

private:
    std::atomic<bool> sleep_{false};
    std::atomic<bool> mute_{false};
    std::atomic<bool> night_{false};
    std::atomic<int> brightness_{100};

    static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static void Save(const char* key, bool v) {
        Settings s("kira", true);
        s.SetBool(key, v);
    }
};

}  // namespace kira

#endif  // _KIRA_CONTROLS_H_
