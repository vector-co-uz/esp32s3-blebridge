/*
 * ESP32-S3 BLE Bridge — Native Linux HID-over-UDP Receiver (uinput)
 *
 * Создаёт виртуальную клавиатуру и мышь прямо в ядре Linux через /dev/uinput.
 * Не требует X11, Wayland или Python. Работает в консоли/TTY, Proxmox, Unraid, Raspberry Pi.
 *
 * Сборка:
 *   gcc -O2 hid_receiver_linux.c -o hid_receiver
 *
 * Запуск (требуются права root или доступ к /dev/uinput):
 *   sudo ./hid_receiver [--port 4321]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/uinput.h>

static int uinput_fd = -1;
static int sock_fd = -1;
static volatile int running = 1;

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = type;
    ie.code = code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0) {
        // ignore write error
    }
}

static void syn(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

// HID Modifiers bits -> Linux KEY_*
static const uint16_t mod_to_linux[8] = {
    KEY_LEFTCTRL,   // 0x01
    KEY_LEFTSHIFT,  // 0x02
    KEY_LEFTALT,    // 0x04
    KEY_LEFTMETA,   // 0x08 (Win/GUI)
    KEY_RIGHTCTRL,  // 0x10
    KEY_RIGHTSHIFT, // 0x20
    KEY_RIGHTALT,   // 0x40
    KEY_RIGHTMETA   // 0x80
};

// HID Usage Page 0x07 (Keyboard) -> Linux input.h KEY_*
static uint16_t hid_to_linux[256];

static void init_key_map(void) {
    memset(hid_to_linux, 0, sizeof(hid_to_linux));
    // Letters A-Z: 0x04..0x1D
    hid_to_linux[0x04] = KEY_A; hid_to_linux[0x05] = KEY_B; hid_to_linux[0x06] = KEY_C;
    hid_to_linux[0x07] = KEY_D; hid_to_linux[0x08] = KEY_E; hid_to_linux[0x09] = KEY_F;
    hid_to_linux[0x0A] = KEY_G; hid_to_linux[0x0B] = KEY_H; hid_to_linux[0x0C] = KEY_I;
    hid_to_linux[0x0D] = KEY_J; hid_to_linux[0x0E] = KEY_K; hid_to_linux[0x0F] = KEY_L;
    hid_to_linux[0x10] = KEY_M; hid_to_linux[0x11] = KEY_N; hid_to_linux[0x12] = KEY_O;
    hid_to_linux[0x13] = KEY_P; hid_to_linux[0x14] = KEY_Q; hid_to_linux[0x15] = KEY_R;
    hid_to_linux[0x16] = KEY_S; hid_to_linux[0x17] = KEY_T; hid_to_linux[0x18] = KEY_U;
    hid_to_linux[0x19] = KEY_V; hid_to_linux[0x1A] = KEY_W; hid_to_linux[0x1B] = KEY_X;
    hid_to_linux[0x1C] = KEY_Y; hid_to_linux[0x1D] = KEY_Z;

    // Numbers 1-9, 0
    hid_to_linux[0x1E] = KEY_1; hid_to_linux[0x1F] = KEY_2; hid_to_linux[0x20] = KEY_3;
    hid_to_linux[0x21] = KEY_4; hid_to_linux[0x22] = KEY_5; hid_to_linux[0x23] = KEY_6;
    hid_to_linux[0x24] = KEY_7; hid_to_linux[0x25] = KEY_8; hid_to_linux[0x26] = KEY_9;
    hid_to_linux[0x27] = KEY_0;

    // Control
    hid_to_linux[0x28] = KEY_ENTER;
    hid_to_linux[0x29] = KEY_ESC;
    hid_to_linux[0x2A] = KEY_BACKSPACE;
    hid_to_linux[0x2B] = KEY_TAB;
    hid_to_linux[0x2C] = KEY_SPACE;
    hid_to_linux[0x2D] = KEY_MINUS;
    hid_to_linux[0x2E] = KEY_EQUAL;
    hid_to_linux[0x2F] = KEY_LEFTBRACE;
    hid_to_linux[0x30] = KEY_RIGHTBRACE;
    hid_to_linux[0x31] = KEY_BACKSLASH;
    hid_to_linux[0x33] = KEY_SEMICOLON;
    hid_to_linux[0x34] = KEY_APOSTROPHE;
    hid_to_linux[0x35] = KEY_GRAVE;
    hid_to_linux[0x36] = KEY_COMMA;
    hid_to_linux[0x37] = KEY_DOT;
    hid_to_linux[0x38] = KEY_SLASH;
    hid_to_linux[0x39] = KEY_CAPSLOCK;

    // Function keys
    hid_to_linux[0x3A] = KEY_F1;  hid_to_linux[0x3B] = KEY_F2;
    hid_to_linux[0x3C] = KEY_F3;  hid_to_linux[0x3D] = KEY_F4;
    hid_to_linux[0x3E] = KEY_F5;  hid_to_linux[0x3F] = KEY_F6;
    hid_to_linux[0x40] = KEY_F7;  hid_to_linux[0x41] = KEY_F8;
    hid_to_linux[0x42] = KEY_F9;  hid_to_linux[0x43] = KEY_F10;
    hid_to_linux[0x44] = KEY_F11; hid_to_linux[0x45] = KEY_F12;

    // Navigation
    hid_to_linux[0x49] = KEY_INSERT;
    hid_to_linux[0x4A] = KEY_HOME;
    hid_to_linux[0x4B] = KEY_PAGEUP;
    hid_to_linux[0x4C] = KEY_DELETE;
    hid_to_linux[0x4D] = KEY_END;
    hid_to_linux[0x4E] = KEY_PAGEDOWN;
    hid_to_linux[0x4F] = KEY_RIGHT;
    hid_to_linux[0x50] = KEY_LEFT;
    hid_to_linux[0x51] = KEY_DOWN;
    hid_to_linux[0x52] = KEY_UP;
}

static int setup_uinput_device(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    }
    if (fd < 0) {
        perror("[!] Ошибка открытия /dev/uinput (запустите с sudo)");
        return -1;
    }

    // Включаем события клавиш (EV_KEY) и относительных перемещений (EV_REL)
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    // Мышь: перемещения и кнопки
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    // Клавиатура: все маппированные клавиши
    for (int i = 0; i < 256; i++) {
        if (hid_to_linux[i] != 0) {
            ioctl(fd, UI_SET_KEYBIT, hid_to_linux[i]);
        }
    }
    // Модификаторы
    for (int i = 0; i < 8; i++) {
        ioctl(fd, UI_SET_KEYBIT, mod_to_linux[i]);
    }

    // Мультимедиа
    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP);
    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);
    ioctl(fd, UI_SET_KEYBIT, KEY_MUTE);
    ioctl(fd, UI_SET_KEYBIT, KEY_PLAYPAUSE);
    ioctl(fd, UI_SET_KEYBIT, KEY_NEXTSONG);
    ioctl(fd, UI_SET_KEYBIT, KEY_PREVIOUSSONG);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1209; // Generic
    usetup.id.product = 0x0001;
    strcpy(usetup.name, "ESP32-S3 BLE Bridge Virtual HID");

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("[!] UI_DEV_SETUP error");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("[!] UI_DEV_CREATE error");
        close(fd);
        return -1;
    }

    return fd;
}

static uint8_t prev_mods = 0;
static uint8_t prev_keys[6] = {0};
static uint8_t prev_mouse_btns = 0;

static void handle_keyboard_packet(const uint8_t* data, size_t len) {
    if (len < 8) return;
    uint8_t mod = data[0];
    const uint8_t* keys = &data[2];

    // Модификаторы
    for (int i = 0; i < 8; i++) {
        int was = (prev_mods & (1 << i)) != 0;
        int now = (mod & (1 << i)) != 0;
        if (now && !was) emit(uinput_fd, EV_KEY, mod_to_linux[i], 1);
        else if (was && !now) emit(uinput_fd, EV_KEY, mod_to_linux[i], 0);
    }
    prev_mods = mod;

    // Отпущенные клавиши
    for (int i = 0; i < 6; i++) {
        uint8_t pk = prev_keys[i];
        if (pk != 0) {
            int still_pressed = 0;
            for (int j = 0; j < 6; j++) {
                if (keys[j] == pk) { still_pressed = 1; break; }
            }
            if (!still_pressed && hid_to_linux[pk] != 0) {
                emit(uinput_fd, EV_KEY, hid_to_linux[pk], 0);
            }
        }
    }

    // Нажатые клавиши
    for (int i = 0; i < 6; i++) {
        uint8_t ck = keys[i];
        if (ck != 0) {
            int was_pressed = 0;
            for (int j = 0; j < 6; j++) {
                if (prev_keys[j] == ck) { was_pressed = 1; break; }
            }
            if (!was_pressed && hid_to_linux[ck] != 0) {
                emit(uinput_fd, EV_KEY, hid_to_linux[ck], 1);
            }
        }
    }
    memcpy(prev_keys, keys, 6);
    syn(uinput_fd);
}

static void handle_mouse_packet(const uint8_t* data, size_t len) {
    if (len < 5) return;
    uint8_t btns = data[0];
    int8_t dx = (int8_t)data[1];
    int8_t dy = (int8_t)data[2];
    int8_t wheel = (int8_t)data[3];

    // Кнопки
    int btn_defs[3][2] = {
        {0x01, BTN_LEFT},
        {0x02, BTN_RIGHT},
        {0x04, BTN_MIDDLE}
    };
    for (int i = 0; i < 3; i++) {
        int was = (prev_mouse_btns & btn_defs[i][0]) != 0;
        int now = (btns & btn_defs[i][0]) != 0;
        if (now && !was) emit(uinput_fd, EV_KEY, btn_defs[i][1], 1);
        else if (was && !now) emit(uinput_fd, EV_KEY, btn_defs[i][1], 0);
    }
    prev_mouse_btns = btns;

    if (dx != 0) emit(uinput_fd, EV_REL, REL_X, dx);
    if (dy != 0) emit(uinput_fd, EV_REL, REL_Y, dy);
    if (wheel != 0) emit(uinput_fd, EV_REL, REL_WHEEL, wheel);
    syn(uinput_fd);
}

static void handle_consumer_packet(const uint8_t* data, size_t len) {
    if (len < 2) return;
    uint16_t code = data[0] | ((uint16_t)data[1] << 8);
    uint16_t lk = 0;
    if (code == 0x00E9) lk = KEY_VOLUMEUP;
    else if (code == 0x00EA) lk = KEY_VOLUMEDOWN;
    else if (code == 0x00E2) lk = KEY_MUTE;
    else if (code == 0x00CD) lk = KEY_PLAYPAUSE;
    else if (code == 0x00B5) lk = KEY_NEXTSONG;
    else if (code == 0x00B6) lk = KEY_PREVIOUSSONG;

    if (lk != 0) {
        emit(uinput_fd, EV_KEY, lk, 1);
        syn(uinput_fd);
        emit(uinput_fd, EV_KEY, lk, 0);
        syn(uinput_fd);
    }
}

int main(int argc, char* argv[]) {
    int port = 4321;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    init_key_map();

    printf("==================================================================\n");
    printf("   ESP32-S3 BLE Bridge — Linux uinput Receiver (v1.0)\n");
    printf("==================================================================\n");

    uinput_fd = setup_uinput_device();
    if (uinput_fd < 0) {
        return 1;
    }
    printf(" [+] Виртуальное устройство /dev/uinput успешно создано в ядре Linux\n");

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("[!] Ошибка создания UDP сокета");
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        return 1;
    }

    int opt = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("[!] Ошибка bind UDP сокета");
        close(sock_fd);
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        return 1;
    }

    printf(" [i] Прослушивание UDP порта: %d\n", port);
    printf(" [i] Нажмите Ctrl+C для остановки\n\n");

    uint8_t buf[256];
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int first_pkt = 1;

    while (running) {
        ssize_t n = recvfrom(sock_fd, buf, sizeof(buf), 0, (struct sockaddr*)&caddr, &clen);
        if (n <= 0) continue;

        if (first_pkt) {
            printf(" [+] Первый пакет получен от ESP32-S3 (%s:%d)!\n",
                   inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
            first_pkt = 0;
        }

        uint8_t type = buf[0];
        const uint8_t* payload = &buf[1];
        size_t plen = n - 1;

        if (type == 0x01) {
            handle_keyboard_packet(payload, plen);
        } else if (type == 0x02) {
            handle_mouse_packet(payload, plen);
        } else if (type == 0x03) {
            handle_consumer_packet(payload, plen);
        } else if (type == 0x04 && plen >= 10) {
            int lx = (int)payload[0] - 128;
            int ly = (int)payload[1] - 128;
            int rx = (int)payload[2] - 128;
            int ry = (int)payload[3] - 128;
            uint8_t l2 = payload[4], r2 = payload[5], hat = payload[6];
            uint32_t btns = payload[7] | (payload[8] << 8) | (payload[9] << 16);
            printf("\r[Gamepad] LX:%4d LY:%4d | RX:%4d RY:%4d | L2:%3d R2:%3d | Hat:%d | Btns:0x%04X   ",
                   lx, ly, rx, ry, l2, r2, hat, btns);
            fflush(stdout);
        }
    }

    printf("\n\n[i] Остановка приёмника и удаление виртуального устройства...\n");
    close(sock_fd);
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    printf("[i] Готово.\n");
    return 0;
}
