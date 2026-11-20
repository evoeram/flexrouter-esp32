#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Конфиг в NVS (Preferences, namespace "arcc", ключ "cfg") в виде JSON.
// Формат см. в README проекта и в plan.md п. 4.
class ConfigStore {
public:
    static ConfigStore& instance();

    void begin();            // загрузка из NVS или дефолт
    bool save();             // doc -> NVS
    void factoryReset();     // стереть наш конфиг (креды Wi-Fi чистит WifiMgr)

    JsonDocument& json() { return _doc; }

    String deviceName() const { return _doc["device"]["name"] | "flexrouter-esp32"; }
    String hostname() const { return _doc["device"]["hostname"] | "flexrouter"; }
    String wifiSsid() const { return _doc["wifi"]["ssid"] | ""; }
    bool wifiConfigured() const { return _doc["wifi"]["ssid"] | "" != ""; }

private:
    ConfigStore() = default;
    JsonDocument _doc;
};

static const char* CONFIG_DEFAULT_JSON =
R"rawliteral({
  "device": { "name": "flexrouter-esp32", "hostname": "flexrouter" },
  "wifi": { "ssid": "", "pass": "", "ip": "", "gw": "", "mask": "", "dns": "" },
  "modules": [
    { "id": 0, "type": "LinearOutput", "pins": { "1": 48, "2": 39, "3": 40 } },
    { "id": 0, "type": "Indicator", "driver": "none" },
    { "id": 1, "type": "Button", "activeLow": true, "pins": { "1": 4, "2": 5, "3": 6 } },
    { "id": 1, "type": "BinaryInput", "activeLow": true, "pins": { "1": 7, "2": 15, "3": 16 } },
    { "id": 1, "type": "Encoder", "pins": { "1a": 17, "1b": 18 } },
    { "id": 1, "type": "Axis", "pins": { "1": 1 }, "cal": { "1": { "min": 0, "max": 4095 } } }
  ],
  "io": { "buttonDebounceMs": 10, "axisThreshold": 4, "encoderDetent": 4 },
  "transport": { "tcp": false, "tcpPort": 4001 }
})rawliteral";
