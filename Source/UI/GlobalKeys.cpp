#include "GlobalKeys.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace lathe::globalkeys
{
namespace
{
std::function<void()>& action()
{
    static std::function<void()> f;
    return f;
}
} // namespace

bool spaceIsForPlayback()
{
    if (Component::getCurrentlyModalComponent() != nullptr)
        return false;   // a dialog: its buttons and fields come first
    if (auto* focused = Component::getCurrentlyFocusedComponent())
        if (dynamic_cast<TextInputTarget*> (focused) != nullptr && focused->isShowing())
            return false;   // typing a name, a note, a value
    return true;
}

bool handleSpace()
{
    if (! spaceIsForPlayback() || ! action())
        return false;
    action()();
    return true;
}

#if JUCE_WINDOWS
namespace
{
HHOOK hook = nullptr;

bool isTextField (HWND h)
{
    wchar_t name[64] = {};
    if (h == nullptr || GetClassNameW (h, name, 63) == 0)
        return false;
    // Standard edit boxes and rich-edit controls (plug-ins' own text fields).
    const String cls (name);
    return cls.containsIgnoreCase ("edit");
}

LRESULT CALLBACK onMessage (int code, WPARAM removing, LPARAM param)
{
    if (code == HC_ACTION && removing == PM_REMOVE)
    {
        auto* msg = reinterpret_cast<MSG*> (param);
        if (msg->message == WM_KEYDOWN && msg->wParam == VK_SPACE)
        {
            const bool modifier = (GetKeyState (VK_CONTROL) & 0x8000) || (GetKeyState (VK_MENU) & 0x8000)
                               || (GetKeyState (VK_SHIFT) & 0x8000) || (GetKeyState (VK_LWIN) & 0x8000)
                               || (GetKeyState (VK_RWIN) & 0x8000);
            if (! modifier && ! isTextField (msg->hwnd) && spaceIsForPlayback())
            {
                const bool repeat = (msg->lParam & (1 << 30)) != 0;
                if (repeat || handleSpace())
                    msg->message = WM_NULL;   // nobody else gets it (and no WM_CHAR follows)
            }
        }
    }
    return CallNextHookEx (hook, code, removing, param);
}
} // namespace

void install (std::function<void()> toggle)
{
    action() = std::move (toggle);
    if (hook == nullptr)
        hook = SetWindowsHookExW (WH_GETMESSAGE, onMessage, nullptr, GetCurrentThreadId());
}

void uninstall()
{
    if (hook != nullptr)
        UnhookWindowsHookEx (hook);
    hook = nullptr;
    action() = nullptr;
}

#elif ! JUCE_MAC
// Linux: no app-wide hook. MainComponent and plug-in windows call handleSpace().
void install (std::function<void()> toggle)   { action() = std::move (toggle); }
void uninstall()                              { action() = nullptr; }
#endif

#if JUCE_MAC
// The monitor lives in GlobalKeys.mm; it needs the action from here.
std::function<void()>& macAction()   { return action(); }
#endif
} // namespace lathe::globalkeys
