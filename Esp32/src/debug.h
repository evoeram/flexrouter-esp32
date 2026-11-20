#pragma once
#include <Arduino.h>

#ifdef DIAG_USE_CDC
// Диагностическая сборка: дебаг-лог идёт на USB-CDC (COM-порт ПК, 115200),
// ARCC-транспорт по CDC при этом отключён (см. main.cpp). Для отладки Wi-Fi/AP.
extern HWCDC& DbgPort;
#else
// Дебаг-порт: UART0 (GPIO43/44), 115200. USB-CDC занят протоколом ARCC,
// поэтому лог идёт на второй порт платы (или пины TX0/RX0).
#include <HardwareSerial.h>
extern HardwareSerial DbgPort;
#endif
