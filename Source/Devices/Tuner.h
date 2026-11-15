#pragma once

#include "Device.h"

namespace lathe::devices
{
/** The Tuner: an effect that shows the pitch of what goes through it (Tuner.cpp). */
std::unique_ptr<Device> makeTuner();

namespace tuner
{
/** One look at a signal: its pitch, how clearly it's one pitch (0..1), and its level. hz is 0
    when there's no clear pitch (silence, noise, a chord). */
struct Reading
{
    float hz = 0.0f, clarity = 0.0f, level = 0.0f;
};

/** The pitch of `n` samples at `sampleRate`, by YIN (de Cheveigne and Kawahara): the lag where
    the signal best matches itself, so a guitar's strong harmonics don't fool it into the octave
    above. Looks between minHz and maxHz. Message thread or any thread; no allocation beyond `work`
    (resized to n / 2 if smaller). */
Reading detect (const float* samples, int n, double sampleRate, std::vector<float>& work,
                float minHz = 25.0f, float maxHz = 1600.0f);

/** The nearest note to hz with A4 at `reference` Hz: its MIDI number and how far off it is, in cents. */
struct Note
{
    int midi = -1;
    float cents = 0.0f;
};
Note nearest (float hz, float reference = 440.0f);
} // namespace tuner
} // namespace lathe::devices
