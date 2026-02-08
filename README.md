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
git clone https://github.com/abdullahdogan/rpi-gpio-keyboard-c.git
cd rpi-gpio-keyboard-c
make

sudo systemctl enable gpio-keyboard

```
**Hızlı Kurulum (Raspberry Pi CM5)**
```bash
git clone https://github.com/abdullahdogan/rpi-gpio-keyboard-c.git
cd rpi-gpio-keyboard-c
chmod +x install.sh
sudo ./install.sh
```

**Güncelle**
```
cd ~/rpi-gpio-keyboard-c
git pull
sudo bash install.sh
```

**Lokal değişikleri Sil ve Güncelle**
```
cd ~/rpi-gpio-keyboard-c
git fetch origin
git reset --hard origin/main

sudo bash install.sh
sudo systemctl restart gpio-keyboard
```



