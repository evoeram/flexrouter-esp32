// arcc_core.h - протокольное ядро ARCC, транспортно-независимое.
// Приём команд ПК (5/10 байт, ресинк по первому байту), отправка 18-байтных пакетов.
#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>
#include "arcc_transport.h"

class ArccCore {
public:
    static ArccCore& instance() { static ArccCore s; return s; }

    void addTransport(ArccTransport* t) { _transports.push_back(t); }
    void begin();                        // begin() всех транспортов
    void netUp();                        // сеть поднята - транспортам можно стартовать TCP
    void loop();                         // вычитать байты из всех транспортов

    // TX API (вызывает arcc_hw и веб-симулятор)
    void sendButton(uint8_t mid, uint8_t controlId, bool pressed);
    void sendBinaryMask(uint8_t mid, uint32_t mask);
    void sendEncoder(uint8_t mid, uint8_t controlId, bool cw);
    void sendAxis(uint8_t mid, const uint16_t values[8]);  // индекс = controlId-1

    bool hostReady() const;              // есть хоть один готовый транспорт
    const char* activeTransportName();   // имя транспорта, готового к отправке

    uint32_t txCount = 0;
    uint32_t rxCount = 0;
    // наблюдатель пакетов (веб-монитор): pkt - 18 байт, tx - направление
    std::function<void(const uint8_t* pkt, bool tx)> onPacket;

private:
    std::vector<ArccTransport*> _transports;
    uint8_t _rx[64];
    size_t _rxLen = 0;

    void feedByte(uint8_t b);
    void handleHostPacket(const uint8_t* pkt, size_t len);
    void sendPacket(const uint8_t* pkt, size_t len);
    void emit(const uint8_t* pkt, bool tx);
};
