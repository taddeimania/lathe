#pragma once
#include "Widgets.h"

namespace lathe
{
/** View > Play Along: where the notes of the song's key are on a guitar, bass, ukulele and
    keyboard, so you can jam along. It follows the playhead (the start marker while stopped) and
    the key lane: when a key change is a bar away, the next key's notes fade in. */
class PlayAlongWindow : public DocumentWindow
{
public:
    /** Opens the window, or brings it to the front if it's already open. */
    static void show (AppContext&);
    static void closeIfOpen();
    static bool isOpen()   { return current != nullptr; }

    explicit PlayAlongWindow (AppContext&);
    ~PlayAlongWindow() override;
    void closeButtonPressed() override;

    /** What the window shows at a beat (for tests and the screenshots). */
    struct State
    {
        const KeyChange* key = nullptr;    // the key now, or null
        const KeyChange* next = nullptr;   // the next key change, or null
        float nextFade = 0.0f;             // 0, rising to 1 over the last bar before `next`
    };
    static State stateAt (const Edit&, double beat);

private:
    class Content;
    static Component::SafePointer<PlayAlongWindow> current;
    AppContext& ctx;
};
} // namespace lathe
