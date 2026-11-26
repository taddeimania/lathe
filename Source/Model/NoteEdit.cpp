#include "NoteEdit.h"

#include <map>

namespace lathe::noteedit
{
namespace
{
    template <typename Fn>
    void forTargets (Notes& notes, const Selection& sel, Fn&& fn)
    {
        for (int i : sel)
            if (i >= 0 && i < (int) notes.size())
                fn (notes[(size_t) i]);
    }

    Selection all (const Notes& notes)
    {
        Selection s;
        for (int i = 0; i < (int) notes.size(); ++i)
            s.insert (i);
        return s;
    }

    Selection valid (const Notes& notes, const Selection& sel)
    {
        Selection s;
        for (int i : sel)
            if (i >= 0 && i < (int) notes.size())
                s.insert (i);
        return s;
    }
}

std::pair<double, double> span (const Notes& notes, const Selection& selection)
{
    const auto sel = selection.empty() ? all (notes) : valid (notes, selection);
    if (sel.empty())
        return { 0.0, 0.0 };
    double lo = 1.0e12, hi = -1.0e12;
    for (int i : sel)
    {
        lo = jmin (lo, notes[(size_t) i].start);
        hi = jmax (hi, notes[(size_t) i].start + notes[(size_t) i].length);
    }
    return { lo, hi };
}

Notes copy (const Notes& notes, const Selection& selection)
{
    const auto sel = valid (notes, selection);
    if (sel.empty())
        return {};
    const double from = span (notes, sel).first;
    Notes out;
    for (int i : sel)
    {
        auto n = notes[(size_t) i];
        n.start -= from;
        out.push_back (n);
    }
    return out;
}

Selection paste (Notes& notes, const Notes& clipboard, double atBeat, double clipLength)
{
    Selection added;
    for (auto n : clipboard)
    {
        n.start += jmax (0.0, atBeat);
        if (n.start >= clipLength - 1.0e-9)
            continue;
        notes.push_back (n);
        added.insert ((int) notes.size() - 1);
    }
    return added;
}

Selection duplicate (Notes& notes, const Selection& selection, double gridStep, double clipLength)
{
    const auto sel = valid (notes, selection);
    if (sel.empty())
        return {};
    const auto [lo, hi] = span (notes, sel);
    double offset = hi - lo;
    if (gridStep > 0.0)
        offset = jmax (gridStep, std::ceil (offset / gridStep - 1.0e-6) * gridStep);
    return paste (notes, copy (notes, sel), lo + offset, clipLength);
}

void remove (Notes& notes, const Selection& selection)
{
    const auto sel = valid (notes, selection);
    for (auto it = sel.rbegin(); it != sel.rend(); ++it)
        notes.erase (notes.begin() + *it);
}

void nudge (Notes& notes, const Selection& selection, double beats)
{
    const auto sel = valid (notes, selection);
    if (sel.empty())
        return;
    // Keep the group together: stop at beat 0 rather than squashing the first notes there.
    const double earliest = span (notes, sel).first;
    const double delta = jmax (-earliest, beats);
    forTargets (notes, sel, [&] (Note& n) { n.start += delta; });
}

void lengthen (Notes& notes, const Selection& sel, double beats, double minLength)
{
    forTargets (notes, sel, [&] (Note& n) { n.length = jmax (minLength, n.length + beats); });
}

void moveStart (Note& n, double newStart, double minLength)
{
    const double end = n.start + n.length;
    n.start = jlimit (0.0, jmax (0.0, end - minLength), newStart);
    n.length = end - n.start;
}

void transpose (Notes& notes, const Selection& sel, int semitones)
{
    forTargets (notes, sel, [&] (Note& n) { n.pitch = jlimit (0, 127, n.pitch + semitones); });
}

void offsetVelocity (Notes& notes, const Notes& original, const Selection& targets, float amount)
{
    for (int i : targets)
        if (i >= 0 && i < (int) notes.size() && i < (int) original.size())
            notes[(size_t) i].velocity = jlimit (0.02f, 1.0f, original[(size_t) i].velocity + amount);
}

int velocityLine (Notes& notes, const Selection& targets, double b1, float v1, double b2, float v2)
{
    if (b2 < b1)
    {
        std::swap (b1, b2);
        std::swap (v1, v2);
    }
    int changed = 0;
    for (int i : targets)
    {
        if (i < 0 || i >= (int) notes.size())
            continue;
        auto& n = notes[(size_t) i];
        if (n.start < b1 - 1.0e-9 || n.start > b2 + 1.0e-9)
            continue;
        const double t = b2 - b1 > 1.0e-9 ? (n.start - b1) / (b2 - b1) : 1.0;
        n.velocity = jlimit (0.02f, 1.0f, (float) (v1 + (v2 - v1) * t));
        ++changed;
    }
    return changed;
}

void legato (Notes& notes, const Selection& selection)
{
    const auto sel = valid (notes, selection);
    std::set<double> starts;
    for (int i : sel)
        starts.insert (notes[(size_t) i].start);
    for (int i : sel)
    {
        auto& n = notes[(size_t) i];
        auto next = starts.upper_bound (n.start + 1.0e-9);
        if (next != starts.end())
            n.length = *next - n.start;
    }
}

Selection splitAtGrid (Notes& notes, const Selection& selection, double gridStep)
{
    Selection out;
    if (gridStep <= 0.0)
        return valid (notes, selection);

    Notes pieces;
    const auto sel = valid (notes, selection);
    for (int i : sel)
    {
        auto& n = notes[(size_t) i];
        const double end = n.start + n.length;
        double cut = (std::floor (n.start / gridStep + 1.0e-6) + 1.0) * gridStep;
        double from = n.start;
        bool first = true;
        while (cut < end - 1.0e-6)
        {
            if (first)
                n.length = cut - n.start;
            else
                pieces.push_back ({ n.pitch, from, cut - from, n.velocity });
            first = false;
            from = cut;
            cut += gridStep;
        }
        if (! first)
            pieces.push_back ({ n.pitch, from, end - from, n.velocity });
        out.insert (i);
    }
    for (auto& p : pieces)
    {
        notes.push_back (p);
        out.insert ((int) notes.size() - 1);
    }
    return out;
}

Selection glue (Notes& notes, const Selection& selection)
{
    const auto sel = valid (notes, selection);
    std::map<int, std::vector<int>> byPitch;
    for (int i : sel)
        byPitch[notes[(size_t) i].pitch].push_back (i);

    Selection gone;
    for (auto& [pitch, list] : byPitch)
    {
        if (list.size() < 2)
            continue;
        int first = list.front();
        double end = 0.0;
        for (int i : list)
        {
            if (notes[(size_t) i].start < notes[(size_t) first].start)
                first = i;
            end = jmax (end, notes[(size_t) i].start + notes[(size_t) i].length);
        }
        notes[(size_t) first].length = end - notes[(size_t) first].start;
        for (int i : list)
            if (i != first)
                gone.insert (i);
    }

    // What's left of the selection, with indices shifted past the removed notes.
    Selection out;
    for (int i : sel)
        if (! gone.count (i))
            out.insert (i - (int) std::distance (gone.begin(), gone.lower_bound (i)));
    remove (notes, gone);
    return out;
}

void reverse (Notes& notes, const Selection& selection)
{
    const auto sel = valid (notes, selection);
    if (sel.empty())
        return;
    const auto [lo, hi] = span (notes, sel);
    forTargets (notes, sel, [&] (Note& n) { n.start = lo + hi - (n.start + n.length); });
}

void invert (Notes& notes, const Selection& selection)
{
    const auto sel = valid (notes, selection);
    if (sel.empty())
        return;
    int lo = 127, hi = 0;
    forTargets (notes, sel, [&] (Note& n) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); });
    forTargets (notes, sel, [&] (Note& n) { n.pitch = jlimit (0, 127, lo + hi - n.pitch); });
}

double quantizeTarget (double beat, const QuantizeOptions& o)
{
    const double step = o.gridStep;
    if (step <= 0.0)
        return beat;
    // Odd grid lines (the off-beats) sit later by up to a third of a step: at full swing a pair
    // of steps plays as a triplet's long and short.
    const double late = jlimit (0.0f, 1.0f, o.swing) * step / 3.0;
    const auto base = (long long) std::floor (beat / step);
    double best = beat, bestDistance = 1.0e12;
    for (long long k = base - 1; k <= base + 2; ++k)
    {
        const double at = (double) k * step + ((k & 1) != 0 ? late : 0.0);
        if (const double d = std::abs (at - beat); d < bestDistance)
        {
            best = at;
            bestDistance = d;
        }
    }
    return best;
}

void quantize (Notes& notes, const Selection& selection, const QuantizeOptions& o)
{
    const auto sel = selection.empty() ? all (notes) : valid (notes, selection);
    const double s = jlimit (0.0f, 1.0f, o.strength);
    for (int i : sel)
    {
        auto& n = notes[(size_t) i];
        const double qStart = jmax (0.0, quantizeTarget (n.start, o));
        const double newStart = n.start + (qStart - n.start) * s;
        if (o.ends)
        {
            const double qLength = jmax (o.gridStep, quantizeTarget (n.start + n.length, o) - qStart);
            n.length = jmax (0.01, n.length + (qLength - n.length) * s);
        }
        n.start = jmax (0.0, newStart);
    }
}

Selection notesOfPitch (const Notes& notes, int pitch)
{
    Selection s;
    for (int i = 0; i < (int) notes.size(); ++i)
        if (notes[(size_t) i].pitch == pitch)
            s.insert (i);
    return s;
}
} // namespace lathe::noteedit
