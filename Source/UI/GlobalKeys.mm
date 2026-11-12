#include "GlobalKeys.h"

#import <AppKit/AppKit.h>

namespace lathe::globalkeys
{
std::function<void()>& macAction();   // GlobalKeys.cpp

namespace
{
id monitor = nil;
constexpr unsigned short spaceKeyCode = 49;   // kVK_Space
}

void install (std::function<void()> toggle)
{
    macAction() = std::move (toggle);
    if (monitor != nil)
        return;

    // Every key press in Lathe's windows comes through here first, plug-in views included.
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask: NSEventMaskKeyDown
                                                    handler: ^NSEvent* (NSEvent* event)
    {
        if (event.keyCode != spaceKeyCode)
            return event;
        const auto modifiers = event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl
                                                      | NSEventModifierFlagOption | NSEventModifierFlagShift);
        if (modifiers != 0)
            return event;
        // Typing in a plug-in's own text field (Cocoa's field editor is an NSText).
        NSResponder* responder = [[NSApp keyWindow] firstResponder];
        if (responder != nil && [responder isKindOfClass: [NSText class]])
            return event;
        if (! spaceIsForPlayback())
            return event;
        if (event.isARepeat)
            return nil;   // held down: once is enough
        return handleSpace() ? nil : event;
    }];
   #if ! __has_feature(objc_arc)
    [monitor retain];
   #endif
}

void uninstall()
{
    if (monitor != nil)
    {
        [NSEvent removeMonitor: monitor];
       #if ! __has_feature(objc_arc)
        [monitor release];
       #endif
    }
    monitor = nil;
    macAction() = nullptr;
}
} // namespace lathe::globalkeys
