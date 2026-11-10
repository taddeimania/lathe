#pragma once

#include "../Common.h"

namespace lathe::devices
{
/** What a device is, as Lathe lists it: the plug-in menu, the browser, the AI assistant. */
enum class Kind { effect, instrument };

/** One control on a device's panel. */
struct Control
{
    const char* param;   // parameter id
    const char* label;   // as shown under the knob
    const char* tip;     // one sentence, shown on hover
};

/** A titled group of controls on the panel. */
struct Section
{
    const char* title;
    std::vector<Control> controls;
};

/** A named starting point: parameter id -> value (in the parameter's own units). */
struct Preset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values;
};

/** How the panel shows a gain-reduction (or other) meter. */
struct Meter
{
    const char* label;       // "Reduction"
    float minDb, maxDb;      // shown range, e.g. -24..0
};

//==============================================================================
/** Lathe's built-in instruments and effects. They load like any plug-in (same
    chain, saving, undo and assistant tools) but come with Lathe: nothing to
    install, scan or sign, and a bandmate who opens your project has them too.

    A device declares its parameters, its panel (one row of type buttons and a
    few sections of knobs) and its presets; Device does the rest: state, presets
    as programs, the description Lathe saves in projects, and the editor. */
class Device : public AudioPluginInstance
{
public:
    Device (const String& identifier, const String& displayName, Kind, AudioProcessorValueTreeState::ParameterLayout);

    // --- what the panel shows
    virtual String blurb() const = 0;
    /** The choice parameter shown as a row of buttons at the top (empty: none). */
    virtual String modeParam() const   { return {}; }
    /** Called when the person picks a mode on the panel (not from automation or a preset):
        a device may move other controls to where that mode starts. */
    virtual void modeChosen (int)      {}
    virtual std::vector<Section> sections() const = 0;
    /** False greys a control out while it does nothing (Time while Sync is on). */
    virtual bool isControlActive (const String& /*param*/) const   { return true; }
    virtual std::vector<Preset> presets() const   { return {}; }
    virtual std::optional<Meter> meter() const    { return std::nullopt; }
    /** The meter's reading (dB), any thread. */
    virtual float meterValue() const   { return 0.0f; }
    /** A line under the knobs (e.g. what LEARN is doing), message thread. Empty: none. */
    virtual String statusLine() const  { return {}; }
    /** An action button in the header (e.g. "Learn"). Empty: none. */
    virtual String actionName() const  { return {}; }
    virtual void action()              {}
    virtual bool actionActive() const  { return false; }

    // --- parameters
    float get (const char* id) const;
    int geti (const char* id) const   { return roundToInt (get (id)); }
    void set (const char* id, float value);   // in the parameter's own units, notifying the host
    AudioProcessorValueTreeState state;

    // --- AudioPluginInstance
    void fillInPluginDescription (PluginDescription&) const override;
    const String getName() const override   { return displayName; }
    bool acceptsMidi() const override       { return kind == Kind::instrument; }
    bool producesMidi() const override      { return false; }
    bool isMidiEffect() const override      { return false; }
    double getTailLengthSeconds() const override   { return 0.0; }
    bool hasEditor() const override         { return true; }
    AudioProcessorEditor* createEditor() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void releaseResources() override {}

    int getNumPrograms() override;
    int getCurrentProgram() override        { return currentPreset; }
    void setCurrentProgram (int) override;
    const String getProgramName (int) override;
    void changeProgramName (int, const String&) override {}

    void getStateInformation (MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    const String identifier, displayName;
    const Kind kind;

protected:
    std::atomic<float>* raw (const char* id) const;
    int currentPreset = 0;

private:
    mutable std::map<String, std::atomic<float>*> rawCache;
};

//==============================================================================
// The catalogue.

inline const char* kFormatName = "Lathe";

struct Info
{
    String id;          // "reverb": saved in projects as lathe:<id>
    String name;        // "Reverb"
    Kind kind;
    String blurb;
    std::function<std::unique_ptr<Device>()> create;
};

/** Every built-in device, in the order Lathe lists them. */
const std::vector<Info>& all();
/** The description Lathe uses for a device (format "Lathe", file "lathe:<id>"). */
PluginDescription describe (const Info&);
/** The device a description (saved or listed) means, if it's one of Lathe's. */
const Info* find (const PluginDescription&);
std::unique_ptr<AudioPluginInstance> create (const PluginDescription&);
inline bool isBuiltIn (const PluginDescription& d)   { return d.pluginFormatName == kFormatName; }
/** A built-in effect that's just been put on a return plays only the effect (Mix 100%),
    since the return's dry sound is already in the mix. Does nothing for anything else. */
void startOnReturn (AudioPluginInstance&);

// Small helpers for devices' parameter layouts.
std::unique_ptr<AudioParameterFloat> percent (const char* id, const char* name, float def);
std::unique_ptr<AudioParameterFloat> decibels (const char* id, const char* name, float lo, float hi, float def);
std::unique_ptr<AudioParameterFloat> number (const char* id, const char* name, NormalisableRange<float>, float def,
                                             std::function<String (float)> text);
std::unique_ptr<AudioParameterChoice> choice (const char* id, const char* name, const StringArray&, int def);
std::unique_ptr<AudioParameterBool> toggle (const char* id, const char* name, bool def);
String msText (float ms);
String secondsText (float s);
String hzText (float hz);
} // namespace lathe::devices
