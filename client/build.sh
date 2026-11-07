#!/bin/sh
set -e
echo "Сборка hid_receiver для Linux (uinput)..."
gcc -O2 hid_receiver_linux.c -o hid_receiver
echo "Сборка завершена успешно: ./hid_receiver"
echo "Запуск: sudo ./hid_receiver --port 4321"
