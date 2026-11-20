#include "web_server.h"
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <map>
#include "../config/config_store.h"
#include "../log/event_log.h"
#include "../arcc/arcc_core.h"
#include "../arcc/arcc_hw.h"
#include "../wifi/wifi_manager.h"

// --------------------------- вспомогательные ---------------------------

static void statusJson(JsonDocument& doc) {
    ConfigStore& cfg = ConfigStore::instance();
    WifiMgr& wifi = WifiMgr::instance();
    ArccCore& core = ArccCore::instance();
    doc.clear();
    doc["t"] = "status";
    doc["name"] = cfg.deviceName();
    doc["hostname"] = cfg.hostname();
    doc["fw"] = FW_VERSION;
    doc["mode"] = wifi.modeStr();
    doc["ssid"] = wifi.ssid();
    doc["ip"] = wifi.ip();
    doc["rssi"] = wifi.rssi();
    doc["heap"] = (uint32_t)ESP.getFreeHeap();
    doc["minHeap"] = (uint32_t)ESP.getMinFreeHeap();
    doc["uptime"] = millis() / 1000;
    doc["host"] = core.hostReady();
    doc["transport"] = core.activeTransportName();
    doc["tx"] = core.txCount;
    doc["rx"] = core.rxCount;
}

// Журнал декодированного пакета для веб-монитора
static void packetJson(JsonDocument& doc, const uint8_t* p, bool tx) {
    doc.clear();
    doc["t"] = "pkt";
    doc["dir"] = tx ? "tx" : "rx";
    String hex;
    hex.reserve(ARCC_PACKET_TX_SIZE * 2);
    char b[3];
    for (int i = 0; i < ARCC_PACKET_TX_SIZE; i++) {
        snprintf(b, sizeof(b), "%02X", p[i]);
        hex += b;
    }
    doc["hex"] = hex;
    switch (p[0]) {
        case ARCC_BUTTON: {
            doc["mod"] = "button";
            doc["ctl"] = p[16];
            doc["on"] = p[17] == 1;
            break;
        }
        case ARCC_BINARY_INPUT: {
            doc["mod"] = "binary";
            char m[9];
            snprintf(m, sizeof(m), "%02X%02X%02X%02X", p[14], p[15], p[16], p[17]);
            doc["mask"] = String("0x") + m;
            break;
        }
        case ARCC_ENCODER: {
            doc["mod"] = "encoder";
            doc["ctl"] = p[15];
            doc["clicks"] = p[16];
            doc["cw"] = p[17] == 1;
            break;
        }
        case ARCC_AXIS: {
            doc["mod"] = "axis";
            JsonArray vals = doc["vals"].to<JsonArray>();
            for (int j = 0; j < 8; j++)
                vals.add(p[2 + j * 2] * 256 + p[2 + j * 2 + 1]);
            break;
        }
        case ARCC_LINEAR_OUT: {
            doc["mod"] = "lamp";
            doc["ctl"] = p[3];
            doc["on"] = p[4] == 1;
            break;
        }
        case ARCC_INDICATOR: {
            doc["mod"] = "indicator";
            String text;
            for (int i = 0; i < ARCC_INDICATOR_POSITIONS; i++)
                text += arccIndicatorCodeToChar(p[3 + i]);
            doc["text"] = text;
            break;
        }
        default:
            doc["mod"] = "unknown";
            break;
    }
}

// Симуляция событий ввода (веб -> плата): одинакова для /api/sim и WS {"t":"sim"}
static void handleSim(JsonDocument& doc) {
    ArccHw& hw = ArccHw::instance();
    String cmd = doc["cmd"] | "";
    if (cmd == "lamp")
        hw.setLamp(doc["ctl"] | 0, doc["on"] | false);
    else if (cmd == "button")
        hw.simButton(doc["ctl"] | 0, doc["pressed"] | false);
    else if (cmd == "encoder")
        hw.simEncoder(doc["ctl"] | 0, doc["cw"] | false);
    else if (cmd == "axis")
        hw.simAxis(doc["ctl"] | 0, doc["value"] | 0);
    else if (cmd == "indicator")
        hw.setIndicatorText((const char*)(doc["text"] | ""));
}

static void sendJson(AsyncWebServerRequest* req, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// --------------------------- Валидация карты пинов (этап 2.2) ---------------------------

// Зарезервированные GPIO: 0 (BOOT/strap + заводская кнопка), 19/20 (USB D-/D+),
// 26-37 (flash/PSRAM), 43/44 (UART0), 45/46 (strapping). GPIO48 - бортовой WS2812,
// разрешён как лампа (boot-мигание отключается, если 48 входит в pin map).
static bool isReservedPin(uint8_t pin) {
    if (pin == 0 || pin == 19 || pin == 20 || pin == 43 || pin == 44 || pin == 45 || pin == 46)
        return true;
    return pin >= 26 && pin <= 37;
}

// ADC-пины (GPIO1..20); с включённым Wi-Fi надёжны ADC1 (GPIO1..10)
static bool isAdcPin(uint8_t pin) { return pin >= 1 && pin <= 20; }

// Человекочитаемый controlId из ключа pins ("1a" -> "1a")
static String ctlFromKey(const char* key) { return String(key); }

// Базовый controlId из ключа pins ("1a"/"1b" -> 1)
static int baseCtlFromKey(const char* key) { return atoi(key); }

// Валидация секции modules перед сохранением. Пустая строка - ОК,
// иначе текст ошибки (показывается в веб-морде как есть).
static String validateModules(JsonArray modules) {
    if (modules.isNull())
        return "modules: должен быть массив";
    std::map<uint8_t, String> pinOwner;
    int modIndex = -1;
    for (JsonObject mod : modules) {
        modIndex++;
        String type = mod["type"] | "";
        int id = mod["id"] | -1;
        String modName = type + " id=" + String(id);
        if (type != "Button" && type != "BinaryInput" && type != "Encoder" &&
            type != "Axis" && type != "LinearOutput" && type != "Indicator" &&
            type != "ShiftIn" && type != "ShiftOut")
            return modName + ": неизвестный тип '" + type + "'";
        if (id < 0 || id > 255)
            return modName + ": id должен быть 0..255";
        if (type == "Indicator")
            continue;   // пинов не имеет (кроме max7219 cs/sck/mosi - вне pins)

        if (type == "ShiftIn" || type == "ShiftOut") {
            // пины data/clk/latch + карта бит controlId -> бит каскада
            JsonObject pins = mod["pins"].as<JsonObject>();
            for (const char* k : { "data", "clk", "latch" }) {
                int pin = pins[k] | -1;
                if (pin < 0 || pin > 48)
                    return modName + ": нет пина " + k;
                if (isReservedPin((uint8_t)pin))
                    return modName + " " + k + ": GPIO" + String(pin) + " зарезервирован";
                String owner = modName + " " + k;
                auto it = pinOwner.find((uint8_t)pin);
                if (it != pinOwner.end())
                    return "конфликт GPIO" + String(pin) + ": " + it->second + " и " + owner;
                pinOwner[(uint8_t)pin] = owner;
            }
            int chips = mod["chips"] | 1;
            if (chips < 1 || chips > 4)
                return modName + ": chips должен быть 1..4";
            std::map<int, bool> seenCtl, seenBit;
            for (JsonPair kv : mod["map"].as<JsonObject>()) {
                int ctl = atoi(kv.key().c_str());
                if (ctl < 1 || ctl > 255)
                    return modName + ": плохой controlId '" + kv.key().c_str() + "'";
                if (seenCtl[ctl]) return modName + ": дубль controlId '" + String(kv.key().c_str()) + "'";
                seenCtl[ctl] = true;
                int bit = kv.value() | -1;
                if (bit < 0 || bit >= chips * 8)
                    return modName + " ctl " + String(ctl) + ": бит " + String(bit) +
                           " вне каскада (0.." + String(chips * 8 - 1) + ")";
                if (seenBit[bit]) return modName + ": бит " + String(bit) + " назначен дважды";
                seenBit[bit] = true;
            }
            continue;
        }

        JsonObject pins = mod["pins"].as<JsonObject>();
        if (pins.isNull() || pins.size() == 0)
            return modName + ": нет pins";
        std::map<int, bool> seenCtlA, seenCtlB;
        for (JsonPair kv : pins) {
            const char* key = kv.key().c_str();
            int ctl = baseCtlFromKey(key);
            if (ctl < 1 || ctl > 255)
                return modName + ": плохой controlId '" + key + "'";
            if (type == "Encoder") {
                const char* suffix = key + strlen(key) - 1;
                if (*suffix == 'a') {
                    if (seenCtlA[ctl]) return modName + ": дубль controlId '" + key + "'";
                    seenCtlA[ctl] = true;
                } else if (*suffix == 'b') {
                    if (seenCtlB[ctl]) return modName + ": дубль controlId '" + key + "'";
                    seenCtlB[ctl] = true;
                } else
                    return modName + ": ключ энкодера должен быть 'Na'/'Nb' ('" + key + "')";
            } else {
                if (seenCtlA[ctl]) return modName + ": дубль controlId '" + key + "'";
                seenCtlA[ctl] = true;
            }

            int pin = kv.value() | -1;
            if (pin < 0 || pin > 48)
                return modName + " ctl " + key + ": пин " + String(pin) + " вне диапазона 0..48";
            if (isReservedPin((uint8_t)pin))
                return modName + " ctl " + key + ": GPIO" + String(pin) + " зарезервирован (USB/flash/UART0/strap)";
            String owner = modName + " ctl " + key;
            auto it = pinOwner.find((uint8_t)pin);
            if (it != pinOwner.end())
                return "конфликт GPIO" + String(pin) + ": " + it->second + " и " + owner;
            pinOwner[(uint8_t)pin] = owner;
        }
        if (type == "Encoder") {
            for (auto& c : seenCtlA)
                if (!seenCtlB[c.first])
                    return modName + ": энкодер ctl " + String(c.first) + " без второй фазы (Nb)";
            for (auto& c : seenCtlB)
                if (!seenCtlA[c.first])
                    return modName + ": энкодер ctl " + String(c.first) + " без фазы (Na)";
        }
        if (type == "Axis") {
            for (auto& c : seenCtlA) {
                // пин оси уже проверен на диапазон; здесь - что это ADC
                for (JsonPair kv : pins) {
                    if (baseCtlFromKey(kv.key().c_str()) != c.first)
                        continue;
                    int pin = kv.value() | -1;
                    if (!isAdcPin((uint8_t)pin))
                        return modName + " ctl " + String(c.first) + ": GPIO" + String(pin) +
                               " не ADC (для оси нужен GPIO1..20)";
                }
            }
        }
    }
    return "";
}

// Карта пинов для редактора: зарезервированные GPIO, ADC-пины, уже занятые пины
static void pinsAvailableJson(JsonDocument& doc) {
    doc.clear();
    JsonArray reserved = doc["reserved"].to<JsonArray>();
    for (uint8_t p = 0; p <= 48; p++)
        if (isReservedPin(p))
            reserved.add(p);
    JsonArray adc = doc["adc"].to<JsonArray>();
    for (uint8_t p = 1; p <= 20; p++)
        adc.add(p);
    doc["maxPin"] = 48;
    JsonArray used = doc["used"].to<JsonArray>();
    for (JsonObject mod : ConfigStore::instance().json()["modules"].as<JsonArray>()) {
        String type = mod["type"] | "";
        int id = mod["id"] | 0;
        for (JsonPair kv : mod["pins"].as<JsonObject>()) {
            JsonObject u = used.add<JsonObject>();
            u["pin"] = (int)(kv.value() | -1);
            u["type"] = type;
            u["id"] = id;
            u["ctl"] = ctlFromKey(kv.key().c_str());
        }
    }
}

// --------------------------- WebSocket ---------------------------

static void onWsEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client, AwsEventType type,
                      void* arg, uint8_t* data, size_t len) {
    WebServerMgr& mgr = WebServerMgr::instance();
    if (type == WS_EVT_CONNECT) {
        mgr._wsCount = ws->count();
        // снапшот новому клиенту: статус + io + журнал
        {
            JsonDocument doc;
            statusJson(doc);
            String out;
            serializeJson(doc, out);
            client->text(out);
        }
        {
            JsonDocument doc;
            doc.clear();
            doc["t"] = "io";
            // ioStateJson возвращает готовый объект - обернём
            String io = ArccHw::instance().ioStateJson();
            String out = "{\"t\":\"io\"," + io.substring(1);
            client->text(out);
        }
        EventLog::instance().since(0, [&](const LogEntry& e) {
            String out = "{\"t\":\"log\",\"seq\":" + String(e.seq) + ",\"ms\":" + String(e.ms) +
                         ",\"lvl\":" + String(e.level) + ",\"msg\":\"" + e.text + "\"}";
            client->text(out);
        });
    } else if (type == WS_EVT_DISCONNECT) {
        mgr._wsCount = ws->count();
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT))
            return;
        JsonDocument doc;
        if (deserializeJson(doc, data, len))
            return;
        String t = doc["t"] | "";
        if (t == "sim")
            handleSim(doc);
        else if (t == "status") {
            statusJson(doc);
            String out;
            serializeJson(doc, out);
            client->text(out);
        }
    }
}

// --------------------------- WebServerMgr ---------------------------

void WebServerMgr::begin() {
    // basePath - штатный "/littlefs"; метка раздела - "lfs" (см. partitions.csv),
    // по умолчанию LittleFS ищет раздел с меткой "spiffs" и не находит его
    if (!LittleFS.begin(true, "/littlefs", 10, "lfs")) {
        EventLog::instance().log(LOG_ERR, "LittleFS mount failed, try format");
        LittleFS.format();
        if (LittleFS.begin(false, "/littlefs", 10, "lfs"))
            EventLog::instance().log(LOG_INFO, "LittleFS formatted and mounted");
        else
            EventLog::instance().log(LOG_ERR, "LittleFS format+mount failed");
    }
    if (LittleFS.exists("/index.html"))
        EventLog::instance().log(LOG_INFO, "LittleFS ok: %u/%u bytes",
                                 (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());

    AsyncWebServer* server = new AsyncWebServer(80);
    AsyncWebSocket* ws = new AsyncWebSocket("/ws");
    ws->onEvent(onWsEvent);
    server->addHandler(ws);
    _ws = ws;

    // статика + «красивые» адреса страниц
    server->rewrite("/", "/index.html");
    server->rewrite("/config", "/config.html");
    server->rewrite("/debug", "/debug.html");
    server->rewrite("/admin", "/admin.html");
    server->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server->onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) {
            req->send(200);
            return;
        }
        // Captive-редирект в AP (этап 2.1): неизвестные адреса - включая пробы ОС
        // (/generate_204, /hotspot-detect.html, /connecttest.txt, /fwlink) -
        // ведут на нашу морду, чтобы окно «входа в сеть» открывало конфигуратор.
        if (WifiMgr::instance().mode() == WifiMgr::MODE_AP) {
            req->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
            return;
        }
        req->send(404, "text/plain", "Not found");
    });

    // ---- API: статус ----
    server->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        statusJson(doc);
        sendJson(req, doc);
    });

    // ---- API: io-состояние ----
    server->on("/api/io", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", ArccHw::instance().ioStateJson());
    });

    // ---- API: конфиг ----
    server->on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        String out;
        serializeJson(ConfigStore::instance().json(), out);
        req->send(200, "application/json", out);
    });

    static String cfgBody;
    server->on(
        "/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            JsonDocument body;
            DeserializationError err = deserializeJson(body, cfgBody);
            cfgBody = "";
            if (err) {
                req->send(400, "application/json",
                          String("{\"ok\":false,\"error\":\"json: ") + err.c_str() + "\"}");
                return;
            }
            ConfigStore& cfg = ConfigStore::instance();
            JsonDocument& doc = cfg.json();

            // Валидация карты пинов до сохранения (этап 2.2):
            // конфликты GPIO, дубль controlId, зарезервированные/невалидные пины
            if (!body["modules"].isNull()) {
                String err = validateModules(body["modules"].as<JsonArray>());
                if (err.length()) {
                    req->send(400, "application/json",
                              String("{\"ok\":false,\"error\":\"") + err + "\"}");
                    return;
                }
            }

            String wifiBefore, wifiAfter;
            serializeJson(doc["wifi"], wifiBefore);

            bool changed = false;
            for (const char* key : { "device", "wifi", "modules", "io", "transport" }) {
                if (!body[key].isNull()) {
                    doc[key] = body[key];
                    changed = true;
                }
            }
            if (!changed) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"empty config\"}");
                return;
            }
            cfg.save();
            serializeJson(doc["wifi"], wifiAfter);

            bool wifiChanged = wifiBefore != wifiAfter;
            if (wifiChanged)
                WebServerMgr::instance().requestWifiReconfig();

            bool rebootNeeded = false;
            if (!wifiChanged)
                rebootNeeded = true;
            else {
                // изменилось что-то кроме wifi (модули/имя/транспорт) - тоже ребут
                for (const char* key : { "device", "modules", "io", "transport" })
                    if (!body[key].isNull())
                        rebootNeeded = true;
            }
            if (rebootNeeded)
                WebServerMgr::instance().requestReboot();

            req->send(200, "application/json",
                      rebootNeeded ? "{\"ok\":true,\"reboot\":true}"
                                   : "{\"ok\":true,\"reconnect\":true}");
        },
        NULL,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            (void)req; (void)total;
            if (index == 0)
                cfgBody = "";
            cfgBody.concat((const char*)data, len);
        });

    // ---- API: сканирование Wi-Fi (выбор сети из AP-морды, этап 2.1) ----
    server->on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) {
            // в AP сканирование требует AP_STA; асинхронно, результат - следующим запросом
            if (WifiMgr::instance().mode() == WifiMgr::MODE_AP)
                WiFi.mode(WIFI_AP_STA);
            WiFi.scanNetworks(true /* async */);
            n = WIFI_SCAN_RUNNING;
        }
        JsonDocument doc;
        JsonArray arr = doc["scan"].to<JsonArray>();
        if (n == WIFI_SCAN_RUNNING) {
            doc["busy"] = true;
        } else {
            doc["busy"] = false;
            for (int i = 0; i < n; i++) {
                JsonObject net = arr.add<JsonObject>();
                net["ssid"] = WiFi.SSID(i);
                net["rssi"] = WiFi.RSSI(i);
                net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
            WiFi.scanDelete();
            // после сканирования вернуться в чистый AP (скан требует AP_STA)
            if (WifiMgr::instance().mode() == WifiMgr::MODE_AP)
                WiFi.mode(WIFI_AP);
        }
        sendJson(req, doc);
    });

    // ---- API: карта пинов для редактора (этап 2.2) ----
    server->on("/api/pins/available", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        pinsAvailableJson(doc);
        sendJson(req, doc);
    });

    // ---- API: сырое чтение ADC (кнопка «считать сейчас» в калибровке оси) ----
    server->on("/api/adc/read", HTTP_GET, [](AsyncWebServerRequest* req) {
        int pin = req->hasParam("pin") ? atoi(req->getParam("pin")->value().c_str()) : -1;
        if (!isAdcPin((uint8_t)pin)) {
            req->send(400, "application/json", "{\"error\":\"pin must be ADC GPIO1..20\"}");
            return;
        }
        analogReadResolution(12);
        // два чтения: первое после переключения мультиплексора бывает шумным
        analogRead(pin);
        delayMicroseconds(100);
        uint16_t raw = analogRead(pin);
        JsonDocument doc;
        doc["pin"] = pin;
        doc["raw"] = raw;
        sendJson(req, doc);
    });

    // ---- API: журнал событий ----
    server->on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req) {
        uint32_t since = 0;
        if (req->hasParam("since"))
            since = atoi(req->getParam("since")->value().c_str());
        String out = "{\"events\":[";
        bool first = true;
        EventLog::instance().since(since, [&](const LogEntry& e) {
            if (!first) out += ",";
            first = false;
            out += "{\"seq\":" + String(e.seq) + ",\"ms\":" + String(e.ms) +
                   ",\"lvl\":" + String(e.level) + ",\"msg\":\"" + e.text + "\"}";
        });
        out += "]}";
        req->send(200, "application/json", out);
    });

    // ---- API: симуляция ----
    static String simBody;
    server->on(
        "/api/sim", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, simBody);
            simBody = "";
            if (err) {
                req->send(400, "application/json", "{\"ok\":false}");
                return;
            }
            handleSim(doc);
            req->send(200, "application/json", "{\"ok\":true}");
        },
        NULL,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            (void)req; (void)total;
            if (index == 0)
                simBody = "";
            simBody.concat((const char*)data, len);
        });

    // ---- API: админ ----
    server->on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        WebServerMgr::instance().requestReboot();
    });
    server->on("/api/factory_reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        // сброс + стирание кред Wi-Fi -> после ребута снова AP-портал
        WifiMgr::instance().factoryReset();
    });

    // ---- OTA (фаза 6) ----
    server->on(
        "/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp =
                req->beginResponse(ok ? 200 : 500, "text/plain", ok ? "OK" : Update.errorString());
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok)
                WebServerMgr::instance().requestReboot();
        },
        [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len,
           bool final) {
            if (index == 0) {
                Update.begin(UPDATE_SIZE_UNKNOWN);
                EventLog::instance().log(LOG_INFO, "ota: start %s", filename.c_str());
            }
            if (len)
                Update.write(data, len);
            if (final) {
                bool ok = Update.end(true);
                EventLog::instance().log(ok ? LOG_INFO : LOG_ERR, "ota: %s (%u bytes)",
                                         ok ? "success, reboot" : Update.errorString(),
                                         (unsigned)(index + len));
            }
        });

    server->begin();
    _server = server;
    EventLog::instance().log(LOG_INFO, "web: started on port 80 (%s, mode %s)",
                             WifiMgr::instance().ip().c_str(),
                             WifiMgr::instance().modeStr());
}

void WebServerMgr::loop() {
    AsyncWebSocket* ws = (AsyncWebSocket*)_ws;
    if (!ws)
        return;
    ws->cleanupClients();

    if (_rebootAtMs && millis() >= _rebootAtMs) {
        EventLog::instance().log(LOG_INFO, "system: reboot");
        delay(100);
        ESP.restart();
    }
    if (_wifiReconfig) {
        _wifiReconfig = false;
        WifiMgr::instance().reconfigure();
    }

    // heartbeat статуса каждые 2 с
    if (millis() - _lastStatusMs >= 2000) {
        _lastStatusMs = millis();
        if (ws->count() > 0) {
            JsonDocument doc;
            statusJson(doc);
            String out;
            serializeJson(doc, out);
            ws->textAll(out);
        }
    }
    // новые записи журнала
    EventLog::instance().since(_lastLogSeq, [&](const LogEntry& e) {
        _lastLogSeq = e.seq;
        String out = "{\"t\":\"log\",\"seq\":" + String(e.seq) + ",\"ms\":" + String(e.ms) +
                     ",\"lvl\":" + String(e.level) + ",\"msg\":\"" + e.text + "\"}";
        ws->textAll(out);
    });
    // изменения io
    if (ArccHw::instance().consumeIoDirty() && ws->count() > 0) {
        String io = ArccHw::instance().ioStateJson();
        String out = "{\"t\":\"io\"," + io.substring(1);
        ws->textAll(out);
    }
}

void WebServerMgr::broadcastPacket(const uint8_t* pkt, bool tx) {
    AsyncWebSocket* ws = (AsyncWebSocket*)_ws;
    if (!ws || ws->count() == 0)
        return;
    // Очередь WS не бесконечна: burst дампа (168 пакетов) при медленном браузере
    // разрастал очередь сообщений на клиента до OOM и валил плату. Если клиенты
    // не успевают - пакет в монитор не попадает (протокол продолжает работать).
    if (!ws->availableForWriteAll())
        return;
    static JsonDocument doc;   // переиспользуемый: без аллокаций на каждый пакет
    packetJson(doc, pkt, tx);
    String out;
    serializeJson(doc, out);
    ws->textAll(out);
}
