// web_server.h - веб-морда (фазы 4-6 + этап 2 plan_new.md): статика LittleFS,
// JSON API, WebSocket (/ws) для live-дебага, OTA (/update).
// Поднимается и в STA, и в AP (в AP наш сервер на 192.168.4.1 + captive-редиректы).
#pragma once
#include <Arduino.h>

#define FW_VERSION "0.7.0"

class WebServerMgr {
public:
    static WebServerMgr& instance() { static WebServerMgr s; return s; }

    void begin();
    void loop();

    // live-лента ARCC-пакетов в /debug (вызывается из ArccCore::onPacket)
    void broadcastPacket(const uint8_t* pkt, bool tx);

    size_t wsClients() const { return _wsCount; }
    void requestReboot() { _rebootAtMs = millis() + 1000; }
    void requestWifiReconfig() { _wifiReconfig = true; }

    size_t _wsCount = 0;
    uint32_t _rebootAtMs = 0;
    bool _wifiReconfig = false;

private:
    void* _server = nullptr;     // AsyncWebServer*
    void* _ws = nullptr;         // AsyncWebSocket*
    uint32_t _lastStatusMs = 0;
    uint32_t _lastLogSeq = 0;
};
