#include "EqEditor.h"

namespace lathe
{
namespace
{
constexpr float kMinHz = ChannelEq::minFreq, kMaxHz = ChannelEq::maxFreq;
constexpr float kRangeDb = ChannelEq::maxGain;
} // namespace

EqEditor::EqEditor (AppContext& c, std::shared_ptr<Track> t) : ctx (c), track (std::move (t))
{
    setRepaintsOnMouseActivity (false);
}

Colour EqEditor::bandColour (int band)
{
    static const Colour colours[] = { Colour (0xff9a9a9a), Colour (0xffd2aa5e), Colour (0xffa4b968),
                                      Colour (0xff5f9fc6), Colour (0xffc479a1), Colour (0xff9a9a9a) };
    return colours[jlimit (0, ChannelEq::numBands - 1, band)];
}

String EqEditor::formatFreq (float hz)
{
    if (hz >= 1000.0f)
        return String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";
    return String (roundToInt (hz)) + " Hz";
}

double EqEditor::sampleRate() const
{
    const double sr = ctx.engine.getSampleRate();
    return sr > 0.0 ? sr : 48000.0;
}

Rectangle<float> EqEditor::plotArea() const
{
    return compact ? getLocalBounds().toFloat().reduced (1.0f) : getLocalBounds().toFloat().reduced (1.0f, 6.0f);
}

float EqEditor::freqToX (float hz) const
{
    auto r = plotArea();
    const float norm = std::log (jlimit (kMinHz, kMaxHz, hz) / kMinHz) / std::log (kMaxHz / kMinHz);
    return r.getX() + norm * r.getWidth();
}

float EqEditor::xToFreq (float x) const
{
    auto r = plotArea();
    const float norm = jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth());
    return kMinHz * std::pow (kMaxHz / kMinHz, norm);
}

float EqEditor::gainToY (float db) const
{
    auto r = plotArea();
    return r.getCentreY() - jlimit (-kRangeDb - 6.0f, kRangeDb + 6.0f, db) / kRangeDb * r.getHeight() * 0.5f;
}

float EqEditor::yToGain (float y) const
{
    auto r = plotArea();
    return jlimit (-kRangeDb, kRangeDb, (r.getCentreY() - y) / (r.getHeight() * 0.5f) * kRangeDb);
}

Point<float> EqEditor::nodePosition (int band) const
{
    auto& b = track->eq.bands[band];
    return { freqToX (b.freq.load()), b.isCut() ? plotArea().getCentreY() : gainToY (b.gainDb.load()) };
}

int EqEditor::bandAt (Point<float> p) const
{
    int best = -1;
    float bestDist = 11.0f;
    for (int i = 0; i < ChannelEq::numBands; ++i)
    {
        const float d = nodePosition (i).getDistanceFrom (p);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

void EqEditor::changed()
{
    ctx.edit.markDirty();
    repaint();
    if (onBandChanged)
        onBandChanged();
}

void EqEditor::paint (Graphics& g)
{
    auto r = plotArea();
    const auto& eq = track->eq;
    const bool on = eq.enabled.load();

    g.setColour (theme::lane);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 2.0f);

    if (! compact)
    {
        // grid: decades and 0 / +-6 / +-12 dB
        g.setColour (theme::gridBeat);
        for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            g.drawVerticalLine (roundToInt (freqToX (hz)), r.getY(), r.getBottom());
        for (float db : { -12.0f, -6.0f, 6.0f, 12.0f })
            g.drawHorizontalLine (roundToInt (gainToY (db)), r.getX(), r.getRight());

        g.setColour (theme::textDim.withAlpha (0.6f));
        g.setFont (theme::font (9.5f));
        for (auto [hz, label] : { std::pair { 100.0f, "100" }, { 1000.0f, "1k" }, { 10000.0f, "10k" } })
            g.drawText (label, Rectangle<float> (freqToX (hz) + 2.0f, r.getBottom() - 12.0f, 30.0f, 11.0f), Justification::centredLeft);
    }

    g.setColour (theme::gridBar);
    g.drawHorizontalLine (roundToInt (gainToY (0.0f)), r.getX(), r.getRight());

    // response curve
    const double sr = sampleRate();
    Path curve;
    const int steps = jmax (24, (int) r.getWidth() / (compact ? 2 : 1));
    for (int i = 0; i <= steps; ++i)
    {
        const float x = r.getX() + r.getWidth() * (float) i / (float) steps;
        const float y = gainToY (eq.responseDb (xToFreq (x), sr));
        if (i == 0) curve.startNewSubPath (x, y);
        else        curve.lineTo (x, y);
    }

    const auto colour = on ? (eq.isFlat() ? theme::textDim : theme::accent) : theme::control;
    Path fill (curve);
    fill.lineTo (r.getRight(), gainToY (0.0f));
    fill.lineTo (r.getX(), gainToY (0.0f));
    fill.closeSubPath();
    g.setColour (colour.withAlpha (0.12f));
    g.fillPath (fill);
    g.setColour (colour);
    g.strokePath (curve, PathStrokeType (compact ? 1.2f : 1.6f));

    if (compact)
        return;

    // nodes
    for (int i = 0; i < ChannelEq::numBands; ++i)
    {
        auto& b = eq.bands[i];
        const auto p = nodePosition (i);
        const float radius = (i == selected || i == hover) ? 6.5f : 5.0f;
        const auto c = bandColour (i);
        auto circle = Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p);

        if (b.on.load() && on)
        {
            g.setColour (c);
            g.fillEllipse (circle);
            g.setColour (theme::textDark);
        }
        else
        {
            g.setColour (theme::lane);
            g.fillEllipse (circle);
            g.setColour (c.withAlpha (0.7f));
            g.drawEllipse (circle, 1.2f);
            g.setColour (c);
        }

        g.setFont (theme::font (9.0f, true));
        g.drawText (String (i + 1), circle, Justification::centred);

        if (i == selected)
        {
            g.setColour (theme::text.withAlpha (0.8f));
            g.drawEllipse (circle.expanded (2.0f), 1.0f);
        }
    }
}

void EqEditor::mouseMove (const MouseEvent& e)
{
    const int h = bandAt (e.position);
    if (h != hover)
    {
        hover = h;
        if (h >= 0)
        {
            auto& b = track->eq.bands[h];
            String tip = ChannelEq::bandName (h) + ": drag";
            if (b.hasQ()) tip << ", wheel for width";
            tip << (b.isCut() ? ", double-click to turn on/off" : ", double-click for 0 dB");
            setTooltip (tip);
        }
        else
        {
            setTooltip ("Channel EQ. Drag a numbered point.");
        }
        repaint();
    }
}

void EqEditor::mouseDown (const MouseEvent& e)
{
    dragging = bandAt (e.position);
    if (dragging >= 0)
    {
        selected = dragging;
        dragStartQ = track->eq.bands[dragging].q.load();
        changed();
    }
}

void EqEditor::mouseDrag (const MouseEvent& e)
{
    if (dragging < 0)
        return;

    auto& b = track->eq.bands[dragging];
    if (e.mods.isAltDown() && b.hasQ())
    {
        const float factor = std::pow (2.0f, (float) -e.getDistanceFromDragStartY() / 40.0f);
        b.q = jlimit (0.1f, 18.0f, dragStartQ * factor);
    }
    else
    {
        b.freq = jlimit (kMinHz, kMaxHz, xToFreq (e.position.x));
        if (b.hasGain())
            b.gainDb = std::abs (yToGain (e.position.y)) < 0.25f ? 0.0f : yToGain (e.position.y);   // snaps to 0 dB
        if (! b.on.load())
            b.on = true;
    }
    changed();
}

void EqEditor::mouseUp (const MouseEvent&)
{
    if (dragging >= 0)
        ctx.edit.touched();   // let the mixer thumbnails catch up
    dragging = -1;
}

void EqEditor::mouseDoubleClick (const MouseEvent& e)
{
    const int i = bandAt (e.position);
    if (i < 0)
        return;

    auto& b = track->eq.bands[i];
    if (b.isCut())
        b.on = ! b.on.load();
    else
        b.gainDb = 0.0f;
    selected = i;
    changed();
    ctx.edit.touched();
}

void EqEditor::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    const int i = bandAt (e.position) >= 0 ? bandAt (e.position) : selected;
    auto& b = track->eq.bands[i];
    if (! b.hasQ())
        return;

    b.q = jlimit (0.1f, 18.0f, b.q.load() * std::pow (2.0f, w.deltaY * (w.isReversed ? -1.0f : 1.0f) * 2.0f));
    selected = i;
    changed();
}

//==============================================================================
EqDevice::EqDevice (AppContext& c, std::shared_ptr<Track> t)
    : ctx (c), track (std::move (t)), editor (c, track)
{
    styleToggle (power, "", theme::accent, "EQ on / off");
    power.onClick = [this]
    {
        track->eq.enabled = ! track->eq.enabled.load();
        ctx.edit.touched();
    };
    addAndMakeVisible (power);

    styleToggle (bandOn, "On", theme::accent, "Turn the selected band on or off");
    bandOn.onClick = [this]
    {
        auto& b = track->eq.bands[editor.getSelectedBand()];
        b.on = ! b.on.load();
        ctx.edit.touched();
    };
    addAndMakeVisible (bandOn);

    resetButton.setTooltip ("Flatten the EQ");
    resetButton.setWantsKeyboardFocus (false);
    resetButton.onClick = [this] { track->eq.reset(); ctx.edit.touched(); };
    addAndMakeVisible (resetButton);

    readout.setFont (theme::font (11.5f));
    readout.setColour (Label::textColourId, theme::text);
    readout.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (readout);

    editor.onBandChanged = [this] { updateReadout(); };
    addAndMakeVisible (editor);
    update();
}

void EqDevice::update()
{
    power.setToggleState (track->eq.enabled.load(), dontSendNotification);
    updateReadout();
    editor.repaint();
    repaint();
}

void EqDevice::updateReadout()
{
    const int i = editor.getSelectedBand();
    auto& b = track->eq.bands[i];
    String s = String (i + 1) + "  " + ChannelEq::bandName (i) + "   " + EqEditor::formatFreq (b.freq.load());
    if (b.hasGain())
    {
        const float db = b.gainDb.load();
        s << "   " << (db > 0.0f ? "+" : "") << String (db, 1) << " dB";
    }
    if (b.hasQ())
        s << "   Q " << String (b.q.load(), 2);
    readout.setText (s, dontSendNotification);
    readout.setColour (Label::textColourId, b.on.load() ? theme::text : theme::textDim);
    bandOn.setToggleState (b.on.load(), dontSendNotification);
}

void EqDevice::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panelRaised);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, 3.0f, 1.0f);

    g.setColour (track->eq.enabled.load() ? theme::text : theme::textDim);
    g.setFont (theme::font (13.0f, true));
    g.drawText ("EQ", getLocalBounds().removeFromTop (28).withTrimmedLeft (30), Justification::centredLeft);
}

void EqDevice::paintOverChildren (Graphics& g)
{
    auto p = power.getBounds().toFloat().withSizeKeepingCentre (9.0f, 9.0f);
    g.setColour (power.getToggleState() ? theme::textDark : theme::textDim);
    g.drawEllipse (p, 1.5f);
}

void EqDevice::resized()
{
    auto r = getLocalBounds().reduced (5);
    auto top = r.removeFromTop (18);
    power.setBounds (top.removeFromLeft (20));
    top.removeFromLeft (30);
    resetButton.setBounds (top.removeFromRight (46));
    top.removeFromRight (4);
    bandOn.setBounds (top.removeFromRight (32));
    top.removeFromRight (6);
    readout.setBounds (top);
    r.removeFromTop (5);
    editor.setBounds (r);
}
} // namespace lathe
