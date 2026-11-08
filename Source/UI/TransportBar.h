#pragma once

#include "Widgets.h"

namespace lathe
{
/** Number you drag up/down to change, or double-click to type. */
class DragNumber : public Component
{
public:
    DragNumber (double minValue, double maxValue, int decimals, double dragStep);

    void setValue (double v, bool notify);
    double getValue() const noexcept   { return value; }
    std::function<void (double)> onChange;
    String suffix;

    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    void resized() override;

private:
    double value = 0, minimum, maximum, step, dragStart = 0;
    int decimals;
    std::unique_ptr<TextEditor> editor;
};

class TransportIcon : public Button
{
public:
    enum class Shape { play, stop, record };
    TransportIcon (Shape s, Colour on);
    void paintButton (Graphics&, bool highlighted, bool down) override;

private:
    Shape shape;
    Colour onColour;
};

class TransportBar : public Component
{
public:
    explicit TransportBar (AppContext&);

    void refresh();     // called ~30x a second
    /** Record button / R key: arms the selected track if nothing is armed, then toggles recording. */
    void record();
    void resized() override;
    void paint (Graphics&) override;

    std::function<void (bool mixer)> onViewChange;
    std::function<void()> onOpenSettings;
    void setMixerShown (bool);

private:
    AppContext& ctx;
    TransportIcon playButton { TransportIcon::Shape::play, theme::accent };
    TransportIcon stopButton { TransportIcon::Shape::stop, theme::text };
    TransportIcon recordButton { TransportIcon::Shape::record, theme::record };
    TextButton clickButton, loopButton;
    DragNumber tempo { 20.0, 400.0, 2, 0.5 };
    DragNumber sigNum { 1.0, 32.0, 0, 0.2 }, sigDen { 1.0, 32.0, 0, 0.2 };
    Label position, time;
    TextButton arrangeButton { "Arrange" }, mixButton { "Mix" };
    Label cpu;
    TextButton deviceButton;
    int refreshCounter = 0;
    bool showingRecording = false;
};
} // namespace lathe
