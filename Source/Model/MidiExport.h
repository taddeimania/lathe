#pragma once

#include "Edit.h"

namespace lathe::midiexport
{
/** One track of the file: MIDI clips (positions in song beats) played on one channel. */
struct Item
{
    String name;
    int channel = 1;
    std::vector<Clip> clips;
};

/** Every MIDI track that has notes, in song order, each as one item. */
std::vector<Item> songItems (const Edit&);

/** Builds a Standard MIDI File (type 1, 960 ticks per quarter note): a first track with the song's
    name, tempo and time signature, then one track per item with its name, notes, controllers and
    pitch bend. Times are measured from `fromBeat`; only what each clip plays (notes starting
    inside it, cut at its end) goes in. */
MidiFile build (const Edit&, const std::vector<Item>&, double fromBeat, const String& title);

/** Writes the file (replacing one that's there). Returns an error, or an empty string. */
String write (const MidiFile&, const File&);

/** How many notes the items' clips play (what build() writes, from the start of the song). */
int countNotes (const std::vector<Item>&);

/** A name that's safe as a file name ("Verse / Chorus" -> "Verse - Chorus"). */
String fileNameFor (const String&);
} // namespace lathe::midiexport
