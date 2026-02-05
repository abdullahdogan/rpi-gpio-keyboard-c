#!/usr/bin/env bash
set -e

APP_NAME="gpio-keyboard"
INSTALL_DIR="/opt/rpi-gpio-keyboard-c"
SERVICE_FILE="gpio-keyboard.service"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "======================================="
echo " GPIO Keyboard - Install"
echo "======================================="

if [ "$EUID" -ne 0 ]; then
  echo "Run as root:"
  echo "  sudo bash install.sh"
  exit 1
fi

echo "[1/7] Packages"
apt update
apt install -y git gcc make pkg-config libgpiod-dev

echo "[2/7] uinput"
modprobe uinput || true
if [ ! -e /dev/uinput ]; then
  echo "ERROR: /dev/uinput not found (uinput module not loaded?)"
  exit 1
fi
echo uinput > /etc/modules-load.d/uinput.conf

echo "[3/7] Copy files"
mkdir -p "$INSTALL_DIR"
cp -r "$SCRIPT_DIR/src" "$SCRIPT_DIR/Makefile" "$INSTALL_DIR"

echo "[4/7] Build"
cd "$INSTALL_DIR"
make clean || true
make
chmod +x gpio_keyboard

echo "[5/7] systemd"
cp "$SCRIPT_DIR/systemd/$SERVICE_FILE" /etc/systemd/system/

# Ensure ExecStart points to installed binary
sed -i "s|^ExecStart=.*|ExecStart=$INSTALL_DIR/gpio_keyboard|" \
  /etc/systemd/system/$SERVICE_FILE

systemctl daemon-reload
systemctl enable $APP_NAME
systemctl restart $APP_NAME

echo "[6/7] Status"
systemctl status $APP_NAME --no-pager || true

echo "[7/7] Done"
echo "Tip: debug run:"
echo "  sudo systemctl stop gpio-keyboard"
echo "  $INSTALL_DIR/gpio_keyboard --debug"
