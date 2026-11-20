# Чтение дебаг-лога платы с COM-порта (CDC) N секунд.
# usage: python read_com.py COM12 40
import sys, time
import serial

port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.5)
try:
    s.dtr = False
    s.rts = False
except Exception:
    pass
t0 = time.time()
while time.time() - t0 < secs:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
