# Performance: cores, buffer size and the CPU meter

## Processing cores

Lathe plays tracks and their plug-ins on several cores at once. While one core works on
your drum bus, another is running a synth and a third a vocal chain, so a session that
would overload one core fits comfortably. The result sounds exactly the same as playing
one track at a time, down to the last bit.

**Options > Audio / MIDI Settings > Processing cores** chooses how many:

- **Auto** (the default): one per performance core. On Apple silicon that's the
  performance cores only, because the efficiency cores would set the pace.
- **1**: one track at a time. Try it if a plug-in behaves oddly, then tell the plug-in's
  maker.
- **A number**: any number up to your computer's core count.

Small sessions stay on one core: sharing out a block only pays when there's real work in it,
and Lathe decides that every block.

Tracks that take another track as their input (a synth printed to an audio track, say) wait
for that track, so long chains can only use as many cores as there are independent tracks.

## Buffer size

The buffer size (Options > Audio / MIDI Settings) trades latency for headroom. At 128
samples (about 2.7 ms at 48 kHz) every block has to be ready in 2.7 ms; at 512 samples you
have 10.7 ms but hear yourself later. Every plug-in has a little fixed cost per block, so the
same session shows a higher CPU percentage at small buffers. If you're mixing rather than
recording, a bigger buffer gives you more room.

## The CPU meter

The **CPU** number at the top is how much of the time between two blocks Lathe needed to
make one: the share of the audio deadline. Over 100% means the block was late, which you
hear as a click or dropout. With several cores it's still the share of the deadline (the time
until the last core finished), so it goes down as cores are added.

Other DAWs measure differently, so the numbers aren't directly comparable. To compare two
programs fairly, use the same buffer size and sample rate in both, and compare their CPU in
Activity Monitor (Mac) or Task Manager (Windows).

## Benchmark

To see what multiple cores do on your computer, run Lathe from a terminal with
`--benchmark`:

```sh
/Applications/Lathe.app/Contents/MacOS/Lathe --benchmark ~/Desktop/lathe-benchmark.csv
```

(Windows: `Lathe.exe --benchmark lathe-benchmark.csv`; Linux: `./Lathe --benchmark`.)

It builds made-up sessions full of heavy stand-in plug-ins (1 to 12 synths, a band with
returns, synths printed to audio tracks), plays each on one core and then on all of them at
buffer sizes of 128, 256 and 512, and times a 32-bar export. It doesn't open your audio
device or load your plug-ins, and it can run while Lathe is open. It takes a couple of
minutes. Each line shows the share of the deadline used, on average and at the 99th
percentile (the slowest 1% of blocks, which is what causes dropouts).
