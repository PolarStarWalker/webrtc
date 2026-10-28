#!/usr/bin/env bash
# setup.sh — установка окружения для проекта webrtc на Debian/Ubuntu (ARM64, Orange Pi)
# Запуск: bash setup.sh

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${PROJECT_DIR}/.venv"
PYTHON_BIN="${PYTHON_BIN:-python3}"

log()  { printf '\033[1;34m[setup]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn ]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; }

# ---------- 1. Проверки ----------
if [[ $EUID -eq 0 ]]; then
  warn "Скрипт запущен от root. Рекомендуется запускать от обычного пользователя с sudo."
fi

if ! command -v sudo >/dev/null 2>&1 && [[ $EUID -ne 0 ]]; then
  err "Не найден sudo. Установите sudo или запустите скрипт от root."
  exit 1
fi

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
  err "Python ($PYTHON_BIN) не найден в PATH."
  exit 1
fi

log "Python: $($PYTHON_BIN --version 2>&1)"
log "Проект: $PROJECT_DIR"

SUDO=""
if [[ $EUID -ne 0 ]]; then
  SUDO="sudo"
fi

# ---------- 2. Системные пакеты ----------
log "Обновление списка пакетов apt..."
$SUDO apt update

# 2.1 Базовые Python-биндинги для GObject/GTK
log "Установка python3-gi, python3-gi-cairo, gir1.2-gtk-3.0..."
$SUDO apt install -y \
  python3-gi \
  python3-gi-cairo \
  gir1.2-gtk-3.0

# 2.2 GStreamer: плагины, ICE, python-биндинги
log "Установка GStreamer: plugins-bad, nice, gir1.2-gstreamer-1.0, python3-gst-1.0..."
$SUDO apt install -y \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-nice \
  gir1.2-gstreamer-1.0 \
  python3-gst-1.0

# 2.3 КРИТИЧНО: typelib для GstWebRTC (пространство имён, из-за которого была ошибка
#     "Namespace GstWebRTC not available")
log "Установка gir1.2-gst-plugins-bad-1.0 (typelib для GstWebRTC)..."
$SUDO apt install -y gir1.2-gst-plugins-bad-1.0

# 2.4 Опционально: dev-пакеты для girepository (полезны, если понадобится собирать
#     PyGObject через pip). Не критичны, но не мешают.
if ! dpkg -s libgirepository1.0-dev >/dev/null 2>&1 \
   && ! dpkg -s libgirepository-2.0-dev >/dev/null 2>&1; then
  log "Установка dev-пакета girepository (на случай сборки PyGObject)..."
  $SUDO apt install -y libgirepository1.0-dev 2>/dev/null \
    || $SUDO apt install -y libgirepository-2.0-dev 2>/dev/null \
    || warn "Не удалось установить libgirepository*-dev — это не критично."
fi

# ---------- 3. Проверка GStreamer WebRTC в системе ----------
log "Проверка плагина webrtcbin через gst-inspect-1.0..."
if command -v gst-inspect-1.0 >/dev/null 2>&1; then
  if gst-inspect-1.0 webrtcbin >/dev/null 2>&1; then
    log "webrtcbin доступен в системе ✓"
  else
    warn "gst-inspect-1.0 не видит webrtcbin. Проверьте установку gstreamer1.0-plugins-bad."
  fi
else
  warn "gst-inspect-1.0 не найден. Установите gstreamer1.0-tools, если нужна диагностика."
fi

log "Проверка typelib GstWebRTC-1.0..."
if find /usr -name "GstWebRTC-1.0.typelib" 2>/dev/null | grep -q .; then
  log "typelib GstWebRTC-1.0 найден ✓"
else
  err "typelib GstWebRTC-1.0 НЕ найден. Установите gir1.2-gst-plugins-bad-1.0."
  exit 1
fi

# ---------- 4. Виртуальное окружение ----------
if [[ -d "$VENV_DIR" ]]; then
  # Проверяем, создан ли он с --system-site-packages
  if grep -q "include-system-site-packages = true" "$VENV_DIR/pyvenv.cfg" 2>/dev/null; then
    log "Существующий .venv уже с --system-site-packages, переиспользуем."
  else
    warn "Существующий .venv без доступа к системным пакетам — пересоздаём."
    rm -rf "$VENV_DIR"
    "$PYTHON_BIN" -m venv "$VENV_DIR" --system-site-packages
  fi
else
  log "Создание виртуального окружения с --system-site-packages..."
  "$PYTHON_BIN" -m venv "$VENV_DIR" --system-site-packages
fi

# ---------- 5. Проверка импорта из venv ----------
log "Проверка импорта gi и GstWebRTC из venv..."
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"

if ! python - <<'PY'
import sys
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
from gi.repository import Gst, GstWebRTC  # noqa: F401
print("OK: gi + Gst + GstWebRTC импортируются из venv")
PY
then
  err "Не удалось импортировать gi/GstWebRTC из venv. См. вывод выше."
  deactivate || true
  exit 1
fi

deactivate || true

# ---------- 6. Опционально: зависимости Python проекта ----------
REQ_FILE=""
for f in requirements.txt pyproject.toml; do
  if [[ -f "${PROJECT_DIR}/${f}" ]]; then
    REQ_FILE="${PROJECT_DIR}/${f}"
    break
  fi
done

if [[ -n "$REQ_FILE" ]]; then
  log "Найден $REQ_FILE — устанавливаю Python-зависимости в venv..."
  # shellcheck disable=SC1091
  source "$VENV_DIR/bin/activate"
  python -m pip install --upgrade pip
  if [[ "$(basename "$REQ_FILE")" == "requirements.txt" ]]; then
    python -m pip install -r "$REQ_FILE"
  else
    python -m pip install -e "$PROJECT_DIR"
  fi
  deactivate || true
else
  warn "requirements.txt / pyproject.toml не найдены — Python-зависимости не ставлю."
fi

# ---------- 7. Итог ----------
cat <<EOF

✅ Установка завершена.

Дальнейшие шаги:
  cd "$PROJECT_DIR"
  source .venv/bin/activate
  ./scripts/server.py

Если скрипт снова упадёт с "Namespace GstWebRTC not available":
  1) Проверьте: find /usr -name 'GstWebRTC-1.0.typelib'
  2) При необходимости пересоздайте venv:
     rm -rf .venv && python3 -m venv .venv --system-site-packages
EOF

