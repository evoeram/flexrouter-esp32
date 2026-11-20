#!/usr/bin/env python3
# arcc_harness.py - тестер протокола ARCC на стороне ПК (фаза 3, п. 8 plan.md).
# Имитирует то, что шлёт FlexRouter (ArccHardwareDataProcessor), и декодирует
# 18-байтные пакеты от платы.
#
# Примеры:
#   python arcc_harness.py --port COM5 watch
#   python arcc_harness.py --host 192.168.4.1 watch          # по TCP (transport.tcp: true)
#   python arcc_harness.py --port COM5 lamp 1 on
#   python arcc_harness.py --host 192.168.4.1 lamp 1 on
#   python arcc_harness.py --port COM5 indicator "12.5"
#   python arcc_harness.py --port COM5 dump-keys 1
#   python arcc_harness.py --port COM5 dump-lines 1
#   python arcc_harness.py --port COM5 selftest      # полная последовательность
#   python arcc_harness.py --list-ports
#
# Требования: pip install pyserial

import argparse
import re
import socket
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial не установлен: pip install pyserial")

BAUD = 1250000  # формальная скорость ARCC (для USB-CDC не важна)

MOD_AXIS, MOD_BUTTON, MOD_ENCODER, MOD_LINEAR, MOD_INDICATOR, MOD_BINARY = 1, 2, 3, 4, 5, 8
INDICATOR_SIGNS = "01234567890123456789 FDC-_^Iro.E"


def indicator_buffer(text: str) -> bytes:
    """Порт C# StringToIndicatorBuffer: заголовок 3 байта + 7 кодов."""
    result = bytearray([0, 0, 0]) + bytearray([20] * 7)
    buf_pos = 9
    i = len(text) - 1
    while i >= 0:
        if buf_pos == 2:
            break
        symbol = text[i]
        if symbol == ",":
            symbol = "."
        pos = INDICATOR_SIGNS.find(symbol)
        if pos == -1:
            result[buf_pos] = 20
            buf_pos -= 1
            i -= 1
            continue
        if i > 0 and symbol == "." and "0" <= text[i - 1] <= "9":
            pos = INDICATOR_SIGNS.find(text[i - 1], 10)
            i -= 1
        result[buf_pos] = pos
        buf_pos -= 1
        i -= 1
    return bytes(result)


def pkt_lamp(module_id: int, control_id: int, on: bool) -> bytes:
    return bytes([MOD_LINEAR, module_id, 1, control_id, 1 if on else 0])


def pkt_indicator(module_id: int, text: str) -> bytes:
    b = bytearray(indicator_buffer(text))
    b[0], b[1], b[2] = MOD_INDICATOR, module_id, 1
    return bytes(b)


def pkt_dump_keys(module_id: int) -> bytes:
    return bytes([MOD_BUTTON, module_id, 3, 0, 0])


def pkt_dump_lines(module_id: int) -> bytes:
    return bytes([MOD_BINARY, module_id, 2, 0, 0])


def decode_rx(block: bytes) -> str:
    """Декодировать 18-байтный пакет плата -> ПК."""
    t, mid = block[0], block[1]
    if t == MOD_BUTTON:
        if block[15] in (1, 2) or block[16] == 0:
            return f"Button  mid={mid} (служебный/дамп, игнор)"
        return f"Button  mid={mid} ctl={block[16]} {'НАЖАТА' if block[17] == 1 else 'отпущена'}"
    if t == MOD_BINARY:
        mask = int.from_bytes(block[14:18], "big")
        return f"Binary  mid={mid} маска=0x{mask:08X} ({mask:#b})"
    if t == MOD_ENCODER:
        return f"Encoder mid={mid} ctl={block[15]} щелчков={block[16]} {'по час.' if block[17] == 1 else 'против'}"
    if t == MOD_AXIS:
        vals = [block[2 + j * 2] * 256 + block[3 + j * 2] for j in range(8)]
        ctl_vals = {8 - j: v for j, v in enumerate(vals) if v}
        return f"Axis    mid={mid} {ctl_vals}"
    return f"??      тип={t} {block.hex(' ')}"


def open_port(args) -> "serial.Serial":
    ser = serial.Serial(args.port, BAUD, bytesize=8, parity="N", stopbits=1, timeout=0.1)
    ser.rts = False
    # как FlexRouter: DTR-импульс 1 с при открытии порта
    ser.dtr = True
    time.sleep(1.0)
    ser.dtr = False
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


class TcpPort:
    """Обёртка TCP-сокета с интерфейсом, совместимым с pyserial (read/write/close)."""

    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=3)
        self.sock.settimeout(0.1)

    def write(self, data: bytes) -> int:
        return self.sock.send(data)

    def read(self, size: int = 256) -> bytes:
        try:
            return self.sock.recv(size)
        except socket.timeout:
            return b""

    def close(self):
        self.sock.close()


def read_and_print(ser, duration: float = 1.0) -> int:
    """Читать поток, склеивать хвосты (как ProcessDataFromSerialPort), печатать пакеты."""
    tail = b""
    end = time.time() + duration
    count = 0
    while time.time() < end:
        data = ser.read(256)
        if not data:
            continue
        buf = tail + data
        n = len(buf) // 18
        for i in range(n):
            block = buf[i * 18:(i + 1) * 18]
            print("  <-", decode_rx(block))
            count += 1
        tail = buf[n * 18:]
    return count


def cmd_selftest(ser, args):
    print("== SELFTEST ==")
    print("1. Лампа ctl=1 ВКЛ")
    ser.write(pkt_lamp(0, 1, True)); time.sleep(0.5)
    print("2. Лампа ctl=1 ВЫКЛ")
    ser.write(pkt_lamp(0, 1, False)); time.sleep(0.5)
    print("3. Индикатор '12.5'")
    ser.write(pkt_indicator(0, "12.5")); time.sleep(0.5)
    print("4. Дамп кнопок (модуль 1) -> жду 2 с пакеты Button")
    ser.write(pkt_dump_keys(1))
    n = read_and_print(ser, 2.0)
    print(f"   получено {n} Button-пакетов")
    print("5. Дамп бинарного ввода (модуль 1)")
    ser.write(pkt_dump_lines(1))
    read_and_print(ser, 1.0)
    print("6. Слушаю поток 3 с (дергайте кнопки/энкодеры на плате)...")
    read_and_print(ser, 3.0)
    print("== SELFTEST завершён ==")


def main():
    p = argparse.ArgumentParser(description="ARCC protocol harness (FlexRouter <-> ESP32)")
    p.add_argument("--port", help="COM-порт платы (напр. COM5)")
    p.add_argument("--host", help="TCP-подключение к плате (transport.tcp), напр. 192.168.4.1")
    p.add_argument("--tcp-port", type=int, default=4001, help="TCP-порт платы (по умолчанию 4001)")
    p.add_argument("--list-ports", action="store_true")
    sub = p.add_subparsers(dest="cmd")
    sub.add_parser("watch", help="слушать и декодировать пакеты платы")
    sp = sub.add_parser("lamp"); sp.add_argument("ctl", type=int); sp.add_argument("state", choices=["on", "off"])
    ip = sub.add_parser("indicator"); ip.add_argument("text")
    dp = sub.add_parser("dump-keys"); dp.add_argument("module", type=int)
    dl = sub.add_parser("dump-lines"); dl.add_argument("module", type=int)
    sub.add_parser("selftest")
    args = p.parse_args()

    if args.list_ports:
        for pt in list_ports.comports():
            print(f"{pt.device}  {pt.description}")
        return
    if (not args.port and not args.host) or not args.cmd:
        p.print_help()
        return

    if args.host:
        ser = TcpPort(args.host, args.tcp_port)
        print(f"TCP {args.host}:{args.tcp_port} подключён (ARCC)")
    else:
        ser = open_port(args)
        print(f"Порт {args.port} открыт (ARCC, {BAUD} 8N1, DTR-импульс 1 c)")

    if args.cmd == "watch":
        print("Ctrl+C для выхода")
        try:
            read_and_print(ser, 3600)
        except KeyboardInterrupt:
            pass
    elif args.cmd == "lamp":
        ser.write(pkt_lamp(0, args.ctl, args.state == "on"))
        print(f"-> лампа ctl={args.ctl} {args.state}")
        read_and_print(ser, 0.5)
    elif args.cmd == "indicator":
        ser.write(pkt_indicator(0, args.text))
        print(f'-> индикатор "{args.text}"')
        read_and_print(ser, 0.5)
    elif args.cmd == "dump-keys":
        ser.write(pkt_dump_keys(args.module))
        print(f"-> дамп кнопок, модуль {args.module}")
        read_and_print(ser, 3.0)
    elif args.cmd == "dump-lines":
        ser.write(pkt_dump_lines(args.module))
        print(f"-> дамп бинарного ввода, модуль {args.module}")
        read_and_print(ser, 1.0)
    elif args.cmd == "selftest":
        cmd_selftest(ser, args)

    ser.close()


if __name__ == "__main__":
    main()
