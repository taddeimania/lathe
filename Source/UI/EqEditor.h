#pragma once

#include "Widgets.h"

namespace lathe
{
/** The channel EQ's curve with a draggable node per band.
    Drag: frequency and gain. Wheel or Alt-drag on a bell: width (Q).
    Double-click a node: a cut turns on/off, the others go back to 0 dB. */
class EqEditor : public Component, public SettableTooltipClient
{
public:
    EqEditor (AppContext&, std::shared_ptr<Track>);

    /** Compact mode draws only the curve (mixer strip thumbnail) and ignores the mouse. */
    void setCompact (bool c)   { compact = c; setInterceptsMouseClicks (! c, false); repaint(); }
    int getSelectedBand() const noexcept   { return selected; }
    std::function<void()> onBandChanged;

    void paint (Graphics&) override;
    void mouseMove (const MouseEvent&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;
    void mouseExit (const MouseEvent&) override   { hover = -1; repaint(); }

    static Colour bandColour (int band);
    static String formatFreq (float hz);

private:
    Rectangle<float> plotArea() const;
    float freqToX (float hz) const;
    float xToFreq (float x) const;
    float gainToY (float db) const;
    float yToGain (float y) const;
    Point<float> nodePosition (int band) const;
    int bandAt (Point<float>) const;
    double sampleRate() const;
    void changed();

    AppContext& ctx;
    std::shared_ptr<Track> track;
    bool compact = false;
    int selected = 2, hover = -1, dragging = -1;
    float dragStartQ = 1.0f;
};

/** The EQ as a device in the device panel: power, band readout and the curve. */
class EqDevice : public Component
{
public:
    EqDevice (AppContext&, std::shared_ptr<Track>);
    void update();
    void paint (Graphics&) override;
    void paintOverChildren (Graphics&) override;
    void resized() override;

private:
    void updateReadout();

    AppContext& ctx;
    std::shared_ptr<Track> track;
    TextButton power, bandOn, resetButton { "Reset" };
    EqEditor editor;
    Label readout;
};
} // namespace lathe
