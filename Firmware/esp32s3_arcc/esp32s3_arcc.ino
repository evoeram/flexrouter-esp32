// esp32s3_arcc.ino - ESP32-S3 как ARCC-совместимый контроллер для FlexRouter
//
// Связь: нативный USB-CDC (USB CDC On Boot = Enabled), протокол ARCC (см. plan.md раздел 2.3):
//  - TX: 18-байтные пакеты (Button/BinaryInput/Encoder/Axis) по изменению состояния ввода
//  - RX: пакеты 5/10 байт (лампы, индикатор, команды дампа), ресинк по первому байту
//  - Ответ на дамп кнопок: 168 Button-пакетов, на дамп бинарного ввода: один пакет с маской
//
// Отладка: UART0 (проверьте "USB CDC On Boot: Enabled", отладка идёт на второй USB-порт
// или пины TX0/RX0, если плата собрана на CH343-мосте).
//
// Плата: ESP32S3 Dev Module, USB CDC On Boot: Enabled, Flash Size по вашей плате (N16R8: 16MB).

#include "config.h"
#include "arcc_protocol.h"

#if ARCC_DEBUG
  #define DBG_PRINT(x)    Serial0.print(x)
  #define DBG_PRINTLN(x)  Serial0.println(x)
  #define DBG_PRINTF(...) Serial0.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINTF(...)
#endif

// --------------------------- Состояние ввода ---------------------------

struct DebouncedInput {
    bool stableState;
    bool lastRawState;
    uint32_t lastChangeMs;
};

static DebouncedInput s_buttons[BUTTON_COUNT];
static DebouncedInput s_binaryLines[BINARY_LINE_COUNT];
static uint32_t s_binaryMask = 0;          // бит n-1 = линия controlId n
static uint32_t s_lastSentBinaryMask = 0xFFFFFFFF; // чтобы первый пакет ушёл всегда

struct EncoderState {
    uint8_t quadState;     // последний код Грея A/B
    int8_t  quadSteps;     // накопленные квадра-шаги
};
static EncoderState s_encoders[ENCODER_COUNT];

static uint16_t s_axisValues[8];           // отправленные значения, индекс = controlId-1, 0..1023
static bool     s_axisChanged[8];

// --------------------------- Приём ПК -> ESP32 ---------------------------

static uint8_t s_rxBuffer[64];
static size_t s_rxLen = 0;

// Индикатор: коды 7 позиций
static uint8_t s_indicator[ARCC_INDICATOR_POSITIONS];

// --------------------------- Соединение ---------------------------

// Сборка: USBMode=hwcdc (USB-Serial-JTAG, работает на любой ревизии S3), CDCOnBoot=cdc
// В ядре 2.0.9 у HWCDC нет доступа к DTR, поэтому готовность хоста определяется
// по таймауту: 2.5 c - больше, чем DTR-импульс роутера (1 c). Данные, отправленные
// до открытия порта роутером, отбрасываются безвозвратно - это безопасно.
#define HOST_READY_DELAY_MS 2500
static bool s_hostReady = false;

// --------------------------- TX ---------------------------

static void sendPacket(const uint8_t *pkt, size_t len) {
    if (!s_hostReady)
        return;
    Serial.write(pkt, len);
}

static void sendButtonState(uint8_t controlId, bool pressed) {
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeButton(pkt, MODULE_ID_BUTTONS, controlId, pressed);
    sendPacket(pkt, sizeof(pkt));
}

static void sendBinaryMask() {
    if (s_binaryMask == s_lastSentBinaryMask)
        return;
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeBinaryInput(pkt, MODULE_ID_BINARY_INPUT, s_binaryMask);
    sendPacket(pkt, sizeof(pkt));
    s_lastSentBinaryMask = s_binaryMask;
}

static void sendEncoderClick(uint8_t controlId, bool clockwise) {
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeEncoder(pkt, MODULE_ID_ENCODERS, controlId, 1, clockwise);
    sendPacket(pkt, sizeof(pkt));
}

// --------------------------- Ответы на дамп ---------------------------

static void handleDumpAllKeys(uint8_t moduleId) {
    if (moduleId != MODULE_ID_BUTTONS)
        return;
    DBG_PRINTF("Dump keys: module %u\n", moduleId);
    // Роутер ждёт до 1 секунды - слать всё сразу, без пауз
    for (size_t i = 0; i < BUTTON_COUNT; i++)
        sendButtonState(BUTTONS[i].controlId, s_buttons[i].stableState);
    // Не назначенные кнопки сообщаются как отпущенные
    if (BUTTON_COUNT < 168) {
        uint8_t pkt[ARCC_PACKET_TX_SIZE];
        for (uint8_t id = BUTTON_COUNT + 1; id <= 168; id++) {
            arccMakeButton(pkt, MODULE_ID_BUTTONS, id, false);
            sendPacket(pkt, sizeof(pkt));
        }
    }
}

static void handleDumpAllLines(uint8_t moduleId) {
    if (moduleId != MODULE_ID_BINARY_INPUT)
        return;
    DBG_PRINTF("Dump lines: module %u\n", moduleId);
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeBinaryInput(pkt, MODULE_ID_BINARY_INPUT, s_binaryMask);
    sendPacket(pkt, sizeof(pkt));
    s_lastSentBinaryMask = s_binaryMask;
}

// --------------------------- Вывод ---------------------------

static void setLamp(uint8_t controlId, bool on) {
    for (size_t i = 0; i < LAMP_COUNT; i++) {
        if (LAMPS[i].controlId != controlId)
            continue;
        if (LAMPS[i].isRgb) {
            // Бортовой WS2812: зелёный при включении, чёрный при выключении
            neopixelWrite(LAMPS[i].pin, 0, on ? 64 : 0, 0);
        } else {
            bool level = LAMPS[i].activeLow ? !on : on;
            digitalWrite(LAMPS[i].pin, level ? HIGH : LOW);
        }
        DBG_PRINTF("Lamp %u -> %d\n", controlId, on);
        return;
    }
}

static void applyIndicator(const uint8_t *codes) {
    memcpy(s_indicator, codes, ARCC_INDICATOR_POSITIONS);
#if ARCC_DEBUG
    DBG_PRINT("Indicator: ");
    for (int i = 0; i < ARCC_INDICATOR_POSITIONS; i++) {
        uint8_t c = codes[i];
        DBG_PRINT(c < sizeof(INDICATOR_SIGNS) - 1 ? String(INDICATOR_SIGNS[c]) : String("?"));
        DBG_PRINT(c == 20 ? "( )" : "");
    }
    DBG_PRINTLN("");
#endif
    // Фаза 3: здесь вывод на MAX7219
}

// --------------------------- Разбор потока ПК -> ESP32 ---------------------------

static void handleHostPacket(const uint8_t *pkt, size_t len) {
    switch (pkt[0]) {
        case ARCC_LINEAR_OUT:
            if (len >= 5 && pkt[2] == LAMP_SET_STATE)
                setLamp(pkt[3], pkt[4] == 1);
            break;
        case ARCC_INDICATOR:
            if (len >= 10 && pkt[2] == INDICATOR_SET_TEXT)
                applyIndicator(pkt + 3);
            break;
        case ARCC_BUTTON:
            if (len >= 5 && pkt[2] == DUMP_ALL_KEYS)
                handleDumpAllKeys(pkt[1]);
            break;
        case ARCC_BINARY_INPUT:
            if (len >= 5 && pkt[2] == DUMP_ALL_LINES)
                handleDumpAllLines(pkt[1]);
            break;
        default:
            break;
    }
}

static void processRx() {
    while (Serial.available() > 0) {
        if (s_rxLen >= sizeof(s_rxBuffer)) {
            // Переполнение - сброс и ресинк
            s_rxLen = 0;
        }
        s_rxBuffer[s_rxLen++] = (uint8_t)Serial.read();

        // Пытаемся разобрать накопленное с головы буфера
        for (;;) {
            if (s_rxLen == 0)
                break;
            uint8_t need = arccHostPacketLength(s_rxBuffer[0]);
            if (need == 0) {
                // Невалидный первый байт - ресинк: отбрасываем 1 байт и ищем дальше
                memmove(s_rxBuffer, s_rxBuffer + 1, s_rxLen - 1);
                s_rxLen--;
                continue;
            }
            if (s_rxLen < need)
                break; // ждём остальное
            handleHostPacket(s_rxBuffer, need);
            memmove(s_rxBuffer, s_rxBuffer + need, s_rxLen - need);
            s_rxLen -= need;
        }
    }
}

// --------------------------- Опрос ввода ---------------------------

static bool readDebounced(DebouncedInput &in, uint8_t pin, bool activeLow) {
    bool raw = digitalRead(pin) == (activeLow ? LOW : HIGH);
    uint32_t now = millis();
    if (raw != in.lastRawState) {
        in.lastRawState = raw;
        in.lastChangeMs = now;
    } else if (raw != in.stableState && (now - in.lastChangeMs) >= DEBOUNCE_MS) {
        in.stableState = raw;
        return true; // состояние изменилось
    }
    return false;
}

static void scanButtons() {
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        if (readDebounced(s_buttons[i], BUTTONS[i].pin, BUTTONS[i].activeLow))
            sendButtonState(BUTTONS[i].controlId, s_buttons[i].stableState);
    }
}

static void scanBinaryLines() {
    bool maskChanged = false;
    for (size_t i = 0; i < BINARY_LINE_COUNT; i++) {
        if (readDebounced(s_binaryLines[i], BINARY_LINES[i].pin, BINARY_LINES[i].activeLow)) {
            uint8_t bit = BINARY_LINES[i].controlId - 1;
            if (bit < 28) {
                if (s_binaryLines[i].stableState)
                    s_binaryMask |= (1UL << bit);
                else
                    s_binaryMask &= ~(1UL << bit);
                maskChanged = true;
            }
        }
    }
    if (maskChanged)
        sendBinaryMask();
}

// Таблица переходов квадратуры: индекс - предыдущий код (B<<1)|A, значение - шаг (-1/+1)
static const int8_t QUAD_TABLE[16] = {
    0, +1, -1,  0,
   -1,  0,  0, +1,
   +1,  0,  0, -1,
    0, -1, +1,  0
};

static void scanEncoders() {
    for (size_t i = 0; i < ENCODER_COUNT; i++) {
        uint8_t a = digitalRead(ENCODERS[i].pinA) ? 1 : 0;
        uint8_t b = digitalRead(ENCODERS[i].pinB) ? 1 : 0;
        uint8_t code = (b << 1) | a;
        int8_t step = QUAD_TABLE[(s_encoders[i].quadState << 2) | code];
        s_encoders[i].quadState = code;
        if (step == 0)
            continue;
        s_encoders[i].quadSteps += step;
        if (s_encoders[i].quadSteps >= ENCODER_DETENT || s_encoders[i].quadSteps <= -ENCODER_DETENT) {
            bool clockwise = s_encoders[i].quadSteps > 0;
            s_encoders[i].quadSteps = 0;
            sendEncoderClick(ENCODERS[i].controlId, clockwise);
        }
    }
}

static void scanAxes() {
    for (size_t i = 0; i < AXIS_COUNT; i++) {
        uint8_t controlId = AXES[i].controlId;
        if (controlId < 1 || controlId > 8)
            continue;
        uint32_t raw = analogRead(AXES[i].adcPin);
        int32_t range = AXES[i].maxValue - AXES[i].minValue;
        if (range <= 0)
            continue;
        int32_t scaled = ((int32_t)raw - AXES[i].minValue) * 1023 / range;
        if (scaled < 0) scaled = 0;
        if (scaled > 1023) scaled = 1023;
        // Порог 1 - роутер дополнительно фильтрует дребезг сам
        if (abs(scaled - (int32_t)s_axisValues[controlId - 1]) <= 1)
            continue;
        s_axisValues[controlId - 1] = (uint16_t)scaled;
        s_axisChanged[controlId - 1] = true;
    }
    // Пакет осей отправляется целиком: частичная отправка обнулила бы остальные оси
    bool anyChanged = false;
    for (size_t i = 0; i < 8; i++) {
        if (s_axisChanged[i]) {
            anyChanged = true;
            break;
        }
    }
    if (!anyChanged)
        return;
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    uint16_t packetValues[8];
    // Пара байтов j (2+j*2) соответствует ControlId (8-j) -> j = 8 - controlId
    for (uint8_t c = 1; c <= 8; c++)
        packetValues[8 - c] = s_axisValues[c - 1];
    arccMakeAxis(pkt, MODULE_ID_AXES, packetValues);
    sendPacket(pkt, sizeof(pkt));
    memset(s_axisChanged, 0, sizeof(s_axisChanged));
}

// --------------------------- Настройка и главный цикл ---------------------------

void setup() {
#if ARCC_DEBUG
    Serial0.begin(115200);
#endif
    // USB CDC (USB-Serial-JTAG): скорость формальная, реальный поток идёт по USB
    Serial.begin(1250000);
    Serial.setRxBufferSize(512);
    Serial.setTxBufferSize(512);

    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        pinMode(BUTTONS[i].pin, BUTTONS[i].activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
        s_buttons[i].stableState = digitalRead(BUTTONS[i].pin) == (BUTTONS[i].activeLow ? LOW : HIGH);
        s_buttons[i].lastRawState = s_buttons[i].stableState;
    }
    for (size_t i = 0; i < BINARY_LINE_COUNT; i++) {
        pinMode(BINARY_LINES[i].pin, BINARY_LINES[i].activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
        s_binaryLines[i].stableState = digitalRead(BINARY_LINES[i].pin) == (BINARY_LINES[i].activeLow ? LOW : HIGH);
        s_binaryLines[i].lastRawState = s_binaryLines[i].stableState;
    }
    for (size_t i = 0; i < ENCODER_COUNT; i++) {
        pinMode(ENCODERS[i].pinA, INPUT_PULLUP);
        pinMode(ENCODERS[i].pinB, INPUT_PULLUP);
        uint8_t a = digitalRead(ENCODERS[i].pinA) ? 1 : 0;
        uint8_t b = digitalRead(ENCODERS[i].pinB) ? 1 : 0;
        s_encoders[i].quadState = (b << 1) | a;
    }
    for (size_t i = 0; i < LAMP_COUNT; i++) {
        if (!LAMPS[i].isRgb)
            pinMode(LAMPS[i].pin, OUTPUT);
    }
#ifdef LED_BUILTIN_PIN
    pinMode(LED_BUILTIN_PIN, OUTPUT);
#endif
    analogReadResolution(12);
    memset(s_indicator, 20, sizeof(s_indicator));
    DBG_PRINTLN("ESP32-S3 ARCC firmware started");
}

void loop() {
    static uint32_t lastScan = 0;

    if (!s_hostReady && millis() >= HOST_READY_DELAY_MS) {
        s_hostReady = true;
        DBG_PRINTLN("Host assumed ready");
    }

    processRx();

    if (millis() - lastScan >= INPUT_SCAN_MS) {
        lastScan = millis();
        scanButtons();
        scanBinaryLines();
        scanEncoders();
        scanAxes();
    }
}
