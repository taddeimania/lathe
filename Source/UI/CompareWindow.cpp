#include "CompareWindow.h"
#include "Theme.h"

namespace lathe
{
Component::SafePointer<CompareWindow> CompareWindow::current;

namespace
{
String timeText (double seconds)
{
    const int s = (int) std::floor (jmax (0.0, seconds));
    return String (s / 60) + ":" + String (s % 60).paddedLeft ('0', 2);
}

String dbText (float v, int decimals = 1)
{
    return (v > 0.05f ? "+" : "") + String (v, decimals);
}
} // namespace

//==============================================================================
class CompareWindow::Content : public Component,
                               public FileDragAndDropTarget,
                               private ChangeListener,
                               private Timer
{
public:
    Content (Compare& c, std::function<void (const String&)> st) : cmp (c), status (std::move (st))
    {
        for (int s = 0; s < Compare::numSlots; ++s)
        {
            auto& b = actions[(size_t) s];
            addAndMakeVisible (b);
        }
        actions[Compare::before].setButtonText ("Mark Before");
        actions[Compare::before].setTooltip ("Render the mix as it is now, to compare your next changes against");
        actions[Compare::before].onClick = [this] { if (! cancelIfBusy (Compare::before)) renderSlot (Compare::before); };
        actions[Compare::now].setButtonText ("Update Now");
        actions[Compare::now].setTooltip ("Render the mix as it is now (over the same bars as Before)");
        actions[Compare::now].onClick = [this] { if (! cancelIfBusy (Compare::now)) renderSlot (Compare::now); };
        actions[Compare::reference].setButtonText ("Choose File...");
        actions[Compare::reference].setTooltip ("A finished track to compare against. You can also drop an audio file on this window");
        actions[Compare::reference].onClick = [this] { if (! cancelIfBusy (Compare::reference)) chooseReference(); };

        playButton.onClick = [this] { togglePlay(); };
        playButton.setTooltip ("Play or stop the comparison (Space)");
        addAndMakeVisible (playButton);

        match.setButtonText ("Match loudness");
        match.setTooltip ("Play every version at the loudness of the quietest, so the louder one doesn't sound better just for being louder");
        match.setToggleState (cmp.getMatchLoudness(), dontSendNotification);
        match.onClick = [this] { cmp.setMatchLoudness (match.getToggleState()); };
        addAndMakeVisible (match);

        hint.setText ("Click a version to hear it, or press 1, 2 or 3. It switches in place, so you hear the same moment. Space plays and stops.",
                      dontSendNotification);
        hint.setColour (Label::textColourId, theme::textDim);
        hint.setFont (theme::font (12.5f));
        hint.setJustificationType (Justification::topLeft);
        hint.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (hint);

        summary.setColour (Label::textColourId, theme::text);
        summary.setFont (theme::font (13.5f));
        summary.setJustificationType (Justification::topLeft);
        summary.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (summary);

        setWantsKeyboardFocus (true);
        cmp.addChangeListener (this);
        setSize (760, 640);
        update();
        startTimerHz (20);
    }

    ~Content() override
    {
        cmp.stop();   // closing the window stops what it plays
        cmp.removeChangeListener (this);
    }

    void togglePlay()
    {
        if (cmp.isPlaying())
            cmp.stop();
        else
            cmp.play();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 14);
        auto tilesRow = r.removeFromTop (96);
        const int gap = 12;
        const int labelW = 150;
        hint.setBounds (tilesRow.removeFromLeft (labelW).withTrimmedRight (14).withTrimmedTop (4));
        const int w = (tilesRow.getWidth() - 2 * gap) / 3;
        for (int s = 0; s < Compare::numSlots; ++s)
        {
            tiles[(size_t) s] = tilesRow.removeFromLeft (w);
            tilesRow.removeFromLeft (gap);
        }
        r.removeFromTop (8);
        auto actionRow = r.removeFromTop (28);
        for (int s = 0; s < Compare::numSlots; ++s)
            actions[(size_t) s].setBounds (tiles[(size_t) s].getX(), actionRow.getY(), tiles[(size_t) s].getWidth(), 28);

        r.removeFromTop (14);
        auto transport = r.removeFromTop (32);
        playButton.setBounds (transport.removeFromLeft (labelW - 12));
        transport.removeFromLeft (12);
        match.setBounds (transport.removeFromRight (150));
        transport.removeFromRight (12);
        bar = transport;

        r.removeFromTop (16);
        summary.setBounds (r.removeFromBottom (64));
        r.removeFromBottom (10);
        table = r;
    }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);

        // The three versions.
        for (int s = 0; s < Compare::numSlots; ++s)
        {
            const auto slot = (Compare::Slot) s;
            const auto& v = cmp.get (slot);
            const auto area = tiles[(size_t) s].toFloat();
            const bool heard = cmp.getHeard() == slot;
            g.setColour (heard ? theme::panelRaised.brighter (0.08f) : theme::lane);
            g.fillRoundedRectangle (area, 6.0f);
            g.setColour (heard ? theme::accent : theme::border);
            g.drawRoundedRectangle (area.reduced (heard ? 1.0f : 0.5f), 6.0f, heard ? 2.0f : 1.0f);

            auto inner = tiles[(size_t) s].reduced (14, 10);
            g.setColour (v.has() ? theme::text : theme::textDim);
            g.setFont (theme::font (19.0f, true));
            g.drawText (Compare::slotName (slot), inner.removeFromTop (28), Justification::centredLeft);
            g.setFont (theme::font (12.5f));
            g.setColour (theme::textDim);
            String line1, line2;
            if (cmp.isBusy() && (int) cmp.busySlot() == s)
                line1 = cmp.busyText() + "... " + String (roundToInt (cmp.getProgress() * 100.0)) + "%";
            else if (! v.has())
                line1 = s == Compare::reference ? "Choose a finished track you like"
                      : s == Compare::before ? "Not marked yet" : "Not rendered yet";
            else
            {
                if (s == Compare::reference)
                    line1 = v.file.getFileName();
                else
                    line1 = cmp.rangeText (slot) + ", " + timeText ((double) v.audio->getNumSamples() / jmax (1.0, v.sampleRate));
                line2 = Time (v.madeAt).toString (false, true, false, true);
                if (v.measures.lufsValid)
                    line2 << "   " << String (v.measures.lufs, 1) << " LUFS";
                const float gain = cmp.gainFor (slot);
                if (cmp.getMatchLoudness() && std::abs (gain) >= 0.05f)
                    line2 << ", plays at " << dbText (gain) << " dB";
            }
            g.drawFittedText (line1, inner.removeFromTop (20), Justification::centredLeft, 1, 0.85f);
            g.drawFittedText (line2, inner.removeFromTop (20), Justification::centredLeft, 1, 0.85f);
        }

        // Where it is.
        {
            const auto area = bar.toFloat().reduced (0.0f, 6.0f);
            g.setColour (theme::lane);
            g.fillRoundedRectangle (area, 4.0f);
            const double len = cmp.getLengthSeconds();
            if (len > 0.0)
            {
                const auto done = area.withWidth (area.getWidth() * (float) jlimit (0.0, 1.0, cmp.getPositionSeconds() / len));
                g.setColour (theme::accent.withAlpha (cmp.isPlaying() ? 0.9f : 0.5f));
                g.fillRoundedRectangle (done, 4.0f);
            }
            g.setColour (theme::text);
            g.setFont (theme::font (12.5f));
            g.drawText (timeText (cmp.getPositionSeconds()) + " / " + timeText (len), bar.reduced (10, 0), Justification::centredRight);
        }

        paintTable (g);
    }

    void mouseDown (const MouseEvent& e) override
    {
        for (int s = 0; s < Compare::numSlots; ++s)
            if (tiles[(size_t) s].contains (e.getPosition()))
            {
                cmp.setHeard ((Compare::Slot) s);
                return;
            }
        if (bar.contains (e.getPosition()))
            seekTo (e.x);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (bar.contains (e.getMouseDownPosition()))
            seekTo (e.x);
    }

    bool keyPressed (const KeyPress& k) override
    {
        const auto c = k.getTextCharacter();
        if (c >= '1' && c <= '3' && ! k.getModifiers().isAnyModifierKeyDown())
        {
            cmp.setHeard ((Compare::Slot) (c - '1'));
            return true;
        }
        if (k == KeyPress::spaceKey)
        {
            togglePlay();
            return true;
        }
        if (k == KeyPress::escapeKey && cmp.isBusy())
        {
            cmp.cancel();
            return true;
        }
        return false;
    }

    // Drop an audio file to make it the reference.
    bool isInterestedInFileDrag (const StringArray& files) override
    {
        return files.size() == 1 && File (files[0]).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;m4a;aac;caf");
    }
    void filesDropped (const StringArray& files, int, int) override   { loadReference (File (files[0])); }

private:
    Compare& cmp;
    std::function<void (const String&)> status;
    std::array<Rectangle<int>, Compare::numSlots> tiles;
    std::array<TextButton, Compare::numSlots> actions;
    TextButton playButton;
    ToggleButton match;
    Label hint, summary;
    Rectangle<int> bar, table;
    /** A version's button while it's being made: Cancel. True if it cancelled. */
    bool cancelIfBusy (Compare::Slot s)
    {
        if (! cmp.isBusy() || cmp.busySlot() != s)
            return false;
        cmp.cancel();
        return true;
    }
    std::unique_ptr<FileChooser> chooser;

    void timerCallback() override
    {
        if (cmp.isPlaying() && cmp.songIsPlaying())
            cmp.stop();   // someone pressed Play on the song
        repaint (bar);
        if (cmp.isBusy())
            repaint();
    }

    void changeListenerCallback (ChangeBroadcaster*) override   { update(); }

    void update()
    {
        playButton.setButtonText (cmp.isPlaying() ? "Stop" : "Play");
        bool any = false;
        for (int s = 0; s < Compare::numSlots; ++s)
            any = any || cmp.get ((Compare::Slot) s).has();
        playButton.setEnabled (any);
        // While a version is being made, its button cancels it (so does Escape); the others wait.
        actions[Compare::before].setButtonText ("Mark Before");
        actions[Compare::now].setButtonText ("Update Now");
        actions[Compare::reference].setButtonText (cmp.get (Compare::reference).has() ? "Change File..." : "Choose File...");
        for (int s = 0; s < Compare::numSlots; ++s)
        {
            auto& b = actions[(size_t) s];
            const bool making = cmp.isBusy() && (int) cmp.busySlot() == s;
            if (making)
                b.setButtonText ("Cancel");
            b.setEnabled (! cmp.isBusy() || making);
        }

        auto text = cmp.summary();
        if (text.isEmpty())
            text = cmp.get (Compare::before).has() ? "Change the mix, then press Update Now to hear what changed."
                                                   : "Press Mark Before, change the mix, then Update Now. Or ask your AI assistant to mark Before before it mixes.";
        summary.setText (text, dontSendNotification);
        repaint();
    }

    void seekTo (int x)
    {
        const double len = cmp.getLengthSeconds();
        if (len <= 0.0 || bar.getWidth() <= 0)
            return;
        cmp.setPositionSeconds (len * jlimit (0.0, 1.0, (double) (x - bar.getX()) / (double) bar.getWidth()));
        repaint (bar);
    }

    void renderSlot (Compare::Slot s)
    {
        auto r = cmp.render (s, [this, s] (Result res)
        {
            if (res.failed() && status)
                status ("Compare: " + res.getErrorMessage());
            else if (status)
                status ("Compare: " + Compare::slotName (s) + " is ready");
            if (res.wasOk())
                cmp.setHeard (s);
        });
        if (r.failed())
        {
            if (status) status ("Compare: " + r.getErrorMessage());
        }
        repaint();
    }

    void loadReference (const File& f)
    {
        auto r = cmp.loadReference (f, [this] (Result res)
        {
            if (status) status (res.failed() ? "Compare: " + res.getErrorMessage() : "Compare: the reference is ready");
        });
        if (r.failed())
        {
            if (status) status ("Compare: " + r.getErrorMessage());
        }
        repaint();
    }

    void chooseReference()
    {
        const auto& v = cmp.get (Compare::reference);
        const auto start = v.has() ? v.file.getParentDirectory() : File::getSpecialLocation (File::userMusicDirectory);
        chooser = std::make_unique<FileChooser> ("Choose a reference track", start, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.aac;*.caf");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                      loadReference (fc.getResult());
                              });
    }

    // --- the numbers, one row per measurement, one column per version
    void paintTable (Graphics& g)
    {
        if (table.isEmpty())
            return;
        struct Row { String name; std::function<String (const Compare::Measures&)> value; std::function<float (const Compare::Measures&)> number; };
        std::vector<Row> rows {
            { "Loudness (LUFS)", [] (auto& m) { return m.lufsValid ? String (m.lufs, 1) : String ("quiet"); }, [] (auto& m) { return m.lufs; } },
            { "True peak (dBTP)", [] (auto& m) { return String (m.truePeakDb, 1); }, [] (auto& m) { return m.truePeakDb; } },
            { "Crest (dB)", [] (auto& m) { return String (m.crestDb, 1); }, [] (auto& m) { return m.crestDb; } },
            { "Width (side vs mid, dB)", [] (auto& m) { return m.widthDb < -40.0f ? String ("mono") : String (m.widthDb, 1); }, [] (auto& m) { return m.widthDb; } },
            { "Correlation", [] (auto& m) { return String (m.correlation, 2); }, nullptr },
        };
        for (int b = 0; b < 7; ++b)
            rows.push_back ({ String (Compare::bandNames[b]) + " (dB)",
                              [b] (auto& m) { return String (m.bandsDb[b], 1); },
                              [b] (auto& m) { return m.bandsDb[b]; } });

        const int rowH = jlimit (18, 24, table.getHeight() / ((int) rows.size() + 1));
        auto r = table;
        int y = r.getY();
        g.setFont (theme::font (12.5f));
        g.setColour (theme::textDim);
        g.drawText ("Measured", r.getX(), y, 150, rowH, Justification::centredLeft);
        g.drawText ("Balance: each band's share of the whole", tiles[0].getX(), y, tiles[2].getRight() - tiles[0].getX(), rowH, Justification::centredRight);
        y += rowH;

        const auto& before = cmp.get (Compare::before);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (i == 5)
            {
                g.setColour (theme::border);
                g.drawHorizontalLine (y + 2, (float) r.getX(), (float) r.getRight());
                y += 6;
            }
            if (i % 2 == 0)
            {
                g.setColour (theme::lane.withAlpha (0.6f));
                g.fillRect (r.getX(), y, r.getWidth(), rowH);
            }
            g.setColour (theme::textDim);
            g.setFont (theme::font (13.0f));
            g.drawText (rows[i].name, r.getX() + 6, y, 150, rowH, Justification::centredLeft);
            for (int s = 0; s < Compare::numSlots; ++s)
            {
                const auto& v = cmp.get ((Compare::Slot) s);
                const auto cell = Rectangle<int> (tiles[(size_t) s].getX() + 14, y, tiles[(size_t) s].getWidth() - 28, rowH);
                if (! v.has() || ! v.measures.valid)
                {
                    g.setColour (theme::textDim.withAlpha (0.5f));
                    g.drawText ("-", cell, Justification::centredLeft);
                    continue;
                }
                g.setColour (theme::text);
                g.setFont (theme::font (13.0f));
                g.drawText (rows[i].value (v.measures), cell, Justification::centredLeft);
                // Now against Before, where both are there.
                if (s == Compare::now && before.has() && before.measures.valid && rows[i].number != nullptr)
                {
                    const float d = rows[i].number (v.measures) - rows[i].number (before.measures);
                    if (std::abs (d) >= 0.1f)
                    {
                        g.setColour (std::abs (d) >= 1.0f ? theme::accent : theme::textDim);
                        g.setFont (theme::font (12.0f));
                        g.drawText (dbText (d) + " vs Before", cell, Justification::centredRight);
                    }
                }
            }
            y += rowH;
        }
    }
};

//==============================================================================
CompareWindow::CompareWindow (Compare& c, std::function<void (const String&)> status)
    : DocumentWindow ("Compare", theme::panel, DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new Content (c, std::move (status)), true);
    setResizable (true, true);
    setResizeLimits (640, 560, 1400, 1200);
    centreWithSize (760, 640);
}

void CompareWindow::closeButtonPressed()   { delete this; }

void CompareWindow::show (Compare& c, std::function<void (const String&)> status)
{
    if (auto* w = current.getComponent())
    {
        w->toFront (true);
        return;
    }
    auto* w = new CompareWindow (c, std::move (status));
    current = w;
    w->setVisible (true);
    if (auto* content = w->getContentComponent())
        content->grabKeyboardFocus();
}

void CompareWindow::closeIfOpen()
{
    if (auto* w = current.getComponent())
        delete w;
}

bool CompareWindow::takeSpace()
{
    auto* w = current.getComponent();
    if (w == nullptr || ! w->isActiveWindow())
        return false;
    if (auto* content = dynamic_cast<Content*> (w->getContentComponent()))
    {
        content->togglePlay();
        return true;
    }
    return false;
}
} // namespace lathe
