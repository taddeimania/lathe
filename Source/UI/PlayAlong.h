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

    /** What a diagram shows at a beat: the song's keys, or (track set) the keys that track
        plays in (its own, after the song's until its first). For tests and the screenshots. */
    struct State
    {
        std::optional<KeyChange> key;    // the key now, if there is one
        std::optional<KeyChange> next;   // the next key change, if there is one
        float nextFade = 0.0f;           // 0, rising to 1 over the last bar before `next`
    };
    static State stateAt (const Edit&, double beat, const Track* track = nullptr);

    /** The track an instrument's diagram follows on its own: a track with keys of its own whose
        name fits ("Bass" for Bass; "Gtr", "Lead Guitar" for Guitar; "Uke"; "Keys", "Piano" for
        Keyboard), or null (the song's key). */
    static std::shared_ptr<Track> autoFollow (const Edit&, const String& instrument);

private:
    class Content;
    static Component::SafePointer<PlayAlongWindow> current;
    AppContext& ctx;
};
} // namespace lathe
