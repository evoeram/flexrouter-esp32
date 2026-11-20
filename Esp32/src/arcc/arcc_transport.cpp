#include "arcc_transport.h"
#include "../debug.h"
#include <AsyncTCP.h>

// --------------------------- Serial (USB CDC) ---------------------------

bool SerialArccTransport::begin() {
    Serial.setRxBufferSize(1024);
    Serial.setTxBufferSize(1024);
    // 1250000 - С„РѕСЂРјР°Р»СЊРЅР°СЏ СЃРєРѕСЂРѕСЃС‚СЊ (РјР°СЂРєРµСЂ РїСЂРѕС„РёР»СЏ ARCC), РїРѕ USB РЅРµ РІР°Р¶РЅР°
    Serial.begin(1250000);
    return true;
}

bool SerialArccTransport::ready() {
    if (_ready)
        return true;
    if (Serial)
        _ready = true;
    else if (millis() > HOST_READY_TIMEOUT_MS) {
        _ready = true;
        DbgPort.println("[arcc] serial host assumed ready (timeout)");
    }
    return _ready;
}

// --------------------------- TCP ---------------------------

TcpArccTransport::~TcpArccTransport() {
    delete (AsyncServer*)_server;
}

bool TcpArccTransport::startServer() {
    AsyncServer* srv = new AsyncServer(_port);
    srv->onClient(
        [](void* arg, AsyncClient* c) {
            ((TcpArccTransport*)arg)->onClientStatic(arg, c);
        },
        this);
    srv->begin();
    _server = srv;
    DbgPort.printf("[arcc] tcp server on port %u\n", _port);
    return true;
}

void TcpArccTransport::netUp() {
    if (_serverUp)
        return;
    _serverUp = startServer();
}

void TcpArccTransport::onClientStatic(void* arg, void* client) {
    AsyncClient* c = (AsyncClient*)client;
    if (_client) {
        // С‚РѕР»СЊРєРѕ РѕРґРёРЅ РєР»РёРµРЅС‚ (FlexRouter)
        c->close();
        return;
    }
    _client = c;
    _head = _tail = 0;
    c->onData(
        [](void* arg, AsyncClient* cl, void* data, size_t len) {
            (void)cl;
            ((TcpArccTransport*)arg)->onDataStatic(data, len);
        },
        this);
    auto disc = [](void* arg, AsyncClient* cl) {
        ((TcpArccTransport*)arg)->onDisconnectStatic(cl);
    };
    c->onDisconnect(disc, this);
    c->onError(
        [](void* arg, AsyncClient* cl, int8_t err) {
            (void)err;
            ((TcpArccTransport*)arg)->onDisconnectStatic(cl);
        },
        this);
    DbgPort.printf("[arcc] tcp client connected from %s\n", c->remoteIP().toString().c_str());
}

void TcpArccTransport::onDataStatic(void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        size_t next = (_head + 1) % RING_SIZE;
        if (next == _tail)   // РїРµСЂРµРїРѕР»РЅРµРЅРёРµ - РѕС‚Р±СЂР°СЃС‹РІР°РµРј СЃР°РјС‹Рµ СЃС‚Р°СЂС‹Рµ
            _tail = (_tail + 1) % RING_SIZE;
        _ring[_head] = p[i];
        _head = next;
    }
}

void TcpArccTransport::onDisconnectStatic(void* client) {
    if (_client == client) {
        _client = nullptr;
        DbgPort.println("[arcc] tcp client disconnected");
    }
}

int TcpArccTransport::available() {
    return (_head + RING_SIZE - _tail) % RING_SIZE;
}

int TcpArccTransport::read() {
    if (_head == _tail)
        return -1;
    uint8_t b = _ring[_tail];
    _tail = (_tail + 1) % RING_SIZE;
    return b;
}

size_t TcpArccTransport::write(const uint8_t* data, size_t len) {
    AsyncClient* c = (AsyncClient*)_client;
    if (!c || !c->connected())
        return 0;
    return c->write((const char*)data, len) ? len : 0;
}


