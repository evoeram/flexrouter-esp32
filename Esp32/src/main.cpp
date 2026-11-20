// main.cpp - bootstrap (plan.md п. 4, этап 2 plan_new.md):
// конфиг (NVS) -> ARCC-ядро (Serial CDC [+ TCP]) -> hw по карте пинов ->
// Wi-Fi state machine -> веб-сервер (в STA и в AP: в AP наш WebServerMgr
// на 192.168.4.1, WiFiManager-портал больше не используется).
#include <Arduino.h>
#include "debug.h"
#include "config/config_store.h"
#include "log/event_log.h"
#include "arcc/arcc_core.h"
#include "arcc/arcc_hw.h"
#include "arcc/arcc_transport.h"
#include "wifi/wifi_manager.h"
#include "web/web_server.h"

#ifndef DIAG_USE_CDC
HardwareSerial DbgPort(0);   // UART0, 115200 - дебаг-лог (в DIAG-сборке - CDC, см. debug.h)
#endif

#ifdef DIAG_USE_CDC
HWCDC& DbgPort = Serial;
#endif

// BOOT-кнопка: удержание 5 с = заводской сброс (фаза 2)
static const uint8_t FACTORY_BTN_PIN = 0;
static const uint32_t FACTORY_HOLD_MS = 5000;

static void wifiTask(void*) {
    WifiMgr::instance().begin();
    for (;;) {
        WifiMgr::instance().loop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void setup() {
    DbgPort.begin(115200);
    DbgPort.println("\n[boot] FlexRouter ESP32-S3 ARCC " FW_VERSION);

    EventLog::instance().log(LOG_INFO, "boot fw=" FW_VERSION);

    ConfigStore::instance().begin();

    // ARCC-ядро: Serial CDC всегда; TCP - если включён в конфиге
    // (в DIAG-сборке CDC отдан дебаг-логу, ARCC остаётся только по TCP)
#ifndef DIAG_USE_CDC
    static SerialArccTransport serialTransport;
    ArccCore::instance().addTransport(&serialTransport);
#endif
    JsonDocument& cfg = ConfigStore::instance().json();
    if (cfg["transport"]["tcp"] | false) {
        static TcpArccTransport tcpTransport(cfg["transport"]["tcpPort"] | 4001);
        ArccCore::instance().addTransport(&tcpTransport);
    }
    ArccCore::instance().onPacket = [](const uint8_t* pkt, bool tx) {
        WebServerMgr::instance().broadcastPacket(pkt, tx);
    };
    ArccCore::instance().begin();

    ArccHw::instance().begin(cfg);

    pinMode(FACTORY_BTN_PIN, INPUT_PULLUP);

    // Wi-Fi (в т.ч. блокирующий AP-портал) - в отдельной задаче,
    // чтобы ARCC-приём/передача не останавливались во время provisioning
    xTaskCreatePinnedToCore(wifiTask, "wifi", 12288, nullptr, 1, nullptr, 0);

    // Подтверждение жизни прошивки: два зелёных мига на бортовом WS2812.
    // Если GPIO48 назначен лампой в pin map - не мигаем (п. 5.1 plan_new.md):
    // иначе boot-мигание попадёт в ARCC-выход как ложная команда лампы.
    bool pin48IsLamp = false;
    for (JsonObject mod : cfg["modules"].as<JsonArray>()) {
        if (String(mod["type"] | "") != "LinearOutput")
            continue;
        for (JsonPair kv : mod["pins"].as<JsonObject>())
            if ((uint8_t)(kv.value() | 0) == 48)
                pin48IsLamp = true;
    }
    if (!pin48IsLamp) {
        for (int i = 0; i < 2; i++) {
            neopixelWrite(48, 0, 160, 0);
            delay(150);
            neopixelWrite(48, 0, 0, 0);
            delay(150);
        }
    }
    // Веб-сервер поднимается в loop() при первом подключении сети (STA или AP):
    // в AP больше нет портала WiFiManager - порт 80 свободен для нашей морды.
}

void loop() {
    ArccCore::instance().loop();     // приём команд ПК
    ArccHw::instance().loop();       // скан ввода (1 кГц)

    static bool webUp = false;
    if (!webUp && WifiMgr::instance().mode() != WifiMgr::MODE_BOOT) {
        WebServerMgr::instance().begin();
        webUp = true;
    }
    if (webUp)
        WebServerMgr::instance().loop();

    // TCP-транспорт ARCC стартуем, как только поднялась сеть (STA или AP):
    // AsyncServer до инициализации Wi-Fi валит плату (assert в AsyncTCP)
    static bool arccNetUp = false;
    if (!arccNetUp && WifiMgr::instance().mode() != WifiMgr::MODE_BOOT) {
        ArccCore::instance().netUp();
        arccNetUp = true;
    }

    // заводской сброс по удержанию BOOT
    static uint32_t btnDownSince = 0;
    if (digitalRead(FACTORY_BTN_PIN) == LOW) {
        if (btnDownSince == 0)
            btnDownSince = millis();
        else if (millis() - btnDownSince > FACTORY_HOLD_MS) {
            EventLog::instance().log(LOG_INFO, "button: factory reset (hold 5s)");
            WifiMgr::instance().factoryReset();   // сотрёт конфиг + креды, ребут
        }
    } else {
        btnDownSince = 0;
    }
}
