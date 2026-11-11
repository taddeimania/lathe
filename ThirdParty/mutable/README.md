# Vendored DSP: Émilie Gillet's eurorack code

The sound engines of four Lathe devices come from Émilie Gillet's open-source
module firmware:

| Lathe device | Folder |
|---|---|
| Multi Synth | `plaits/` |
| Resonator | `rings/` |
| Grains | `beads/` |
| Random Notes | `marbles/` |

`stmlib/` is the shared support library they all use.

## Source

- https://github.com/pichenettes/eurorack at commit `f4cfa769` (DSP folders)
- https://github.com/pichenettes/stmlib at commit `d18def81`

Only the DSP is here: no firmware, drivers, UI, calibration, bootloader, test
programs or hardware files. Lathe's own wrappers are `Source/Devices/Mutable.*`.

## Licence

Everything in this folder is under the MIT licence, which is in the header of
each file (and in `LICENSE-stmlib.txt` for stmlib). It is compatible with
Lathe's AGPL. Keep those headers in place. The parts of the original
repositories under other licences (the AVR firmware and test programs under the
GPL, the hardware under CC-BY-SA) are not included.

The original product and module names are trademarks of their owner. Lathe
doesn't use them for its devices.

## Changes from the originals

All marked `Lathe:` in the code where they're more than a line.

- `beads/dsp/delay_line.h`: the 12-bit packed sample formats used GCC
  `__attribute__((packed))` bitfields, which MSVC lays out differently. They now
  use a 3-byte `Packed12x2` struct with the same bit layout.
- `beads/dsp/granular_processor.h`, `beads/dsp/processor.cc`,
  `beads/dsp/trigger_generator.cc`: variable-length arrays (a GCC extension)
  are now fixed arrays of `kMaxProcessBlock` (24, in `beads/dsp/frame.h`), the
  largest block the firmware renders. `trigger_generator.h` includes
  `frame.h` for it.
- `beads/`, `plaits/`, `rings/` `resources.cc`: empty lookup tables get a `NULL`
  entry, since zero-length arrays don't compile on MSVC.
- `plaits/user_data.h`: includes `<cstdio>`.
- `marbles/preset_scales.h` (new): the six preset scales, which the firmware
  keeps in `settings.cc` with the flash-storage code.
- `rings/dsp/performance_state.h`, `rings/dsp/part.h`, `rings/dsp/part.cc`: a
  `strum_level` (0 to 1) sets how hard the internal exciter strikes and plucks,
  so velocity changes loudness. At 1 it behaves exactly as before.

The code is built with `TEST` defined (in `CMakeLists.txt`), which picks its
portable C++ paths instead of the ARM assembly.
