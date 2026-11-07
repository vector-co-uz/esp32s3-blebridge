using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;

namespace Esp32BleBridge
{
    class Program
    {
        #region Win32 SendInput API
        [StructLayout(LayoutKind.Sequential)]
        struct INPUT
        {
            public uint type;
            public InputUnion u;
        }

        [StructLayout(LayoutKind.Explicit)]
        struct InputUnion
        {
            [FieldOffset(0)] public MOUSEINPUT mi;
            [FieldOffset(0)] public KEYBDINPUT ki;
            [FieldOffset(0)] public HARDWAREINPUT hi;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct MOUSEINPUT
        {
            public int dx;
            public int dy;
            public uint mouseData;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct KEYBDINPUT
        {
            public ushort wVk;
            public ushort wScan;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct HARDWAREINPUT
        {
            public uint uMsg;
            public ushort wParamL;
            public ushort wParamH;
        }

        const uint INPUT_MOUSE = 0;
        const uint INPUT_KEYBOARD = 1;

        const uint KEYEVENTF_EXTENDEDKEY = 0x0001;
        const uint KEYEVENTF_KEYUP = 0x0002;

        const uint MOUSEEVENTF_MOVE = 0x0001;
        const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
        const uint MOUSEEVENTF_LEFTUP = 0x0004;
        const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
        const uint MOUSEEVENTF_RIGHTUP = 0x0010;
        const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020;
        const uint MOUSEEVENTF_MIDDLEUP = 0x0040;
        const uint MOUSEEVENTF_WHEEL = 0x0800;

        const int WHEEL_DELTA = 120;

        [DllImport("user32.dll", SetLastError = true)]
        static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

        static void SendKey(ushort vk, bool down)
        {
            INPUT[] inputs = new INPUT[1];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].u.ki.wVk = vk;
            inputs[0].u.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            // Extended keys like arrow keys, Insert, Delete, etc.
            if ((vk >= 0x21 && vk <= 0x28) || (vk >= 0x2D && vk <= 0x2E) || vk == 0x5B || vk == 0x5C || (vk >= 0xAD && vk <= 0xB3))
            {
                inputs[0].u.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
        }

        static void SendMouse(int dx, int dy, int wheel, byte buttons, ref byte prevButtons)
        {
            List<INPUT> list = new List<INPUT>();

            // Buttons
            bool prevL = (prevButtons & 0x01) != 0;
            bool nowL  = (buttons & 0x01) != 0;
            if (nowL != prevL)
            {
                INPUT inp = new INPUT { type = INPUT_MOUSE };
                inp.u.mi.dwFlags = nowL ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                list.Add(inp);
            }

            bool prevR = (prevButtons & 0x02) != 0;
            bool nowR  = (buttons & 0x02) != 0;
            if (nowR != prevR)
            {
                INPUT inp = new INPUT { type = INPUT_MOUSE };
                inp.u.mi.dwFlags = nowR ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                list.Add(inp);
            }

            bool prevM = (prevButtons & 0x04) != 0;
            bool nowM  = (buttons & 0x04) != 0;
            if (nowM != prevM)
            {
                INPUT inp = new INPUT { type = INPUT_MOUSE };
                inp.u.mi.dwFlags = nowM ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                list.Add(inp);
            }
            prevButtons = buttons;

            // Move
            if (dx != 0 || dy != 0)
            {
                INPUT inp = new INPUT { type = INPUT_MOUSE };
                inp.u.mi.dwFlags = MOUSEEVENTF_MOVE;
                inp.u.mi.dx = dx;
                inp.u.mi.dy = dy;
                list.Add(inp);
            }

            // Wheel
            if (wheel != 0)
            {
                INPUT inp = new INPUT { type = INPUT_MOUSE };
                inp.u.mi.dwFlags = MOUSEEVENTF_WHEEL;
                inp.u.mi.mouseData = (uint)(wheel * WHEEL_DELTA);
                list.Add(inp);
            }

            if (list.Count > 0)
            {
                SendInput((uint)list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
            }
        }
        #endregion

        #region Keycode Mapping
        static readonly ushort[] ModVk = new ushort[]
        {
            0xA2, // 0x01: LCtrl
            0xA0, // 0x02: LShift
            0xA4, // 0x04: LAlt
            0x5B, // 0x08: LWin
            0xA3, // 0x10: RCtrl
            0xA1, // 0x20: RShift
            0xA5, // 0x40: RAlt
            0x5C  // 0x80: RWin
        };

        static readonly Dictionary<byte, ushort> HidToVk = new Dictionary<byte, ushort>();

        static void InitKeyMap()
        {
            // Letters A-Z (HID 0x04..0x1D -> VK_A..VK_Z = 0x41..0x5A)
            for (byte i = 0; i < 26; i++)
                HidToVk[(byte)(0x04 + i)] = (ushort)(0x41 + i);

            // Numbers 1-9 (HID 0x1E..0x26 -> 0x31..0x39)
            for (byte i = 0; i < 9; i++)
                HidToVk[(byte)(0x1E + i)] = (ushort)(0x31 + i);
            HidToVk[0x27] = 0x30; // 0

            // Special keys
            HidToVk[0x28] = 0x0D; // Enter
            HidToVk[0x29] = 0x1B; // Esc
            HidToVk[0x2A] = 0x08; // Backspace
            HidToVk[0x2B] = 0x09; // Tab
            HidToVk[0x2C] = 0x20; // Space
            HidToVk[0x2D] = 0xBD; // -
            HidToVk[0x2E] = 0xBB; // =
            HidToVk[0x2F] = 0xDB; // [
            HidToVk[0x30] = 0xDD; // ]
            HidToVk[0x31] = 0xDC; // \
            HidToVk[0x33] = 0xBA; // ;
            HidToVk[0x34] = 0xDE; // '
            HidToVk[0x35] = 0xC0; // `
            HidToVk[0x36] = 0xBC; // ,
            HidToVk[0x37] = 0xBE; // .
            HidToVk[0x38] = 0xBF; // /
            HidToVk[0x39] = 0x14; // CapsLock
            for (byte i = 0; i < 12; i++)
                HidToVk[(byte)(0x3A + i)] = (ushort)(0x70 + i); // F1..F12

            HidToVk[0x4A] = 0x24; // Home
            HidToVk[0x4B] = 0x21; // PgUp
            HidToVk[0x4C] = 0x2E; // Del
            HidToVk[0x4D] = 0x23; // End
            HidToVk[0x4E] = 0x22; // PgDn
            HidToVk[0x4F] = 0x27; // Right
            HidToVk[0x50] = 0x25; // Left
            HidToVk[0x51] = 0x28; // Down
            HidToVk[0x52] = 0x26; // Up
            HidToVk[0x49] = 0x2D; // Insert
        }
        #endregion

        static byte prevMods = 0;
        static HashSet<byte> prevKeys = new HashSet<byte>();
        static byte prevMouseBtns = 0;

        static void Main(string[] args)
        {
            int port = 4321;
            for (int i = 0; i < args.Length; i++)
            {
                if ((args[i] == "-p" || args[i] == "--port") && i + 1 < args.Length)
                {
                    int.TryParse(args[i + 1], out port);
                }
            }

            InitKeyMap();

            Console.Title = "ESP32-S3 BLE Bridge — HID Receiver (Windows)";
            Console.ForegroundColor = ConsoleColor.Cyan;
            Console.WriteLine("==================================================================");
            Console.WriteLine("   ESP32-S3 BLE Bridge — Native Windows HID Receiver (v1.0)");
            Console.WriteLine("==================================================================");
            Console.ResetColor();
            Console.WriteLine(" [i] Прослушивание UDP порта: {0}", port);
            Console.WriteLine(" [i] Для выхода нажмите Ctrl+C\n");

            UdpClient udp = null;
            try
            {
                udp = new UdpClient(port);
            }
            catch (Exception ex)
            {
                Console.ForegroundColor = ConsoleColor.Red;
                Console.WriteLine(" [!] Ошибка открытия UDP сокета на порту {0}: {1}", port, ex.Message);
                Console.ResetColor();
                return;
            }

            IPEndPoint remoteEp = new IPEndPoint(IPAddress.Any, 0);
            bool firstPkt = true;

            while (true)
            {
                try
                {
                    byte[] data = udp.Receive(ref remoteEp);
                    if (data == null || data.Length < 2) continue;

                    if (firstPkt)
                    {
                        Console.ForegroundColor = ConsoleColor.Green;
                        Console.WriteLine(" [+] Первый пакет получен от ESP32-S3 ({0})!", remoteEp);
                        Console.ResetColor();
                        firstPkt = false;
                    }

                    byte type = data[0];
                    if (type == 0x01 && data.Length >= 9) // Keyboard: [type, mod, 0, k0..k5]
                    {
                        byte mod = data[1];
                        HashSet<byte> currentKeys = new HashSet<byte>();
                        for (int i = 3; i < data.Length && i < 9; i++)
                        {
                            if (data[i] != 0) currentKeys.Add(data[i]);
                        }

                        // Modifiers diff
                        for (int b = 0; b < 8; b++)
                        {
                            bool was = (prevMods & (1 << b)) != 0;
                            bool now = (mod & (1 << b)) != 0;
                            if (now && !was) SendKey(ModVk[b], true);
                            else if (was && !now) SendKey(ModVk[b], false);
                        }
                        prevMods = mod;

                        // Released keys
                        foreach (byte k in prevKeys)
                        {
                            if (!currentKeys.Contains(k))
                            {
                                ushort vk;
                                if (HidToVk.TryGetValue(k, out vk)) SendKey(vk, false);
                            }
                        }

                        // Pressed keys
                        foreach (byte k in currentKeys)
                        {
                            if (!prevKeys.Contains(k))
                            {
                                ushort vk;
                                if (HidToVk.TryGetValue(k, out vk)) SendKey(vk, true);
                            }
                        }
                        prevKeys = currentKeys;
                    }
                    else if (type == 0x02 && data.Length >= 6) // Mouse: [type, btn, dx, dy, wheel, pan]
                    {
                        byte btn = data[1];
                        sbyte dx = (sbyte)data[2];
                        sbyte dy = (sbyte)data[3];
                        sbyte wheel = (sbyte)data[4];
                        SendMouse(dx, dy, wheel, btn, ref prevMouseBtns);
                    }
                    else if (type == 0x03 && data.Length >= 3) // Consumer: [type, codeLo, codeHi]
                    {
                        ushort code = (ushort)(data[1] | (data[2] << 8));
                        ushort vk = 0;
                        if (code == 0x00E9) vk = 0xAF;      // Vol Up
                        else if (code == 0x00EA) vk = 0xAE; // Vol Down
                        else if (code == 0x00E2) vk = 0xAD; // Mute
                        else if (code == 0x00CD) vk = 0xB3; // Play/Pause
                        else if (code == 0x00B5) vk = 0xB0; // Next
                        else if (code == 0x00B6) vk = 0xB1; // Prev

                        if (vk != 0)
                        {
                            SendKey(vk, true);
                            SendKey(vk, false);
                        }
                    }
                    else if (type == 0x04 && data.Length >= 11) // Gamepad: [type, lx+128, ly+128, rx+128, ry+128, l2, r2, hat, btn0, btn1, btn2]
                    {
                        int lx = (int)data[1] - 128;
                        int ly = (int)data[2] - 128;
                        int rx = (int)data[3] - 128;
                        int ry = (int)data[4] - 128;
                        byte l2 = data[5];
                        byte r2 = data[6];
                        byte hat = data[7];
                        int btns = data[8] | (data[9] << 8) | (data[10] << 16);
                        Console.Write("\r[Gamepad] LX:{0,4} LY:{1,4} | RX:{2,4} RY:{3,4} | L2:{4,3} R2:{5,3} | Hat:{6} | Btns:0x{7:X4}   ",
                            lx, ly, rx, ry, l2, r2, hat, btns);
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine(" [!] Error: {0}", ex.Message);
                }
            }
        }
    }
}
