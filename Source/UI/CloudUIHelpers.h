#pragma once

// Small helpers shared by the cloud windows (CloudUI.cpp, CloudLibrary.cpp).
#include "Widgets.h"

namespace lathe::cloudui
{
inline String timeAgo (int64 ms)
{
    const auto seconds = (Time::currentTimeMillis() - ms) / 1000;
    if (seconds < 60)       return "just now";
    if (seconds < 3600)     return String (seconds / 60) + " min ago";
    if (seconds < 86400)    return String (seconds / 3600) + " h ago";
    if (seconds < 7 * 86400) return String (seconds / 86400) + " days ago";
    return Time (ms).formatted ("%d %b %Y");
}

inline String shortName (const String& email)
{
    return email.upToFirstOccurrenceOf ("@", false, false);
}

inline void styleEditor (TextEditor& e, bool password = false)
{
    e.setFont (theme::font (14.0f));
    e.setIndents (8, 6);
    if (password)
        e.setPasswordCharacter ((juce_wchar) 0x2022);
}

inline void alert (const String& title, const String& message)
{
    AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::NoIcon)
                                .withTitle (title).withMessage (message).withButton ("OK"), nullptr);
}
} // namespace lathe::cloudui
