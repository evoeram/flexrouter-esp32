#include "wifi_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "../config/config_store.h"
#include "../log/event_log.h"

static const uint32_t STA_CONNECT_TIMEOUT_MS = 15000;
static const uint8_t  STA_MAX_FAILS = 3;
static const char*    AP_SSID = "FlexRouter-Config";
static const char*    AP_PASS = "flexrouter";

const char* WifiMgr::modeStr() const {
    switch (_mode) {
        case MODE_STA: return "sta";
        case MODE_STA_TRY: return "sta_try";
        case MODE_AP: return "ap";
        default: return "boot";
    }
}

String WifiMgr::ip() const {
    if (_mode == MODE_STA)
        return WiFi.localIP().toString();
    if (_mode == MODE_AP)
        return WiFi.softAPIP().toString();
    return "";
}

void WifiMgr::begin() {
    ConfigStore& cfg = ConfigStore::instance();
    _ssid = cfg.wifiSsid();
    WiFi.persistent(true);
    WiFi.setAutoReconnect(false);
    if (_ssid.length() == 0) {
        EventLog::instance().log(LOG_INFO, "wifi: no credentials -> AP config mode");
        startAp();
    } else {
        startSta();
    }
}

void WifiMgr::startSta() {
    ConfigStore& cfg = ConfigStore::instance();
    JsonDocument& doc = cfg.json();

    stopAp();
    WiFi.mode(WIFI_STA);
    // статический IP, если задан
    const char* ip = doc["wifi"]["ip"] | "";
    if (ip && strlen(ip) > 0) {
        IPAddress local, gw, mask, dns;
        local.fromString(ip);
        gw.fromString((const char*)(doc["wifi"]["gw"] | ""));
        mask.fromString((const char*)(doc["wifi"]["mask"] | "255.255.255.0"));
        dns.fromString((const char*)(doc["wifi"]["dns"] | ""));
        WiFi.config(local, gw, mask, dns);
    }
    WiFi.begin(_ssid.c_str(), (const char*)(doc["wifi"]["pass"] | ""));
    _tryStartMs = millis();
    _mode = MODE_STA_TRY;
    EventLog::instance().log(LOG_INFO, "wifi: connecting to '%s' (attempt %u)", _ssid.c_str(), _failCount + 1);
}

// Собственный AP (этап 2.1 plan_new.md, вариант А): неблокирующий,
// веб-морда поднимается на 192.168.4.1, captive-DNS тянет туда все имена.
void WifiMgr::startAp() {
    stopAp();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    _mode = MODE_AP;
    _dnsUp = _dns.start(53, "*", WiFi.softAPIP());
    EventLog::instance().log(LOG_INFO, "wifi: AP '%s' up, ip=%s (captive dns: %s)",
                             AP_SSID, WiFi.softAPIP().toString().c_str(), _dnsUp ? "on" : "off");
}

void WifiMgr::stopAp() {
    if (_dnsUp) {
        _dns.stop();
        _dnsUp = false;
    }
}

void WifiMgr::loop() {
    switch (_mode) {
        case MODE_STA_TRY:
            if (WiFi.status() == WL_CONNECTED) {
                _failCount = 0;
                _mode = MODE_STA;
                EventLog::instance().log(LOG_INFO, "wifi: connected '%s' ip=%s",
                                         _ssid.c_str(), WiFi.localIP().toString().c_str());
                onStaUp();
            } else if (millis() - _tryStartMs > STA_CONNECT_TIMEOUT_MS) {
                _failCount++;
                if (_failCount >= STA_MAX_FAILS) {
                    EventLog::instance().log(LOG_WARN, "wifi: %u fails -> AP config mode", _failCount);
                    startAp();
                } else {
                    EventLog::instance().log(LOG_WARN, "wifi: connect timeout, retry");
                    startSta();
                }
            }
            break;
        case MODE_STA:
            if (WiFi.status() != WL_CONNECTED) {
                EventLog::instance().log(LOG_WARN, "wifi: disconnected, reconnecting");
                _mode = MODE_STA_TRY;
                _tryStartMs = millis();
                WiFi.disconnect();
                WiFi.begin();
            }
            break;
        case MODE_AP:
            if (_dnsUp)
                _dns.processNextRequest();
            // периодический статус: видно в логе, ассоциируются ли клиенты с AP
            if (millis() - _lastApStatMs >= 10000) {
                _lastApStatMs = millis();
                EventLog::instance().log(LOG_INFO, "ap: stations=%u ip=%s",
                                         WiFi.softAPgetStationNum(), WiFi.softAPIP().toString().c_str());
            }
            break;
        default:
            break;
    }
}

void WifiMgr::onStaUp() {
    String host = ConfigStore::instance().hostname();
    if (MDNS.begin(host.c_str())) {
        MDNS.addService("http", "tcp", 80);
        EventLog::instance().log(LOG_INFO, "mdns: http://%s.local", host.c_str());
    }
}

void WifiMgr::reconfigure() {
    ConfigStore& cfg = ConfigStore::instance();
    String newSsid = cfg.wifiSsid();
    EventLog::instance().log(LOG_INFO, "wifi: reconfigure -> '%s'", newSsid.c_str());
    if (newSsid.length() == 0) {
        factoryReset();
        return;
    }
    _ssid = newSsid;
    _failCount = 0;
    WiFi.disconnect(true);
    delay(100);
    startSta();
}

void WifiMgr::factoryReset() {
    EventLog::instance().log(LOG_INFO, "wifi: factory reset (erase creds), reboot");
    ConfigStore::instance().factoryReset();
    stopAp();
    WiFi.persistent(true);
    WiFi.disconnect(true, true);   // стереть креды из NVS
    delay(200);
    ESP.restart();
}
