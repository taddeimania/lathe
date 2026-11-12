#include "Compare.h"
#include "../Assistant/MixAnalysis.h"

namespace lathe
{
const char* Compare::bandNames[7] = { "Sub", "Bass", "Low mids", "Mids", "Upper mids", "Presence", "Air" };

String Compare::slotName (Slot s)
{
    switch (s)
    {
        case before:    return "Before";
        case now:       return "Now";
        case reference: return "Reference";
    }
    return {};
}

namespace
{
constexpr double kMaxReferenceSeconds = 20.0 * 60.0;
constexpr int kFade = 512;   // samples to crossfade over when switching what you hear

/** The mix part of a MixAnalysis report, as numbers. */
Compare::Measures measure (const AudioBuffer<float>& audio, double sampleRate)
{
    Compare::Measures m;
    const int64 length = audio.getNumSamples();
    if (length <= 0 || sampleRate <= 0.0)
        return m;
    assistant::MixAnalysis analysis ({ { "Mix", true, false } }, sampleRate, sampleRate * 2.0, 1.0, length, 4);
    for (int64 at = 0; at < length; at += 4096)
    {
        const int n = (int) jmin ((int64) 4096, length - at);
        analysis.add (0, audio, (int) at, n, at);
    }
    const auto report = analysis.report();
    const auto mix = report["mix"];
    if (! mix.isObject())
        return m;
    m.valid = true;
    m.lufsValid = mix["integrated_lufs"].isDouble() || mix["integrated_lufs"].isInt();
    m.lufs = m.lufsValid ? (float) (double) mix["integrated_lufs"] : -70.0f;
    m.truePeakDb = (float) (double) mix.getProperty ("true_peak_dbtp", -120.0);
    m.crestDb = (float) (double) mix.getProperty ("crest_db", 0.0);
    const auto stereo = mix["stereo"];
    m.correlation = (float) (double) stereo.getProperty ("correlation", 1.0);
    m.widthDb = stereo.hasProperty ("side_vs_mid_db") ? (float) (double) stereo["side_vs_mid_db"] : -60.0f;
    const auto bands = mix["band_balance_db"];
    for (int b = 0; b < 7; ++b)
        m.bandsDb[b] = (float) (double) bands.getProperty (Identifier (assistant::MixAnalysis::bands[b].name), -60.0);
    return m;
}
} // namespace

//==============================================================================
/** One background job: rendering the mix, or reading the reference. */
class Compare::Job : public Thread
{
public:
    Job (Compare& c, Slot s) : Thread ("Lathe compare"), owner (c), slot (s) {}
    ~Job() override   { signalThreadShouldExit(); stopThread (60000); }

    Compare& owner;
    Slot slot;
    Engine::ExportSettings settings;   // rendering
    File file;                         // reading
    double targetRate = 0.0;

    // results
    Result result = Result::ok();
    Version version;
    std::atomic<bool> finished { false };

    void run() override
    {
        result = file != File() ? readFile() : renderMix();
        if (result.wasOk() && version.audio != nullptr && ! threadShouldExit())
            version.measures = measure (*version.audio, version.sampleRate);
        finished = true;
    }

private:
    Result renderMix()
    {
        auto audio = std::make_shared<AudioBuffer<float>>();
        auto prepare = [&] (const std::vector<Engine::RenderTap>&, int64 length) -> Result
        {
            if (length <= 0 || length > (int64) (targetRate * kMaxReferenceSeconds))
                return Result::fail ("That's too long to compare (20 minutes at most).");
            audio->setSize (2, (int) length);
            audio->clear();
            return Result::ok();
        };
        auto listen = [&] (int tap, const AudioBuffer<float>& b, int start, int count, int64 at)
        {
            if (tap != 0 || at >= audio->getNumSamples())
                return;
            const int n = (int) jmin ((int64) count, (int64) audio->getNumSamples() - at);
            for (int ch = 0; ch < 2; ++ch)
                audio->copyFrom (ch, (int) at, b, jmin (ch, b.getNumChannels() - 1), start, n);
        };
        auto r = owner.engine.renderOffline (settings, [this] (double p) { owner.progress = p; return ! threadShouldExit(); },
                                             prepare, listen);
        if (r.failed())
            return Result::fail (r.getErrorMessage().replace ("exporting", "comparing"));
        version.audio = audio;
        version.sampleRate = targetRate;
        version.startBeat = settings.startBeat;
        version.endBeat = settings.endBeat;
        return Result::ok();
    }

    Result readFile()
    {
        AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
        {
           #if JUCE_MAC
            const String kinds = "WAV, AIFF, FLAC, Ogg, MP3 and M4A";
           #else
            const String kinds = "WAV, AIFF, FLAC, Ogg and MP3";
           #endif
            return Result::fail ("Lathe can't read " + file.getFileName() + " (" + kinds + " files work).");
        }
        if (reader->sampleRate <= 0.0 || (double) reader->lengthInSamples / reader->sampleRate > kMaxReferenceSeconds)
            return Result::fail (file.getFileName() + " is too long to compare (20 minutes at most).");

        const int srcLength = (int) reader->lengthInSamples;
        AudioBuffer<float> src (2, srcLength);
        src.clear();
        reader->read (&src, 0, srcLength, 0, true, reader->numChannels > 1);
        if (reader->numChannels == 1)
            src.copyFrom (1, 0, src, 0, 0, srcLength);
        owner.progress = 0.5;
        if (threadShouldExit())
            return Result::fail ("Cancelled.");

        auto audio = std::make_shared<AudioBuffer<float>>();
        if (std::abs (reader->sampleRate - targetRate) < 0.5)
            *audio = std::move (src);
        else
        {
            const double ratio = reader->sampleRate / targetRate;
            const int outLength = (int) std::floor ((double) srcLength / ratio);
            audio->setSize (2, outLength);
            for (int ch = 0; ch < 2; ++ch)
            {
                LagrangeInterpolator resampler;
                resampler.process (ratio, src.getReadPointer (ch), audio->getWritePointer (ch), outLength, srcLength, 0);
            }
        }
        version.audio = audio;
        version.sampleRate = targetRate;
        version.file = file;
        return Result::ok();
    }
};

//==============================================================================
/** What the audio thread plays: the versions as they were when it was made (immutable),
    plus a few atomics the window changes while it plays. */
class Compare::Player : public Engine::OutsidePlayer
{
public:
    std::array<std::shared_ptr<const AudioBuffer<float>>, numSlots> audio;
    std::array<std::atomic<float>, numSlots> gain {};   // linear
    std::atomic<int> heard { (int) now };
    std::atomic<bool> playing { false };
    std::atomic<int64> position { 0 };
    int64 length = 0;

    bool isPlaying() const noexcept override   { return playing.load (std::memory_order_relaxed); }

    void render (AudioBuffer<float>& out, int len) noexcept override
    {
        out.clear (0, len);
        if (length <= 0)
            return;
        int64 pos = position.load (std::memory_order_relaxed);
        const int want = heard.load (std::memory_order_relaxed);
        for (int i = 0; i < len; ++i)
        {
            if (pos >= length)
                pos = 0;   // round and round
            if (want != current && fadeLeft == 0)
            {
                previous = current;
                current = want;
                fadeLeft = kFade;
            }
            const float mixNew = fadeLeft > 0 ? 1.0f - (float) fadeLeft / (float) kFade : 1.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                float v = sample (current, ch, pos) * mixNew;
                if (fadeLeft > 0)
                    v += sample (previous, ch, pos) * (1.0f - mixNew);
                out.setSample (ch, i, v);
            }
            if (fadeLeft > 0)
                --fadeLeft;
            ++pos;
        }
        position.store (pos, std::memory_order_relaxed);
    }

private:
    int current = (int) now, previous = (int) now, fadeLeft = 0;   // audio thread only

    float sample (int slot, int ch, int64 pos) const noexcept
    {
        if (! isPositiveAndBelow (slot, numSlots))
            return 0.0f;
        const auto& a = audio[(size_t) slot];
        if (a == nullptr || pos >= a->getNumSamples())
            return 0.0f;
        return a->getSample (ch, (int) pos) * gain[(size_t) slot].load (std::memory_order_relaxed);
    }
};

//==============================================================================
Compare::Compare (Engine& e, Edit& ed, PropertiesFile& p) : engine (e), edit (ed), settings (p) {}

Compare::~Compare()
{
    stopTimer();
    job.reset();
    if (player != nullptr)
        engine.setOutsidePlayer (nullptr);
}

Result Compare::startJob (std::unique_ptr<Job> j, const String& what, std::function<void (Result)> done)
{
    if (job != nullptr)
        return Result::fail ("Compare is still " + busyWhat.toLowerCase() + ".");
    j->targetRate = engine.getSampleRate();
    if (j->targetRate <= 0.0)
        return Result::fail ("No audio device is running, so Lathe can't compare (Options > Audio / MIDI Settings).");
    progress = 0.0;
    busyWhat = what;
    whenDone = std::move (done);
    job = std::move (j);
    job->startThread();
    startTimerHz (15);
    sendChangeMessage();
    return Result::ok();
}

Result Compare::render (Slot slot, std::function<void (Result)> done)
{
    jassert (slot != reference);
    if (engine.isRecording())
        return Result::fail ("Stop recording first.");
    if (engine.isExporting())
        return Result::fail ("Lathe is already rendering. Wait for that to finish.");

    auto j = std::make_unique<Job> (*this, slot);
    auto& s = j->settings;
    s.mix = true;
    s.stems = false;
    s.tailSeconds = 0.0;
    if (slot == now && versions[before].has())
    {
        s.startBeat = versions[before].startBeat;   // the same bars, so they line up
        s.endBeat = versions[before].endBeat;
    }
    else if (edit.loopEnabled && edit.loopEnd > edit.loopStart)
    {
        s.startBeat = edit.loopStart;
        s.endBeat = edit.loopEnd;
    }
    else
    {
        s.startBeat = 0.0;
        s.endBeat = edit.getEndBeat();
    }
    if (s.endBeat <= s.startBeat + 1.0e-6)
        return Result::fail ("The song is empty: there's nothing to compare.");

    if (engine.isPlaying())
        engine.stop();
    stop();
    return startJob (std::move (j), slot == before ? "Rendering Before" : "Rendering Now", std::move (done));
}

Result Compare::loadReference (const File& f, std::function<void (Result)> done)
{
    if (! f.existsAsFile())
        return Result::fail ("There's no file at " + f.getFullPathName() + ".");
    auto j = std::make_unique<Job> (*this, reference);
    j->file = f;
    return startJob (std::move (j), "Reading the reference", std::move (done));
}

void Compare::timerCallback()
{
    if (job == nullptr || ! job->finished.load())
    {
        sendChangeMessage();   // progress
        return;
    }
    stopTimer();
    auto finished = std::move (job);
    finished->stopThread (1000);
    busyWhat.clear();
    if (finished->result.wasOk() && finished->version.has())
    {
        auto v = std::move (finished->version);
        v.madeAt = Time::currentTimeMillis();
        if (finished->slot == reference)
            rememberReference (v.file);
        versions[(size_t) finished->slot] = std::move (v);
        installPlayer (false);
    }
    auto done = std::move (whenDone);
    sendChangeMessage();
    if (done)
        done (finished->result);
}

void Compare::clear (Slot s)
{
    versions[(size_t) s] = {};
    if (s == reference)
        rememberReference ({});
    installPlayer (isPlaying());
    sendChangeMessage();
}

void Compare::projectChanged()
{
    stop();
    versions[before] = {};
    versions[now] = {};
    versions[reference] = {};
    installPlayer (false);
    sendChangeMessage();
    if (const auto f = rememberedReference(); f.existsAsFile())
        loadReference (f);
}

//==============================================================================
void Compare::installPlayer (bool keepPlaying)
{
    const int64 pos = player != nullptr ? player->position.load() : 0;
    auto p = std::make_shared<Player>();
    int64 length = 0;
    for (int s = 0; s < numSlots; ++s)
        p->audio[(size_t) s] = versions[(size_t) s].audio;
    // The song's versions set the length; the reference alone if there's nothing else.
    for (auto s : { before, now })
        if (versions[s].has())
            length = jmax (length, (int64) versions[s].audio->getNumSamples());
    if (length == 0 && versions[reference].has())
        length = versions[reference].audio->getNumSamples();
    p->length = length;
    p->position = length > 0 ? pos % length : 0;
    p->heard = (int) heard;
    p->playing = keepPlaying && length > 0;
    for (int s = 0; s < numSlots; ++s)
        p->gain[(size_t) s] = Decibels::decibelsToGain (gainFor ((Slot) s));
    player = p;
    engine.setOutsidePlayer (player);
}

void Compare::play()
{
    if (player == nullptr || player->length <= 0)
        return;
    if (engine.isPlaying())
        engine.stop();
    player->playing = true;
    sendChangeMessage();
}

void Compare::stop()
{
    if (player != nullptr && player->playing.exchange (false))
        sendChangeMessage();
}

bool Compare::isPlaying() const   { return player != nullptr && player->playing.load(); }

void Compare::setHeard (Slot s)
{
    heard = s;
    if (player != nullptr)
        player->heard = (int) s;
    sendChangeMessage();
}

double Compare::getPositionSeconds() const
{
    const double sr = engine.getSampleRate();
    return player != nullptr && sr > 0.0 ? (double) player->position.load() / sr : 0.0;
}

double Compare::getLengthSeconds() const
{
    const double sr = engine.getSampleRate();
    return player != nullptr && sr > 0.0 ? (double) player->length / sr : 0.0;
}

void Compare::setPositionSeconds (double t)
{
    if (player == nullptr || player->length <= 0)
        return;
    player->position = jlimit ((int64) 0, player->length - 1, (int64) (t * engine.getSampleRate()));
    sendChangeMessage();
}

void Compare::setMatchLoudness (bool on)
{
    matchLoudness = on;
    if (player != nullptr)
        for (int s = 0; s < numSlots; ++s)
            player->gain[(size_t) s] = Decibels::decibelsToGain (gainFor ((Slot) s));
    sendChangeMessage();
}

float Compare::gainFor (Slot s) const
{
    if (! matchLoudness || ! versions[(size_t) s].measures.lufsValid)
        return 0.0f;
    float quietest = 1000.0f;
    for (auto& v : versions)
        if (v.has() && v.measures.lufsValid)
            quietest = jmin (quietest, v.measures.lufs);
    return quietest - versions[(size_t) s].measures.lufs;   // down to the quietest: never louder
}

//==============================================================================
void Compare::rememberReference (const File& f)
{
    if (edit.projectFile == File())
        return;
    auto map = JSON::parse (settings.getValue ("compareReferences"));
    if (! map.isObject())
        map = var (new DynamicObject());
    const Identifier key (edit.projectFile.getFullPathName());
    if (f == File())
        map.getDynamicObject()->removeProperty (key);
    else
        map.getDynamicObject()->setProperty (key, f.getFullPathName());
    settings.setValue ("compareReferences", JSON::toString (map, true));
    settings.saveIfNeeded();
}

File Compare::rememberedReference() const
{
    if (edit.projectFile == File())
        return {};
    const auto map = JSON::parse (settings.getValue ("compareReferences"));
    const auto path = map[Identifier (edit.projectFile.getFullPathName())].toString();
    return path.isNotEmpty() && File::isAbsolutePath (path) ? File (path) : File();
}

//==============================================================================
var Compare::measuresToVar (const Measures& m)
{
    DynamicObject::Ptr o = new DynamicObject();
    auto r = [] (float v) { return std::round (v * 10.0f) / 10.0f; };
    o->setProperty ("integrated_lufs", m.lufsValid ? var (r (m.lufs)) : var ("too quiet to measure"));
    o->setProperty ("true_peak_dbtp", r (m.truePeakDb));
    o->setProperty ("crest_db", r (m.crestDb));
    if (m.widthDb < -40.0f) o->setProperty ("width", "mono");
    else                    o->setProperty ("side_vs_mid_db", r (m.widthDb));
    o->setProperty ("correlation", std::round (m.correlation * 100.0f) / 100.0f);
    DynamicObject::Ptr bands = new DynamicObject();
    for (int b = 0; b < 7; ++b)
        bands->setProperty (Identifier (assistant::MixAnalysis::bands[b].name), r (m.bandsDb[b]));
    o->setProperty ("band_balance_db", var (bands.get()));
    return var (o.get());
}

var Compare::toVar() const
{
    DynamicObject::Ptr o = new DynamicObject();
    for (int s = 0; s < numSlots; ++s)
    {
        const auto& v = versions[(size_t) s];
        if (! v.has())
            continue;
        auto m = measuresToVar (v.measures);
        auto* mo = m.getDynamicObject();
        mo->setProperty ("seconds", std::round ((double) v.audio->getNumSamples() / jmax (1.0, v.sampleRate) * 10.0) / 10.0);
        if (s == reference)
            mo->setProperty ("file", v.file.getFileName());
        o->setProperty (Identifier (slotName ((Slot) s).toLowerCase()), m);
    }
    o->setProperty ("summary", summary());
    return var (o.get());
}

String Compare::rangeText (Slot s) const
{
    const auto& v = versions[(size_t) s];
    if (! v.has() || s == reference)
        return {};
    const double perBar = jmax (1.0e-6, edit.beatsPerBar());
    auto bar = [perBar] (double beats)
    {
        const double b = 1.0 + beats / perBar;
        return std::abs (b - std::round (b)) < 0.01 ? String (roundToInt (b)) : String (b, 1);
    };
    return "Bars " + bar (v.startBeat) + " to " + bar (v.endBeat);
}

String Compare::summary() const
{
    const auto& b = versions[before];
    const auto& n = versions[now];
    const auto& r = versions[reference];
    StringArray out;
    auto signedDb = [] (float d) { return (d >= 0 ? "+" : "") + String (d, 1) + " dB"; };

    if (b.has() && n.has() && b.measures.valid && n.measures.valid)
    {
        StringArray changes;
        if (b.measures.lufsValid && n.measures.lufsValid && std::abs (n.measures.lufs - b.measures.lufs) >= 0.5f)
            changes.add ("loudness " + signedDb (n.measures.lufs - b.measures.lufs));
        // The bands that moved most against the whole.
        std::vector<std::pair<float, int>> moved;
        for (int i = 0; i < 7; ++i)
            moved.push_back ({ n.measures.bandsDb[i] - b.measures.bandsDb[i], i });
        std::sort (moved.begin(), moved.end(), [] (auto& x, auto& y) { return std::abs (x.first) > std::abs (y.first); });
        for (int k = 0; k < 2; ++k)
            if (std::abs (moved[(size_t) k].first) >= 0.5f)
                changes.add (String (bandNames[moved[(size_t) k].second]).toLowerCase() + " " + signedDb (moved[(size_t) k].first));
        if (std::abs (n.measures.widthDb - b.measures.widthDb) >= 1.0f && n.measures.widthDb > -40.0f)
            changes.add (n.measures.widthDb > b.measures.widthDb ? "wider" : "narrower");
        if (std::abs (n.measures.crestDb - b.measures.crestDb) >= 1.0f)
            changes.add (n.measures.crestDb < b.measures.crestDb ? "less dynamic (crest " + signedDb (n.measures.crestDb - b.measures.crestDb) + ")"
                                                                  : "more dynamic (crest " + signedDb (n.measures.crestDb - b.measures.crestDb) + ")");
        out.add (changes.isEmpty() ? "Now measures about the same as Before."
                                   : "Now against Before: " + changes.joinIntoString (", ") + ".");
    }

    const auto* mine = n.has() ? &n : b.has() ? &b : nullptr;
    if (r.has() && r.measures.valid && mine != nullptr && mine->measures.valid)
    {
        const auto which = mine == &n ? String ("Now") : String ("Before");
        StringArray against;
        std::vector<std::pair<float, int>> diff;
        for (int i = 0; i < 7; ++i)
            diff.push_back ({ mine->measures.bandsDb[i] - r.measures.bandsDb[i], i });
        std::sort (diff.begin(), diff.end(), [] (auto& x, auto& y) { return std::abs (x.first) > std::abs (y.first); });
        for (int k = 0; k < 2; ++k)
            if (std::abs (diff[(size_t) k].first) >= 1.0f)
                against.add (String (bandNames[diff[(size_t) k].second]).toLowerCase() + " " + signedDb (diff[(size_t) k].first));
        if (mine->measures.lufsValid && r.measures.lufsValid && std::abs (mine->measures.lufs - r.measures.lufs) >= 1.0f)
            against.add ("loudness " + signedDb (mine->measures.lufs - r.measures.lufs));
        if (std::abs (mine->measures.widthDb - r.measures.widthDb) >= 2.0f && r.measures.widthDb > -40.0f)
            against.add (mine->measures.widthDb > r.measures.widthDb ? "wider" : "narrower");
        out.add (against.isEmpty() ? which + " is close to the reference in balance and loudness."
                                   : which + " against the reference: " + against.joinIntoString (", ") + ".");

        // Did the change move towards the reference?
        if (b.has() && n.has() && b.measures.valid && n.measures.valid)
        {
            float distBefore = 0.0f, distNow = 0.0f;
            for (int i = 0; i < 7; ++i)
            {
                distBefore += std::abs (b.measures.bandsDb[i] - r.measures.bandsDb[i]);
                distNow += std::abs (n.measures.bandsDb[i] - r.measures.bandsDb[i]);
            }
            if (std::abs (distNow - distBefore) >= 1.0f)
                out.add (distNow < distBefore ? "The change brought the balance closer to the reference."
                                              : "The change moved the balance further from the reference.");
        }
    }
    return out.joinIntoString (" ");
}
} // namespace lathe
