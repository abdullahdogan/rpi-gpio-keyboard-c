# Raspberry Pi GPIO Keyboard (C)

GPIO pinlerinden gelen buton sinyallerini
Linux sanal klavye tuşlarına çevirir.

## Özellikler
- Raspberry Pi OS Trixie uyumlu
- C dili
- systemd ile açılışta başlar
- Wayland / X11 bağımsız

## Kurulum
```bash
git clone https://github.com/KULLANICI_ADI/rpi-gpio-keyboard-c.git
cd rpi-gpio-keyboard-c
make
sudo systemctl enable gpio-keyboard