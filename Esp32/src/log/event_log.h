#pragma once
#include <Arduino.h>
#include "../debug.h"

enum : uint8_t { LOG_INFO = 0, LOG_WARN = 1, LOG_ERR = 2 };

struct LogEntry {
    uint32_t seq;
    uint32_t ms;
    uint8_t level;
    char text[80];
};

// Кольцевой буфер событий: Wi-Fi, конфиг, ошибки, значимые действия ARCC.
// ARCC-пакеты сюда не пишутся (их live-лента идёт через WebSocket в /debug).
class EventLog {
public:
    static EventLog& instance() { static EventLog s; return s; }

    void begin() {}

    void log(uint8_t level, const char* fmt, ...) {
        LogEntry& e = _ring[_head];
        e.seq = ++_seq;
        e.ms = millis();
        e.level = level;
        va_list args;
        va_start(args, fmt);
        vsnprintf(e.text, sizeof(e.text), fmt, args);
        va_end(args);
        _head = (_head + 1) % CAP;
        if (_count < CAP) _count++;
        if (_serialEcho) {
            DbgPort.printf("[%10lu] %s\n", (unsigned long)e.ms, e.text);
        }
    }

    template <typename F>
    void since(uint32_t sinceSeq, F&& cb) const {
        if (_count == 0) return;
        for (size_t i = 0; i < _count; i++) {
            const LogEntry& e = _ring[( _head + CAP - _count + i) % CAP];
            if (e.seq > sinceSeq) cb(e);
        }
    }

private:
    static const size_t CAP = 128;
    LogEntry _ring[CAP] = {};
    size_t _head = 0, _count = 0;
    uint32_t _seq = 0;
public:
    bool _serialEcho = true;
};

