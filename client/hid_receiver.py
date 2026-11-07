#!/usr/bin/env python3
"""
ESP32-S3 BLE Bridge — WiFi HID-over-UDP Receiver (v1.0)
Принимает UDP-пакеты от ESP32-S3 и эмулирует клавиатуру и мышь на ПК через pynput.

Установка зависимостей:
  pip install pynput

Запуск:
  python hid_receiver.py [--port 4321] [--bind 0.0.0.0]

Формат пакетов от ESP32-S3 (первый байт = тип):
  0x01 = Клавиатура: [mod, 0, k0, k1, k2, k3, k4, k5]  (HID Boot Report, 8 байт)
  0x02 = Мышь:       [кнопки, dx, dy, wheel, pan]      (5 байт)
  0x03 = Consumer:   [code_lo, code_hi]                (2 байта)
  0x04 = Gamepad:    [lx+128, ly+128, rx+128, ry+128, l2, r2, hat, btn0, btn1, btn2] (10 байт)
"""

import socket
import struct
import argparse
import sys

try:
    from pynput.keyboard import Key, Controller as KbController, KeyCode
    from pynput.mouse import Button, Controller as MouseController
except ImportError:
    print("\n[!] Ошибка: Библиотека 'pynput' не установлена.")
    print("    Установите её командой:")
    print("    pip install pynput\n")
    sys.exit(1)

keyboard = KbController()
mouse = MouseController()

# HID Usage Page 0x0C (Consumer Control) -> pynput Key
CONSUMER_MAP = {
    0x00B5: getattr(Key, 'media_next', None),
    0x00B6: getattr(Key, 'media_previous', None),
    0x00CD: getattr(Key, 'media_play_pause', None),
    0x00E9: getattr(Key, 'media_volume_up', None),
    0x00EA: getattr(Key, 'media_volume_down', None),
    0x00E2: getattr(Key, 'media_volume_mute', None),
}

# HID keycode -> pynput Key / KeyCode
HID_TO_PYNPUT = {}

# Буквы a-z: HID 0x04..0x1D
for i in range(26):
    HID_TO_PYNPUT[0x04 + i] = KeyCode.from_char(chr(ord('a') + i))

# Цифры 1-9: HID 0x1E..0x26, 0 = 0x27
for i in range(1, 10):
    HID_TO_PYNPUT[0x1E + i - 1] = KeyCode.from_char(str(i))
HID_TO_PYNPUT[0x27] = KeyCode.from_char('0')

# Специальные клавиши
HID_TO_PYNPUT.update({
    0x28: Key.enter,
    0x29: Key.esc,
    0x2A: Key.backspace,
    0x2B: Key.tab,
    0x2C: Key.space,
    0x2D: KeyCode.from_char('-'),
    0x2E: KeyCode.from_char('='),
    0x2F: KeyCode.from_char('['),
    0x30: KeyCode.from_char(']'),
    0x31: KeyCode.from_char('\\'),
    0x33: KeyCode.from_char(';'),
    0x34: KeyCode.from_char("'"),
    0x35: KeyCode.from_char('`'),
    0x36: KeyCode.from_char(','),
    0x37: KeyCode.from_char('.'),
    0x38: KeyCode.from_char('/'),
    0x39: Key.caps_lock,
    0x3A: Key.f1,
    0x3B: Key.f2,
    0x3C: Key.f3,
    0x3D: Key.f4,
    0x3E: Key.f5,
    0x3F: Key.f6,
    0x40: Key.f7,
    0x41: Key.f8,
    0x42: Key.f9,
    0x43: Key.f10,
    0x44: Key.f11,
    0x45: Key.f12,
    0x4A: Key.home,
    0x4B: Key.page_up,
    0x4C: Key.delete,
    0x4D: Key.end,
    0x4E: Key.page_down,
    0x4F: Key.right,
    0x50: Key.left,
    0x51: Key.down,
    0x52: Key.up,
})

# Модификаторы HID -> pynput Key
MOD_MAP = {
    0x01: Key.ctrl_l,
    0x02: Key.shift_l,
    0x04: Key.alt_l,
    0x08: Key.cmd_l,
    0x10: Key.ctrl_r,
    0x20: Key.shift_r,
    0x40: Key.alt_r,
    0x80: Key.cmd_r,
}

_prev_mods = 0
_prev_keys = set()
_prev_mouse_btns = 0

def handle_keyboard(data):
    global _prev_mods, _prev_keys
    if len(data) < 8:
        return
    mod = data[0]
    keys = set(k for k in data[2:8] if k != 0)

    # Модификаторы
    for bit, key in MOD_MAP.items():
        was = (_prev_mods & bit) != 0
        now = (mod & bit) != 0
        if now and not was:
            try: keyboard.press(key)
            except Exception: pass
        elif was and not now:
            try: keyboard.release(key)
            except Exception: pass

    # Отпущенные клавиши
    for kc in _prev_keys - keys:
        k = HID_TO_PYNPUT.get(kc)
        if k:
            try: keyboard.release(k)
            except Exception: pass

    # Нажатые клавиши
    for kc in keys - _prev_keys:
        k = HID_TO_PYNPUT.get(kc)
        if k:
            try: keyboard.press(k)
            except Exception: pass

    _prev_mods = mod
    _prev_keys = keys

def handle_mouse(data):
    global _prev_mouse_btns
    if len(data) < 5:
        return
    btns = data[0]
    dx = struct.unpack_from('b', data, 1)[0]
    dy = struct.unpack_from('b', data, 2)[0]
    wheel = struct.unpack_from('b', data, 3)[0]

    # Кнопки мыши: 0x01=ЛКМ, 0x02=ПКМ, 0x04=СКМ
    btn_defs = [(0x01, Button.left), (0x02, Button.right), (0x04, Button.middle)]
    for bit, btn in btn_defs:
        was = (_prev_mouse_btns & bit) != 0
        now = (btns & bit) != 0
        if now and not was:
            try: mouse.press(btn)
            except Exception: pass
        elif was and not now:
            try: mouse.release(btn)
            except Exception: pass
    _prev_mouse_btns = btns

    # Перемещение
    if dx != 0 or dy != 0:
        mouse.move(dx, dy)

    # Скролл
    if wheel != 0:
        mouse.scroll(0, wheel)

def handle_consumer(data):
    if len(data) < 2:
        return
    code = data[0] | (data[1] << 8)
    key = CONSUMER_MAP.get(code)
    if key:
        try:
            keyboard.press(key)
            keyboard.release(key)
        except Exception:
            pass

def handle_gamepad(data):
    if len(data) >= 10:
        lx = data[0] - 128
        ly = data[1] - 128
        rx = data[2] - 128
        ry = data[3] - 128
        l2, r2, hat = data[4], data[5], data[6]
        btns = data[7] | (data[8] << 8) | (data[9] << 16)
        print(f"\r[Gamepad] LX:{lx:+4d} LY:{ly:+4d} | RX:{rx:+4d} RY:{ry:+4d} | L2:{l2:3d} R2:{r2:3d} | Hat:{hat} | Btns:0x{btns:04X}", end='', flush=True)

def process_packet(raw):
    if len(raw) < 2:
        return
    ptype = raw[0]
    payload = raw[1:]
    if ptype == 0x01:
        handle_keyboard(payload)
    elif ptype == 0x02:
        handle_mouse(payload)
    elif ptype == 0x03:
        handle_consumer(payload)
    elif ptype == 0x04:
        handle_gamepad(payload)

def main():
    parser = argparse.ArgumentParser(description="ESP32-S3 BLE Bridge — HID-over-UDP Receiver")
    parser.add_argument('--port', type=int, default=4321, help='UDP порт для прослушивания (по умолчанию: 4321)')
    parser.add_argument('--bind', default='0.0.0.0', help='IP адрес привязки (по умолчанию: 0.0.0.0)')
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((args.bind, args.port))
    except Exception as e:
        print(f"[!] Ошибка привязки к {args.bind}:{args.port} — {e}")
        sys.exit(1)

    print("=" * 65)
    print("  ESP32-S3 BLE Bridge — WiFi HID-over-UDP Receiver v1.0")
    print(f"  Прослушивание UDP на {args.bind}:{args.port}")
    print("  Нажмите Ctrl+C для остановки")
    print("=" * 65)

    pkt_count = 0
    try:
        while True:
            data, addr = sock.recvfrom(256)
            pkt_count += 1
            if pkt_count == 1:
                print(f"[+] Первый пакет получен от ESP32-S3: {addr[0]}:{addr[1]}")
            process_packet(data)
    except KeyboardInterrupt:
        print("\n\n[i] Приёмник остановлен пользователем.")
    finally:
        sock.close()

if __name__ == '__main__':
    main()
