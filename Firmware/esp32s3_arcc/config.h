// config.h - карта маппинга "пин <-> контрол ARCC" для FlexRouter
// Все модули живут внутри одной ESP32-S3 и различаются по ModuleId.
// ModuleId каждой группы ввода/вывода должен совпадать с тем, что выбран в профиле роутера.

#pragma once

// --- Идентификаторы модулей ввода (в профиле роутера: Motherboard -> ModuleId) ---
#define LED_BUILTIN_PIN         48  // бортовой WS2812 (не путать: зажигается только через neopixelWrite)
#define MODULE_ID_BUTTONS       1   // модуль кнопок/тумблеров (ControlId 1..168)
#define MODULE_ID_BINARY_INPUT  1   // модуль бинарного ввода (ControlId 1..28), внутри роутера станет 257
#define MODULE_ID_ENCODERS      1   // модуль энкодеров (ControlId 1..14)
#define MODULE_ID_AXES          1   // модуль осей (ControlId 1..8, значение 0..1023)

// --- Идентификаторы модулей вывода ---
// ВНИМАНИЕ: ModuleId ламп в профиле задаётся полем "Ext" (расширение) при назначении.
// В profile001.apa лампа назначена с Ext.0 -> moduleId = 0
#define MODULE_ID_LAMPS         0   // лампы (ControlId 1..29)
#define MODULE_ID_INDICATOR     0   // 7-сегментный индикатор (7 позиций)

// --- Кнопки/тумблеры (пакет Button, тип 2) ---
// activeLow: true - вход замыкает на GND (подтяжка вверх), false - на 3.3V
struct ButtonConfig {
    uint8_t pin;
    uint8_t controlId;   // 1..168
    bool    activeLow;
};

static const ButtonConfig BUTTONS[] = {
    { 4,  1, true },
    { 5,  2, true },
    { 6,  3, true },
};
static const size_t BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

// --- Линии бинарного ввода (пакет BinaryInput, тип 8, маска до 28 линий) ---
// ControlId = номер линии (1..28), бит маски = controlId - 1
struct BinaryLineConfig {
    uint8_t pin;
    uint8_t controlId;   // 1..28
    bool    activeLow;
};

static const BinaryLineConfig BINARY_LINES[] = {
    { 7,  1, true },
    { 15, 2, true },
    { 16, 3, true },
};
static const size_t BINARY_LINE_COUNT = sizeof(BINARY_LINES) / sizeof(BINARY_LINES[0]);

// --- Энкодеры (пакет Encoder, тип 3) ---
// Квадратура A/B на любых GPIO (прерывания не обязательны, опрос 1 кГц)
struct EncoderConfig {
    uint8_t pinA;
    uint8_t pinB;
    uint8_t controlId;   // 1..14
};

static const EncoderConfig ENCODERS[] = {
    { 17, 18, 1 },
};
static const size_t ENCODER_COUNT = sizeof(ENCODERS) / sizeof(ENCODERS[0]);

// --- Оси (пакет Axis, тип 1) ---
// ControlId 1..8; ВНИМАНИЕ: в пакете байты идут в обратном порядке -
// первая пара байтов [2..3] соответствует ControlId 8, последняя [16..17] - ControlId 1
struct AxisConfig {
    uint8_t adcPin;      // GPIO1..GPIO20 (ADC1) на ESP32-S3
    uint8_t controlId;   // 1..8
    uint16_t minValue;   // калибровка
    uint16_t maxValue;
};

static const AxisConfig AXES[] = {
    { 1, 1, 0, 4095 },
};
static const size_t AXIS_COUNT = sizeof(AXES) / sizeof(AXES[0]);

// --- Лампы (пакет LinearOutput, тип 4: [3]=controlId, [4]=вкл/выкл) ---
// isRgb: true - бортовой WS2812 (RGB_BUILTIN), зажигается через neopixelWrite,
//        обычный digitalWrite на него не действует
struct LampConfig {
    uint8_t pin;
    uint8_t controlId;   // 1..29
    bool    activeLow;   // true - реле/светодиод включаются низким уровнем
    bool    isRgb;
};

static const LampConfig LAMPS[] = {
    { LED_BUILTIN_PIN, 1, false, true },   // бортовой светодиод WS2812
    { 39,              2, false, false },  // внешние линии под реле/LED
    { 40,              3, false, false },
};
static const size_t LAMP_COUNT = sizeof(LAMPS) / sizeof(LAMPS[0]);

// --- Индикатор (пакет Indicator, тип 5, 7 позиций) ---
// Фаза 3: MAX7219. Пока индикатор только принимается и логируется.

// --- Прочее ---
#define DEBOUNCE_MS        10     // антидребезг кнопок
#define INPUT_SCAN_MS      1      // период опроса ввода
#define ENCODER_DETENT     4      // полных квадра-шагов на щелчок
#define ARCC_DEBUG         0      // 1 - отладочный лог в UART0 (второй COM-порт на платах с CH343)
