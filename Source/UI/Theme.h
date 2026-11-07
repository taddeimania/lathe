#pragma once

#include "../Common.h"

namespace lathe
{
/** One place for every colour and size, so the UI stays consistent. */
namespace theme
{
    inline const Colour window      { 0xff2b2b2b };
    inline const Colour panel       { 0xff333333 };
    inline const Colour panelRaised { 0xff3b3b3b };
    inline const Colour header      { 0xff383838 };
    inline const Colour headerSel   { 0xff464646 };
    inline const Colour lane        { 0xff262626 };
    inline const Colour laneAlt     { 0xff292929 };
    inline const Colour gridBar     { 0xff434343 };
    inline const Colour gridBeat    { 0xff323232 };
    inline const Colour gridFine    { 0xff2c2c2c };
    inline const Colour border      { 0xff1d1d1d };
    inline const Colour control     { 0xff4a4a4a };
    inline const Colour controlHi   { 0xff555555 };
    inline const Colour text        { 0xffdadada };
    inline const Colour textDim     { 0xff8e8e8e };
    inline const Colour textDark    { 0xff1e1e1e };

    inline const Colour accent      { 0xffe9a23b };   // play, playhead, selection
    inline const Colour record      { 0xffdb4b40 };
    inline const Colour solo        { 0xff5ea9d8 };
    inline const Colour mute        { 0xffcfb44b };
    inline const Colour meterLow    { 0xff74be5c };
    inline const Colour meterMid    { 0xffd8c34a };
    inline const Colour meterHigh   { 0xffdb4b40 };

    constexpr int transportHeight = 40;
    constexpr int statusHeight = 22;
    constexpr int rulerHeight = 26;
    constexpr int headerWidth = 230;
    constexpr int trackHeight = 62;
    constexpr int stripWidth = 104;

    inline Font font (float size = 13.0f, bool bold = false)
    {
        return Font (FontOptions (size, bold ? Font::bold : Font::plain));
    }

    /** Fader position (0..1) for a dB value: 0 dB sits at 75%, like most consoles. */
    float dbToFaderPos (float db);
    float faderPosToDb (float pos);
    String formatDb (float db);
}

class LatheLookAndFeel : public LookAndFeel_V4
{
public:
    LatheLookAndFeel();

    void drawButtonBackground (Graphics&, Button&, const Colour&, bool highlighted, bool down) override;
    void drawButtonText (Graphics&, TextButton&, bool highlighted, bool down) override;
    Font getTextButtonFont (TextButton&, int buttonHeight) override;

    void drawComboBox (Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, ComboBox&) override;
    Font getComboBoxFont (ComboBox&) override;
    void positionComboBoxText (ComboBox&, Label&) override;

    void drawLinearSlider (Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, Slider::SliderStyle, Slider&) override;
    void drawRotarySlider (Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, Slider&) override;

    void drawPopupMenuBackground (Graphics&, int width, int height) override;
    Font getPopupMenuFont() override;

    void drawScrollbar (Graphics&, ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                        int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    int getDefaultScrollbarWidth() override   { return 10; }

    Font getLabelFont (Label&) override;
    Font getAlertWindowMessageFont() override  { return theme::font (14.0f); }
    Font getAlertWindowTitleFont() override    { return theme::font (15.0f, true); }

    void drawTooltip (Graphics&, const String& text, int width, int height) override;
};
} // namespace lathe
