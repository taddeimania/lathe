#pragma once
#include "../Model/Edit.h"

namespace lathe
{
/** Edit > Undo History: every step since the project was opened, newest at the top, with who
    made it (you or the AI assistant) and when. Click a step to take the song back to just
    before it; click a step that was undone (greyed) to bring it and everything before it back.
    It is the same trail Cmd/Ctrl+Z walks, kept for the session. */
class UndoHistoryWindow : public DocumentWindow
{
public:
    /** Opens the window, or brings it to the front if it's already open. */
    static void show (Edit& edit, std::function<bool()> canChange, std::function<void (const String&)> status);
    /** Closes it (when Lathe quits). */
    static void closeIfOpen();

    UndoHistoryWindow (Edit&, std::function<bool()> canChange, std::function<void (const String&)> status);
    void closeButtonPressed() override;

    /** The rows as the window shows them, newest first (for tests). */
    struct Row
    {
        String label;
        bool byAssistant = false;
        bool undone = false;     // greyed: undone, can be redone
        bool start = false;      // the bottom row: the song as it was opened
        int64 timeMs = 0;
        int depthWhenClicked = 0;
    };
    static std::vector<Row> rowsFor (const Edit&);

private:
    class Content;
    static Component::SafePointer<UndoHistoryWindow> current;
};
} // namespace lathe
