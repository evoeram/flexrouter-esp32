// arcc_protocol.h - сборка/разбор пакетов протокола ARCC
// (расшифровка: FlexRouter/Hardware/Arcc/ArccHardwareDataProcessor.cs, plan.md п. 3)
//
// ESP32 -> ПК: фиксированные пакеты по 18 байт, первый байт - тип модуля
// ПК -> ESP32: пакеты переменной длины, длина определяется по байту [0]:
//              2 -> 5 байт (дамп кнопок), 4 -> 5 байт (лампа), 5 -> 10 байт (индикатор),
//              8 -> 5 байт (дамп бинарного ввода). Иначе - ресинк.

#pragma once
#include <Arduino.h>

enum ArccModuleType : uint8_t {
    ARCC_AXIS         = 1,
    ARCC_BUTTON       = 2,
    ARCC_ENCODER      = 3,
    ARCC_LINEAR_OUT   = 4,
    ARCC_INDICATOR    = 5,
    ARCC_BINARY_INPUT = 8,
};

enum ArccHostCommand : uint8_t {
    LAMP_SET_STATE      = 1,
    INDICATOR_SET_TEXT  = 1,
    DUMP_ALL_KEYS       = 3,
    DUMP_ALL_LINES      = 2,
};

static const uint8_t ARCC_PACKET_TX_SIZE = 18;
static const uint8_t ARCC_INDICATOR_POSITIONS = 7;
static const uint8_t ARCC_BUTTON_MAX_ID = 168;
static const uint8_t ARCC_BINARY_MAX_LINES = 28;

// Заполнить 18-байтный пакет нулями и проставить тип + moduleId
static inline void arccBeginPacket(uint8_t *pkt, uint8_t moduleType, uint8_t moduleId) {
    memset(pkt, 0, ARCC_PACKET_TX_SIZE);
    pkt[0] = moduleType;
    pkt[1] = moduleId;
}

// Button: [15] != 1 и != 2 (иначе роутер игнорирует пакет), [16]=controlId, [17]=pressed
static inline void arccMakeButton(uint8_t *pkt, uint8_t moduleId, uint8_t controlId, bool pressed) {
    arccBeginPacket(pkt, ARCC_BUTTON, moduleId);
    pkt[15] = 0;
    pkt[16] = controlId;
    pkt[17] = pressed ? 1 : 0;
}

// BinaryInput: [14..17] - маска big-endian, бит n-1 = линия controlId n (1..28)
static inline void arccMakeBinaryInput(uint8_t *pkt, uint8_t moduleId, uint32_t mask) {
    arccBeginPacket(pkt, ARCC_BINARY_INPUT, moduleId);
    pkt[14] = (uint8_t)(mask >> 24);
    pkt[15] = (uint8_t)(mask >> 16);
    pkt[16] = (uint8_t)(mask >> 8);
    pkt[17] = (uint8_t)(mask);
}

// Encoder: [15]=controlId (1..14), [16]=щелчков, [17]=направление (1 - по часовой)
static inline void arccMakeEncoder(uint8_t *pkt, uint8_t moduleId, uint8_t controlId,
                                   uint8_t clicks, bool clockwise) {
    arccBeginPacket(pkt, ARCC_ENCODER, moduleId);
    pkt[15] = controlId;
    pkt[16] = clicks;
    pkt[17] = clockwise ? 1 : 0;
}

// Axis: 8 осей по 16 бит big-endian, 0..1023. Пара j (байты 2+j*2) = ControlId 8-j
static inline void arccMakeAxis(uint8_t *pkt, uint8_t moduleId, const uint16_t values[8]) {
    arccBeginPacket(pkt, ARCC_AXIS, moduleId);
    for (int j = 0; j < 8; j++) {
        pkt[2 + j * 2]     = (uint8_t)(values[j] >> 8);
        pkt[2 + j * 2 + 1] = (uint8_t)(values[j]);
    }
}

// Длина пакета ПК -> ESP32 по первому байту; 0 - байт невалиден (ресинк)
static inline uint8_t arccHostPacketLength(uint8_t firstByte) {
    switch (firstByte) {
        case ARCC_BUTTON:       return 5;
        case ARCC_LINEAR_OUT:   return 5;
        case ARCC_INDICATOR:    return 10;
        case ARCC_BINARY_INPUT: return 5;
        default:                return 0;
    }
}

// Строка символов индикатора роутера (индекс в строке = код позиции в пакете).
// Индексы 10..19 - "цифра+точка", 20 - пустая позиция
static const char INDICATOR_SIGNS[] = "01234567890123456789 FDC-_^Iro.E";

// Текст -> 7 кодов позиций индикатора (порт C# StringToIndicatorBuffer).
// Текст выравнивается по правому краю; '.' после цифры -> цифра из второго ряда (с точкой)
static inline void arccTextToIndicatorCodes(const String& text, uint8_t* out) {
    for (uint8_t i = 0; i < ARCC_INDICATOR_POSITIONS; i++) out[i] = 20;
    int8_t bufPos = ARCC_INDICATOR_POSITIONS - 1;
    for (int i = (int)text.length() - 1; i >= 0; i--) {
        if (bufPos < 0)
            break;
        char symbol = text[i];
        if (symbol == ',')
            symbol = '.';
        const char* p = strchr(INDICATOR_SIGNS, symbol);
        if (!p) {
            out[bufPos] = 20;
            bufPos--;
            continue;
        }
        int pos = (int)(p - INDICATOR_SIGNS);
        if (i > 0 && symbol == '.' && text[i - 1] >= '0' && text[i - 1] <= '9') {
            pos = 10 + (text[i - 1] - '0');
            i--;
        }
        out[bufPos] = (uint8_t)pos;
        bufPos--;
    }
}

// Код позиции -> символ (для веб-дебага)
static inline char arccIndicatorCodeToChar(uint8_t code) {
    return code < sizeof(INDICATOR_SIGNS) - 1 ? INDICATOR_SIGNS[code] : '?';
}
