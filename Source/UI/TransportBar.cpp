#include "TransportBar.h"

namespace lathe
{
DragNumber::DragNumber (double minValue, double maxValue, int decimalPlaces, double dragStep)
    : minimum (minValue), maximum (maxValue), step (dragStep), decimals (decimalPlaces)
{
    setMouseCursor (MouseCursor::UpDownResizeCursor);
    setWantsKeyboardFocus (false);
}

void DragNumber::setValue (double v, bool notify)
{
    v = jlimit (minimum, maximum, decimals == 0 ? std::round (v) : v);
    if (v == value)
        return;

    value = v;
    repaint();
    if (notify && onChange)
        onChange (value);
}

void DragNumber::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::lane);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    if (editor == nullptr)
    {
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f));
        g.drawText (String (value, decimals) + suffix, getLocalBounds(), Justification::centred);
    }
}

void DragNumber::mouseDown (const MouseEvent&)
{
    dragStart = value;
}

void DragNumber::mouseDrag (const MouseEvent& e)
{
    const double fine = e.mods.isShiftDown() ? 0.1 : 1.0;
    const double delta = -e.getDistanceFromDragStartY() * step * fine;
    double v = dragStart + delta;
    if (decimals > 0 && ! e.mods.isShiftDown())
        v = std::round (v);
    setValue (v, true);
}

void DragNumber::mouseDoubleClick (const MouseEvent&)
{
    editor = std::make_unique<TextEditor>();
    editor->setFont (theme::font (14.0f));
    editor->setJustification (Justification::centred);
    editor->setText (String (value, decimals), false);
    editor->selectAll();
    editor->setBounds (getLocalBounds());
    addAndMakeVisible (*editor);
    editor->grabKeyboardFocus();

    auto finish = [this] (bool commit)
    {
        if (editor == nullptr)
            return;
        const auto text = editor->getText();
        MessageManager::callAsync ([safe = Component::SafePointer<DragNumber> (this), commit, text]
        {
            if (safe == nullptr)
                return;
            safe->editor.reset();
            if (commit && text.containsAnyOf ("0123456789"))
                safe->setValue (text.getDoubleValue(), true);
            safe->repaint();
        });
    };

    editor->onReturnKey = [finish] { finish (true); };
    editor->onEscapeKey = [finish] { finish (false); };
    editor->onFocusLost = [finish] { finish (true); };
}

void DragNumber::resized()
{
    if (editor != nullptr)
        editor->setBounds (getLocalBounds());
}

//==============================================================================
TransportIcon::TransportIcon (Shape s, Colour on) : Button ({}), shape (s), onColour (on)
{
    setWantsKeyboardFocus (false);
}

void TransportIcon::paintButton (Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool on = getToggleState();

    if (shape == Shape::record && on)
    {
        // Recording: a solid red button with a slow pulse, unmistakable next to Play.
        const double phase = std::sin (Time::getMillisecondCounterHiRes() * MathConstants<double>::twoPi / 1100.0);
        const float glow = 0.80f + 0.20f * (float) phase;
        g.setColour (onColour.withMultipliedBrightness (glow));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (Colours::white.withAlpha (0.65f + 0.35f * glow));
        g.fillEllipse (r.withSizeKeepingCentre (12.0f, 12.0f));
        return;
    }

    g.setColour (on ? onColour.withAlpha (0.18f) : (highlighted ? theme::controlHi : theme::control));
    if (down) g.setColour (theme::controlHi.darker (0.1f));
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    auto icon = r.withSizeKeepingCentre (12.0f, 12.0f);
    g.setColour (on ? onColour : theme::text);

    switch (shape)
    {
        case Shape::play:
        {
            Path p;
            p.addTriangle (icon.getX() + 1.0f, icon.getY(), icon.getX() + 1.0f, icon.getBottom(), icon.getRight(), icon.getCentreY());
            g.fillPath (p);
            break;
        }
        case Shape::stop:
            g.fillRect (icon.reduced (1.0f));
            break;
        case Shape::record:
            g.setColour (on ? onColour : theme::record.withSaturation (0.35f));
            g.fillEllipse (icon);
            break;
    }
}

//==============================================================================
TransportBar::TransportBar (AppContext& c) : ctx (c)
{
    for (auto* b : { (Button*) &playButton, (Button*) &stopButton, (Button*) &recordButton })
        addAndMakeVisible (b);

    playButton.setTooltip ("Play from the start marker  (Space)");
    stopButton.setTooltip ("Stop. Press again to go back to the start  (Space)");
    recordButton.setTooltip ("Record on armed tracks  (R)");

    playButton.onClick = [this] { ctx.engine.play(); };
    stopButton.onClick = [this] { ctx.engine.stop(); };
    recordButton.onClick = [this] { record(); };

    styleToggle (clickButton, "Click", theme::accent, "Metronome  (C)");
    clickButton.onClick = [this]
    {
        ctx.engine.metronomeOn = ! ctx.engine.metronomeOn.load();
        ctx.settings.setValue ("metronome", ctx.engine.metronomeOn.load());
    };
    addAndMakeVisible (clickButton);

    styleToggle (loopButton, "Loop", theme::accent, "Loop the bracketed region  (L)");
    loopButton.onClick = [this]
    {
        ctx.edit.loopEnabled = ! ctx.edit.loopEnabled;
        ctx.edit.changed();
    };
    addAndMakeVisible (loopButton);

    tempo.suffix = " BPM";
    tempo.onChange = [this] (double v) { ctx.edit.setTempo (v); };
    sigNum.onChange = [this] (double v) { ctx.edit.timeSigNum = (int) v; ctx.edit.changed(); };
    sigDen.onChange = [this] (double v)
    {
        // Only sensible denominators.
        int d = (int) v;
        d = d <= 2 ? 2 : d <= 4 ? 4 : d <= 8 ? 8 : 16;
        sigDen.setValue (d, false);
        ctx.edit.timeSigDen = d;
        ctx.edit.changed();
    };
    for (auto* n : { &tempo, &sigNum, &sigDen })
        addAndMakeVisible (n);

    for (auto* l : { &position, &time })
    {
        l->setFont (theme::font (15.0f));
        l->setJustificationType (Justification::centred);
        l->setColour (Label::backgroundColourId, theme::lane);
        l->setColour (Label::outlineColourId, theme::border);
        addAndMakeVisible (l);
    }
    position.setTooltip ("Bar . beat . sixteenth");

    for (auto* b : { &arrangeButton, &mixButton })
    {
        b->setClickingTogglesState (false);
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    arrangeButton.setTooltip ("Arrangement  (Tab)");
    mixButton.setTooltip ("Mixer  (Tab)");
    arrangeButton.onClick = [this] { setMixerShown (false); if (onViewChange) onViewChange (false); };
    mixButton.onClick = [this] { setMixerShown (true); if (onViewChange) onViewChange (true); };
    setMixerShown (false);

    cpu.setFont (theme::font (12.0f));
    cpu.setColour (Label::textColourId, theme::textDim);
    cpu.setJustificationType (Justification::centredRight);
    addAndMakeVisible (cpu);

    deviceButton.setTooltip ("Audio / MIDI settings");
    deviceButton.onClick = [this] { if (onOpenSettings) onOpenSettings(); };
    deviceButton.setColour (TextButton::buttonColourId, theme::panelRaised);
    addAndMakeVisible (deviceButton);

    refresh();
}

void TransportBar::record()
{
    // Nothing armed? Arm the selected track, so record "just works".
    bool anyArmed = false;
    for (auto& t : ctx.edit.tracks)
        anyArmed = anyArmed || t->arm.load();

    if (! anyArmed && ! ctx.engine.isRecording())
    {
        if (auto t = ctx.edit.findTrack (ctx.selection.trackId); t != nullptr && t->holdsClips())
        {
            t->arm = true;
            ctx.edit.touched();
        }
    }

    ctx.engine.toggleRecord();
}

void TransportBar::setMixerShown (bool mixer)
{
    arrangeButton.setToggleState (! mixer, dontSendNotification);
    mixButton.setToggleState (mixer, dontSendNotification);
}

void TransportBar::refresh()
{
    auto& e = ctx.edit;
    auto& eng = ctx.engine;

    playButton.setToggleState (eng.isPlaying(), dontSendNotification);
    recordButton.setToggleState (eng.isRecording(), dontSendNotification);

    const bool rec = eng.isRecording();
    if (rec)
        recordButton.repaint();   // the pulse
    if (rec != showingRecording)
    {
        showingRecording = rec;
        for (auto* l : { &position, &time })
        {
            l->setColour (Label::textColourId, rec ? theme::record.brighter (0.25f) : theme::text);
            l->setColour (Label::outlineColourId, rec ? theme::record : theme::border);
        }
        repaint();
    }
    clickButton.setToggleState (eng.metronomeOn.load(), dontSendNotification);
    loopButton.setToggleState (e.loopEnabled, dontSendNotification);

    tempo.setValue (e.tempo, false);
    sigNum.setValue (e.timeSigNum, false);
    sigDen.setValue (e.timeSigDen, false);

    const double beats = eng.isPlaying() ? eng.getPlayheadBeats() : e.insertMarker;
    const double beatLen = 4.0 / e.timeSigDen;
    const int bar = (int) std::floor (beats / e.beatsPerBar());
    const double inBar = beats - bar * e.beatsPerBar();
    const int beat = (int) std::floor (inBar / beatLen);
    const int sixteenth = (int) std::floor ((inBar - beat * beatLen) / 0.25);
    position.setText (String (bar + 1) + "." + String (beat + 1) + "." + String (sixteenth + 1), dontSendNotification);

    const double seconds = beats * 60.0 / e.tempo;
    const int mins = (int) (seconds / 60.0);
    time.setText (String (mins) + ":" + String (seconds - mins * 60.0, 3).paddedLeft ('0', 6), dontSendNotification);

    if (++refreshCounter % 15 == 0)
    {
        cpu.setText ("CPU " + String (roundToInt (eng.getCpuUsage() * 100.0)) + "%", dontSendNotification);
        auto* d = eng.deviceManager.getCurrentAudioDevice();
        deviceButton.setButtonText (d != nullptr ? d->getName() + "  " + String (d->getCurrentSampleRate() / 1000.0, 1) + "k / "
                                                     + String (d->getCurrentBufferSizeSamples())
                                                 : String ("No audio device"));
        deviceButton.setColour (TextButton::textColourOffId, d != nullptr ? theme::textDim : theme::record);
    }
}

void TransportBar::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    if (showingRecording)
    {
        // A red edge under the whole transport while recording.
        g.setColour (theme::record);
        g.fillRect (0, getHeight() - 3, getWidth(), 3);
    }
    else
    {
        g.setColour (theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    g.setColour (theme::textDim);
    g.setFont (theme::font (14.0f));
    g.drawText ("/", sigNum.getBounds().withX (sigNum.getRight()).withWidth (10), Justification::centred);
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced (8, 7);

    tempo.setBounds (r.removeFromLeft (92));
    r.removeFromLeft (8);
    sigNum.setBounds (r.removeFromLeft (30));
    r.removeFromLeft (10);
    sigDen.setBounds (r.removeFromLeft (30));
    r.removeFromLeft (12);
    clickButton.setBounds (r.removeFromLeft (48));

    auto right = r.removeFromRight (420);
    deviceButton.setBounds (right.removeFromRight (230));
    right.removeFromRight (8);
    cpu.setBounds (right.removeFromRight (62));
    right.removeFromRight (12);
    mixButton.setBounds (right.removeFromRight (52));
    arrangeButton.setBounds (right.removeFromRight (70));

    // transport cluster in the middle
    const int clusterWidth = 34 * 3 + 8 + 48 + 12 + 92 + 6 + 110;
    auto mid = r.withSizeKeepingCentre (jmin (r.getWidth(), clusterWidth), r.getHeight());
    playButton.setBounds (mid.removeFromLeft (34));
    mid.removeFromLeft (2);
    stopButton.setBounds (mid.removeFromLeft (34));
    mid.removeFromLeft (2);
    recordButton.setBounds (mid.removeFromLeft (34));
    mid.removeFromLeft (8);
    loopButton.setBounds (mid.removeFromLeft (48));
    mid.removeFromLeft (12);
    position.setBounds (mid.removeFromLeft (92));
    mid.removeFromLeft (6);
    time.setBounds (mid.removeFromLeft (110));
}
} // namespace lathe
