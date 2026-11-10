#pragma once

#include "Device.h"

namespace lathe::devices
{
std::unique_ptr<Device> makeBassSynth();
std::unique_ptr<Device> makePolySynth();
std::unique_ptr<Device> makeDrums();

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

float noteHz (float midiNote);
float polyBlep (float t, float dt);
} // namespace synth
} // namespace lathe::devices
