# QLVGM

QLVGM converts a YM2203 VGM file into a bootable raw QLAY Microdrive image for
the Sinclair QL and QSound2. The image contains a QLZ1-compressed frame stream,
a position-independent MC68008 player, and a SuperBASIC BOOT program. An
optional raw mode-4 or mode-8 screen dump can remain visible during playback.

The first release deliberately targets the common Furnace-style fixtures
rather than every command in the VGM specification. It accepts uncompressed
VGM 1.50 through 1.71 files using:

- one YM2203 clocked at either 3,993,600 Hz or QSound2's native 2 MHz;
- YM2203 writes (0x55);
- 16-bit, 60 Hz, 50 Hz, and short waits (0x61-0x63, 0x70-0x7F);
- the end command (0x66); and
- an optional command-aligned VGM loop.

VGZ input, other chips and command types, dual-chip files, and clock-dependent
YM2203 timer/prescaler programming are rejected with an offset or register
diagnostic.

VGZ files can be decomressed to VGM files using gzip, as `file.vgz` is just
`file.vgm.gz`.

## Build

QLVGM follows the adjacent [QLASM](https://github.com/josef-jelinek/qlasm)
project: production is a GNU11 unity translation unit and uses only the host C
runtime.

~~~sh
cc -std=gnu11 -Wall -Wextra -o build build.c
./build
./build test
~~~

`./build` and `./build debug` create a hosted `./qlvgm` with debug information.
`./build release` creates an optimized, stripped executable linked with the
host C runtime. `./build test` builds and runs the hosted test suite.

QLASM is a runtime tool rather than a linked dependency. Its default is the
adjacent checkout:

~~~text
../qlasm/qlasm
~~~

The default is resolved relative to the `qlvgm` executable, not the current
directory. Set `QLASM` to use another executable. `QLVGM_PLAYER` can override
the target source that otherwise resolves as `player.asm` beside the `qlvgm`
executable. QLAY Microdrive output is built into QLVGM.

## Usage

~~~text
qlvgm [OPTIONS] INPUT.vgm OUTPUT.mdv

--rate 50|60
--no-pitch-conversion
--screen FILE
--screen-mode 4|8
--sectors N
--medium-name NAME
--random-id N
--force
--verbose
--help
--version
~~~

For example:

~~~sh
./qlvgm vgms/CastleSeeYouBackHere.vgm castle.mdv
./qlvgm --rate 60 --medium-name castle --random-id 1 \
    vgms/CastleSeeYouBackHere.vgm castle_ntsc.mdv
./qlvgm --no-pitch-conversion \
    vgms/CastleSeeYouBackHere.vgm castle_raw_pitch.mdv
./qlvgm --screen vgms/xenon_scr vgms/Xenon.vgm xenon.mdv
./qlvgm --screen SCREEN --screen-mode 4 INPUT.vgm OUTPUT.mdv
~~~

The default is a 255-sector, 50 Hz image. QLVGM derives the medium name from the
output path and generates the random ID unless those values are supplied.
Existing output is rejected unless `--force` is present. Fixed random IDs make
otherwise identical images reproducible; cartridge files use a stable UTC
timestamp.

`--screen` accepts exactly 32,768 bytes of raw QL screen memory. The default is
mode 8; use `--screen-mode 4` for a mode-4 dump. Supplying `--screen-mode`
without `--screen` is an error. The screen is stored unchanged as `screen`;
combinations that do not fit the selected cartridge geometry are rejected. By
default, successful conversion prints only a concise creation message.
`--verbose` adds frame, compression, loaded-player, optional-screen, and
target-memory statistics and forwards verbose mode to QLASM.

The cartridge normally contains two ordinary type-0 files:

~~~text
BOOT
qlvgm
~~~

BOOT reserves the loaded image plus its decoded stream, loads `mdv1_qlvgm`,
and calls it. With `--screen`, the cartridge also contains `screen`; BOOT first
selects the requested mode and loads it at the standard QL display address
`$20000`. Mount the image as MDV1 and start Microdrive boot (normally F1).

## Playback model

The 3,993,600 Hz source profile is adapted to QSound2's fixed 2 MHz master
clock. QLVGM scales:

- PSG tone, noise, and envelope periods; and
- the normal and channel-3-special FM block/F-number pairs.

The converter selects the nearest representable target pitch and reports any
clipping. Mixer, volume, patch, key, mode, and envelope-rate bytes retain their
source values. Envelope timing can therefore differ from the source chip even
when pitch is preserved.

VGM paired-chip volume metadata is a software-player mixing hint and is ignored.
The output retains QSound2's fixed hardware FM/PSG balance and adds no
emulator-specific mixer register writes.

`--no-pitch-conversion` provides a diagnostic raw-write path. It preserves
every accepted YM2203 register/value write and its order, without scaling,
paired-register synthesis, duplicate suppression, or clipping warnings. Frame
batching and playback timing are unchanged. A 3,993,600 Hz source should play
approximately one octave lower on QSound2's 2 MHz clock while retaining its
musical relationships. Timer and prescaler programming remains unsupported
when the source and target clocks differ.

Sub-frame writes remain ordered but are applied together on the selected QL
frame. A 50 Hz image uses 882 VGM samples per frame; a 60 Hz image uses 735.
The selected rate must match the emulator's PAL or NTSC timing. Loop points
start a new frame record and repeat indefinitely. If the first loop iteration
depends on register latches left by the intro, QLVGM emits that transition once
before the steady-state loop. A non-looping stream silences the PSG and FM key
states at its end.

The complete frame stream is compressed into one QLZ1 stream on the host and
decompressed contiguously before playback. QLZ1 uses 32-bit compressed and
decoded sizes, a 65,535-byte history window, and byte-oriented literal, short
match, long match, and match-continuation packets. This keeps the cartridge
small without bit-at-a-time decoding or playback-time decompression gaps.

QLZ1 starts with the ASCII magic `QLZ1`, followed by the decoded and payload
sizes as big-endian 32-bit integers. Each payload control byte uses its upper
two bits as the packet type. Literal packets encode 1-64 following bytes;
short and long matches encode 3-66 bytes with an 8-bit-plus-one or big-endian
16-bit distance; continuation packets extend the immediately preceding match.
The 16-bit distance must be nonzero, matches may overlap their output, and a
literal packet clears continuation state. Decoding ends at the declared
decoded size and must consume the exact payload. Truncated packets, invalid
distances, output overruns, and trailing payload are malformed.

The largest supplied fixtures need expanded memory while the compressed file
and decoded stream coexist. Configure [QLRun](https://github.com/josef-jelinek/QLRun)
for QSound2 and 640 KB RAM. Base-memory QLs are not supported by this version.

## Verification

Build and run the hosted tests:

~~~sh
./build test
~~~

The suite generates its VGM data, checks strict parsing, converted and raw
register streams, independently validates and decodes QLZ1 streams, and checks
the built-in QLAY writer. A complete image can optionally be checked with
[QLMDV](https://github.com/josef-jelinek/qlmdv):

~~~sh
../qlmdv/qlmdv list --verbose castle.mdv
../qlmdv/qlmdv inspect castle.mdv
~~~

The files under `vgms/` are manual integration inputs. They are not embedded
into `qlvgm`.

## Repository layout

| Path | Contents |
| --- | --- |
| `build.c` | Debug/release/test build helper |
| `qlvgm.c` | Production unity translation unit |
| `src/runtime.c` | Buffers, file I/O, subprocesses, and temporary paths |
| `src/vgm.c` | Strict VGM parser, frame batching, and 2 MHz pitch conversion |
| `src/qlz.c` | Deterministic QLZ1 compressor and validating decoder |
| `src/qlay.c` | Streaming QLAY Microdrive image writer |
| `src/output.c` | Generated assembly, BOOT, and QLASM orchestration |
| `src/cli.c` | Command-line validation and conversion pipeline |
| `player.asm` | Position-independent QDOS/QSound2 player and QLZ1 decoder |
| `test.c` | Generated-fixture, compression, and QLAY tests |

License terms are in `COPYRIGHT`.
