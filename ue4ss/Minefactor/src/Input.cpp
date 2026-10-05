#include "Input.hpp"

#include <array>
#include <cstdint>
#include <windows.h>

#include "Link.hpp"
#include "Log.hpp"
#include "Player.hpp"

namespace mf
{
    namespace
    {
        // Windows scan code (lParam bits 16-23, +0x80 for extended keys) is the DirectInput code
        // SkyCraft's table maps to SDL scancodes / USB HID usages (what Minecraft 26 uses).
        constexpr auto kDikToSdl = [] {
            std::array<std::uint16_t, 256> t{};
            t[0x01] = 41;  // Esc
            for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
            t[0x0B] = 39;  // 0
            t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
            t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
            t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
            t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
            t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
            t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
            t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
            t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
            t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
            t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
            for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
            t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
            t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
            t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
            t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
            t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
            t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
            t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
            t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
            t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
            return t;
        }();

        // Keys the game keeps while Minecraft drives the player. Everything else is Minecraft's.
        constexpr std::uint32_t kDikEscape = 0x01;   // game menu (or closes an open Minecraft screen, MC side)
        constexpr std::uint32_t kDikConsole = 0x29;  // UE console (ConsoleEnablerMod)
        constexpr std::uint32_t kDikG = 0x22;        // game interact: rebind Abiotic Factor's Interact to G
        constexpr std::uint32_t kDikO = 0x18;        // Minecraft pause / options menu

        HWND window = nullptr;
        WNDPROC original = nullptr;

        BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM)
        {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);
            wchar_t cls[64]{};
            ::GetClassNameW(hwnd, cls, 64);
            if (pid == ::GetCurrentProcessId() && std::wstring_view(cls) == L"UnrealWindow" && ::IsWindowVisible(hwnd))
            {
                window = hwnd;
                return FALSE;
            }
            return TRUE;
        }

        void Button(int sdlButton, bool down)
        {
            Link::Get().PushInput(proto::kInMouseButton, static_cast<std::uint16_t>(sdlButton), down ? 1 : 0);
        }

        // Returns true if the message was Minecraft's (swallowed from the game).
        bool Route(UINT msg, WPARAM wp, LPARAM lp)
        {
            auto& link = Link::Get();
            switch (msg)
            {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
                const std::uint32_t dik = ((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0x80 : 0);
                if (dik == kDikEscape || dik == kDikConsole || dik == kDikG)
                {
                    return false;
                }
                if (dik == kDikO)
                {
                    if (down && !(lp & (1 << 30)))
                    {
                        link.PushInput(proto::kInOpenMenu, 0);
                    }
                    return true;
                }
                if (down && (lp & (1 << 30)))
                {
                    return true;  // auto-repeat: Minecraft tracks held keys itself
                }
                if (const auto sdl = kDikToSdl[dik])
                {
                    link.PushInput(proto::kInKey, sdl, down ? 1 : 0);
                }
                return true;
            }
            case WM_CHAR:
                if (wp >= 0x20 && wp != 0x7F)
                {
                    link.PushInput(proto::kInText, 0, static_cast<std::int32_t>(wp));
                }
                return wp != 0x1B;  // Esc's WM_CHAR stays the game's, like its keydown
            case WM_LBUTTONDOWN: Button(1, true); return true;
            case WM_LBUTTONUP: Button(1, false); return true;
            case WM_MBUTTONDOWN: Button(2, true); return true;
            case WM_MBUTTONUP: Button(2, false); return true;
            case WM_RBUTTONDOWN: Button(3, true); return true;
            case WM_RBUTTONUP: Button(3, false); return true;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                Button(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 4 : 5, msg == WM_XBUTTONDOWN);
                return true;
            case WM_MOUSEWHEEL:
                link.PushInput(proto::kInScroll, 0, GET_WHEEL_DELTA_WPARAM(wp));
                return true;
            default:
                return false;
            }
        }

        LRESULT CALLBACK Hook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_KEYDOWN && wp == VK_F8)
            {
                if (!(lp & (1 << 30)))
                {
                    ToggleMinecraft();
                }
                return 0;
            }
            if (msg == WM_KEYUP && wp == VK_F8)
            {
                return 0;
            }
            if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wp))
            {
                Link::Get().PushInput(proto::kInReleaseAll, 0);
            }
            else if (Puppeting() && !HostMenuOpen() && Route(msg, wp, lp))
            {
                return 0;
            }
            return ::CallWindowProcW(original, hwnd, msg, wp, lp);
        }
    }

    void InputInstall()
    {
        if (original)
        {
            return;
        }
        ::EnumWindows(FindGameWindow, 0);
        if (!window)
        {
            return;
        }
        original = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hook)));
        Log(L"input: hooked the game window ({})", original ? L"ok" : L"FAILED");
    }
}
