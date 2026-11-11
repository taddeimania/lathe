#pragma once

#include "Device.h"

namespace lathe::devices
{
//==============================================================================
// Four devices built on Émilie Gillet's open-source DSP code (MIT licence; see
// ThirdParty/mutable/README.md). Lathe gives them its own names, controls and panel.

/** Multi Synth: an eight-voice synth with 24 ways to make a sound (analog, FM, wavetable,
    chords, speech, strings, drums and more), each shaped by three controls. */
std::unique_ptr<Device> makeMultiSynth();
/** Resonator: strings, bells and struck bars that ring when you play them. */
std::unique_ptr<Device> makeResonator();
/** Grains: chops what goes through it into grains, then smears, freezes and scatters it. */
std::unique_ptr<Device> makeGrains();
/** Random Notes: a note generator that plays random melodies and rhythms, in time and in key. */
std::unique_ptr<Device> makeRandomNotes();

/** Runs DSP written for one fixed sample rate inside a project at another: resamples in and
    out (windowed sinc), and calls straight through when the rates match. The DSP is called
    in chunks of at most `chunk` samples. Audio thread, after prepare(). */
class CoreRate
{
public:
    static constexpr int kMaxChunk = 32;

    void prepare (double hostRate, double coreRate, int chunk);
    bool isDirect() const noexcept      { return direct; }
    double getCoreRate() const noexcept { return core; }
    /** Samples of delay the resampling adds, at the host rate (0 when direct). */
    int latencySamples() const noexcept { return latency; }

    /** An instrument: render (l, r, count) makes `count` samples at the core rate. */
    template <typename Render>
    void generate (float* left, float* right, int numSamples, Render&& render)
    {
        if (direct)
        {
            for (int done = 0; done < numSamples; done += chunk)
                render (left + done, right + done, jmin (chunk, numSamples - done));
            return;
        }
        const int need = (int) std::ceil (numSamples * core / host) + 4;
        float l[kMaxChunk], r[kMaxChunk];
        while (available() < need)
        {
            render (l, r, chunk);
            push (l, r, chunk);
        }
        pull (left, right, numSamples);
    }

    /** An effect: proc (inL, inR, outL, outR, count) at the core rate. */
    template <typename Proc>
    void process (const float* inLeft, const float* inRight, float* left, float* right, int numSamples, Proc&& proc)
    {
        if (direct)
        {
            for (int done = 0; done < numSamples; done += chunk)
                proc (inLeft + done, inRight + done, left + done, right + done, jmin (chunk, numSamples - done));
            return;
        }
        in[0].insert (in[0].end(), inLeft, inLeft + numSamples);
        in[1].insert (in[1].end(), inRight, inRight + numSamples);
        const double ratio = host / core;   // host samples per core sample
        const int need = (int) std::ceil (chunk * ratio) + 4;
        float a[kMaxChunk], b[kMaxChunk], l[kMaxChunk], r[kMaxChunk];
        while ((int) in[0].size() - inRead >= need)
        {
            const int used = down[0].process (ratio, in[0].data() + inRead, a, chunk);
            down[1].process (ratio, in[1].data() + inRead, b, chunk);
            inRead += used;
            proc (a, b, l, r, chunk);
            push (l, r, chunk);
        }
        pull (left, right, numSamples);
    }

private:
    void push (const float* l, const float* r, int n);
    int available() const noexcept   { return (int) out[0].size() - outRead; }
    void pull (float* left, float* right, int numSamples);
    void compact();

    double host = 48000.0, core = 48000.0;
    int chunk = 24, latency = 0;
    bool direct = true;
    WindowedSincInterpolator up[2], down[2];
    std::vector<float> out[2], in[2];
    int outRead = 0, inRead = 0;
};
} // namespace lathe::devices
