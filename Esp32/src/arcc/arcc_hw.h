// arcc_hw.h - входы/выходы по карте пинов из JSON-конфига.
// Кнопки/бинарный ввод - с дребезгом, энкодеры - квадратура, оси - ADC,
// лампы - GPIO/WS2812, индикатор - none/max7219.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "arcc_protocol.h"

struct ButtonIO {
    uint8_t pin, controlId;
    bool activeLow;
    bool stable, lastRaw;
    uint32_t lastChangeMs;
};

struct BinaryIO {
    uint8_t pin, controlId;
    bool activeLow;
    bool stable, lastRaw;
    uint32_t lastChangeMs;
};

struct EncoderIO {
    uint8_t pinA, pinB, controlId;
    uint8_t quadState;
    int8_t quadSteps;
};

struct AxisIO {
    uint8_t pin, controlId;
    uint16_t minV, maxV;
};

struct LampIO {
    uint8_t pin, controlId;
    bool activeLow, rgb;
    bool on;
};

// 74HC165 - каскад сдвиговых регистров на входе (этап 4 plan_new.md).
// Каналы ведут себя как кнопки: дребезг, отправка Button, дамп dumpAllKeys.
struct ShiftInChannel {
    uint8_t controlId;
    uint16_t bit;              // абсолютный бит каскада: 0 = первый бит после latch
    bool stable, lastRaw;
    uint32_t lastChangeMs;
};

// 74HC595 - каскад на выходе (дискретные лампы; rgb/WS2812 не поддерживаются).
struct ShiftOutChannel {
    uint8_t controlId;
    uint16_t bit;              // бит 0 = QA первой (ближайшей к MCU) микросхемы
    bool on;
};

// WS2812: флаг из конфига либо бортовой светодиод DevKit (GPIO48)
static inline bool isRgbLamp(const LampIO& l) {
    return l.rgb || l.pin == 48;
}

class ArccHw {
public:
    static ArccHw& instance() { static ArccHw s; return s; }

    bool begin(JsonDocument& cfg);
    void loop();                       // опрос ввода (внутри троттлинг 1 мс)

    bool consumeIoDirty() { bool d = _ioDirty; _ioDirty = false; return d; }

    // Вывод (команды ARCC и веб-симулятор)
    bool setLamp(uint8_t controlId, bool on);
    bool setIndicator(const uint8_t* codes);
    bool setIndicatorText(const String& text);

    // Дампы (запросы роутера при старте)
    void dumpAllKeys(uint8_t moduleId);
    void dumpAllLines(uint8_t moduleId);

    // Симуляция событий ввода из веба (фаза 5)
    void simButton(uint8_t controlId, bool pressed);
    void simEncoder(uint8_t controlId, bool cw);
    void simAxis(uint8_t controlId, uint16_t value);

    // Состояние для веба (/api/io)
    String ioStateJson();

    size_t lampCount() const { return _lamps.size(); }

private:
    std::vector<ButtonIO> _buttons;
    std::vector<BinaryIO> _binary;
    std::vector<EncoderIO> _encoders;
    std::vector<AxisIO> _axes;
    std::vector<LampIO> _lamps;

    // 74HC165/74HC595 (этап 4)
    struct ShiftInBus {
        uint8_t data = 0, clk = 0, latch = 0;
        uint8_t chips = 1;
        bool activeLow = true;
        std::vector<ShiftInChannel> ch;
    };
    struct ShiftOutBus {
        uint8_t data = 0, clk = 0, latch = 0;
        uint8_t chips = 1;
        bool activeLow = false;
        std::vector<ShiftOutChannel> ch;
        std::vector<uint8_t> shadow;   // теневой регистр каскада
        bool dirty = false;            // есть несброшенные изменения
    };
    bool _hasShiftIn = false, _hasShiftOut = false;
    ShiftInBus _shiftIn;
    ShiftOutBus _shiftOut;

    uint8_t _midButton = 1, _midBinary = 1, _midEncoder = 1, _midAxis = 1, _midShiftIn = 1;
    uint8_t _midLamp = 0, _midIndicator = 0;
    uint16_t _debounceMs = 10;
    uint16_t _axisThreshold = 1;
    uint8_t _encoderDetent = 4;

    uint32_t _binaryMask = 0;
    uint32_t _lastSentBinaryMask = 0xFFFFFFFF;
    uint16_t _axisValues[8] = {0};       // отправленные значения, индекс = controlId-1
    uint16_t _axisRaw[8] = {0};          // сырое ADC, для веба (ioStateJson)
    uint8_t _indicator[ARCC_INDICATOR_POSITIONS];
    bool _ioDirty = true;
    uint32_t _lastScanMs = 0;
    uint32_t _lastAxisSentMs = 0;
    uint32_t _lastLampRefreshMs = 0;

    // MAX7219
    bool _indMax7219 = false;
    uint8_t _indCs = 255, _indSck = 255, _indMosi = 255;

    bool readDebounced(ButtonIO& in, uint8_t pin, bool activeLow);
    bool readDebouncedBin(BinaryIO& in, uint8_t pin, bool activeLow);
    void scanButtons();
    void scanBinary();
    void scanEncoders();
    void scanAxes();
    void scanShiftIn();          // 74HC165: latch + чтение каскада + дребезг
    void shiftOutLatch();        // 74HC595: сброс теневого регистра (раз на цикл)
    void sendBinaryMaskIfChanged();

    // общий дребезг-конвейер: true - состояние сменилось, stable уже обновлён
    bool debouncedStep(bool& lastRaw, bool& stable, uint32_t& lastChangeMs, bool raw);

    void max7219Write(uint8_t reg, uint8_t val);
    void max7219Show();
};
