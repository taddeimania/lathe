#pragma once

#include "../Engine/Engine.h"

namespace lathe
{
//==============================================================================
/** The Compare window's model: up to three versions of the song to switch between while
    they play in step.

    - Before: the mix as it was when someone pressed Mark Before (you, or the AI assistant
      before it changes things), rendered offline.
    - Now: the mix as it is, rendered offline over the same bars.
    - Reference: a finished track you like, from an audio file. Remembered for each song on
      this computer (not part of the song, and not shared).

    Each is measured the way analyze_mix measures a mix (loudness, true peak, crest, width,
    balance across seven bands). Playback goes through Lathe's audio device in place of the
    song; switching what you hear keeps the position, with a short crossfade. With Match
    Loudness on, every version plays at the loudness of the quietest, so louder doesn't win.

    Message thread, except where it says otherwise. */
class Compare : public ChangeBroadcaster, private Timer
{
public:
    enum Slot { before = 0, now = 1, reference = 2 };
    static constexpr int numSlots = 3;
    static String slotName (Slot);

    struct Measures
    {
        bool valid = false;
        float lufs = -70.0f;        // integrated loudness
        bool lufsValid = false;
        float truePeakDb = -120.0f;
        float crestDb = 0.0f;       // peak to RMS
        float widthDb = -60.0f;     // side against mid; very negative = mono
        float correlation = 1.0f;
        float bandsDb[7] {};        // each band's share of the whole, in dB
    };
    struct Version
    {
        std::shared_ptr<const AudioBuffer<float>> audio;   // stereo, at the device rate
        double sampleRate = 0.0;
        Measures measures;
        int64 madeAt = 0;           // when it was rendered or loaded
        double startBeat = 0.0, endBeat = 0.0;   // the bars it covers (Before and Now)
        File file;                  // the reference's file
        bool has() const noexcept   { return audio != nullptr; }
    };

    static const char* bandNames[7];

    Compare (Engine&, Edit&, PropertiesFile&);
    ~Compare() override;

    const Version& get (Slot s) const   { return versions[(size_t) s]; }

    // --- making the versions
    /** Renders the mix (Before or Now) in the background. Over the loop if it's on, else the
        whole song; Now uses Before's bars when there is a Before. `done` is called on the
        message thread. Fails at once if Lathe is busy (playing, recording, exporting). */
    Result render (Slot, std::function<void (Result)> done = {});
    /** Loads and measures an audio file as the reference, in the background, and remembers it
        for this song. */
    Result loadReference (const File&, std::function<void (Result)> done = {});
    void clear (Slot);
    bool isBusy() const noexcept        { return job != nullptr; }
    String busyText() const             { return busyWhat; }
    double getProgress() const noexcept { return progress.load(); }
    /** Call when a project has been opened: forgets Before and Now, and loads the reference
        remembered for it, if any. */
    void projectChanged();

    // --- listening
    void play();
    void stop();
    bool isPlaying() const;
    /** The song itself is playing (which wins: the comparison stops). */
    bool songIsPlaying() const          { return engine.isPlaying(); }
    void setHeard (Slot);
    Slot getHeard() const noexcept      { return heard; }
    /** Seconds into what's playing, and how long the comparison is. */
    double getPositionSeconds() const;
    double getLengthSeconds() const;
    void setPositionSeconds (double);
    void setMatchLoudness (bool);
    bool getMatchLoudness() const noexcept   { return matchLoudness; }
    /** The gain each version plays at (dB), after Match Loudness. */
    float gainFor (Slot) const;

    // --- in words and numbers (the window and the AI assistant)
    static var measuresToVar (const Measures&);
    /** Every version there is, its measurements, and what changed from Before to Now and how
        both stand against the reference. */
    var toVar() const;
    /** A few sentences on the biggest differences. */
    String summary() const;
    /** "Bars 1 to 17": what a Before or Now covers. */
    String rangeText (Slot) const;

private:
    class Job;
    class Player;

    Engine& engine;
    Edit& edit;
    PropertiesFile& settings;
    std::array<Version, numSlots> versions;
    Slot heard = now;
    bool matchLoudness = true;
    std::shared_ptr<Player> player;
    std::unique_ptr<Job> job;
    String busyWhat;
    std::atomic<double> progress { 0.0 };
    std::function<void (Result)> whenDone;

    void timerCallback() override;
    void installPlayer (bool keepPlaying);
    void rememberReference (const File&);
    File rememberedReference() const;
    Result startJob (std::unique_ptr<Job>, const String& what, std::function<void (Result)> done);
    friend class Job;
};
} // namespace lathe
