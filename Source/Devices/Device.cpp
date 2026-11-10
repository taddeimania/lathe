#include "Device.h"
#include "../UI/Theme.h"

namespace lathe::devices
{
//==============================================================================
Device::Device (const String& id, const String& name, Kind k, AudioProcessorValueTreeState::ParameterLayout layout)
    : AudioPluginInstance (BusesProperties()
                               .withInput ("Input", AudioChannelSet::stereo(), k == Kind::effect)
                               .withOutput ("Output", AudioChannelSet::stereo(), true)),
      state (*this, nullptr, Identifier ("Lathe" + id.removeCharacters ("-")), std::move (layout)),
      identifier (id), displayName (name), kind (k)
{
}

std::atomic<float>* Device::raw (const char* id) const
{
    auto it = rawCache.find (id);
    if (it != rawCache.end())
        return it->second;
    auto* p = state.getRawParameterValue (id);
    jassert (p != nullptr);   // a device asked for a parameter it doesn't have
    rawCache[id] = p;
    return p;
}

float Device::get (const char* id) const
{
    auto* p = raw (id);
    return p != nullptr ? p->load (std::memory_order_relaxed) : 0.0f;
}

void Device::set (const char* id, float value)
{
    if (auto* p = state.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void Device::fillInPluginDescription (PluginDescription& d) const
{
    for (auto& info : all())
        if (info.id == identifier)
            d = describe (info);
}

bool Device::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto out = layout.getMainOutputChannelSet();
    if (out != AudioChannelSet::stereo() && out != AudioChannelSet::mono())
        return false;
    if (kind == Kind::effect)
    {
        const auto in = layout.getMainInputChannelSet();
        return in == AudioChannelSet::stereo() || in == AudioChannelSet::mono();
    }
    return true;
}

int Device::getNumPrograms()
{
    return jmax (1, (int) presets().size());
}

const String Device::getProgramName (int index)
{
    const auto list = presets();
    return isPositiveAndBelow (index, (int) list.size()) ? String (list[(size_t) index].name) : String ("Default");
}

void Device::setCurrentProgram (int index)
{
    const auto list = presets();
    if (! isPositiveAndBelow (index, (int) list.size()))
        return;
    currentPreset = index;
    for (auto& [id, value] : list[(size_t) index].values)
        set (id, value);
}

void Device::getStateInformation (MemoryBlock& dest)
{
    auto tree = state.copyState();
    tree.setProperty ("preset", currentPreset, nullptr);
    tree.setProperty ("device", identifier, nullptr);
    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, dest);
}

void Device::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        auto tree = ValueTree::fromXml (*xml);
        if (tree.hasType (state.state.getType()))
        {
            currentPreset = (int) tree.getProperty ("preset", 0);
            state.replaceState (tree);
        }
    }
}

//==============================================================================
// The panel: every device looks like this, so they read as one family.
namespace
{
constexpr int kKnobW = 84, kKnobH = 100, kHeaderH = 56, kModeH = 36, kTitleH = 26, kPad = 12;

class DeviceEditor : public AudioProcessorEditor, private Timer
{
public:
    explicit DeviceEditor (Device& d) : AudioProcessorEditor (d), device (d)
    {
        // Header: name, presets, action.
        presetBox.setTextWhenNothingSelected ("Presets");
        const auto list = device.presets();
        for (int i = 0; i < (int) list.size(); ++i)
            presetBox.addItem (list[(size_t) i].name, i + 1);
        presetBox.onChange = [this]
        {
            if (presetBox.getSelectedId() > 0)
                device.setCurrentProgram (presetBox.getSelectedId() - 1);
        };
        presetBox.setVisible (! list.empty());
        addChildComponent (presetBox);
        for (auto* b : { &prevPreset, &nextPreset })
        {
            b->setVisible (! list.empty());
            b->setWantsKeyboardFocus (false);
            addChildComponent (b);
        }
        prevPreset.onClick = [this] { stepPreset (-1); };
        nextPreset.onClick = [this] { stepPreset (1); };
        prevPreset.setTooltip ("Previous preset");
        nextPreset.setTooltip ("Next preset");

        if (device.actionName().isNotEmpty())
        {
            actionButton.setButtonText (device.actionName());
            actionButton.setClickingTogglesState (false);
            actionButton.onClick = [this] { device.action(); };
            addAndMakeVisible (actionButton);
        }

        // Mode row.
        if (device.modeParam().isNotEmpty())
            if (auto* p = dynamic_cast<AudioParameterChoice*> (device.state.getParameter (device.modeParam())))
                for (int i = 0; i < p->choices.size(); ++i)
                {
                    auto* b = modeButtons.add (new TextButton (p->choices[i]));
                    b->setClickingTogglesState (false);
                    b->setWantsKeyboardFocus (false);
                    b->onClick = [this, i, p]
                    {
                        p->setValueNotifyingHost (p->convertTo0to1 ((float) i));
                        device.modeChosen (i);
                    };
                    addAndMakeVisible (b);
                }

        // Sections.
        for (auto& s : device.sections())
        {
            auto& sec = *sectionViews.add (new SectionView { s.title, {}, {} });
            for (auto& c : s.controls)
            {
                auto* param = device.state.getParameter (c.param);
                jassert (param != nullptr);
                if (param == nullptr)
                    continue;
                auto& v = *sec.controls.add (new ControlView());
                v.param = c.param;
                v.label.setText (c.label, dontSendNotification);
                v.label.setJustificationType (Justification::centred);
                v.label.setFont (theme::font (12.5f));
                v.label.setColour (Label::textColourId, theme::textDim);
                v.label.setTooltip (c.tip);
                addAndMakeVisible (v.label);

                if (dynamic_cast<AudioParameterBool*> (param) != nullptr)
                {
                    v.button = std::make_unique<TextButton> (c.label);
                    v.button->setClickingTogglesState (true);
                    v.button->setTooltip (c.tip);
                    v.button->setColour (TextButton::buttonOnColourId, theme::accent);
                    v.button->setColour (TextButton::textColourOnId, theme::textDark);
                    addAndMakeVisible (*v.button);
                    v.buttonAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (device.state, c.param, *v.button);
                    v.label.setVisible (false);
                }
                else if (auto* ch = dynamic_cast<AudioParameterChoice*> (param))
                {
                    v.box = std::make_unique<ComboBox>();
                    v.box->addItemList (ch->choices, 1);
                    v.box->setTooltip (c.tip);
                    addAndMakeVisible (*v.box);
                    v.boxAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (device.state, c.param, *v.box);
                }
                else
                {
                    v.knob = std::make_unique<Slider> (Slider::RotaryHorizontalVerticalDrag, Slider::TextBoxBelow);
                    v.knob->setColour (Slider::trackColourId, theme::accent);
                    v.knob->setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
                    v.knob->setColour (Slider::textBoxTextColourId, theme::text);
                    v.knob->setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
                    v.knob->setTextBoxStyle (Slider::TextBoxBelow, false, kKnobW - 6, 18);
                    v.knob->setTooltip (c.tip);
                    v.knob->setPopupMenuEnabled (false);
                    addAndMakeVisible (*v.knob);
                    v.knobAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (device.state, c.param, *v.knob);
                    v.knob->setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
                }
            }
        }

        // Size: the sections side by side, the meter after them; wide enough for the header.
        int width = kPad;
        for (auto* s : sectionViews)
            width += s->width() + kPad;
        if (device.meter().has_value())
            width += 56 + kPad;
        const int headerWidth = kPad * 3 + GlyphArrangement::getStringWidthInt (theme::font (16.0f, true), device.displayName)
                              + (list.empty() ? 0 : 252) + (actionButton.isVisible() ? 98 : 0);
        width = jmax (width, 460, headerWidth, modeButtons.size() * 96 + 2 * kPad);
        status = device.statusLine();
        hasStatus = status.isNotEmpty() || device.actionName().isNotEmpty();
        const int height = kHeaderH + 8 + (modeButtons.isEmpty() ? 0 : kModeH + 6) + kTitleH + kKnobH * maxRows() + kPad
                         + (hasStatus ? 30 : 0);
        setSize (width, height);

        syncFromDevice();
        startTimerHz (20);
    }

    ~DeviceEditor() override   { stopTimer(); }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);

        // Header.
        auto header = getLocalBounds().removeFromTop (kHeaderH);
        g.setColour (theme::header);
        g.fillRect (header);
        g.setColour (theme::border);
        g.drawHorizontalLine (kHeaderH - 1, 0.0f, (float) getWidth());
        auto text = header.reduced (kPad, 8).withRight (prevPreset.isVisible() ? prevPreset.getX() - 10 : getWidth() - kPad);
        g.setColour (theme::text);
        g.setFont (theme::font (16.0f, true));
        g.drawText (device.displayName, text, Justification::centredLeft, true);

        // Sections.
        for (auto* s : sectionViews)
        {
            auto r = s->bounds.toFloat();
            g.setColour (theme::panelRaised);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f, true));
            g.drawText (s->title, s->bounds.withHeight (kTitleH).reduced (10, 0), Justification::centredLeft, true);
        }

        // Meter.
        if (auto m = device.meter())
        {
            auto r = meterBounds;
            g.setColour (theme::panelRaised);
            g.fillRoundedRectangle (r.toFloat(), 5.0f);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            g.drawFittedText (m->label, r.removeFromTop (kTitleH).reduced (2, 0), Justification::centred, 2);
            auto bar = r.reduced (16, 6).withTrimmedBottom (18);
            g.setColour (theme::lane);
            g.fillRect (bar);
            const float v = jlimit (m->minDb, m->maxDb, shownMeter);
            const float frac = (m->maxDb - v) / (m->maxDb - m->minDb);   // reduction grows downwards
            g.setColour (theme::accent);
            g.fillRect (bar.withHeight (roundToInt ((float) bar.getHeight() * frac)));
            g.setColour (theme::text);
            g.setFont (theme::font (12.0f));
            g.drawText (String (shownMeter, 1), r.removeFromBottom (18), Justification::centred, false);
        }

        // Status.
        if (status.isNotEmpty())
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawText (status, getLocalBounds().removeFromBottom (28).reduced (kPad, 0), Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop (kHeaderH).reduced (kPad, 13);
        if (actionButton.isVisible())
        {
            actionButton.setBounds (header.removeFromRight (90));
            header.removeFromRight (8);
        }
        if (presetBox.isVisible())
        {
            nextPreset.setBounds (header.removeFromRight (28));
            header.removeFromRight (2);
            presetBox.setBounds (header.removeFromRight (190));
            header.removeFromRight (2);
            prevPreset.setBounds (header.removeFromRight (28));
        }

        r.removeFromTop (8);
        if (! modeButtons.isEmpty())
        {
            auto row = r.removeFromTop (kModeH).reduced (kPad, 2);
            const int w = row.getWidth() / modeButtons.size();
            for (auto* b : modeButtons)
                b->setBounds (row.removeFromLeft (w).reduced (2, 0));
            r.removeFromTop (6);
        }

        if (hasStatus)
            r.removeFromBottom (30);
        r.removeFromBottom (kPad);
        r.removeFromLeft (kPad);
        for (auto* s : sectionViews)
        {
            s->bounds = r.removeFromLeft (s->width());
            r.removeFromLeft (kPad);
            auto inner = s->bounds.withTrimmedTop (kTitleH);
            const int cols = s->cols();
            for (int i = 0; i < s->controls.size(); ++i)
            {
                auto& v = *s->controls[i];
                auto cell = Rectangle<int> (inner.getX() + (i % cols) * kKnobW, inner.getY() + (i / cols) * kKnobH, kKnobW, kKnobH);
                // Every control's label sits on the same line as the knobs' labels.
                auto top = cell.removeFromTop (76);
                auto labelRow = cell.removeFromTop (18);
                if (v.knob != nullptr)
                    v.knob->setBounds (top.reduced (6, 0));
                else if (v.box != nullptr)
                    v.box->setBounds (top.withSizeKeepingCentre (kKnobW - 8, 26).translated (0, 6));
                else if (v.button != nullptr)
                    v.button->setBounds (top.withSizeKeepingCentre (kKnobW - 12, 28).translated (0, 6));
                v.label.setBounds (labelRow);
            }
        }
        if (device.meter().has_value())
            meterBounds = r.removeFromLeft (56);
    }

private:
    struct ControlView
    {
        String param;
        Label label;
        std::unique_ptr<Slider> knob;
        std::unique_ptr<ComboBox> box;
        std::unique_ptr<TextButton> button;
        std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> knobAttachment;
        std::unique_ptr<AudioProcessorValueTreeState::ComboBoxAttachment> boxAttachment;
        std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
    };
    struct SectionView
    {
        String title;
        OwnedArray<ControlView> controls;
        Rectangle<int> bounds;
        int cols() const   { return controls.size() > 5 ? (controls.size() + 1) / 2 : jmax (1, controls.size()); }
        int rows() const   { return (controls.size() + cols() - 1) / cols(); }
        int width() const  { return cols() * kKnobW; }
    };

    int maxRows() const
    {
        int rows = 1;
        for (auto* s : sectionViews)
            rows = jmax (rows, s->rows());
        return rows;
    }

    void stepPreset (int delta)
    {
        const int n = (int) device.presets().size();
        if (n == 0)
            return;
        const int next = ((device.getCurrentProgram() + delta) % n + n) % n;
        device.setCurrentProgram (next);
        syncFromDevice();
    }

    void syncFromDevice()
    {
        presetBox.setSelectedId (device.getCurrentProgram() + 1, dontSendNotification);
        if (device.modeParam().isNotEmpty())
        {
            const int mode = device.geti (device.modeParam().toRawUTF8());
            for (int i = 0; i < modeButtons.size(); ++i)
            {
                auto* b = modeButtons[i];
                const bool on = i == mode;
                b->setToggleState (on, dontSendNotification);
                b->setColour (TextButton::buttonOnColourId, theme::accent);
                b->setColour (TextButton::textColourOnId, theme::textDark);
            }
        }
        if (actionButton.isVisible())
            actionButton.setToggleState (device.actionActive(), dontSendNotification);
        for (auto* s : sectionViews)
            for (auto* v : s->controls)
            {
                const bool active = device.isControlActive (v->param);
                for (auto* c : { (Component*) v->knob.get(), (Component*) v->box.get(), (Component*) v->button.get() })
                    if (c != nullptr && c->isEnabled() != active)
                        c->setEnabled (active);
                v->label.setAlpha (active ? 1.0f : 0.5f);
            }
    }

    void timerCallback() override
    {
        syncFromDevice();
        const auto s = device.statusLine();
        const float m = device.meterValue();
        if (s != status || std::abs (m - shownMeter) > 0.05f)
        {
            status = s;
            shownMeter = m;
            repaint();
        }
    }

    Device& device;
    ComboBox presetBox;
    TextButton prevPreset { "<" }, nextPreset { ">" }, actionButton;
    OwnedArray<TextButton> modeButtons;
    OwnedArray<SectionView> sectionViews;
    Rectangle<int> meterBounds;
    String status;
    bool hasStatus = false;
    float shownMeter = 0.0f;
};
} // namespace

AudioProcessorEditor* Device::createEditor()
{
    return new DeviceEditor (*this);
}

//==============================================================================
PluginDescription describe (const Info& info)
{
    PluginDescription d;
    d.name = info.name;
    d.descriptiveName = info.blurb;
    d.manufacturerName = "Lathe";
    d.pluginFormatName = kFormatName;
    d.category = info.kind == Kind::instrument ? "Instrument" : "Effect";
    d.fileOrIdentifier = "lathe:" + info.id;
    d.uniqueId = d.deprecatedUid = (int) (DefaultHashFunctions::generateHash (d.fileOrIdentifier, std::numeric_limits<int>::max()));
    d.isInstrument = info.kind == Kind::instrument;
    d.numInputChannels = info.kind == Kind::effect ? 2 : 0;
    d.numOutputChannels = 2;
    d.hasSharedContainer = false;
    return d;
}

const Info* find (const PluginDescription& d)
{
    if (! isBuiltIn (d))
        return nullptr;
    const auto id = d.fileOrIdentifier.fromFirstOccurrenceOf ("lathe:", false, false);
    for (auto& info : all())
        if (info.id == id)
            return &info;
    return nullptr;
}

void startOnReturn (AudioPluginInstance& instance)
{
    if (auto* device = dynamic_cast<Device*> (&instance))
        if (device->state.getParameter ("mix") != nullptr)
            device->set ("mix", 1.0f);
}

std::unique_ptr<AudioPluginInstance> create (const PluginDescription& d)
{
    if (auto* info = find (d))
        return info->create();
    return {};
}

//==============================================================================
std::unique_ptr<AudioParameterFloat> percent (const char* id, const char* name, float def)
{
    return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f), def,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; })
                                       .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue() / 100.0f; }));
}

std::unique_ptr<AudioParameterFloat> decibels (const char* id, const char* name, float lo, float hi, float def)
{
    return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (lo, hi), def,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
        {
            const float r = std::round (v * 10.0f) / 10.0f;
            return (r > 0.0f ? "+" : "") + String (r, 1) + " dB";
        }).withValueFromStringFunction ([] (const String& s) { return s.getFloatValue(); }));
}

std::unique_ptr<AudioParameterFloat> number (const char* id, const char* name, NormalisableRange<float> range, float def,
                                             std::function<String (float)> text)
{
    return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
        AudioParameterFloatAttributes().withStringFromValueFunction ([text] (float v, int) { return text (v); })
                                       .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue(); }));
}

std::unique_ptr<AudioParameterChoice> choice (const char* id, const char* name, const StringArray& names, int def)
{
    return std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, names, def);
}

std::unique_ptr<AudioParameterBool> toggle (const char* id, const char* name, bool def)
{
    return std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def);
}

String msText (float ms)        { return ms < 100.0f ? String (ms, 1) + " ms" : String (roundToInt (ms)) + " ms"; }
String secondsText (float s)    { return s < 10.0f ? String (s, 2) + " s" : String (s, 1) + " s"; }
String hzText (float hz)        { return hz < 1000.0f ? String (roundToInt (hz)) + " Hz" : String (hz / 1000.0f, 1) + " kHz"; }
} // namespace lathe::devices
