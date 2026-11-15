#pragma once

#include "Device.h"

namespace lathe::devices
{
std::unique_ptr<Device> makeBassSynth();
std::unique_ptr<Device> makePolySynth();
std::unique_ptr<Device> makeDrums();
/** A modelled piano: struck strings and a soundboard, no samples (Piano.cpp). */
std::unique_ptr<Device> makePiano();

/** The small building blocks the instruments share. */
namespace synth
{
enum class Wave { saw, square, triangle, sine };

/** A band-limited (PolyBLEP) oscillator. `inc` is cycles per sample. */
struct Osc
{
    float phase = 0.0f;
    float next (Wave, float inc);
};

/** A trapezoidal state-variable filter; `k` is 1/Q. */
struct Svf
{
    void set (float hz, float k, float sampleRate);
    float low (float);
    float band (float);
    float high (float);
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, damping = 1.0f, ic1 = 0.0f, ic2 = 0.0f;
};

/** 24 dB/octave low-pass: two state-variable stages, resonance on the second. */
struct Ladder
{
    void set (float hz, float resonance, float sampleRate);
    float tick (float);
    Svf a, b;
};

/** Reads a circular delay line `delaySamples` behind `writePos`, with linear interpolation.
    Whole samples are split off as an integer and wrapped with integer arithmetic, so the
    index is always inside the line (float wrapping can round up to exactly `size`). */
float readDelay (const float* line, int size, int writePos, float delaySamples);

float noteHz (float midiNote);
float polyBlep (float t, float dt);

/** Splits a block at its MIDI events: render (from, to), then the event, in order. */
template <typename Render, typename Event>
void withMidi (int numSamples, const MidiBuffer& midi, Render&& render, Event&& event)
{
    int pos = 0;
    for (const auto m : midi)
    {
        const int at = jlimit (0, numSamples, m.samplePosition);
        if (at > pos)
        {
            render (pos, at);
            pos = at;
        }
        event (m.getMessage());
    }
    if (pos < numSamples)
        render (pos, numSamples);
}
} // namespace synth
} // namespace lathe::devices
