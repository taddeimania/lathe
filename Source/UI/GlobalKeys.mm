#include "GlobalKeys.h"

#import <AppKit/AppKit.h>

namespace lathe::globalkeys
{
std::function<void()>& macAction();   // GlobalKeys.cpp

namespace
{
id monitor = nil;
constexpr unsigned short spaceKeyCode = 49;   // kVK_Space

// One of Lathe's own windows (the main window, a plug-in's window, Compare...), not the Save
// panel or anything else that isn't Lathe's.
bool isLatheWindow (NSWindow* window)
{
    if (window == nil)
        return false;
    for (int i = ComponentPeer::getNumPeers(); --i >= 0;)
        if (auto* peer = ComponentPeer::getPeer (i))
            if (auto* view = (NSView*) peer->getNativeHandle(); view != nil && [view window] == window)
                return true;
    return false;
}

// Cmd+S and the other file shortcuts: handed to Lathe before a plug-in's view can keep them.
bool handleCommandEvent (NSEvent* event)
{
    const auto flags = event.modifierFlags;
    if ((flags & NSEventModifierFlagCommand) == 0 || ! isLatheWindow (event.window ?: [NSApp keyWindow]))
        return false;
    NSString* chars = event.charactersIgnoringModifiers;
    if (chars.length == 0)
        return false;
    const auto c = CharacterFunctions::toLowerCase ((juce_wchar) [chars characterAtIndex: 0]);
    int mods = ModifierKeys::commandModifier;
    if (flags & NSEventModifierFlagShift)   mods |= ModifierKeys::shiftModifier;
    if (flags & NSEventModifierFlagOption)  mods |= ModifierKeys::altModifier;
    if (flags & NSEventModifierFlagControl) mods |= ModifierKeys::ctrlModifier;
    return handleCommand (KeyPress ((int) c, ModifierKeys (mods), c));
}
}

void install (std::function<void()> toggle)
{
    macAction() = std::move (toggle);
    if (monitor != nil)
        return;

    // Every key press in Lathe's windows comes through here first, plug-in views included:
    // Space, and the file shortcuts (handleCommandEvent).
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask: NSEventMaskKeyDown
                                                    handler: ^NSEvent* (NSEvent* event)
    {
        if (event.keyCode != spaceKeyCode)
            return (! event.isARepeat && handleCommandEvent (event)) ? nil : event;
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
