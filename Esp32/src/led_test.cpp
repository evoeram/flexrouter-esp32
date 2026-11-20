// Тест RGB-светодиода: по очереди зажигаем WS2812 на пинах-кандидатах,
// каждый - своим цветом. Какой цвет увидели - тот пин и есть.
// 48=зелёный, 38=красный, 47=синий, 41=белый, 21=жёлтый, 0=фиолетовый
#include <Arduino.h>

struct LedPin { uint8_t pin; const char* color; uint8_t r, g, b; };
static const LedPin CANDIDATES[] = {
    { 48, "GREEN",  0,   128, 0   },
    { 38, "RED",    128, 0,   0   },
    { 47, "BLUE",   0,   0,   128 },
    { 41, "WHITE",  96,  96,  96  },
    { 21, "YELLOW", 128, 128, 0   },
    { 0,  "VIOLET", 96,  0,   128 },
};

void setup() {
    Serial.begin(115200);
}

void loop() {
    for (auto& c : CANDIDATES) {
        neopixelWrite(c.pin, c.r, c.g, c.b);
        delay(1500);
        neopixelWrite(c.pin, 0, 0, 0);
        delay(600);
    }
}
