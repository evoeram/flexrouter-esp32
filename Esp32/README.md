# FlexRouter ESP32-S3 — прошивка + веб-конфигуратор

Реализация плана `../plan_new.md`: ESP32-S3 как ARCC-совместимая плата вывода/ввода для
FlexRouter (Tu-154M, FS2004) + Wi-Fi provisioning + веб-морда (конфигурация, дебаг, OTA).

Прежний скетч `../Firmware/esp32s3_arcc/` — предок этого проекта; здесь его логика
перенесена в модульную структуру и расширена веб-частью.

## Сборка и прошивка

```powershell
pip install platformio            # один раз
cd C:\dev\STM32\Esp32
python -m platformio run          # сборка -> .pio\build\esp32s3\firmware.bin
python -m platformio run -t buildfs       # образ web-морды (data/ -> littlefs.bin)
python -m platformio run -t upload        # прошивка по USB (USB-Serial-JTAG)
python -m platformio run -t uploadfs      # загрузка web-морды
python -m platformio device monitor       # дебаг-лог (UART0, 115200)
```

Дебаг-лог идёт на **UART0** (пины TX0/RX0 или второй USB-порт платы), 115200 —
USB-CDC занят протоколом ARCC.

Плата по умолчанию: ESP32-S3 **N16R8** (16MB flash, 8MB octal PSRAM) —
`board_build.flash_size`/`memory_type` в `platformio.ini` поменять под свой модуль
(для N8R2: `8MB` + `qio_qspi`).

## Первый запуск (provisioning + AP-режим, этап 2.1)

1. Нет сохранённого Wi-Fi → плата поднимает AP **FlexRouter-Config** (пароль `flexrouter`)
   и **наш веб-сервер на http://192.168.4.1** (WiFiManager-портал больше не используется).
   Captive-DNS + HTTP-редирект: окно «входа в сеть» телефона открывает морду.
2. В AP доступно всё: статус, **редактор пинов**, дебаг I/O, скан сетей и ввод кредов
   («Сканировать» на `/config`), сохранение → плата уходит в STA.
3. Не подключился за 3 попытки по 15 с → снова AP.
4. В LAN: `http://flexrouter.local` (mDNS) или по IP.
5. **Заводской сброс**: удержание кнопки BOOT 5 с или кнопка в вебе → стираются конфиг и
   креды Wi-Fi → снова AP.

## Веб-морда (фазы 4–6)

| Страница | Что делает |
|---|---|
| `/` | статус: Wi-Fi, RSSI, heap, uptime, счётчики ARCC, журнал |
| `/config` | Wi-Fi (скан сетей)/статический IP/имя; **визуальный редактор модулей и пинов** (выпадающие списки только свободных GPIO, live-состояние пина, калибровка осей «считать сейчас», валидация на плате); параметры ввода; TCP-транспорт |
| `/debug` | live-монитор ARCC-пакетов (hex+расшифровка, WebSocket); **панель I/O**: плитки ламп с форсом и сбросом, кнопки/28 линий бинарного ввода с подсветкой изменений, счётчики энкодеров, полоски осей (значение + ADC raw), индикатор; **лента событий ввода**; симуляция; журнал |
| `/admin` | OTA-загрузка .bin, перезагрузка, заводской сброс |

JSON API: `GET /api/status`, `GET/POST /api/config` (POST валидирует карту пинов:
конфликт GPIO, зарезервированные пины, дубль controlId → 400 с текстом ошибки),
`GET /api/io` (оси теперь и в сыром ADC: `axesRaw`), `GET /api/log`,
`GET /api/wifi/scan`, `GET /api/pins/available` (reserved/adc/used), `GET /api/adc/read?pin=N`,
`POST /api/sim`, `POST /api/reboot`, `POST /api/factory_reset`, `WS /ws`, `POST /update`.

## Протокол ARCC (кратко)

Полная расшифровка — в `../plan.md` п. 3 и `../FlexRouter/Hardware/Arcc/`.

- ПК → плата (5/10 байт): лампа `[4][mid][1][ctl][0|1]`, индикатор `[5][mid][1][7 кодов]`,
  дамп кнопок `[2][mid][3][0][0]`, дамп бинарного ввода `[8][mid][2][0][0]`. Ресинк по первому байту.
- Плата → ПК: пакеты строго 18 байт гуськом: Button `[15]=0,[16]=ctl,[17]=состояние`;
  BinaryInput `[14..17]` маска big-endian; Encoder `[15]=ctl,[16]=щелчки,[17]=направление`;
  Axis — 8 осей × 2 байта, пара `j` = ControlId `8−j`.
- Транспорт: USB CDC (порт открывается FlexRouter на 1250000 8N1, DTR-импульс 1 с —
  для CDC не важен) и, опционально, TCP-сервер (`transport.tcp`, порт 4001).
  PC-сторона TCP — `ArccNetDevice` в FlexRouter: список адресов в файле
  `ArccNetDevices.txt` рядом с FlexRouter.exe (строка = `host:port`, `#` — комментарий,
  например `192.168.4.1:4001`). Реконнект автоматический (backoff 1с→5с→15с).

## Карта модулей (конфиг)

Хранится в NVS как JSON, правится через `/config` (фрагмент по умолчанию):

```json
{
  "modules": [
    { "id": 0, "type": "LinearOutput", "pins": { "1": 48, "2": 39, "3": 40 } },
    { "id": 0, "type": "Indicator", "driver": "max7219", "cs": 10, "sck": 12, "mosi": 11 },
    { "id": 1, "type": "Button", "activeLow": true, "pins": { "1": 4, "2": 5 } },
    { "id": 1, "type": "BinaryInput", "activeLow": true, "pins": { "1": 7, "2": 15 } },
    { "id": 1, "type": "Encoder", "pins": { "1a": 17, "1b": 18 } },
    { "id": 1, "type": "Axis", "pins": { "1": 1 }, "cal": { "1": { "min": 0, "max": 4095 } } }
  ]
}
```

- `id` — ModuleId в профиле FlexRouter (у ламп — поле «Ext» при назначении).
- Лампа `pin: 48` на DevKit — бортовой WS2812; добавьте `"rgb": true` модулю для `neopixelWrite`.
  Boot-мигание GPIO48 автоматически отключается, если он входит в pin map (п. 5.1 plan_new.md).
- Индикатор: `"driver": "none"` (только веб-дебаг) или `"max7219"` (bit-bang SPI, 7 позиций).
- **74HC165 (входы, этап 4)**: `{"id":1,"type":"ShiftIn","pins":{"data":38,"clk":41,"latch":42},
  "chips":2,"activeLow":true,"map":{"1":0,"2":1,...}}` — controlId → абсолютный бит каскада
  (до 4 микросхем = 32 входа; бит 0 — первый, вышедший из каскада после latch). Каналы ведут
  себя как кнопки: дребезг, Button-пакеты, дамп;moduleId — `id` модуля.
- **74HC595 (выходы, этап 4)**: `{"id":0,"type":"ShiftOut","pins":{"data":21,"clk":47,"latch":14},
  "chips":1,"activeLow":false,"map":{"10":0,"11":1}}` — дискретные лампы через каскад;
  теневой регистр, latch — раз на цикл опроса (не мигает мусором между пакетами).
  rgb/WS2812 на ShiftOut не поддерживаются.
- Зарезервированные пины S3 (защищены и в редакторе, и в POST /api/config):
  USB D−/D+ (19/20), UART0 TX/RX (43/44 — дебаг), flash/PSRAM (26–37),
  GPIO0 (BOOT/заводская кнопка), strap 45/46.

## Тестер протокола (фаза 3/8)

```powershell
pip install pyserial
python tools\arcc_harness\arcc_harness.py --list-ports
python tools\arcc_harness\arcc_harness.py --port COM5 selftest   # лампы, индикатор, дампы, эхо
python tools\arcc_harness\arcc_harness.py --port COM5 watch      # live-декод пакетов платы
```

Имитирует FlexRouter: те же пакеты, DTR-импульс 1 с при открытии порта, склейка хвостов.

## Структура

```
platformio.ini          espressif32 @ 8.x, Arduino; 2 OTA-слота по 2МБ + LittleFS 12МБ
partitions.csv
src/
  main.cpp              bootstrap + заводской сброс по BOOT (5 с)
  debug.h               дебаг-порт (UART0)
  config/               JSON-конфиг в NVS (Preferences + ArduinoJson)
  arcc/
    arcc_protocol.h     пакеты, алфавит индикатора
    arcc_transport.h/.cpp  Serial CDC + TCP-сервер (единый интерфейс)
    arcc_core.h/.cpp    потоковый разбор команд, отправка 18-байтных пакетов
    arcc_hw.h/.cpp      IO по карте пинов, дребезг, квадратура, оси, MAX7219
  wifi/                 state machine: STA ↔ собственный AP (captive-DNS, без WiFiManager)
  web/                  статика LittleFS, JSON API, WebSocket, OTA
  log/event_log.h       кольцевой буфер событий
data/                   веб-морда (index/config/debug/admin + app.js + style.css)
tools/arcc_harness/     Python-тестер ARCC
```

## Что осталось

- Железные тесты: стресс ввода (этап 3.4), макетка 74HC165/595 (этап 4.5),
  сквозной «выдернуть Wi-Fi → реконнект» (этап 1.5).
- Этап 5: multi-axis сверка с профилем (5.2), опциональные ARCC-команды (5.3),
  ghost-байт после реконнекта COM (5.4).
- Отложено: реле/лампы 27В, MAX7219 на стенде, сверка с профилем Ту-154М.
