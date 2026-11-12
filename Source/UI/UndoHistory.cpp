#include "UndoHistory.h"
#include "Theme.h"

namespace lathe
{
Component::SafePointer<UndoHistoryWindow> UndoHistoryWindow::current;

std::vector<UndoHistoryWindow::Row> UndoHistoryWindow::rowsFor (const Edit& edit)
{
    std::vector<Row> rows;
    const auto& done = edit.getDoneSteps();
    const auto undone = edit.getUndoneSteps();   // the next redo first
    const int doneCount = (int) done.size();

    // Newest at the top: the furthest redo, down to the next one...
    for (int j = (int) undone.size() - 1; j >= 0; --j)
    {
        Row r;
        r.label = undone[(size_t) j].label;
        r.byAssistant = undone[(size_t) j].byAssistant;
        r.timeMs = undone[(size_t) j].timeMs;
        r.undone = true;
        r.depthWhenClicked = doneCount + j + 1;   // redo up to and including it
        rows.push_back (r);
    }
    // ...then what's done, newest first...
    for (int i = doneCount - 1; i >= 0; --i)
    {
        Row r;
        r.label = done[(size_t) i].label;
        r.byAssistant = done[(size_t) i].byAssistant;
        r.timeMs = done[(size_t) i].timeMs;
        r.depthWhenClicked = i;                    // back to just before it
        rows.push_back (r);
    }
    // ...and where it started.
    Row start;
    start.label = "Opened";
    start.start = true;
    start.depthWhenClicked = 0;
    rows.push_back (start);
    return rows;
}

//==============================================================================
class UndoHistoryWindow::Content : public Component, private ListBoxModel, private ChangeListener
{
public:
    Content (Edit& e, std::function<bool()> can, std::function<void (const String&)> st)
        : edit (e), canChange (std::move (can)), status (std::move (st))
    {
        hint.setText ("Click a step to undo back to just before it. Greyed steps were undone: click one to redo up to it.",
                      dontSendNotification);
        hint.setColour (Label::textColourId, theme::textDim);
        hint.setFont (theme::font (12.5f));
        hint.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (hint);

        undoAssistant.setButtonText ("Undo the assistant's last changes");
        undoAssistant.setTooltip ("Undoes the run of AI assistant steps at the top of the list, and anything you did after them");
        undoAssistant.onClick = [this] { undoLastAssistantRun(); };
        addAndMakeVisible (undoAssistant);

        list.setModel (this);
        list.setRowHeight (40);
        list.setColour (ListBox::backgroundColourId, theme::lane);
        list.setColour (ListBox::outlineColourId, theme::border);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        edit.historyChanged.addChangeListener (this);
        setSize (460, 520);
        refresh();
    }

    ~Content() override   { edit.historyChanged.removeChangeListener (this); }

    void paint (Graphics& g) override   { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        hint.setBounds (r.removeFromTop (34));
        r.removeFromTop (6);
        undoAssistant.setBounds (r.removeFromBottom (28).removeFromLeft (260));
        r.removeFromBottom (8);
        list.setBounds (r);
    }

private:
    Edit& edit;
    std::function<bool()> canChange;
    std::function<void (const String&)> status;
    Label hint;
    TextButton undoAssistant;
    ListBox list;
    std::vector<Row> rows;

    void refresh()
    {
        rows = rowsFor (edit);
        list.updateContent();
        list.repaint();
        bool anyAssistantDone = false;
        for (auto& r : rows)
            if (! r.undone && r.byAssistant)
                anyAssistantDone = true;
        undoAssistant.setEnabled (anyAssistantDone);
    }

    void changeListenerCallback (ChangeBroadcaster*) override   { refresh(); }

    int getNumRows() override   { return (int) rows.size(); }

    void paintListBoxItem (int row, Graphics& g, int w, int h, bool) override
    {
        if (! isPositiveAndBelow (row, (int) rows.size()))
            return;
        const auto& r = rows[(size_t) row];
        const bool current = ! r.undone && (row == 0 || rows[(size_t) row - 1].undone);   // the state the song is in

        g.fillAll (row % 2 == 0 ? theme::lane : theme::laneAlt);
        if (current)
        {
            g.setColour (theme::headerSel);
            g.fillRect (0, 0, w, h);
            g.setColour (theme::accent);
            g.fillRect (0, 0, 3, h);
        }
        g.setColour (theme::border);
        g.drawHorizontalLine (h - 1, 0.0f, (float) w);

        auto area = Rectangle<int> (0, 0, w, h).reduced (12, 0);
        const float alpha = r.undone ? 0.45f : 1.0f;

        // When.
        g.setFont (theme::font (12.0f));
        g.setColour (theme::textDim.withMultipliedAlpha (alpha));
        const auto when = r.start ? String() : Time (r.timeMs).toString (false, true, false, true);
        g.drawText (when, area.removeFromLeft (52), Justification::centredLeft);

        // Who.
        if (! r.start)
        {
            const String who = r.byAssistant ? "Assistant" : "You";
            auto tag = area.removeFromLeft (r.byAssistant ? 78 : 44).withSizeKeepingCentre (r.byAssistant ? 72 : 38, 20);
            g.setColour ((r.byAssistant ? theme::accent : theme::control).withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (tag.toFloat(), 4.0f);
            g.setColour ((r.byAssistant ? theme::textDark : theme::text).withMultipliedAlpha (alpha));
            g.setFont (theme::font (12.0f, true));
            g.drawText (who, tag, Justification::centred);
            area.removeFromLeft (8);
        }

        // What.
        g.setFont (theme::font (13.5f));
        g.setColour ((r.start ? theme::textDim : theme::text).withMultipliedAlpha (alpha));
        g.drawFittedText (r.label, area, Justification::centredLeft, 2, 0.9f);
    }

    String getTooltipForRow (int row) override
    {
        if (! isPositiveAndBelow (row, (int) rows.size()))
            return {};
        const auto& r = rows[(size_t) row];
        if (r.start)  return "Undo everything since the project was opened";
        if (r.undone) return "Redo up to and including this step";
        return "Undo this step and everything after it";
    }

    void listBoxItemClicked (int row, const MouseEvent&) override
    {
        if (! isPositiveAndBelow (row, (int) rows.size()))
            return;
        goTo (rows[(size_t) row].depthWhenClicked);
    }

    void goTo (int depth)
    {
        if (canChange && ! canChange())
            return;
        const int before = edit.getUndoDepth();
        if (! edit.goToDepth (depth))
            return;
        const int moved = edit.getUndoDepth() - before;
        if (status)
            status (moved < 0 ? "Undid " + String (-moved) + (moved == -1 ? " step" : " steps")
                              : "Redid " + String (moved) + (moved == 1 ? " step" : " steps"));
    }

    void undoLastAssistantRun()
    {
        // From the top of what's done: past anything of yours that came after, back to just
        // before the newest run of the assistant's steps.
        const auto& done = edit.getDoneSteps();
        int i = (int) done.size() - 1;
        while (i >= 0 && ! done[(size_t) i].byAssistant)
            --i;
        if (i < 0)
            return;
        while (i > 0 && done[(size_t) i - 1].byAssistant)
            --i;
        goTo (i);
    }
};

//==============================================================================
UndoHistoryWindow::UndoHistoryWindow (Edit& e, std::function<bool()> can, std::function<void (const String&)> st)
    : DocumentWindow ("Undo History", theme::panel, DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new Content (e, std::move (can), std::move (st)), true);
    setResizable (true, true);
    setResizeLimits (360, 260, 1000, 1400);
    centreWithSize (460, 520);
}

void UndoHistoryWindow::closeButtonPressed()   { delete this; }

void UndoHistoryWindow::closeIfOpen()
{
    if (auto* w = current.getComponent())
        delete w;
}

void UndoHistoryWindow::show (Edit& edit, std::function<bool()> canChange, std::function<void (const String&)> status)
{
    if (auto* w = current.getComponent())
    {
        w->toFront (true);
        return;
    }
    auto* w = new UndoHistoryWindow (edit, std::move (canChange), std::move (status));
    current = w;
    w->setVisible (true);
}
} // namespace lathe
