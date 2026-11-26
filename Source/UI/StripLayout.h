#pragma once

#include <algorithm>

/** How tall a mixer strip's sections are, and which of them fit.

    A strip always keeps its name, pan, fader (with meter and dB readout) and its record / solo /
    mute buttons. The other sections give way, in this order, when the mixer gets short (a tall
    plug-in tray, a small window): the insert list and the EQ thumbnail first (the tray below shows
    both for the selected track), then the routing (In, MIDI to, Out, monitoring), then the sends.
    Last, the strip goes compact: the pan knob moves beside the fader and the row kept for a group
    tag goes. The fader gives up length before anything goes, down to a comfortable length, and
    after everything has gone, down to a minimum. Shorter than that, the mixer scrolls.

    Plain numbers, no JUCE, so the tests can check it without a window. Every strip in the mixer
    uses the same plan, so the rows line up side by side. */
namespace lathe::striplayout
{
constexpr int insertRows = 4;

constexpr int pad       = 6;                          // above and below everything
constexpr int header    = 10 + 20 + 4;                // group tag, name, gap
constexpr int compactHeader = 20 + 2;                 // name, gap
constexpr int labelH    = 13;                         // a section's label
constexpr int ioHeight  = 3 * (labelH + 23) + 23;     // In, MIDI to, Out, monitoring buttons
constexpr int insertsHeight = 6 + labelH + insertRows * 19 + 20;
constexpr int eqHeight  = 4 + labelH + 26;
constexpr int sendRowH  = 42;
constexpr int panHeight = 4 + 34;
constexpr int panBesideWidth = 30;                    // the pan knob's column beside the fader, when compact
constexpr int footer    = 3 + 16 + 22;                // gap, dB readout, buttons
constexpr int faderInset = 4;                         // above and below the fader itself

constexpr int faderComfortable = 120;   // sections go before the fader gets shorter than this
constexpr int faderMinimum      = 48;   // the fader never gets shorter than this; the mixer scrolls

/** Send knobs per row; more returns than this wrap onto more rows. */
constexpr int sendsPerRow = 3;

constexpr int sendRowsFor (int numReturns)   { return numReturns > 0 ? (numReturns + sendsPerRow - 1) / sendsPerRow : 0; }
constexpr int sendsHeight (int sendRows)     { return sendRows > 0 ? 4 + labelH + sendRows * sendRowH : 0; }

/** Everything a strip keeps, with the fader at length `fader`. */
constexpr int essentials (int fader, bool compact = false)
{
    return 2 * pad + (compact ? compactHeader : header + panHeight) + footer + fader + 2 * faderInset;
}

struct Plan
{
    bool io = true, inserts = true, eq = true, sends = true;
    bool compact = false;   // pan beside the fader, no row for a group tag
    int sendRows = 0;
    int height = 0;   // the height strips get: what was asked for, or more when the mixer scrolls

    /** Height of the shown sections other than the essentials. */
    int sectionsHeight() const
    {
        return (io ? ioHeight : 0) + (inserts ? insertsHeight : 0) + (eq ? eqHeight : 0)
             + (sends ? sendsHeight (sendRows) : 0);
    }
    int faderHeight() const   { return height - essentials (0, compact) - sectionsHeight(); }
    bool scrolls (int available) const   { return height > available; }
};

/** Which sections fit in a strip `available` pixels tall, with `numReturns` return tracks. */
inline Plan plan (int available, int numReturns)
{
    Plan p;
    p.sendRows = sendRowsFor (numReturns);
    auto fits = [&] { return essentials (faderComfortable, p.compact) + p.sectionsHeight() <= available; };

    // Least needed first.
    if (! fits()) p.inserts = false;
    if (! fits()) p.eq = false;
    if (! fits()) p.io = false;
    if (! fits()) p.sends = false;
    if (! fits()) p.compact = true;

    p.height = std::max (available, essentials (faderMinimum, p.compact) + p.sectionsHeight());
    return p;
}
} // namespace lathe::striplayout
