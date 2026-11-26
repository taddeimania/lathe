#pragma once

#include "Edit.h"
#include <set>

namespace lathe
{
//==============================================================================
// NoteEdit: what the piano roll does to a clip's notes, without the UI, so it can be tested.
// A selection is a set of indices into the clip's notes. Functions that add notes append them
// and return the indices of the new ones (what the piano roll selects next).
//==============================================================================
namespace noteedit
{
    using Notes = std::vector<Note>;
    using Selection = std::set<int>;

    /** The earliest start and the latest end of the selected notes (or of all, if none are). */
    std::pair<double, double> span (const Notes&, const Selection&);

    /** The selected notes, moved so the earliest starts at beat 0: what Copy puts on the clipboard. */
    Notes copy (const Notes&, const Selection&);

    /** Adds the clipboard's notes from a beat. Notes that would start at or after `clipLength`
        are left out. Returns the new notes' indices. */
    Selection paste (Notes&, const Notes& clipboard, double atBeat, double clipLength);

    /** Copies the selected notes to just after themselves: the selection's span, rounded up to
        whole grid steps. Copies past the clip's end are left out. Returns the copies' indices. */
    Selection duplicate (Notes&, const Selection&, double gridStep, double clipLength);

    /** Deletes the selected notes. */
    void remove (Notes&, const Selection&);

    /** Moves the selected notes later (or earlier, negative) together; never before beat 0. */
    void nudge (Notes&, const Selection&, double beats);

    /** Lengthens (or shortens, negative) the selected notes, keeping each at least `minLength`. */
    void lengthen (Notes&, const Selection&, double beats, double minLength);

    /** Moves the note's start, keeping its end where it is and its length at least `minLength`. */
    void moveStart (Note&, double newStart, double minLength);

    /** Transposes the selected notes, each kept within 0-127. */
    void transpose (Notes&, const Selection&, int semitones);

    /** Raises (or lowers) each target's velocity by the same amount from where it was in
        `original`, so louder notes stay louder: a phrase keeps its dynamics. */
    void offsetVelocity (Notes&, const Notes& original, const Selection& targets, float amount);

    /** Sets the velocity of the targets that start between beats b1 and b2 along a straight
        line from v1 to v2: a ramp drawn across the velocity lane. Returns how many changed. */
    int velocityLine (Notes&, const Selection& targets, double b1, float v1, double b2, float v2);

    /** Each selected note runs on to the next later start among the selection (a chord's
        notes together); the last ones keep their length. */
    void legato (Notes&, const Selection&);

    /** Cuts each selected note at the grid lines inside it. Returns every piece's index. */
    Selection splitAtGrid (Notes&, const Selection&, double gridStep);

    /** Joins the selected notes of each pitch into one, from the first start to the last end,
        at the first one's velocity. Returns the joined notes' indices. */
    Selection glue (Notes&, const Selection&);

    /** Plays the selected notes backwards, in the same span. */
    void reverse (Notes&, const Selection&);

    /** Turns the selected notes upside down, the lowest pitch swapping with the highest. */
    void invert (Notes&, const Selection&);

    struct QuantizeOptions
    {
        double gridStep = 0.25;
        float strength = 1.0f;   // 0-1: how far towards the grid
        float swing = 0.0f;      // 0-1: off-beat grid lines late, up to a triplet feel at 1
        bool ends = true;        // move the ends to the grid too, or keep lengths
    };
    /** Where a beat quantizes to: the nearest grid line, with swing. */
    double quantizeTarget (double beat, const QuantizeOptions&);
    /** Quantizes the selected notes, or every note when none are selected. */
    void quantize (Notes&, const Selection&, const QuantizeOptions&);

    /** Every note of a pitch. */
    Selection notesOfPitch (const Notes&, int pitch);
} // namespace noteedit
} // namespace lathe
