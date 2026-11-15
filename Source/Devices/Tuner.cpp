// Lathe's Tuner: put it on a track and play. The audio thread only copies what goes through into
// a ring (and passes the sound on, or silences it with Mute); the panel reads the newest stretch a
// few dozen times a second and finds its pitch with YIN, on the message thread.
#include "Tuner.h"
#include "../UI/Theme.h"

namespace lathe::devices
{
namespace tuner
{
Reading detect (const float* x, int n, double sr, std::vector<float>& work, float minHz, float maxHz)
{
    Reading r;
    if (x == nullptr || n < 64 || sr <= 0.0)
        return r;
    double energy = 0.0;
    for (int i = 0; i < n; ++i)
        energy += (double) x[i] * x[i];
    r.level = (float) std::sqrt (energy / n);
    if (r.level < 1.0e-3f)   // about -60 dBFS: nothing to tune
        return r;

    const int window = n / 2;
    const int tauMin = jmax (2, (int) (sr / maxHz));
    const int tauMax = jmin (n - window - 2, (int) (sr / minHz));
    if (tauMax <= tauMin + 2)
        return r;
    if ((int) work.size() < tauMax + 2)
        work.resize ((size_t) tauMax + 2);

    // The cumulative mean normalised difference: how unlike itself the signal is `tau` samples on.
    // The first dip under the threshold is the period (its octaves dip too, later).
    constexpr float threshold = 0.12f;
    double running = 0.0;
    work[0] = 1.0f;
    int found = -1;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double d = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const float diff = x[j] - x[j + tau];
            d += (double) diff * diff;
        }
        running += d;
        work[(size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
        if (tau >= tauMin + 1 && found < 0 && work[(size_t) tau - 1] < threshold && work[(size_t) tau] >= work[(size_t) tau - 1])
        {
            found = tau - 1;   // the bottom of the first dip under the threshold
            break;
        }
    }
    if (found < 0)
    {
        // No clear dip: the deepest one, if it's deep enough to mean a pitch.
        int best = tauMin;
        const int last = jmin (tauMax, (int) work.size() - 1);
        for (int tau = tauMin; tau <= last; ++tau)
            if (work[(size_t) tau] < work[(size_t) best])
                best = tau;
        if (work[(size_t) best] > 0.35f)
            return r;
        found = best;
    }
    // A parabola through the dip and its neighbours, for a fraction of a sample.
    double shift = 0.0;
    if (found > 1 && found + 1 < (int) work.size())
    {
        const double a = work[(size_t) found - 1], b = work[(size_t) found], c = work[(size_t) found + 1];
        const double den = a - 2.0 * b + c;
        if (std::abs (den) > 1.0e-12)
            shift = jlimit (-1.0, 1.0, 0.5 * (a - c) / den);
    }
    r.hz = (float) (sr / (found + shift));
    r.clarity = jlimit (0.0f, 1.0f, 1.0f - work[(size_t) found]);
    return r;
}

Note nearest (float hz, float reference)
{
    Note n;
    if (! (hz > 0.0f) || ! (reference > 0.0f))
        return n;
    const double exact = 69.0 + 12.0 * std::log2 ((double) hz / reference);
    n.midi = (int) std::lround (exact);
    n.cents = (float) ((exact - n.midi) * 100.0);
    return n;
}
} // namespace tuner

namespace
{
class Tuner final : public Device
{
public:
    Tuner() : Device ("tuner", "Tuner", Kind::effect, layout())
    {
        setCurrentProgram (0);
    }

    static AudioProcessorValueTreeState::ParameterLayout layout()
    {
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (number ("reference", "A4", { 415.0f, 466.0f, 0.1f }, 440.0f, [] (float v) { return String (v, 1) + " Hz"; }));
        l.add (toggle ("mute", "Mute", false));
        return l;
    }

    String blurb() const override   { return "Shows the pitch of what you play: note, cents and a needle. Mute it to tune in silence."; }

    std::vector<Section> sections() const override
    {
        return { { "Tuning", { { "reference", "A4", "The pitch of A above middle C: 440 Hz unless the band tunes elsewhere." },
                               { "mute", "Mute", "Silence the track while you tune." } } } };
    }

    std::vector<Preset> presets() const override
    {
        return { { "A440", { { "reference", 440.0f } } },
                 { "A442", { { "reference", 442.0f } } },
                 { "A443 (orchestra)", { { "reference", 443.0f } } },
                 { "A432", { { "reference", 432.0f } } },
                 { "A415 (baroque)", { { "reference", 415.0f } } } };
    }

    void prepareToPlay (double rate, int block) override
    {
        ignoreUnused (block);
        sr = rate;
        ring.assign (16384, 0.0f);   // over 0.17 s even at 96 kHz
        written.store (0);
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer&) override
    {
        const int n = buffer.getNumSamples(), chans = buffer.getNumChannels();
        if (ring.empty() || chans == 0)
            return;
        const int size = (int) ring.size();
        int w = written.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            float x = 0.0f;
            for (int c = 0; c < chans; ++c)
                x += buffer.getSample (c, i);
            ring[(size_t) (w & (size - 1))] = x / (float) chans;
            ++w;
        }
        written.store (w, std::memory_order_release);
        if (get ("mute") > 0.5f)
            buffer.clear();
    }

    /** The pitch of the newest ~85 ms (message thread). Holds the last clear reading a moment, so the
        display doesn't flicker between notes. */
    struct Now
    {
        tuner::Reading reading;
        tuner::Note note;
        bool fresh = false;   // false: nothing clear just now (the last one may still be shown)
    };
    Now read()
    {
        Now now;
        if (ring.empty() || sr <= 0.0)
            return now;
        const int size = (int) ring.size();
        const int n = jlimit (1024, size / 2, (int) (sr * 0.085));
        snapshot.resize ((size_t) n);
        const int end = written.load (std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            snapshot[(size_t) i] = ring[(size_t) ((end - n + i) & (size - 1))];
        now.reading = tuner::detect (snapshot.data(), n, sr, work);
        if (now.reading.hz > 0.0f && now.reading.clarity > 0.8f)
        {
            now.note = tuner::nearest (now.reading.hz, get ("reference"));
            now.fresh = true;
            last = now;
            lastAt = Time::getMillisecondCounter();
        }
        else if (last.fresh && Time::getMillisecondCounter() - lastAt < 400)
        {
            now = last;
            now.fresh = false;
        }
        return now;
    }

    AudioProcessorEditor* createEditor() override;

    double sr = 0.0;

private:
    std::vector<float> ring, snapshot, work;
    std::atomic<int> written { 0 };
    Now last;
    uint32 lastAt = 0;
};

//==============================================================================
/** The tuner's panel: the note, a needle from -50 to +50 cents, and the reading in cents and Hz. */
class TunerEditor : public AudioProcessorEditor, private Timer
{
public:
    explicit TunerEditor (Tuner& t) : AudioProcessorEditor (t), tuner (t)
    {
        for (int i = 0; i < (int) tuner.presets().size(); ++i)
            presetBox.addItem (tuner.presets()[(size_t) i].name, i + 1);
        presetBox.setSelectedId (tuner.getCurrentProgram() + 1, dontSendNotification);
        presetBox.onChange = [this]
        {
            if (presetBox.getSelectedId() > 0)
                tuner.setCurrentProgram (presetBox.getSelectedId() - 1);
        };
        addAndMakeVisible (presetBox);

        reference.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        reference.setTextBoxStyle (Slider::TextBoxBelow, false, 80, 18);
        reference.setColour (Slider::trackColourId, theme::accent);
        reference.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        reference.setColour (Slider::textBoxTextColourId, theme::text);
        reference.setTooltip ("The pitch of A above middle C: 440 Hz unless the band tunes elsewhere.");
        reference.setDoubleClickReturnValue (true, 440.0);
        reference.setPopupMenuEnabled (false);
        addAndMakeVisible (reference);
        referenceAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (tuner.state, "reference", reference);
        referenceLabel.setText ("A4", dontSendNotification);
        referenceLabel.setJustificationType (Justification::centred);
        referenceLabel.setColour (Label::textColourId, theme::textDim);
        addAndMakeVisible (referenceLabel);

        mute.setClickingTogglesState (true);
        mute.setColour (TextButton::buttonOnColourId, theme::accent);
        mute.setColour (TextButton::textColourOnId, theme::textDark);
        mute.setTooltip ("Silence the track while you tune.");
        addAndMakeVisible (mute);
        muteAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (tuner.state, "mute", mute);

        setSize (560, 300);
        timerCallback();   // show what's playing straight away
        shownCents = now.note.cents;
        startTimerHz (30);
    }

    ~TunerEditor() override   { stopTimer(); }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);
        auto header = getLocalBounds().removeFromTop (56);
        g.setColour (theme::header);
        g.fillRect (header);
        g.setColour (theme::border);
        g.drawHorizontalLine (55, 0.0f, (float) getWidth());
        g.setColour (theme::text);
        g.setFont (theme::font (16.0f, true));
        g.drawText ("Tuner", header.reduced (12, 8), Justification::centredLeft);

        auto area = display.toFloat();
        g.setColour (theme::panelRaised);
        g.fillRoundedRectangle (area, 6.0f);

        const bool showing = now.note.midi >= 0;
        const float cents = shownCents;
        const float off = std::abs (cents);
        const auto colour = ! showing ? theme::textDim : off < 3.0f ? theme::meterLow : off < 12.0f ? theme::accent : theme::record;

        // The note: its letter big, the octave beside it (C3 is middle C, as in the piano roll).
        auto top = area.reduced (16.0f, 10.0f).removeFromTop (92.0f);
        g.setColour (showing ? (now.fresh ? theme::text : theme::textDim) : theme::textDim);
        if (showing)
        {
            const auto letter = MidiMessage::getMidiNoteName (now.note.midi, true, false, 3);
            const auto octave = String (now.note.midi / 12 - 2);
            g.setFont (theme::font (76.0f, true));
            const float lw = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), letter);
            const float x = top.getCentreX() - (lw + 26.0f) * 0.5f;
            g.drawText (letter, Rectangle<float> (x, top.getY(), lw + 4.0f, top.getHeight()), Justification::centredLeft);
            g.setFont (theme::font (26.0f));
            g.drawText (octave, Rectangle<float> (x + lw + 4.0f, top.getY() + 38.0f, 40.0f, 40.0f), Justification::topLeft);
        }
        else
        {
            g.setFont (theme::font (22.0f));
            g.drawText ("Play a note", top, Justification::centred);
        }

        // The needle: a scale from -50 to +50 cents, the middle third marked.
        auto scale = area.reduced (28.0f, 0.0f).withTrimmedTop (112.0f).removeFromTop (44.0f);
        const float mid = scale.getCentreX(), half = scale.getWidth() * 0.5f;
        g.setColour (theme::lane);
        g.fillRoundedRectangle (scale.withSizeKeepingCentre (scale.getWidth(), 8.0f), 4.0f);
        g.setColour (theme::meterLow.withAlpha (0.35f));
        g.fillRect (Rectangle<float> (mid - half * 0.06f, scale.getY() + 4.0f, half * 0.12f, scale.getHeight() - 8.0f));
        g.setColour (theme::textDim);
        for (int c = -50; c <= 50; c += 10)
        {
            const float x = mid + half * (float) c / 50.0f;
            const float h = c == 0 ? 20.0f : 10.0f;
            g.fillRect (x - 0.5f, scale.getCentreY() - h * 0.5f, 1.0f, h);
        }
        if (showing)
        {
            const float x = mid + half * jlimit (-50.0f, 50.0f, cents) / 50.0f;
            g.setColour (colour);
            g.fillRoundedRectangle (x - 3.0f, scale.getY(), 6.0f, scale.getHeight(), 3.0f);
        }

        // The numbers.
        auto numbers = area.reduced (16.0f, 0.0f).withTrimmedTop (164.0f).removeFromTop (30.0f);
        g.setFont (theme::font (15.0f));
        g.setColour (showing ? colour : theme::textDim);
        g.drawText (showing ? (off < 0.5f ? String ("in tune") : (cents > 0 ? "+" : "") + String (cents, 1) + " cents") : String ("-- cents"),
                    numbers.removeFromLeft (numbers.getWidth() * 0.5f), Justification::centred);
        g.setColour (theme::textDim);
        g.drawText (showing ? String (now.reading.hz, 2) + " Hz" : String ("-- Hz"), numbers, Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop (56).reduced (12, 13);
        presetBox.setBounds (header.removeFromRight (190));
        r.reduce (12, 12);
        auto side = r.removeFromRight (100);
        r.removeFromRight (12);
        display = r;
        auto knob = side.removeFromTop (110);
        referenceLabel.setBounds (knob.removeFromBottom (18));
        reference.setBounds (knob);
        side.removeFromTop (16);
        mute.setBounds (side.removeFromTop (30).reduced (6, 0));
    }

private:
    void timerCallback() override
    {
        now = tuner.read();
        if (now.note.midi >= 0)
            shownCents += 0.35f * (now.note.cents - shownCents);   // a steady needle
        presetBox.setSelectedId (tuner.getCurrentProgram() + 1, dontSendNotification);
        repaint (display);
    }

    Tuner& tuner;
    Tuner::Now now;
    float shownCents = 0.0f;
    Rectangle<int> display;
    ComboBox presetBox;
    Slider reference;
    Label referenceLabel;
    TextButton mute { "Mute" };
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> referenceAttachment;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> muteAttachment;
};

AudioProcessorEditor* Tuner::createEditor()   { return new TunerEditor (*this); }
} // namespace

std::unique_ptr<Device> makeTuner()   { return std::unique_ptr<Device> (new Tuner()); }
} // namespace lathe::devices
