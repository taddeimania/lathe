#pragma once

#include "../Common.h"

//==============================================================================
// Compressed cloud storage for audio. A take is identified everywhere by the SHA-256
// of its WAV file; in the bucket it can be stored "packed": the WAV's own header bytes
// plus its samples as FLAC (lossless). Unpacking writes the identical WAV back, byte
// for byte, which the download checks against the hash. Anything that isn't a plain
// 16- or 24-bit PCM WAV, or wouldn't get smaller, is stored as it is.
//
// Packed layout (little-endian):
//   "LTHFLAC1"                      8 bytes
//   uint32 header length, header    the WAV's bytes before its sample data
//   uint32 trailer length, trailer  the WAV's bytes after it (chunks, pad byte)
//   uint64 sample data length       bytes of PCM in the WAV
//   uint32 channels, bits, sample rate
//   FLAC stream                     to the end of the file
namespace lathe::cloud
{
/** True if the file starts like a packed take. */
bool isPacked (const File&);

/** Writes the packed form of wav to `packed`. False when the file can't be packed or
    wouldn't get at least ~5% smaller (then nothing is written: store the WAV itself).
    progress (0..1) may cancel. */
bool packAudio (const File& wav, const File& packed, const std::function<bool (double)>& progress = {});

/** Rebuilds the original WAV from a packed file. */
Result unpackAudio (const File& packed, const File& wav, const std::function<bool (double)>& progress = {});
} // namespace lathe::cloud
