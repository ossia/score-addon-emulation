# Dependency and asset manifest

## Project licensing policy

The project owner accepts ossia score configurations under GPL or AGPL. AGPL-covered engines, including VES and Monomodule, are therefore not excluded merely for being AGPL. Each selected configuration still needs exact component/version compatibility, corresponding-source and notice compliance, applicable network-use obligations, and a separate audit of firmware/assets. The selected seven-family configuration below is distributable under GPLv3: OpenWurli's source headers grant GPLv3 without an explicit later-version option. This policy does not relabel third-party source licenses.

## Third-party sources

Every third-party source is a Git submodule of a fork in the `jcelerier` GitHub account, checked out on its `ossia-2026-11` branch (`branch = ossia-2026-11` in `.gitmodules`). That branch starts at the exact upstream revision below; local changes, when there are any, are separate commits on top of it, written so they can be proposed upstream. `git submodule update --init --recursive` fetches them all; no submodule has nested submodules. Only the files listed in `cmake/*.cmake` are built, and `cmake/Release.cmake` packages only those files and their license texts.

| Submodule | Upstream | Fork, branch `ossia-2026-11` | Upstream pin | Commits on the branch |
|---|---|---|---|---|
| `3rdparty/libADLMIDI` | [Wohlstand/libADLMIDI](https://github.com/Wohlstand/libADLMIDI) | [jcelerier/libADLMIDI](https://github.com/jcelerier/libADLMIDI/tree/ossia-2026-11) | `44f38c9bb2aabc1c09e9c6060ce0dbdc2fe40bde` | none |
| `3rdparty/emu2149` | [digital-sound-antiques/emu2149](https://github.com/digital-sound-antiques/emu2149) | [jcelerier/emu2149](https://github.com/jcelerier/emu2149/tree/ossia-2026-11) | `441f3dc295a9b82db16921461f0678b483db8304` | none |
| `3rdparty/Nuked-PSG` | [nukeykt/Nuked-PSG](https://github.com/nukeykt/Nuked-PSG) | [jcelerier/Nuked-PSG](https://github.com/jcelerier/Nuked-PSG/tree/ossia-2026-11) | `d15a168c676f4669e23660be9225b34ad7c1764e` | none |
| `3rdparty/libresidfp` | [libsidplayfp/libresidfp](https://github.com/libsidplayfp/libresidfp) | [jcelerier/libresidfp](https://github.com/jcelerier/libresidfp/tree/ossia-2026-11) | `fa9dd25d16863493b36b6459128d6583fb3d179f` | none |
| `3rdparty/munt` | [munt/munt](https://github.com/munt/munt) (`mt32emu/`, 2.8.3) | [jcelerier/munt](https://github.com/jcelerier/munt/tree/ossia-2026-11) | `6e7c01fba7e1d50c8fa705834889fd0eac136075` | 4: queued part-addressed short messages (`Synth::playMsgOnPart`); part-addressed CC120 starts the native all-sound-off decay; queued coalesced release of one melodic key (`Synth::releaseNoteOnChannel`, `Part::releaseNote`); file markers |
| `3rdparty/retromulator` | [reales/retromulator](https://github.com/reales/retromulator) (`source/openWurliLib/`) | [jcelerier/retromulator](https://github.com/jcelerier/retromulator/tree/ossia-2026-11) | `bb7ffd871c7c227aed0671ba39df8188a7a4697d` | none |
| `3rdparty/apu/game-music-emu` | [libgme/game-music-emu](https://github.com/libgme/game-music-emu) | [jcelerier/game-music-emu](https://github.com/jcelerier/game-music-emu/tree/ossia-2026-11) | `f68963b1de0633149b05732113d3c3113c6f63a1` | 2: `Nes_Apu::active_channels`, `Nes_Apu::dmc_playing` host inspection |
| `3rdparty/apu/gb_apu` | [ITotalJustice/gb_apu](https://github.com/ITotalJustice/gb_apu) | [jcelerier/gb_apu](https://github.com/jcelerier/gb_apu/tree/ossia-2026-11) | `3d73d0df027a82d854cacd72a179c2d6a1a9703e` | 7: `high_pass` without negative shifts; charge-factor bypass compares with fixed-point unity; reset clears the capacitors; NR51/NR50 left/right routing; right volume for right wave transitions; level deltas at the start of the sync; `blip_read_samples` without negative shifts |
| `3rdparty/emu2212` | [digital-sound-antiques/emu2212](https://github.com/digital-sound-antiques/emu2212) | [jcelerier/emu2212](https://github.com/jcelerier/emu2212/tree/ossia-2026-11) | `e1735c83707e6ee739a123476344659f7fa9fb18` | 1: `update_output` halves channel output toward zero, so a stopped channel settles at 0 rather than -1 |
| `3rdparty/libvgm` | [ValleyBell/libvgm](https://github.com/ValleyBell/libvgm) (`emu/cores/c6280_mame.c` only) | [jcelerier/libvgm](https://github.com/jcelerier/libvgm/tree/ossia-2026-11) | `a6eed945ecf4e5a25cf82004779c6f7e4fd5c319` | 3: `c6280_mame.c` builds without the libvgm device framework; no left shift of a negative LFO sample; 32 fractional bits of waveform and LFO phase |

The retromulator checkout is large (about 170 MB of working tree, 36 MB of history) for sixteen headers; its submodule is marked `shallow = true`. The libvgm branch's first commit makes `c6280_mame.c` standalone, which libvgm's own build of that core does not expect; it is not meant for upstream.

### Licenses

Each row below is registered with `score_register_license` where the component is built, and appears in score's About dialog, Licenses tab, under its *About entry*: the dialog shows the URL (the fork branch), the SPDX identifier and the full license text. `Emulation_RegistryTest` reads this table and checks that every row is listed with its identifier and text (rows of an instrument that is not built must be absent). A *file* row embeds that file whole; a *notice* row embeds the source file's first comment block carrying its copyright.

<!-- about-licenses:begin -->
| Component | Instrument | About entry | SPDX | License text | Registered as |
|---|---|---|---|---|---|
| libADLMIDI MIDI engine and API | Opale | `libADLMIDI` | GPL-3.0-or-later | `3rdparty/libADLMIDI/LICENSE.txt` | file |
| libADLMIDI chip interfaces and helpers | Opale | `libADLMIDI_chip_interfaces` | LGPL-2.1-or-later | `3rdparty/libADLMIDI/LICENSE.LGPL-2.1.txt` | file |
| ymfm OPL2/OPL3 core (in libADLMIDI) | Opale | `ymfm` | BSD-3-Clause | `3rdparty/libADLMIDI/src/chips/ymfm/LICENSE` | file |
| WOPL codec (in libADLMIDI) | Opale | `wopl` | MIT | `3rdparty/libADLMIDI/src/wopl/LICENSE.txt` | file |
| OPL volume models (in libADLMIDI) | Opale | `opl_volume_models` | MIT | `3rdparty/libADLMIDI/src/models/LICENSE.txt` | file |
| Fixed-capacity note storage (in libADLMIDI) | Opale | `libADLMIDI_fixed_storage` | BSL-1.0 | `3rdparty/libADLMIDI/src/structures/LICENSE` | file |
| File and allocator helpers (in libADLMIDI) | Opale | `libADLMIDI_file_and_allocator_helpers` | MIT | `3rdparty/libADLMIDI/src/midiseq/LICENSE.txt` | file |
| emu2149 | AYstral | `emu2149` | MIT | `3rdparty/emu2149/LICENSE` | file |
| Nuked-PSG | AYstral | `Nuked_PSG` | GPL-2.0-or-later | `3rdparty/Nuked-PSG/LICENSE` | file |
| libresidfp | SIDérant | `libresidfp` | GPL-2.0-or-later | `3rdparty/libresidfp/COPYING` | file |
| mt32emu (munt) | LAdrenaline | `munt` | LGPL-2.1-or-later | `3rdparty/munt/mt32emu/COPYING.LESSER.txt` | file |
| mt32emu SHA-1 (munt) | LAdrenaline | `munt` | BSD-3-Clause | `3rdparty/munt/mt32emu/src/sha1/sha1.h` | notice |
| OpenWurli DSP (retromulator) | Wurlie | `OpenWurli` | GPL-3.0-only | `3rdparty/retromulator/LICENSE.txt` | file |
| game-music-emu NES APU and Blip_Buffer | Consolated | `game_music_emu_nes_apu` | LGPL-2.1-or-later | `3rdparty/apu/game-music-emu/license.txt` | file |
| gb_apu | Consolated | `gb_apu` | MIT | `3rdparty/apu/gb_apu/LICENCE.txt` | file |
| blip_buf (in gb_apu) | Consolated | `gb_apu` | MIT | `3rdparty/apu/gb_apu/blargg/blip_buf.c` | notice |
| emu2212 | SiCC | `emu2212` | MIT | `3rdparty/emu2212/LICENSE` | file |
| HuC6280 MAME core (libvgm) | SiCC | `HuC6280_MAME` | BSD-3-Clause | `3rdparty/notices/HuC6280_MAME-LICENSE.txt` | file |
<!-- about-licenses:end -->

Further notices registered with the same entries: `munt` also embeds `mt32emu/AUTHORS.txt` and the GPL text `mt32emu/COPYING.txt` that the LGPL refers to; `OpenWurli` embeds `3rdparty/notices/OpenWurli-NOTICE.txt` (source selection, asset audit and adaptation notes); `game_music_emu_nes_apu` embeds the notices of `Nes_Apu.cpp`, `Nes_Oscs.cpp` and `Blip_Buffer.cpp`; `libADLMIDI`, `libADLMIDI_chip_interfaces`, `emu2149`, `Nuked_PSG`, `libresidfp` and `HuC6280_MAME` embed the notice of their main source file. libvgm has no license file: `c6280_mame.c` carries `license:BSD-3-Clause` and `copyright-holders:Charles MacDonald`, and `3rdparty/notices/HuC6280_MAME-LICENSE.txt` is that license's text with this holder.

## Selected configuration

Newly authored add-on code is GPL-3.0-or-later, except `Emulation/Wurli/Engine.cpp`, which adapts upstream GPL-3.0-only code and retains that restriction. OPL uses libADLMIDI's MIDI orchestration and **ymfm OPL2/OPL3 only**. Selecting an LGPL/BSD emulator does not remove the GPL obligations of the MIDI layer.

- Upstream: <https://github.com/Wohlstand/libADLMIDI>
- Pin: `44f38c9bb2aabc1c09e9c6060ce0dbdc2fe40bde`
- Checkout: `3rdparty/libADLMIDI`, submodule of <https://github.com/jcelerier/libADLMIDI>, branch `ossia-2026-11`.
- Local upstream patches: **none**. Upstream's contribution policy is retained; all integration code is outside that checkout.
- Build source allow-list: `cmake/AdlMidi.cmake`. No upstream standalone application, audio-device backend, MIDI-file sequencer, or embedded instrument database is built.

| Selected component | Imported paths under libADLMIDI | License / notice |
|---|---|---|
| MIDI engine and API | `include/`, selected `src/adlmidi*.cpp`, supporting headers | GPL-3.0-or-later; `LICENSE.txt`, source headers |
| Chip interface and helpers | `src/chips/opl_chip_base.*`, `ymfm_opl2.*`, `ymfm_opl3.*`, `common/` | LGPL-2.1-or-later; `LICENSE.LGPL-2.1.txt`, interface copyright headers |
| ymfm core | `src/chips/ymfm/` | BSD-3-Clause; that directory's `LICENSE` |
| WOPL codec | `src/wopl/` | MIT; that directory's `LICENSE.txt` |
| OPL volume models | `src/models/` | MIT; that directory's `LICENSE.txt` |
| Fixed-capacity note storage | `src/structures/` | BSL-1.0; that directory's `LICENSE` |
| File/allocator helpers (not the sequencer) | `src/midiseq/file_reader.hpp`, `src/midiseq/impl/dpmi_alloc.hpp` | MIT; source headers and `src/midiseq/LICENSE.txt` |

`score_register_license` registers these notices with score. The curated source ZIP retains the license texts and source copyright notices. Distributing a linked binary requires providing its corresponding source and preserving the applicable notices; a binary-only package is not the release deliverable.

### PSG

| Dependency | API-resolved implementation pin | Selected source | License |
|---|---|---|---|
| [emu2149](https://github.com/digital-sound-antiques/emu2149) | `441f3dc295a9b82db16921461f0678b483db8304` | `emu2149.c`, `emu2149.h` | MIT; `LICENSE` and source attribution notices |
| [Nuked-PSG](https://github.com/nukeykt/Nuked-PSG) | `d15a168c676f4669e23660be9225b34ad7c1764e` | Current `ympsg.c`, `ympsg.h` only | GPL-2.0-or-later; selected source/header notices and `LICENSE` |

Both are submodules under `3rdparty/` of their `jcelerier` forks, whose `ossia-2026-11` branches are the pins without edits. `cmake/Psg.cmake` selects and registers their notices. Deprecated Sega code, examples and assets are excluded. The adapter supplies MIDI allocation, native register scheduling, rate conversion, and optional software envelopes; the cores are not complete keyboard/console emulators.

### SID

- Upstream: [libresidfp](https://github.com/libsidplayfp/libresidfp), API-resolved pin `fa9dd25d16863493b36b6459128d6583fb3d179f`, submodule `3rdparty/libresidfp` of [jcelerier/libresidfp](https://github.com/jcelerier/libresidfp/tree/ossia-2026-11).
- Selected core/filter/oscillator/envelope/DAC and scalar sinc-resampler files are GPL-2.0-or-later, as specified in their headers; `COPYING` contains GPLv2. This is compatible with the GPL-3.0-or-later integration.
- `cmake/Sid.cmake` contains the build allow-list and license registration. Configuration headers are generated in the build tree; no upstream files are changed. The public wrapper, snapshot serializer, standalone tools/tests, runtime SIMD dispatch and asset collections are not built.
- First-use tables, combined-wave configuration, filter curves and FIR setup run off audio. Upstream 6581 range/capacitance setters mutate a shared singleton and are neither exposed nor called. Per-instance filter curves are available.
- The pinned upstream zero-noise setter contains a negative shift; the adapter does not call it and retains default dither. Post-render global Volume still provides exact digital mute.
- The adapter uses the internal chip API rather than the stable public wrapper. Any pin change requires repeating the configuration, isolation, clock/resampler and source-package checks.

GoatTracker import assessment used [format documentation](https://github.com/leafo/goattracker2/blob/master/readme.txt), [`gsong.c`](https://github.com/leafo/goattracker2/blob/master/src/gsong.c) relocation/loading, and [`gplay.c`](https://github.com/leafo/goattracker2/blob/master/src/gplay.c) macro execution. None of those sources or `.ins` assets are imported or redistributed.

### Roland LA

- Upstream: [munt/mt32emu](https://github.com/munt/munt/tree/6e7c01fba7e1d50c8fa705834889fd0eac136075/mt32emu), revision `6e7c01fba7e1d50c8fa705834889fd0eac136075`, library version 2.8.3.
- LAdrenaline is not built for now: `EMULATION_ENABLE_LA` (CMake, default `OFF`) adds its core, process and tests back. Its code stays in the tree; its preset is in score-user-library.
- `3rdparty/munt` is a submodule of [jcelerier/munt](https://github.com/jcelerier/munt/tree/ossia-2026-11): the pin plus the local changes below, one commit each. `cmake/La.cmake` selects the headless C++ synthesizer and internal resampler from `mt32emu/`; no frontends, fake-ROM test assets or ROM images are built or packaged.
- Library code is LGPL-2.1-or-later; the SHA-1 implementation carries its BSD-3-Clause notice. `AUTHORS.txt`, `COPYING.txt`, `COPYING.LESSER.txt` and source notices are retained and registered with score.
- Content identification, supported control/PCM roles and machine compatibility come from Munt's ROM tables and API. The adapter uses supported native memory read/write interfaces for semantic patch application and panic recovery. At this pin, `dumpSysexBank` repeats a CM rhythm block; the adapter neither uses nor modifies that helper.
- Reverb buffers and ROM/engine preparation are allocated before publication to audio. The adapter bounds MIDI/SysEx queues and keeps writable-memory restoration storage preallocated. Real-ROM allocation/timing and sonic behavior remain unverified without user-provided lawful images.
- Local allocation integration changes in `mt32emu/src/Synth.h`, `Synth.cpp`, `Part.h` and `Part.cpp` add timestamped part-addressed queue messages and coalesced melodic-key release. Internal tag bits 31/30 lie outside MIDI's short-message bits 0–23; existing queue timing and abort/retry handling remain in charge. Part-addressed CC120 invokes native decay without resetting unrelated controllers. Ordinary channel MIDI keeps its hardware interpretation. The narrow extensions preserve layered melodic routing independently of native rhythm; their real-ROM audio regressions remain gated on lawful firmware.

### Wurlitzer 200A

- Upstream: [Retromulator OpenWurli port](https://github.com/reales/retromulator/tree/bb7ffd871c7c227aed0671ba39df8188a7a4697d/source/openWurliLib), revision `bb7ffd871c7c227aed0671ba39df8188a7a4697d`; headers attribute the Rust `openwurli-dsp` 0.9.0 model.
- `3rdparty/retromulator` is a submodule of [jcelerier/retromulator](https://github.com/jcelerier/retromulator/tree/ossia-2026-11), the pin without edits. The build includes only the sixteen `ow*.h` model headers of `source/openWurliLib`; the root `LICENSE.txt` (GPLv3) and `3rdparty/notices/OpenWurli-NOTICE.txt` (selection and asset notice) are registered. No JUCE, synthLib device, UI, other Retromulator engines or assets are built or packaged.
- The headers specify GPLv3 without an explicit “or later” grant: treat them as GPL-3.0-only. `Emulation/Wurli/Engine.cpp` separately identifies its adaptation of the pinned `device.cpp` circuit chain under the same terms. `cmake/Wurli.cmake` registers the notices and disables fast-math for the model.
- Pickup tables are generated electrostatic capacitance curves; reed tables are model coefficients; correction-network weights are GPL source coefficients evaluated per note. None is recorded audio, a ROM image, or a manufacturer patch bank. No external asset file is required.
- Score owns MIDI lifetimes, 64-voice allocation/stealing, channel controls, host-rate initialization and output gain. Software velocity/correction/speaker controls are documented as model controls, not invented switches on the physical instrument.

### Chip Wavetable

| Selected source | Exact pin | License / local changes |
|---|---|---|
| [digital-sound-antiques/emu2212](https://github.com/digital-sound-antiques/emu2212) | `e1735c83707e6ee739a123476344659f7fa9fb18` | MIT; `emu2212.c/.h` and LICENSE, one local fix |
| [libvgm HuC6280 MAME core](https://github.com/ValleyBell/libvgm) | `a6eed945ecf4e5a25cf82004779c6f7e4fd5c319` | Charles MacDonald, BSD-3-Clause; patched `c6280_mame.c` and a narrow local adapter |

`3rdparty/emu2212` and `3rdparty/libvgm` are submodules of [jcelerier/emu2212](https://github.com/jcelerier/emu2212/tree/ossia-2026-11) and [jcelerier/libvgm](https://github.com/jcelerier/libvgm/tree/ossia-2026-11). The emu2212 commit halves each channel output with a division instead of an arithmetic right shift, so a stopped channel decays to 0 from below as it does from above instead of holding -1. From libvgm only `emu/cores/c6280_mame.c` is built, through `Emulation/Wavetable/HucCore.c`, which exports a narrow constructor/destructor/register/single-frame API. Its commits remove the libvgm device/framework glue and unaudited helper headers, substitute standard integer typedefs, and replace a negative signed LFO shift with multiplication. No Ootake code is selected. `cmake/Wavetable.cmake` registers both notices.

The local HuC waveform and LFO accumulators use 32 fractional bits in 64-bit counters/increments instead of the upstream 12-bit fraction. This removes an extra software tuning grid at the adapter's clock/16 rendering rate; native 12-bit frequency-divider quantization and the separate noise path remain unchanged.

The permissive HuC source retains upstream limitations: LFO frequency is not hardware-qualified and waveform pointer behavior is not cycle-accurate. Native noise is instance-local and deterministic. The MIDI/resource adapter, resampling, semantic waves and score integration are separately authored; no hardware ROM waves or sample assets were copied.

### Console APU

| Selected source | Exact pin | License |
|---|---|---|
| [libgme/game-music-emu](https://github.com/libgme/game-music-emu) NES subset | `f68963b1de0633149b05732113d3c3113c6f63a1` | LGPL-2.1-or-later |
| [ITotalJustice/gb_apu](https://github.com/ITotalJustice/gb_apu) and its C blip_buf backend | `3d73d0df027a82d854cacd72a179c2d6a1a9703e` | MIT; separate Shay Green blip_buf notice retained |

`3rdparty/apu/game-music-emu` and `3rdparty/apu/gb_apu` are submodules of [jcelerier/game-music-emu](https://github.com/jcelerier/game-music-emu/tree/ossia-2026-11) and [jcelerier/gb_apu](https://github.com/jcelerier/gb_apu/tree/ossia-2026-11); each local change below is one commit there. `3rdparty/apu/sources.json` is the file/blob-level manifest of the upstream files used and the record of those changes. `cmake/Apu.cmake` builds only NES APU/oscillator/Blip_Buffer and GB APU/C blip sources, registering their licenses. It excludes players, CPUs, game/ROM loaders, music, upstream applications and the duplicate C++ GB Blip_Buffer backend.

- Current GME GB code lacks a DMG/CGB selector; the selected GB core has actual model branches and model-specific capacitor filtering, rather than cosmetically relabeling one engine.
- Local NES header accessors inspect active channels and buffered/shifting DMC playback without advancing CPU time. Calling the CPU `read_status(0)` at a host-frame boundary would run backwards to cycle -1; the host does not use that API.
- Local GB patches correct signed high-pass arithmetic, bypass comparison/reset history, NR50/NR51 stereo routing, right-channel wave gain and initial amplitude/envelope/disable delta timestamps. C blip signed-shift UB is removed. Exact changes are the fork commits and are listed in `sources.json`.
- The NES mixer remains upstream's linear mixer. GB's C blip backend adds its fixed high-pass; unsupported upstream bass/treble setters are not exposed. Upstream envelope timer uncertainty is retained as an accuracy limitation, not hidden by a cycle-perfect claim.
- Original default DMC data is a locally authored 257-byte delta-bit triangle pattern. The eight added APU presets embed a second original 257-byte falling-chirp transient, played by **NES PAL Delta woodblock**: LSB-first ±2 DAC steps follow `64 + 22*exp(-t/0.042)*sin(2*pi*(220*t + 880*0.026*(1-exp(-t/0.026))))`, authored at PAL rate 10 (1,662,607/132 bits/s). Neither pattern comes from a recording, firmware or game. User DMC imports embed up to 16 KiB of raw bytes; they are not ROM references or licensed game assets supplied by this project.

## Assets

`factoryBank()` in `Emulation/Opl/Engine.cpp` defines eight original melodic voices and six original drum patches. They are not extracted from a game, manufacturer bank, ROM, or upstream instrument collection. The remaining factory bank slots are explicitly blank. These patches are initialization material, not claims to reproduce a named commercial instrument.

The instruments' native presets are not part of this repository: they are distributed with [score-user-library](https://github.com/ossia/score-user-library) under `Presets/<instrument>`. That set contains 22 OPL, 14 PSG, 13 SID, 8 Wurlitzer, 13 Chip Wavetable and 10 Console APU presets, plus the unchanged ROM-dependent LA preset. The OPL set includes eight newly authored patches and native versions of the 14 original bank voices; its native Short wire hat uses a refined decay, without changing `factoryBank()`. All are authored parameter/wave combinations, not extracted tracker instruments or manufacturer banks. Saved post-render gains are loudness-calibrated for the 80 ROM-free presets; LA is excluded without lawful ROMs. LA's 64 user timbres are also authored initialization material; its ROM-backed factory timbres remain external.

The dependency checkouts include upstream files outside the selected build. Neither its embedded `inst_db*.cpp` databases nor its bank collections are included by `cmake/Release.cmake`. Do not replace this curated source packaging with an indiscriminate recursive copy of `3rdparty/`.

WOPL files supplied by a user remain subject to their own rights. score documents embed imported/edited WOPL data so they can be reopened without the original file; users must have rights to redistribute that data. No manufacturer ROM, sample library, artwork, or font is supplied.

LA requires separately supplied licensed control and PCM ROMs. The add-on records absolute references and content hashes, marks both external dependencies read-only, and never embeds their bytes. score consolidation/Save As must not collect or rewrite them. LA native preset recall preserves the current machine's ROM selection. Do not distribute ROMs with this source archive or assume ownership of a device grants redistribution rights.

## Pin-sensitive integration boundaries

`Emulation/Opl/Engine.cpp` uses the pinned private `MIDIplay` / `OPL3` definitions, compiled with the same feature definitions as the dependency. This is intentional, narrow, and must be requalified when updating the pin:

1. Public `adl_setInstrument` preserves slot pointer identity, while OPL patch caching compares pointers. Patch edits stop active voices and call `clearInstCache()` so subsequent notes use the edited registers.
2. `adl_panic` performs key-off, which does not silence a native release-rate-zero voice. `silenceAll()` supplies attenuation/release writes. At this pin, `adlmidi_opl3.cpp:835` writes the first operator's release register twice; the adapter writes maximum release to all physical operators to stop the remaining carrier. No dependency source is modified.
3. `adl_generateFormat` already advances internal note/modulation timers despite the public header suggesting a separate tick. The adapter does not double-tick.
4. ymfm reports no soft-panning support. That option is disabled in the editor and rejected by preparation; OPL3 native left/centre/right panning remains available through MIDI CC10.
5. WOPL note offsets are 16-bit fields, but this runtime narrows them to signed eight-bit. The codec preserves imported fields; playback rejects sounding offsets outside -128…127 instead of truncating them silently.
6. MIDI master volume uses native chip total-level attenuation. Its minimum is finite attenuation, not digital mute. The add-on's separate post-render Volume provides exact zero; transport panic has independent hard-stop semantics.
7. The ymfm interface queues one register write per native sample in a fixed 2048-entry ring, but upstream only asserts on overflow. `GuardedChip` counts writes and native ticks, refuses an overflowing write, and latches a visible fail-stop error instead of corrupting the ring. It uses the same chip and resampler implementation. Recreate execution to recover.
8. The preparation worker drains the bounded initial register setup before publishing readiness, so the first live note does not wait behind initialization writes. Normal runtime register pacing remains unchanged.

PSG integration also depends on native reset and bus timing: Nuked-PSG initialization deasserts IC without draining its reset pipeline. The adapter clocks that release before accepting the first tone-latch byte, preventing the initial low nibble from being erased. Its bounded coalescing writer avoids the upstream buffered-write queue. AY/YM uses explicit volume-table selection rather than a default-model assumption.

The source lists exclude unselected backends. A missing selected checkout is a configuration error, not a fallback to another instrument. Broader model/backend and platform qualification remains in `../EMULATION_TASK_LEDGER.md`.
