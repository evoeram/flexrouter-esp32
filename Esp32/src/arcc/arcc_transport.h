// arcc_transport.h - транспорт ARCC: Serial (USB-CDC) и опционально TCP (LAN).
// Протокольное ядро (arcc_core) не знает, откуда идут байты.
#pragma once
#include <Arduino.h>

class ArccTransport {
public:
    virtual ~ArccTransport() {}
    virtual const char* name() const = 0;
    virtual bool begin() = 0;
    virtual bool ready() = 0;              // линк/порт открыт, писать можно
    // Сеть поднята (Wi-Fi STA/AP активен). Транспортам, зависящим от стека TCP
    // (AsyncServer), можно отложить begin() до этого момента - иначе assert в AsyncTCP.
    virtual void netUp() {}
    virtual int available() = 0;
    virtual int read() = 0;
    virtual size_t write(const uint8_t* data, size_t len) = 0;
};

// USB CDC (USB-Serial-JTAG). Скорость формальная (1250000), реальный поток - по USB.
// Готовность хоста: у CDC нет настоящего DTR, ориентируемся на Serial.connected()
// либо на таймаут 2.5 с (DTR-импульс роутера - 1 с, порт к этому моменту точно открыт).
class SerialArccTransport : public ArccTransport {
public:
    const char* name() const override { return "serial"; }
    bool begin() override;
    bool ready() override;
    int available() override { return Serial.available(); }
    int read() override { return Serial.read(); }
    size_t write(const uint8_t* data, size_t len) override { return Serial.write(data, len); }

private:
    bool _ready = false;
    static const uint32_t HOST_READY_TIMEOUT_MS = 2500;
};

// TCP-сервер (фаза 7, вариант B): один клиент, порт из конфига (по умолчанию 4001).
// FlexRouter подключается по LAN и говорит те же пакеты ARCC.
class TcpArccTransport : public ArccTransport {
public:
    explicit TcpArccTransport(uint16_t port) : _port(port) {}
    ~TcpArccTransport() override;
    const char* name() const override { return "tcp"; }
    // begin() отложен: AsyncServer нельзя поднимать до старта Wi-Fi (crash в AsyncTCP)
    bool begin() override { return true; }
    void netUp() override;
    bool ready() override { return _client != nullptr; }
    int available() override;
    int read() override;
    size_t write(const uint8_t* data, size_t len) override;

private:
    static const size_t RING_SIZE = 4096;
    uint16_t _port;
    void* _server = nullptr;   // AsyncServer*
    void* _client = nullptr;   // AsyncClient*
    bool _serverUp = false;
    uint8_t _ring[RING_SIZE];
    volatile size_t _head = 0, _tail = 0;

    bool startServer();
    void onClientStatic(void* arg, void* client);
    void onDataStatic(void* data, size_t len);
    void onDisconnectStatic(void* client);
};
