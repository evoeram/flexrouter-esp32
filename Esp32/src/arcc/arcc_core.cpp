#include "arcc_core.h"
#include "arcc_hw.h"
#include "../log/event_log.h"

void ArccCore::begin() {
    for (auto* t : _transports)
        t->begin();
}

void ArccCore::netUp() {
    for (auto* t : _transports)
        t->netUp();
}

void ArccCore::loop() {
    for (auto* t : _transports) {
        int n = t->available();
        while (n-- > 0) {
            int b = t->read();
            if (b >= 0) {
                rxCount++;
                feedByte((uint8_t)b);
            }
        }
    }
}

// Потоковый разбор ПК -> плата: пакеты 5/10 байт, ресинк по первому байту.
// «Призрачный» байт 0x00 в начале потока отбрасывается сам (невалидный первый байт).
void ArccCore::feedByte(uint8_t b) {
    if (_rxLen >= sizeof(_rx)) {
        _rxLen = 0;   // переполнение - сброс и ресинк
    }
    _rx[_rxLen++] = b;
    for (;;) {
        if (_rxLen == 0)
            break;
        uint8_t need = arccHostPacketLength(_rx[0]);
        if (need == 0) {
            memmove(_rx, _rx + 1, _rxLen - 1);
            _rxLen--;
            continue;
        }
        if (_rxLen < need)
            break;
        handleHostPacket(_rx, need);
        memmove(_rx, _rx + need, _rxLen - need);
        _rxLen -= need;
    }
}

void ArccCore::handleHostPacket(const uint8_t* pkt, size_t len) {
    emit(pkt, false);
    switch (pkt[0]) {
        case ARCC_LINEAR_OUT:
            if (len >= 5 && pkt[2] == LAMP_SET_STATE)
                ArccHw::instance().setLamp(pkt[3], pkt[4] == 1);
            break;
        case ARCC_INDICATOR:
            if (len >= 10 && pkt[2] == INDICATOR_SET_TEXT)
                ArccHw::instance().setIndicator(pkt + 3);
            break;
        case ARCC_BUTTON:
            if (len >= 5 && pkt[2] == DUMP_ALL_KEYS)
                ArccHw::instance().dumpAllKeys(pkt[1]);
            break;
        case ARCC_BINARY_INPUT:
            if (len >= 5 && pkt[2] == DUMP_ALL_LINES)
                ArccHw::instance().dumpAllLines(pkt[1]);
            break;
        default:
            break;
    }
}

void ArccCore::sendPacket(const uint8_t* pkt, size_t len) {
    bool sent = false;
    for (auto* t : _transports) {
        if (t->ready()) {
            t->write(pkt, len);
            sent = true;
        }
    }
    if (sent)
        txCount++;
    emit(pkt, true);
}

void ArccCore::emit(const uint8_t* pkt, bool tx) {
    if (onPacket)
        onPacket(pkt, tx);
}

void ArccCore::sendButton(uint8_t mid, uint8_t controlId, bool pressed) {
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeButton(pkt, mid, controlId, pressed);
    sendPacket(pkt, sizeof(pkt));
}

void ArccCore::sendBinaryMask(uint8_t mid, uint32_t mask) {
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeBinaryInput(pkt, mid, mask);
    sendPacket(pkt, sizeof(pkt));
}

void ArccCore::sendEncoder(uint8_t mid, uint8_t controlId, bool cw) {
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeEncoder(pkt, mid, controlId, 1, cw);
    sendPacket(pkt, sizeof(pkt));
}

void ArccCore::sendAxis(uint8_t mid, const uint16_t values[8]) {
    uint8_t pkt[ARCC_PACKET_TX_SIZE];
    arccMakeAxis(pkt, mid, values);
    sendPacket(pkt, sizeof(pkt));
}

bool ArccCore::hostReady() const {
    for (auto* t : _transports)
        if (t->ready())
            return true;
    return false;
}

const char* ArccCore::activeTransportName() {
    for (auto* t : _transports)
        if (t->ready())
            return t->name();
    return "none";
}
