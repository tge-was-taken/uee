# uee

A minimal PS2 runtime: it statically recompiles a PS2 game's code to C++, links
it against a high level emulation of the console's kernel, and runs the result
as a native program.

**This is an experiment, not a product.** Almost all of the code was written by
Claude (Claude Code) under my direction, as a long running exercise in how far
an AI assisted project can get on a problem where correctness is checked
against real hardware. It is not actively developed, there is no roadmap, no
support and no release. Nothing here is a finished thing you can play games
on.

It is also not a general purpose emulator, and it is not trying to become one.
The goal was a runtime small enough to read, aimed at making one game run on
other platforms, with hardware accurate behaviour only where that game needs it.

## How it works

Guest code is translated ahead of the host build. `translate_elf` walks a PS2
ELF, discovers functions, and emits C++ for each one; the result compiles into
the host binary. An interpreter covers everything translation cannot answer
statically, so an indirect jump to undiscovered code still runs, just slowly.
VU1 microprograms are translated the same way, from a manifest captured during a
real game run.

The kernel is HLE. There is no BIOS image: syscalls, threads, semaphores, DMA
handling and the IOP side are reimplemented, so a game boots from its own ELF.

- 103 of the 131 EE syscall slots are implemented.
- 130 IOP service entries across 15 modules (`thbase`, `thsemap`, `thevent`,
  `intrman`, `sysmem`, `sysclib`, `sifcmd`, `sifman`, `loadcore`, `libsd`,
  `timrman`, `dmacman`, `secrman`, `vblank`, `stdio`).
- The IOP also runs real code: an R3000A interpreter executes the IRX modules
  shipped on the game's own disc, so the parts that are not HLE are the
  console's own.

Behaviour was derived by reverse engineering retail firmware and modules, not
from documentation or by copying an emulator: annotated disassembly of the EE
kernel and EELOAD, of the IOP's THREADMAN and SYSCLIB, and PCSX2 save states
used as a source of known good IOP RAM. Where a behaviour is reimplemented from
a specific binary, the comment cites the module, hash, address and symbol.

Devices implemented: EE core (R5900), FPU, VU0 macro and micro, VU1, DMAC, GIF,
VIF0/VIF1, INTC, timers, SIF (including RPC answered on the IOP's own threads),
SPU2 (ADPCM, envelopes, reverb registers), CDVD backed by an ISO or a folder,
and a memory card backed by a card image.

Roughly 32,000 lines of C++ under `src`, 5,300 in `tests`.

## What does not work

- **There is no GS rasterizer.** The GS here is register state, scanline timing
  and a GIF capture path. Pixels come from outside: PCSX2's GS library through
  the `pcsx2-gs` frontend, or a game specific native renderer. If you want
  something that draws a PS2 frame on its own, this is not it.
- **One retail game is driven end to end.** Persona 3 FES (SLUS-216.21) boots,
  renders, saves and plays. Everything else is untested, and "untested" here
  means "will probably not boot".
- Movie playback is HLE at `sceMpegGetPicture`, with a patch standing in for the
  SFD state the game expects.
- No save states, no netplay, no configuration UI, no input remapping beyond a
  first gamepad.
- The `pcsx2-gs` frontend currently aborts on this machine, because the only GS
  hook library available here is a Devel build and that path needs Release.
- 34 of the hardware autotest programs fail; see below.

## Performance

PS2 NTSC output is 59.94 fields per second, so 60 is full speed.

Measured 2026-09-13 on a Windows host (Ryzen desktop, RTX 4070 SUPER), fast VU
mode, in game (the dorm lounge, a scene with real VU load), windowed:

| renderer | fields/s |
|---|---|
| PCSX2 software GS | 112 |
| PCSX2 D3D11 | 146 |
| PCSX2 D3D12 | 147 |

The same build presented through WSLg managed 70, which is why there is a
Windows cross build at all. Before VU microprograms were translated rather than
interpreted, the same scene ran at 13 fields/s.

Translation cost, measured on a homebrew pinball game: 6.79 host instructions
per guest instruction with the ELF's symbols, 7.22 with function discovery
alone, and the interpreter down to 0.1% of the profile. The figure is quoted
against a real game on purpose: synthetic benchmarks hid a 6.2x regression,
because the thing they did not exercise was C++ virtual calls.

The Linux and Windows builds were checked to produce byte identical GIF output
over 5,039 fields (3.47 GB of capture).

## Tests

```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

279 of 347 pass as of 2026-10-16. The 68 failures are 34 programs, each run
twice, once interpreted and once translated; interpreter and translated results
agree everywhere, which is the property those pairs exist to check.

Three kinds of test carry the weight:

- **ps2autotests**, behavioural tests with output recorded on real hardware.
  Authoritative, and the reason the EE and VU work at all.
- **Vector tests**, state captured from PCSX2's interpreter for VU1, VIF and
  SPU2, replayed and compared.
- **Guest tests**, small PS2 programs built with the ps2dev toolchain, each run
  both interpreted and translated.

Of the 34 failing programs, 14 are the `gs.*` drawing family, which cannot pass
without a rasterizer. Most of the rest are artifacts of how the recordings were
made rather than runtime defects: free running registers (`Random`, `Count`,
`ErrorEPC`), a constant heap offset because the console ran a USB flavoured
build of each test, and a GIF path left busy by the tests themselves. A handful
are real gaps, each understood and none small: no write back EE data cache, no
EE store buffer, and transfers that take no device visible time.

## Building

C++23, CMake 3.25, GCC 15 or Clang 20 and up. Dependencies are fetched
(spdlog, toml++, minicoro, xxHash). The guest tests additionally need the ps2dev
toolchain (`EE_GCC`, `PS2SDK`); without it they are skipped with a message.

Frontends live under `frontends/`. The `p3fes` one needs `SKY2HOOK_DIR`
pointing at the renderer port it hooks into; configure without it and that
target silently does not exist.

No game data, firmware, or disc image is included, and none is obtainable from
this repository. You need your own dump of a disc you own.

## Credit

Credit for what is understood about this console belongs to the PS2 emulation
community, and above all to PCSX2 and Play!. Decades of their work is what
makes a project like this possible at all.

No code was copied from them. That is a weaker claim than it sounds, and it is
worth stating plainly: this was written by a model trained on the open internet,
which includes their source, their commit history and the discussions around
them. Where something here is right about a hardware quirk, the reasonable
assumption is that they are the ones who worked it out, and that the knowledge
reached this repository through the training data rather than through
independent discovery. Nothing in it should be read as a claim of novelty.

The test material is theirs too. The behavioural tests come from ps2autotests,
the vector tests are state captured from PCSX2's interpreter, the guest tests
are built with the ps2dev toolchain, and the `pcsx2-gs` frontend draws by
loading PCSX2's own GS.
