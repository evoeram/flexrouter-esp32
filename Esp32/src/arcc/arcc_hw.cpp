#include "arcc_hw.h"
#include "arcc_core.h"
#include <algorithm>
#include "../log/event_log.h"
#include "../config/config_store.h"

// --------------------------- Инициализация ---------------------------

bool ArccHw::begin(JsonDocument& cfg) {
    _debounceMs = cfg["io"]["buttonDebounceMs"] | 10;
    _axisThreshold = cfg["io"]["axisThreshold"] | 1;
    _encoderDetent = cfg["io"]["encoderDetent"] | 4;

    struct EncPair { uint8_t pinA, pinB; };
    std::vector<std::pair<uint8_t, EncPair>> encMap;

    JsonArray modules = cfg["modules"].as<JsonArray>();
    if (modules.isNull())
        return false;

    for (JsonObject mod : modules) {
        String type = mod["type"] | "";
        uint8_t id = mod["id"] | 0;

        if (type == "LinearOutput") {
            _midLamp = id;
            bool activeLow = mod["activeLow"] | false;
            bool rgb = mod["rgb"] | false;
            JsonObject pins = mod["pins"].as<JsonObject>();
            for (JsonPair kv : pins)
                _lamps.push_back({ (uint8_t)(kv.value() | 0), (uint8_t)atoi(kv.key().c_str()),
                                   activeLow, rgb, false });
        } else if (type == "Indicator") {
            _midIndicator = id;
            String driver = mod["driver"] | "none";
            if (driver == "max7219") {
                _indMax7219 = true;
                _indCs = mod["cs"] | 10;
                _indSck = mod["sck"] | 12;
                _indMosi = mod["mosi"] | 11;
                uint8_t intensity = mod["intensity"] | 4;
                pinMode(_indCs, OUTPUT);
                digitalWrite(_indCs, HIGH);
                pinMode(_indSck, OUTPUT);
                pinMode(_indMosi, OUTPUT);
                max7219Write(0x0F, 0x00);  // display test off
                max7219Write(0x0B, 0x06);  // scan limit: 7 цифр
                max7219Write(0x09, 0x00);  // без BCD-декодера
                max7219Write(0x0A, intensity & 0x0F);
                max7219Write(0x0C, 0x01);  // shutdown off
                max7219Show();
            }
        } else if (type == "Button") {
            _midButton = id;
            bool activeLow = mod["activeLow"] | true;
            JsonObject pins = mod["pins"].as<JsonObject>();
            for (JsonPair kv : pins)
                _buttons.push_back({ (uint8_t)(kv.value() | 0), (uint8_t)atoi(kv.key().c_str()),
                                     activeLow, false, false, 0 });
        } else if (type == "BinaryInput") {
            _midBinary = id;
            bool activeLow = mod["activeLow"] | true;
            JsonObject pins = mod["pins"].as<JsonObject>();
            for (JsonPair kv : pins)
                _binary.push_back({ (uint8_t)(kv.value() | 0), (uint8_t)atoi(kv.key().c_str()),
                                    activeLow, false, false, 0 });
        } else if (type == "Encoder") {
            _midEncoder = id;
            JsonObject pins = mod["pins"].as<JsonObject>();
            for (JsonPair kv : pins) {
                String key(kv.key().c_str());
                if (key.length() < 2) continue;
                uint8_t ctl = (uint8_t)atoi(key.c_str());
                uint8_t pin = (uint8_t)(kv.value() | 0);
                auto it = std::find_if(encMap.begin(), encMap.end(),
                                       [&](auto& p) { return p.first == ctl; });
                if (it == encMap.end()) {
                    encMap.push_back({ ctl, { 0, 0 } });
                    it = encMap.end() - 1;
                }
                if (key[key.length() - 1] == 'a')
                    it->second.pinA = pin;
                else
                    it->second.pinB = pin;
            }
        } else if (type == "Axis") {
            _midAxis = id;
            JsonObject pins = mod["pins"].as<JsonObject>();
            JsonObject cal = mod["cal"].as<JsonObject>();
            for (JsonPair kv : pins) {
                uint8_t ctl = (uint8_t)atoi(kv.key().c_str());
                uint8_t pin = (uint8_t)(kv.value() | 0);
                uint16_t minV = 0, maxV = 4095;
                if (!cal.isNull()) {
                    String key(kv.key().c_str());
                    minV = cal[key]["min"] | 0;
                    maxV = cal[key]["max"] | 4095;
                }
                _axes.push_back({ pin, ctl, minV, maxV });
            }
        } else if (type == "ShiftIn") {
            // 74HC165: {"id":..,"type":"ShiftIn","pins":{"data":..,"clk":..,"latch":..},
            //           "chips":2,"map":{"1":0,"2":1,...}} - controlId -> бит каскада
            _hasShiftIn = true;
            _midShiftIn = id;
            _shiftIn.data = mod["pins"]["data"] | 0;
            _shiftIn.clk = mod["pins"]["clk"] | 0;
            _shiftIn.latch = mod["pins"]["latch"] | 0;
            _shiftIn.chips = mod["chips"] | 1;
            _shiftIn.activeLow = mod["activeLow"] | true;
            for (JsonPair kv : mod["map"].as<JsonObject>())
                _shiftIn.ch.push_back({ (uint8_t)atoi(kv.key().c_str()),
                                        (uint16_t)(kv.value() | 0), false, false, 0 });
            pinMode(_shiftIn.data, INPUT);
            pinMode(_shiftIn.clk, OUTPUT);
            pinMode(_shiftIn.latch, OUTPUT);
            digitalWrite(_shiftIn.clk, LOW);
            digitalWrite(_shiftIn.latch, HIGH);
        } else if (type == "ShiftOut") {
            // 74HC595: {"id":0,"type":"ShiftOut","pins":{"data":..,"clk":..,"latch":..},
            //           "chips":1,"activeLow":false,"map":{"1":0,...}} - controlId -> бит каскада
            _hasShiftOut = true;
            _shiftOut.data = mod["pins"]["data"] | 0;
            _shiftOut.clk = mod["pins"]["clk"] | 0;
            _shiftOut.latch = mod["pins"]["latch"] | 0;
            _shiftOut.chips = mod["chips"] | 1;
            _shiftOut.activeLow = mod["activeLow"] | false;
            for (JsonPair kv : mod["map"].as<JsonObject>())
                _shiftOut.ch.push_back({ (uint8_t)atoi(kv.key().c_str()),
                                         (uint16_t)(kv.value() | 0), false });
            pinMode(_shiftOut.data, OUTPUT);
            pinMode(_shiftOut.clk, OUTPUT);
            pinMode(_shiftOut.latch, OUTPUT);
            _shiftOut.shadow.assign(_shiftOut.chips, _shiftOut.activeLow ? 0xFF : 0x00);
            shiftOutLatch();
        }
    }

    for (auto& p : encMap) {
        if (p.second.pinA && p.second.pinB)
            _encoders.push_back({ p.second.pinA, p.second.pinB, p.first, 0, 0 });
    }

    // Настройка GPIO
    for (auto& b : _buttons) {
        pinMode(b.pin, b.activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
        b.stable = b.lastRaw = digitalRead(b.pin) == (b.activeLow ? LOW : HIGH);
    }
    for (auto& l : _binary) {
        pinMode(l.pin, l.activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
        l.stable = l.lastRaw = digitalRead(l.pin) == (l.activeLow ? LOW : HIGH);
        if (l.stable && l.controlId >= 1 && l.controlId <= ARCC_BINARY_MAX_LINES)
            _binaryMask |= (1UL << (l.controlId - 1));
    }
    for (auto& e : _encoders) {
        pinMode(e.pinA, INPUT_PULLUP);
        pinMode(e.pinB, INPUT_PULLUP);
        e.quadState = (digitalRead(e.pinB) ? 1 : 0) << 1 | (digitalRead(e.pinA) ? 1 : 0);
    }
    for (auto& a : _axes) {
        pinMode(a.pin, INPUT);
        analogReadResolution(12);
    }
    for (auto& l : _lamps) {
        if (isRgbLamp(l))
            neopixelWrite(l.pin, 0, 0, 0);
        else {
            pinMode(l.pin, OUTPUT);
            digitalWrite(l.pin, l.activeLow ? HIGH : LOW);
        }
    }

    memset(_indicator, 20, sizeof(_indicator));
    EventLog::instance().log(LOG_INFO,
        "ARCC hw: %u btn, %u bin, %u enc, %u axes, %u lamps, ind=%s%s",
        (unsigned)_buttons.size(), (unsigned)_binary.size(), (unsigned)_encoders.size(),
        (unsigned)_axes.size(), (unsigned)_lamps.size(), _indMax7219 ? "max7219" : "none",
        _hasShiftIn ? " +165" : "");
    return true;
}

// --------------------------- Опрос ввода ---------------------------

void ArccHw::loop() {
    uint32_t now = millis();
    if (now - _lastScanMs < 1)
        return;
    _lastScanMs = now;
    scanButtons();
    scanBinary();
    scanEncoders();
    scanAxes();
    scanShiftIn();
    shiftOutLatch();
    // Периодический рефрес RGB-ламп: одиночный кадр WS2812 может не удержаться
    // (RMT/помехи), повторная отправка текущего состояния - штатная практика
    if (now - _lastLampRefreshMs >= 250) {
        _lastLampRefreshMs = now;
        for (auto& l : _lamps)
            if (isRgbLamp(l))
                neopixelWrite(l.pin, 0, l.on ? 160 : 0, 0);
    }
}

bool ArccHw::readDebounced(ButtonIO& in, uint8_t pin, bool activeLow) {
    return readDebouncedBin(*(BinaryIO*)&in, pin, activeLow);
}

bool ArccHw::readDebouncedBin(BinaryIO& in, uint8_t pin, bool activeLow) {
    bool raw = digitalRead(pin) == (activeLow ? LOW : HIGH);
    return debouncedStep(in.lastRaw, in.stable, in.lastChangeMs, raw);
}

// Дребезг: true - стабильное состояние сменилось (сырое значение передаётся снаружи,
// чтобы конвейер работала и с GPIO, и с битами сдвигового регистра 74HC165)
bool ArccHw::debouncedStep(bool& lastRaw, bool& stable, uint32_t& lastChangeMs, bool raw) {
    uint32_t now = millis();
    if (raw != lastRaw) {
        lastRaw = raw;
        lastChangeMs = now;
    } else if (raw != stable && (now - lastChangeMs) >= _debounceMs) {
        stable = raw;
        return true;
    }
    return false;
}

// --------------------------- 74HC165 / 74HC595 (этап 4) ---------------------------

void ArccHw::scanShiftIn() {
    if (!_hasShiftIn)
        return;
    // latch: параллельная загрузка регистров (короткий импульс)
    digitalWrite(_shiftIn.latch, LOW);
    delayMicroseconds(2);
    digitalWrite(_shiftIn.latch, HIGH);

    // читаем chips*8 бит: бит 0 - первый, вышедший из каскада после latch
    const int totalBits = _shiftIn.chips * 8;
    std::vector<uint8_t> bytes(_shiftIn.chips, 0);
    for (int i = 0; i < totalBits; i++) {
        if (digitalRead(_shiftIn.data) == HIGH)
            bytes[i / 8] |= (0x80 >> (i % 8));
        digitalWrite(_shiftIn.clk, HIGH);
        delayMicroseconds(1);
        digitalWrite(_shiftIn.clk, LOW);
    }

    for (auto& c : _shiftIn.ch) {
        if (c.bit >= (uint16_t)totalBits)
            continue;
        bool raw = (bytes[c.bit / 8] >> (7 - (c.bit % 8))) & 1;
        if (_shiftIn.activeLow)
            raw = !raw;
        if (debouncedStep(c.lastRaw, c.stable, c.lastChangeMs, raw)) {
            ArccCore::instance().sendButton(_midShiftIn, c.controlId, c.stable);
            _ioDirty = true;
        }
    }
}

void ArccHw::shiftOutLatch() {
    if (!_hasShiftOut || !_shiftOut.dirty)
        return;
    _shiftOut.dirty = false;
    // каскад: байт для последней (дальней) микросхемы уходит первым;
    // бит 0 канала = QA ближайшей к MCU микросхемы
    for (int i = _shiftOut.chips - 1; i >= 0; i--)
        shiftOut(_shiftOut.data, _shiftOut.clk, MSBFIRST, _shiftOut.shadow[i]);
    digitalWrite(_shiftOut.latch, LOW);
    delayMicroseconds(2);
    digitalWrite(_shiftOut.latch, HIGH);
}

void ArccHw::scanButtons() {
    for (auto& b : _buttons) {
        if (readDebounced(b, b.pin, b.activeLow)) {
            ArccCore::instance().sendButton(_midButton, b.controlId, b.stable);
            _ioDirty = true;
        }
    }
}

void ArccHw::scanBinary() {
    bool changed = false;
    for (auto& l : _binary) {
        if (readDebouncedBin(l, l.pin, l.activeLow)) {
            if (l.controlId >= 1 && l.controlId <= ARCC_BINARY_MAX_LINES) {
                if (l.stable)
                    _binaryMask |= (1UL << (l.controlId - 1));
                else
                    _binaryMask &= ~(1UL << (l.controlId - 1));
                changed = true;
            }
        }
    }
    if (changed) {
        sendBinaryMaskIfChanged();
        _ioDirty = true;
    }
}

// Таблица переходов квадратуры: индекс - предыдущий код (B<<1)|A, значение - шаг
static const int8_t QUAD_TABLE[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

void ArccHw::scanEncoders() {
    for (auto& e : _encoders) {
        uint8_t a = digitalRead(e.pinA) ? 1 : 0;
        uint8_t b = digitalRead(e.pinB) ? 1 : 0;
        uint8_t code = (b << 1) | a;
        int8_t step = QUAD_TABLE[(e.quadState << 2) | code];
        e.quadState = code;
        if (step == 0)
            continue;
        e.quadSteps += step;
        if (e.quadSteps >= _encoderDetent || e.quadSteps <= -_encoderDetent) {
            bool cw = e.quadSteps > 0;
            e.quadSteps = 0;
            ArccCore::instance().sendEncoder(_midEncoder, e.controlId, cw);
            _ioDirty = true;
        }
    }
}

void ArccHw::scanAxes() {
    if (_axes.empty())
        return;
    // Ограничение частоты: шум/дрожание потенциометра не должен заливать линк
    uint32_t now = millis();
    if (now - _lastAxisSentMs < 25)
        return;
    bool anyChanged = false;
    for (auto& a : _axes) {
        if (a.controlId < 1 || a.controlId > 8)
            continue;
        uint32_t raw = analogRead(a.pin);
        _axisRaw[a.controlId - 1] = (uint16_t)raw;
        int32_t range = a.maxV - a.minV;
        if (range <= 0)
            continue;
        int32_t scaled = ((int32_t)raw - a.minV) * 1023 / range;
        if (scaled < 0) scaled = 0;
        if (scaled > 1023) scaled = 1023;
        if (abs(scaled - (int32_t)_axisValues[a.controlId - 1]) <= _axisThreshold)
            continue;
        _axisValues[a.controlId - 1] = (uint16_t)scaled;
        anyChanged = true;
    }
    if (!anyChanged)
        return;
    // Пакет осей целиком: частичная отправка обнулила бы остальные оси.
    // Пара j (2+j*2) = ControlId (8-j) -> j = 8 - controlId
    uint16_t packetValues[8] = {0};
    for (uint8_t c = 1; c <= 8; c++)
        packetValues[8 - c] = _axisValues[c - 1];
    ArccCore::instance().sendAxis(_midAxis, packetValues);
    _lastAxisSentMs = now;
    _ioDirty = true;
}

void ArccHw::sendBinaryMaskIfChanged() {
    if (_binaryMask == _lastSentBinaryMask)
        return;
    ArccCore::instance().sendBinaryMask(_midBinary, _binaryMask);
    _lastSentBinaryMask = _binaryMask;
}

// --------------------------- Вывод ---------------------------

bool ArccHw::setLamp(uint8_t controlId, bool on) {
    for (auto& l : _lamps) {
        if (l.controlId != controlId)
            continue;
        l.on = on;
        if (isRgbLamp(l)) {
            // бортовой WS2812: зелёный при включении
            neopixelWrite(l.pin, 0, on ? 160 : 0, 0);
        } else {
            bool level = l.activeLow ? !on : on;
            digitalWrite(l.pin, level ? HIGH : LOW);
        }
        _ioDirty = true;
        return true;
    }
    // лампы на 74HC595: меняем теневой регистр, latch - в конце цикла (shiftOutLatch)
    for (auto& c : _shiftOut.ch) {
        if (c.controlId != controlId)
            continue;
        c.on = on;
        uint8_t byteVal = _shiftOut.shadow[c.bit / 8];
        uint8_t mask = 1 << (c.bit % 8);
        bool level = _shiftOut.activeLow ? !on : on;
        if (level)
            byteVal |= mask;
        else
            byteVal &= ~mask;
        _shiftOut.shadow[c.bit / 8] = byteVal;
        _shiftOut.dirty = true;
        _ioDirty = true;
        return true;
    }
    EventLog::instance().log(LOG_WARN, "lamp ctl %u not found in pin map", controlId);
    return false;
}

bool ArccHw::setIndicator(const uint8_t* codes) {
    memcpy(_indicator, codes, ARCC_INDICATOR_POSITIONS);
    if (_indMax7219)
        max7219Show();
    _ioDirty = true;
    return true;
}

bool ArccHw::setIndicatorText(const String& text) {
    uint8_t codes[ARCC_INDICATOR_POSITIONS];
    arccTextToIndicatorCodes(text, codes);
    return setIndicator(codes);
}

// --------------------------- Дампы ---------------------------

void ArccHw::dumpAllKeys(uint8_t moduleId) {
    if (moduleId == _midButton) {
        EventLog::instance().log(LOG_INFO, "dump keys: module %u", moduleId);
        // Роутер ждёт до 1 с - слать всё сразу
        for (auto& b : _buttons)
            ArccCore::instance().sendButton(_midButton, b.controlId, b.stable);
        // Не назначенные кнопки сообщаются как отпущенные
        uint8_t maxCtl = 0;
        for (auto& b : _buttons)
            if (b.controlId > maxCtl) maxCtl = b.controlId;
        for (uint8_t id = maxCtl + 1; id <= ARCC_BUTTON_MAX_ID; id++)
            ArccCore::instance().sendButton(_midButton, id, false);
        return;
    }
    // входы 74HC165 дампятся как отдельный Button-модуль
    if (_hasShiftIn && moduleId == _midShiftIn) {
        EventLog::instance().log(LOG_INFO, "dump keys: shift module %u", moduleId);
        uint8_t maxCtl = 0;
        for (auto& c : _shiftIn.ch) {
            ArccCore::instance().sendButton(_midShiftIn, c.controlId, c.stable);
            if (c.controlId > maxCtl) maxCtl = c.controlId;
        }
        for (uint8_t id = maxCtl + 1; id <= ARCC_BUTTON_MAX_ID; id++)
            ArccCore::instance().sendButton(_midShiftIn, id, false);
    }
}

void ArccHw::dumpAllLines(uint8_t moduleId) {
    if (moduleId != _midBinary)
        return;
    EventLog::instance().log(LOG_INFO, "dump lines: module %u", moduleId);
    ArccCore::instance().sendBinaryMask(_midBinary, _binaryMask);
    _lastSentBinaryMask = _binaryMask;
}

// --------------------------- Симуляция (фаза 5) ---------------------------

void ArccHw::simButton(uint8_t controlId, bool pressed) {
    ArccCore::instance().sendButton(_midButton, controlId, pressed);
}

void ArccHw::simEncoder(uint8_t controlId, bool cw) {
    ArccCore::instance().sendEncoder(_midEncoder, controlId, cw);
}

void ArccHw::simAxis(uint8_t controlId, uint16_t value) {
    if (controlId < 1 || controlId > 8)
        return;
    _axisValues[controlId - 1] = value;
    uint16_t packetValues[8] = {0};
    for (uint8_t c = 1; c <= 8; c++)
        packetValues[8 - c] = _axisValues[c - 1];
    ArccCore::instance().sendAxis(_midAxis, packetValues);
}

// --------------------------- Состояние для веба ---------------------------

String ArccHw::ioStateJson() {
    JsonDocument doc;
    JsonObject buttons = doc["buttons"].to<JsonObject>();
    for (auto& b : _buttons)
        buttons[String(b.controlId)] = b.stable;
    for (auto& c : _shiftIn.ch)
        buttons[String(c.controlId)] = c.stable;
    JsonObject lamps = doc["lamps"].to<JsonObject>();
    for (auto& l : _lamps)
        lamps[String(l.controlId)] = l.on;
    for (auto& c : _shiftOut.ch)
        lamps[String(c.controlId)] = c.on;
    doc["binaryMask"] = _binaryMask;
    JsonObject axes = doc["axes"].to<JsonObject>();
    for (auto& a : _axes)
        axes[String(a.controlId)] = _axisValues[a.controlId - 1];
    JsonObject axesRaw = doc["axesRaw"].to<JsonObject>();
    for (auto& a : _axes)
        axesRaw[String(a.controlId)] = _axisRaw[a.controlId - 1];
    String ind;
    for (int i = 0; i < ARCC_INDICATOR_POSITIONS; i++)
        ind += arccIndicatorCodeToChar(_indicator[i]);
    doc["indicator"] = ind;
    String out;
    serializeJson(doc, out);
    return out;
}

// --------------------------- MAX7219 (7 позиций) ---------------------------

// Коды 0..31 (см. INDICATOR_SIGNS) -> сегменты a..g,dp (bit0..7)
static const uint8_t SEG7_PATTERNS[32] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,  // 0-9
    0xBF, 0x86, 0xBB, 0xCF, 0xE6, 0xED, 0xFD, 0x87, 0xFF, 0xEF,  // 0-9 с точкой
    0x00,       // 20 пробел
    0x71,       // 21 F
    0x5E,       // 22 D
    0x39,       // 23 C
    0x40,       // 24 -
    0x08,       // 25 _
    0x21,       // 26 ^
    0x30,       // 27 I
    0x5C,       // 28 o
    0x50,       // 29 r
    0x80,       // 30 .
    0x79        // 31 E
};

void ArccHw::max7219Write(uint8_t reg, uint8_t val) {
    digitalWrite(_indCs, LOW);
    uint16_t frame = ((uint16_t)reg << 8) | val;
    for (int i = 15; i >= 0; i--) {
        digitalWrite(_indMosi, (frame >> i) & 1 ? HIGH : LOW);
        digitalWrite(_indSck, HIGH);
        digitalWrite(_indSck, LOW);
    }
    digitalWrite(_indCs, HIGH);
}

void ArccHw::max7219Show() {
    // позиция 0 - левая; digit0 MAX7219 - правая
    for (uint8_t d = 0; d < ARCC_INDICATOR_POSITIONS; d++) {
        uint8_t code = _indicator[ARCC_INDICATOR_POSITIONS - 1 - d];
        max7219Write(0x01 + d, code < 32 ? SEG7_PATTERNS[code] : 0);
    }
}
