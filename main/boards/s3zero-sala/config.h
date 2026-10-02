#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Waveshare ESP32-S3-Zero (4MB flash, 2MB PSRAM quad) - asistente de sala
// Mismo cableado que el YAML de ESPHome "asistente-sala"

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// INMP441 (bus I2S propio)
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_13
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_12
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

// MAX98357A (bus I2S propio)
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_4
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_5

// WS2812 de estado
#define BUILTIN_LED_GPIO        GPIO_NUM_8

// Boton BOOT de la placa (click: hablar / cortar; al arrancar: modo WiFi)
#define BOOT_BUTTON_GPIO        GPIO_NUM_0

// PIR HC-SR501 (expuesto como herramienta MCP)
#define PIR_GPIO                GPIO_NUM_11

// OLED SH1106 128x64 por I2C
#define DISPLAY_SDA_PIN GPIO_NUM_10
#define DISPLAY_SCL_PIN GPIO_NUM_9
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define SH1106

// En ESPHome la pantalla se veia derecha sin rotar.
// Si queda dada vuelta, cambiar ambos a true.
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

#endif // _BOARD_CONFIG_H_
