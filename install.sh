
#!/usr/bin/env bash

set -e

APP_NAME="gpio-keyboard"
INSTALL_DIR="/opt/rpi-gpio-keyboard-c"
SERVICE_FILE="gpio-keyboard.service"

echo "======================================="
echo " GPIO Keyboard – Otomatik Kurulum"
echo "======================================="

# 1. ROOT KONTROLÜ
if [[ $EUID -ne 0 ]]; then
  echo "❌ Lütfen root olarak çalıştırın:"
  echo "   sudo ./install.sh"
  exit 1
fi

# 2. SİSTEM BİLGİSİ
echo "[1/8] Sistem kontrolü"
grep -q "Raspberry Pi" /proc/device-tree/model || {
  echo "⚠️ Raspberry Pi tespit edilemedi, devam ediliyor..."
}

# 3. GEREKLİ PAKETLER
echo "[2/8] Gerekli paketler kontrol ediliyor"

REQUIRED_PKGS=(git gcc make)

MISSING_PKGS=()
for pkg in "${REQUIRED_PKGS[@]}"; do
  if ! dpkg -s "$pkg" &>/dev/null; then
    MISSING_PKGS+=("$pkg")
  fi
done

if [ ${#MISSING_PKGS[@]} -ne 0 ]; then
  echo "→ Eksik paketler kuruluyor: ${MISSING_PKGS[*]}"
  apt update
  apt install -y "${MISSING_PKGS[@]}"
else
  echo "✓ Tüm paketler mevcut"
fi

# 4. uinput MODÜLÜ
echo "[3/8] uinput modülü kontrolü"

if ! lsmod | grep -q uinput; then
  modprobe uinput
fi

if ! grep -q "^uinput$" /etc/modules-load.d/uinput.conf 2>/dev/null; then
  echo "uinput" > /etc/modules-load.d/uinput.conf
fi

# 5. DOSYALARI KOPYALA
echo "[4/8] Dosyalar kopyalanıyor"

mkdir -p "$INSTALL_DIR"
cp -r src Makefile "$INSTALL_DIR"
cd "$INSTALL_DIR"

# 6. DERLEME
echo "[5/8] Derleniyor"

make clean || true
make

chmod +x gpio_keyboard

# 7. SYSTEMD SERVİSİ
echo "[6/8] systemd servisi kuruluyor"

cp "$(dirname "$0")/systemd/$SERVICE_FILE" /etc/systemd/system/

sed -i "s|ExecStart=.*|ExecStart=$INSTALL_DIR/gpio_keyboard|" \
  /etc/systemd/system/$SERVICE_FILE

systemctl daemon-reload
systemctl enable $APP_NAME
systemctl restart $APP_NAME

# 8. DURUM
echo "[7/8] Servis durumu"
systemctl status $APP_NAME --no-pager

echo "[8/8] Kurulum tamamlandı ✅"
echo "Reboot sonrası otomatik çalışacaktır."
