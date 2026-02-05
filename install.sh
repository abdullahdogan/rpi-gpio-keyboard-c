#!/usr/bin/env bash
set -e

APP_NAME="gpio-keyboard"
INSTALL_DIR="/opt/rpi-gpio-keyboard-c"
SERVICE_FILE="gpio-keyboard.service"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "======================================="
echo " GPIO Keyboard - Otomatik Kurulum"
echo "======================================="

if [ "$EUID" -ne 0 ]; then
  echo "Lütfen root olarak çalıştırın:"
  echo "sudo bash install.sh"
  exit 1
fi

echo "[1/7] Gerekli paketler"
apt update
apt install -y git gcc make

echo "[2/7] uinput modülü"
modprobe uinput
echo uinput > /etc/modules-load.d/uinput.conf

echo "[3/7] Dosyalar kopyalanıyor"
mkdir -p "$INSTALL_DIR"
cp -r "$SCRIPT_DIR/src" "$SCRIPT_DIR/Makefile" "$INSTALL_DIR"

cd "$INSTALL_DIR"

echo "[4/7] Derleme"
make clean || true
make
chmod +x gpio_keyboard

echo "[5/7] systemd servisi kuruluyor"
cp "$SCRIPT_DIR/systemd/$SERVICE_FILE" /etc/systemd/system/

sed -i "s|ExecStart=.*|ExecStart=$INSTALL_DIR/gpio_keyboard|" \
  /etc/systemd/system/$SERVICE_FILE

systemctl daemon-reload
systemctl enable $APP_NAME
systemctl restart $APP_NAME

echo "[6/7] Servis durumu"
systemctl status $APP_NAME --no-pager

echo "[7/7] Kurulum tamamlandı"
