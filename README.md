<h1 align="center">ESP32 S3 BLE Bridge</h1>

<p align="center">
  <img src="https://img.shields.io/badge/ESP32--S3-BLE%20Bridge-E7352C?style=for-the-badge&logo=espressif&logoColor=white" alt="ESP32-S3 BLE Bridge">
  <img src="https://img.shields.io/badge/BLE%20%E2%86%92%20USB-HID-6C63FF?style=for-the-badge&logo=bluetooth&logoColor=white" alt="BLE to USB HID">
  <img src="https://img.shields.io/badge/status-experimental-F59E0B?style=for-the-badge" alt="Experimental status">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/AI%20assisted-Gemini%20Argon-8E75FF?style=for-the-badge&logo=googlegemini&logoColor=white" alt="AI assisted with Gemini Argon">
  <img src="https://img.shields.io/badge/AI%20assisted-ChatGPT%20Sol-10A37F?style=for-the-badge&logo=openai&logoColor=white" alt="AI assisted with ChatGPT Sol">
</p>

<p align="center">
  <a href="web/web_console.html">
    <img src="https://img.shields.io/badge/%F0%9F%8C%90%20Open-WebSerial%20Console-22C55E?style=for-the-badge&logo=googlechrome&logoColor=white" alt="Open WebSerial Console">
  </a>
</p>

USB-адаптер на базе ESP32-S3, который подключается к BLE HID-устройствам и передаёт их ввод на компьютер как проводное USB-устройство.

```text
BLE-клавиатура / мышь / геймпад → ESP32-S3 → USB HID → компьютер
```

## Как появился проект

В моей домашней инфраструктуре несколько разных устройств: сервер на **Proxmox**, сервер **Unraid**, **MacBook** и **Raspberry Pi**. Обычно ими удобно управлять удалённо, но иногда сервер не загружается, пропадает сеть или требуется зайти в BIOS, загрузчик либо локальную консоль.

В такие моменты приходится подключать проводную клавиатуру и мышь или постоянно держать рядом отдельный комплект с USB-приёмником 2,4 ГГц. Перекладывать устройства, кабели и ресиверы между компьютерами оказалось неудобно.

Так появилась идея сделать собственный универсальный аналог **Logi Bolt**: небольшой адаптер, который принимает ввод от обычной BLE-клавиатуры и мыши, а для подключённого компьютера выглядит как стандартное проводное USB HID-устройство. Адаптер можно быстро переставить в нужный сервер, мини-компьютер или ноутбук без установки драйверов и дополнительного программного обеспечения.

Это не официальный продукт Logitech и не реализация протокола Logi Bolt — название используется только как понятное сравнение идеи. Цель проекта — универсальный BLE → USB HID-мост для разных устройств.

## Возможности

- BLE-клавиатура и мышь → USB HID
- экспериментальная поддержка Google Stadia Controller
- сохранение до пяти BLE-устройств
- автоматическое переподключение
- настройка формата HID и чувствительности мыши
- индикация состояния через WS2812 на GPIO 21
- браузерная WebSerial-консоль
- обновление прошивки через USB OTA

## Требования

- плата ESP32-S3 с Native USB
- ESP32 Arduino Core 3.3.x
- NimBLE-Arduino 2.5.x

Настройки Arduino IDE:

```text
Board: ESP32S3 Dev Module
USB Mode: USB-OTG (TinyUSB)
USB CDC On Boot: Enabled
Partition Scheme: схема с OTA-разделами
```

## Установка

1. Установите ESP32 Arduino Core и библиотеку NimBLE-Arduino.
2. Откройте файл прошивки `.ino`.
3. Выберите указанные выше параметры платы.
4. Соберите и загрузите прошивку.
5. Откройте HTML-консоль в Chrome или Edge и подключитесь к USB-порту ESP32-S3.

## Статус

Проект пока находится на очень ранней стадии и остаётся достаточно сырым. Возможны ошибки, зависания, задержки и несовместимость с отдельными HID-устройствами. Формат отчётов зависит от HID Report Map конкретной клавиатуры, мыши или геймпада, поэтому некоторые дополнительные клавиши и составные Logitech HID-интерфейсы могут потребовать отдельного профиля.

Несмотря на это, адаптер уже используется в реальной работе: прямо сейчас я пишу код и общаюсь с ИИ через этот же ESP32 S3 BLE Bridge, подключённый к **Logitech MX Keys Mini**. Проект развивается в процессе ежедневного использования, а найденные проблемы постепенно исправляются.

## Благодарности

Проект **ESP32 S3 BLE Bridge** был вдохновлён идеями и веб-консолью проекта [finger563/esp-usb-ble-hid](https://github.com/finger563/esp-usb-ble-hid).

Спасибо автору и участникам исходного проекта за их работу над BLE HID, USB HID и инструментами управления адаптером через браузер.

## О разработке

Я вайбкодер: весь код этого проекта написан и доработан с помощью **Gemini Argon** и **ChatGPT Sol**. Я формулировал задачи, проверял результаты, тестировал прошивку на оборудовании и направлял дальнейшую разработку.

