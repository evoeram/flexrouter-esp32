// wifi_manager.h - state machine Wi-Fi (фаза 2 + этап 2.1 plan_new.md):
// нет креденшелов -> собственный AP (наш веб-сервер на 192.168.4.1);
// есть -> STA; не подключился за N с -> повтор; 3 неудачи -> снова AP.
// В AP поднимается captive-DNS (все имена -> 192.168.4.1), HTTP-редиректы -
// в web_server (onNotFound). WiFiManager больше не используется: портал
// был блокирующим и занимал порт 80, не давая поднять веб-морду в AP.
// «Заводской сброс» по удержанию BOOT-кнопки 5 с или из веба.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>

class WifiMgr {
public:
    enum Mode : uint8_t { MODE_BOOT, MODE_STA_TRY, MODE_STA, MODE_AP };
    static WifiMgr& instance() { static WifiMgr s; return s; }

    void begin();
    void loop();
    Mode mode() const { return _mode; }
    const char* modeStr() const;
    String ip() const;
    String apIp() const { return WiFi.softAPIP().toString(); }
    String ssid() const { return _ssid; }
    int8_t rssi() const { return _mode == MODE_STA ? WiFi.RSSI() : 0; }

    // применить новые креденшелы из веб-конфигуратора
    void reconfigure();
    void factoryReset();

private:
    Mode _mode = MODE_BOOT;
    String _ssid;
    uint32_t _tryStartMs = 0;
    uint8_t _failCount = 0;
    DNSServer _dns;
    bool _dnsUp = false;
    uint32_t _lastApStatMs = 0;

    void startSta();
    void startAp();
    void stopAp();
    void onStaUp();
};
