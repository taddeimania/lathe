#include "Theme.h"

namespace lathe
{
namespace theme
{
    // Piecewise mapping: the top quarter of the throw covers 0..+6 dB, the
    // next half covers -24..0, and the bottom quarter fades out to silence.
    float dbToFaderPos (float db)
    {
        if (db <= kSilenceDb) return 0.0f;
        if (db >= 0.0f)       return 0.75f + 0.25f * jmin (db, 6.0f) / 6.0f;
        if (db >= -24.0f)     return 0.25f + 0.5f * (db + 24.0f) / 24.0f;
        return 0.25f * (db - kSilenceDb) / (-24.0f - kSilenceDb);
    }

    float faderPosToDb (float p)
    {
        p = jlimit (0.0f, 1.0f, p);
        if (p <= 0.0f)  return kSilenceDb;
        if (p >= 0.75f) return (p - 0.75f) / 0.25f * 6.0f;
        if (p >= 0.25f) return (p - 0.25f) / 0.5f * 24.0f - 24.0f;
        return kSilenceDb + p / 0.25f * (-24.0f - kSilenceDb);
    }

    String formatDb (float db)
    {
        if (db <= kSilenceDb) return "-inf";
        return (db > 0.05f ? "+" : "") + String (db, 1);
    }
}

LatheLookAndFeel::LatheLookAndFeel()
{
    using namespace theme;

    setColourScheme ({ window, panel, panelRaised, control, text, control, text, accent, text });

    setColour (ResizableWindow::backgroundColourId, window);
    setColour (DocumentWindow::textColourId, text);
    setColour (TextButton::buttonColourId, control);
    setColour (TextButton::buttonOnColourId, accent);
    setColour (TextButton::textColourOffId, text);
    setColour (TextButton::textColourOnId, textDark);
    setColour (ComboBox::backgroundColourId, panelRaised);
    setColour (ComboBox::outlineColourId, border);
    setColour (ComboBox::textColourId, text);
    setColour (ComboBox::arrowColourId, textDim);
    setColour (PopupMenu::backgroundColourId, Colour (0xff303030));
    setColour (PopupMenu::textColourId, text);
    setColour (PopupMenu::highlightedBackgroundColourId, accent);
    setColour (PopupMenu::highlightedTextColourId, textDark);
    setColour (Label::textColourId, text);
    setColour (Label::textWhenEditingColourId, text);
    setColour (Label::backgroundWhenEditingColourId, lane);
    setColour (Label::outlineWhenEditingColourId, accent);
    setColour (TextEditor::backgroundColourId, lane);
    setColour (TextEditor::textColourId, text);
    setColour (TextEditor::outlineColourId, border);
    setColour (TextEditor::focusedOutlineColourId, accent);
    setColour (TextEditor::highlightColourId, accent.withAlpha (0.4f));
    setColour (CaretComponent::caretColourId, accent);
    setColour (Slider::thumbColourId, text);
    setColour (Slider::trackColourId, accent);
    setColour (Slider::backgroundColourId, lane);
    setColour (Slider::textBoxTextColourId, text);
    setColour (Slider::textBoxBackgroundColourId, lane);
    setColour (Slider::textBoxOutlineColourId, border);
    setColour (ScrollBar::thumbColourId, control);
    setColour (ListBox::backgroundColourId, lane);
    setColour (ListBox::outlineColourId, border);
    setColour (ToggleButton::textColourId, text);
    setColour (ToggleButton::tickColourId, accent);
    setColour (ToggleButton::tickDisabledColourId, textDim);
    setColour (AlertWindow::backgroundColourId, panel);
    setColour (AlertWindow::textColourId, text);
    setColour (AlertWindow::outlineColourId, border);
    setColour (ProgressBar::backgroundColourId, lane);
    setColour (ProgressBar::foregroundColourId, accent);
    setColour (TooltipWindow::backgroundColourId, Colour (0xff1f1f1f));
    setColour (TooltipWindow::textColourId, text);
    setColour (GroupComponent::outlineColourId, control);
    setColour (GroupComponent::textColourId, textDim);
    setColour (TreeView::backgroundColourId, lane);
}

void LatheLookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour& background, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? b.findColour (TextButton::buttonOnColourId) : background;

    if (! b.isEnabled())
        c = c.withMultipliedAlpha (0.5f);
    else if (down)
        c = c.darker (0.15f);
    else if (highlighted)
        c = c.brighter (0.08f);

    g.setColour (c);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, 2.0f, 1.0f);
}

Font LatheLookAndFeel::getTextButtonFont (TextButton&, int buttonHeight)
{
    return theme::font (jmin (13.0f, buttonHeight * 0.6f));
}

void LatheLookAndFeel::drawButtonText (Graphics& g, TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    auto c = b.findColour (b.getToggleState() ? TextButton::textColourOnId : TextButton::textColourOffId);
    g.setColour (b.isEnabled() ? c : c.withMultipliedAlpha (0.5f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (3, 0), Justification::centred, 1, 0.8f);
}

void LatheLookAndFeel::drawComboBox (Graphics& g, int width, int height, bool, int, int, int, int, ComboBox& box)
{
    auto r = Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    g.setColour (box.findColour (ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (box.findColour (ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    // small down-pointing triangle
    const float cx = (float) width - 8.0f, cy = (float) height * 0.5f;
    Path p;
    p.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 2.0f);
    g.setColour (box.findColour (ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.fillPath (p);
}

Font LatheLookAndFeel::getComboBoxFont (ComboBox& box)
{
    return theme::font (jmin (12.5f, box.getHeight() * 0.62f));
}

void LatheLookAndFeel::positionComboBoxText (ComboBox& box, Label& label)
{
    label.setBounds (1, 1, box.getWidth() - 14, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setBorderSize ({ 0, 5, 0, 0 });
}

void LatheLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float, float, Slider::SliderStyle style, Slider& s)
{
    if (style == Slider::LinearVertical)
    {
        // Fader: thin slot plus a flat rectangular cap.
        const float cx = x + width * 0.5f;
        g.setColour (theme::border);
        g.fillRoundedRectangle (cx - 2.0f, (float) y, 4.0f, (float) height, 2.0f);

        auto cap = Rectangle<float> (0, 0, jmin (26.0f, width - 2.0f), 12.0f).withCentre ({ cx, sliderPos });
        // A fader editing a cue mix carries the cue's colour on its cap.
        const auto capColour = s.isColourSpecified (Slider::thumbColourId) ? s.findColour (Slider::thumbColourId) : theme::controlHi;
        g.setColour (s.isEnabled() ? capColour : theme::control.withAlpha (0.6f));
        g.fillRoundedRectangle (cap, 2.0f);
        g.setColour (s.isEnabled() ? theme::text : theme::textDim);
        g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 6.0f, 1.5f));
        g.setColour (theme::border);
        g.drawRoundedRectangle (cap, 2.0f, 1.0f);
        return;
    }

    if (style == Slider::LinearHorizontal || style == Slider::LinearBar)
    {
        auto r = Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
        auto slot = r.withSizeKeepingCentre (r.getWidth(), 4.0f);
        g.setColour (theme::border);
        g.fillRoundedRectangle (slot, 2.0f);
        g.setColour (s.findColour (Slider::trackColourId).withAlpha (s.isEnabled() ? 0.8f : 0.3f));
        g.fillRoundedRectangle (slot.withRight (sliderPos), 2.0f);
        g.setColour (theme::text);
        g.fillRoundedRectangle (Rectangle<float> (4.0f, 10.0f).withCentre ({ sliderPos, r.getCentreY() }), 1.0f);
        return;
    }

    LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, s);
}

void LatheLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int width, int height, float pos,
                                         float startAngle, float endAngle, Slider& s)
{
    auto r = Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (2.0f);
    const float size = jmin (r.getWidth(), r.getHeight());
    auto c = r.withSizeKeepingCentre (size, size);
    const float radius = size * 0.5f - 1.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);

    Path track;
    track.addCentredArc (c.getCentreX(), c.getCentreY(), radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (theme::border);
    g.strokePath (track, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    // Bipolar knobs (pan) fill from the centre; others from the left.
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    Path value;
    value.addCentredArc (c.getCentreX(), c.getCentreY(), radius, radius, 0.0f, jmin (fromAngle, angle), jmax (fromAngle, angle), true);
    g.setColour (s.findColour (Slider::trackColourId).withAlpha (s.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (value, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    const auto tip = c.getCentre().getPointOnCircumference (radius - 4.0f, angle);
    g.setColour (theme::text);
    g.drawLine ({ c.getCentre().getPointOnCircumference (radius * 0.2f, angle), tip }, 1.5f);
}

void LatheLookAndFeel::drawPopupMenuBackground (Graphics& g, int width, int height)
{
    g.fillAll (findColour (PopupMenu::backgroundColourId));
    g.setColour (theme::border);
    g.drawRect (0, 0, width, height);
}

Font LatheLookAndFeel::getPopupMenuFont()
{
    return theme::font (13.5f);
}

void LatheLookAndFeel::drawScrollbar (Graphics& g, ScrollBar&, int x, int y, int width, int height, bool vertical,
                                      int thumbStart, int thumbSize, bool over, bool down)
{
    g.setColour (theme::lane);
    g.fillRect (x, y, width, height);

    Rectangle<int> thumb = vertical ? Rectangle<int> (x + 2, thumbStart, width - 4, thumbSize)
                                    : Rectangle<int> (thumbStart, y + 2, thumbSize, height - 4);
    g.setColour (over || down ? theme::controlHi : theme::control);
    g.fillRoundedRectangle (thumb.toFloat(), 3.0f);
}

Font LatheLookAndFeel::getLabelFont (Label& l)
{
    return l.getFont();
}

void LatheLookAndFeel::drawTooltip (Graphics& g, const String& text, int width, int height)
{
    g.fillAll (findColour (TooltipWindow::backgroundColourId));
    g.setColour (theme::border);
    g.drawRect (0, 0, width, height);
    g.setColour (theme::text);
    g.setFont (theme::font (12.5f));
    g.drawFittedText (text, Rectangle<int> (width, height).reduced (6, 2), Justification::centredLeft, 4);
}

void LatheLookAndFeel::drawAlertBox (Graphics& g, AlertWindow& w, const Rectangle<int>& textArea, TextLayout& text)
{
    // Flat panel, no icon: matches the rest of the UI.
    g.fillAll (w.findColour (AlertWindow::backgroundColourId));
    g.setColour (theme::border);
    g.drawRect (w.getLocalBounds());
    g.setColour (w.findColour (AlertWindow::textColourId));
    text.draw (g, textArea.toFloat());
}

} // namespace lathe
