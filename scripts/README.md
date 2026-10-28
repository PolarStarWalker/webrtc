# PoC: USB (UVC) H.264 → WebRTC passthrough, multi-client

Одна USB-камера, отдающая H.264, раздаётся через WebRTC нескольким
зрителям (браузер или Python-клиент). Перекодирования нет:
`v4l2src ! h264parse ! tee → (rtph264pay ! webrtcbin) × N`.
HTML страницы встроен прямо в `server.py` (константа `HTML_PAGE`).

## Требования

Linux + USB-камера, отдающая H.264 через UVC. Проверка:

```bash
v4l2-ctl --list-formats-ext -d /dev/video0 | grep -i h264
```

Если пусто — камера H.264 не умеет, PoC не подойдёт.

## Системные пакеты (один раз)

```bash
sudo apt update && sudo apt install -y \
  python3-gi python3-gi-cairo \
  gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-libav v4l-utils
```

## venv

```bash
python3 -m venv --system-site-packages .venv
source .venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
```

`--system-site-packages` нужен, чтобы venv «видел» системный `gi`
(PyGObject собирать из pip не требуется).

## Запуск сервера

```bash
python server.py
# [+] Найдено H.264 камер: 1
#     /dev/video0
# [*] HTTP/WebSocket: http://0.0.0.0:8080
```

Открыть `http://<IP-сервера>:8080` — браузер подключится и покажет поток.

## Запуск Python-клиента

```bash
python client.py ws://<IP-сервера>:8080/ws
```

Откроет окно с видео. Для headless-теста без дисплея замените в
`client.py` строку `autovideosink` на `fakesink sync=false`.

## Несколько зрителей одновременно

Откройте `http://<IP>:8080` на нескольких устройствах или запустите
`client.py` на нескольких машинах. Сервер создаёт под каждого зрителя
отдельную ветку `rtph264pay ! webrtcbin` через `tee`; все они получают
один и тот же H.264-поток без перекодирования.

В логе сервера:

```
[+] Клиент подключён (всего: 1)
[+] Клиент подключён (всего: 2)
[-] Клиент отключён (всего: 1)
```

## Как устроен пайплайн

Источник (один на процесс, живёт с момента запуска сервера):

```
v4l2src device=/dev/video0 io-mode=mmap
  ! video/x-h264
  ! h264parse config-interval=-1               # SPS/PPS перед каждым IDR
  ! video/x-h264,stream-format=byte-stream,alignment=au
  ! tee name=t
```

Ветка на каждого зрителя (создаётся при подключении WS, уничтожается
при отключении):

```
t.src_%u
  ! queue
  ! rtph264pay pt=96 config-interval=-1
  ! application/x-rtp,media=video,encoding-name=H264,payload=96
  ! webrtcbin stun-server=stun://stun.l.google.com:19302
```

Нет `avdec_h264`, нет `x264enc`, нет `videoconvert` — только упаковка
готового H.264 в RTP/SRTP. `h264parse config-interval=-1` вставляет
SPS/PPS перед каждым IDR, поэтому зритель, подключившийся посреди
потока, синхронизируется на ближайшем ключевом кадре.

## Заметки по портированию на C++

- Структура идентична: `v4l2src ! h264parse ! tee → (rtph264pay ! webrtcbin) × N`.
- `webrtcbin` сигналы (`pad-added`, `on-ice-candidate`) переносятся 1:1.
- Заменить `aiohttp` на `libwebsockets`/`Boost.Beast`/`uWebSockets`.
- Заменить `json` на `nlohmann/json`.
- HTML страницу можно оставить той же — просто вставить её как строковый
  литерал в C++ и отдавать на `/`.