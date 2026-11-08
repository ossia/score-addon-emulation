# Emulation instruments for ossia score

A native score add-on with seven instruments: **Opale** (OPL), **AYstral** (PSG), **SIDérant** (SID), **LAdrenaline** (Roland LA), **Wurlie** (Wurlitzer 200A), **SiCC** (SCC/SCC+/HuC6280 wavetable), and **Consolated** (NES/Game Boy APU). Hardware models are choices within each process, not separate plug-ins. FM4 remains skipped. VES/MAME is backend research only, not a generic hardware-panel process; the remaining roadmap stays in the task ledger.

The process library lists all seven instruments under **Synths → Emulation** (`Synths/Emulation`); chip/system names remain in descriptions and search tags. Instrument renaming preserves process UUIDs, serialization keys and factory preset data; allocation-state migrations are described below. Default process titles retain Unicode independently of address-safe scripting names; authored instance names remain unchanged. The host's default-title correction is required to display **SIDérant** without replacing its accent on the canvas.

## Voice allocation

The instruments use the same **Voice allocation** terminology:

- **Mono** selects one musical note with last-note priority. Releasing it returns to the newest still-held or sustained candidate, restoring that note's MIDI channel and velocity. AYstral, SIDérant and SiCC play their available physical oscillators together; native shared resources still apply.
- **Poly** automatically allocates the existing bounded voice/partial pool. Repeated note-ons for the same MIDI channel/key retrigger one musical note rather than consuming another voice.
- **MIDI channel** provides independent monophonic lines on supported input channels. Chip instruments map channels to their physical voices; Opale retains libADLMIDI's dynamic hardware allocation, and LAdrenaline retains authored part receive assignments and layers. This is not an input-channel filter.

Sustain belongs to the note's input channel. A sustained current note keeps priority until that channel's pedal rises; another channel's pedal or same-pitch note-off cannot release it. Repeated note-ons retain FIFO note-off ownership, including old releases after voice stealing, channel stops and live allocation changes. Allocation changes stop existing notes and discard fallback candidates without resetting channel controllers. This does not preserve active notes across document loads or hardware-engine recreation.

Consolated offers **Mono / MIDI channel** only: its pulse, triangle/wave, noise and DMC roles are not interchangeable polyphonic voices. LAdrenaline's separately routed rhythm part remains native polyphonic percussion in every mode. Native articulation, routing, resource limits and shared controller behavior remain separate from allocation; notably SIDérant's CC7 still controls the single chip-volume register.


## Opale — OPL2 / OPL3

1. Build the add-on with score, then add **Opale** from the process library.
2. Connect a MIDI process/device to **MIDI In** and route **Stereo Out** to audio.
3. Choose an original patch with **Instrument**, or import/drop a `.wopl` bank. Instrument remains a connectable, automatable hardware-patch control.
4. **Operators** shows the two or four operators used by the selected voice mode. Select a row for its scaling and modulation controls below. **Patch** shows the applicable routing, tuning and lifetime fields; **Setup** contains engine capacity, bank addressing and global modulation depths. Hidden operator values remain authored state, not reset defaults.
5. Save the score document to retain the edited bank and configuration. There is no separate bank-export feature or built-in keyboard; use score's MIDI processes or a MIDI device to play.

Eight original melodic patches and six original drum patches are included. There is no bundled GM bank or manufacturer ROM. Blank slots remain blank rather than silently substituting a named commercial preset.

Operator values are native register values: higher **Attenuation** and **Sustain attenuation** mean less amplitude. **Multiplier code** is the chip's discrete frequency code, not a continuous ratio. Attack/decay/release are chip rates, not milliseconds. **Sustain mode** selects sustained-envelope behavior; operator Vibrato/Tremolo toggles are distinct from global modulation depths.

Patch **Tune 1/2** are semitone offsets. **Key-on ms / Key-off ms** are native bank voice-lifetime estimates used by libADLMIDI's allocation policy, not note-trigger delays or envelope controls. Secondary feedback is editable in both true four-operator and dual-two-operator modes; secondary tuning/detune belong to dual mode. Unsupported soft panning is not shown.

Header **Volume** is linear post-render gain from 0 (exact digital mute) to 32, with 1 remaining unity. Boost makes quiet native sounds usable without changing their chip registers or velocity response. It is automatable at sample offsets, independent of MIDI volume/expression and native chip attenuation. Muting continues rendering the chip, so unmuting does not restart notes or oscillator/envelope phases. Selecting or editing a patch does not reset this gain.

**Voice allocation** offers global Mono, the existing native Poly allocation, and sixteen MIDI-channel mono lines. Original MIDI channels still select programs, banks, percussion and expression. Sustain/sostenuto defer musical note-offs without leaving displaced mono notes latched in the native engine. Live mode changes preserve controllers; actual engine replacement resets native controllers while retaining pending note-off ownership.

### Configuration and resources

| Setting | Behavior |
|---|---|
| OPL2 | Nine native two-operator channels per chip; waveforms 0–3; no true four-operator voices |
| OPL3 | Eighteen two-operator channels per chip; up to six pairs reserved for true four-operator voices; waveforms 0–7 |
| Chips | Explicit expansion, 1–16 in the score editor; default one chip |
| Four-op channels | Total reservation across chips, not per-chip and not unlimited polyphony; must be zero for OPL2 |
| Dual 2-op | Two independent two-operator voices, distinct from a true four-operator algorithm |
| Rhythm | Native percussion allocation consumes chip resources; enable it on a percussion-bank patch |
| Backend | Pinned ymfm only; other emulator implementations are not selectable yet |
| Soft pan | Unavailable with this ymfm interface; native OPL3 left/centre/right panning remains available through CC10 |

Live configuration edits and native preset recalls are validated before publication; rejection retains the accepted controls and bank. Incompatible configurations are not silently coerced to another model. True four-operator banks require OPL3 and a nonzero four-op reservation. Sounding native note offsets outside -128…127 are preserved in files but rejected by this runtime. Import failures follow the explicit resource policy below rather than selecting a substitute bank.

Bank and program numbers are zero-based. For melodic patches, MSB/LSB are seven-bit bank selects and Program is the MIDI program. For percussion, MSB must be zero, LSB selects the kit (128–255 for XG SFX kits), and Program selects the drum note being edited. Editor selection applies bank/program changes to melodic MIDI channel 1 or percussion channel 10; it does not generate notes. Incoming MIDI retains independent bank/program/controller state on all sixteen channels, and incoming drum notes determine which kit instruments sound.

The header's **Instrument** chooser is also a connectable, automatable integer inlet (ID 75), like Deuterium's hardware-patch controls. Its value is the bank's stable vector slot, not the visible menu position or MIDI Program number: blank slots are not renumbered. Selection applies the complete melodic/percussion/MSB/LSB/program address before MIDI at the same sample offset. The four separate address controls remain available. An absent address is an error and silences the affected editing part instead of retaining its previous sound; undo restores that exact address. Negative Instrument values are reserved reversible encodings of unresolved addresses, not ordinary selectable patches.

### Timing, edits, and stop behavior

- MIDI channel messages and complete/reassembled SysEx7 are injected at their score sample offsets. A single UMP group (0) is supported. SysEx8 is not supported.
- Native patch controls and depth controls accept graph automation. Patch changes deliberately stop active voices; they are **not** continuous modulation of an already sounding operator. Identical parameter values do not repeatedly kill voices.
- Model/chip/four-op changes prepare a replacement engine off the audio callback and stop existing voices. They are setup changes, not sample-accurate chip reconfiguration.
- WOPL import and rhythm topology changes are editor setup actions, not realtime graph-stream operations. Import decodes asynchronously; superseded imports/preparations are cancelled.
- The adapter retains resampler state across render blocks. Changing host sample rate requires recreating execution. Output channels retain the complete host buffer size even for short loop slices. The first window clears recycled audio outside its active range; later windows preserve earlier samples from the same host tick. This applies to all seven families.
- Native pause, stop, seek, offset changes, and rendered loop boundaries clean up notes. Loop cleanup occurs at the rendered token boundary, not when future tokens are queued. Paused/zero-length/backwards tokens do not start notes or render new audio; release/reset controllers can still be consumed. Restarting at the same tick accepts new events without replaying the paused boundary's events. Panic and patch edits stop release-rate-zero voices as well as ordinary envelopes.
- Per-token event storage is bounded at 8192 events; overflowing it stops voices and silences the token. SysEx assembly is bounded at 64 KiB; oversized messages are rejected with an error. A separate guard prevents ymfm register-FIFO corruption under dense bursts: execution fails silent with an error until the engine is recreated.
- MIDI master volume follows native finite total-level attenuation. The separate header **Volume** control provides exact digital mute.

The engine models the chip and libADLMIDI's instrument/MIDI policy. It is not a firmware emulator for every keyboard that happened to contain an OPL chip. No hardware-reference accuracy claim is made by the initial integration.

### OPL documents and assets

Documents store a versioned inline WOPL bank plus stable control IDs. Imported and edited musical state survives removal of the original file. The original path remains an advisory reference; importing a missing or invalid file clears the active bank and reports the failure, without falling back to the factory bank.

WOPL import and document persistence preserve supported native patch fields, blank programs, bank names, addresses, volume model, global flags, and versioned delay fields. The internal codec upgrades older representations only when edited data requires it; it is retained for document state, not exposed as bank export. Import is limited to 16 MiB in the score process; the headless codec has a 64 MiB limit.

The bank, setup controls, and editor selection are persisted. Live MIDI program/controller state and SysEx performance changes are replayed by the score sequence, not captured as a machine snapshot. Running oscillator/envelope phases, active notes, and chip/resampler continuity are likewise not saved. Ensure you have redistribution rights before sharing score documents containing a third-party WOPL bank.

Native OPL presets capture the whole edited bank, setup, Volume and selected instrument. Recall does not reopen the advisory import path. Legacy control-only presets remain readable; invalid legacy and whole-bank recalls are atomic, including incompatibilities in non-selected bank slots.

Document schema 4 appends Voice allocation at ID 76; schemas 1–3 and older presets restore Poly. Schema 3 appended Instrument at ID 75 without renumbering existing ports; schema 2 added Volume at ID 74. Schema-1 JSON and binary documents restore unity gain, and the earlier schemas derive Instrument from the saved address and bank.

## Native presets

All seven families use score's standard process preset menu and library, like FoMo and Kaboom. There is no generic preset inlet or separate score-preset selector in their editors.

The ROM-free factory library contains **80 native `.scp` presets**:

| Family | Presets | Sound coverage |
|---|---:|---|
| Opale — OPL2 / OPL3 | 22 | Keys, basses, reeds, brass, pads, bells and six percussion sounds; two- and four-operator patches |
| AYstral — AY / YM / Sega PSG | 14 | Pulse leads, bass, plucks, slow swells, shared-envelope bells and tone/noise percussion |
| SIDérant — SID 6581 / 8580 | 13 | Unison and poly keys, filtered bass, sync vowels, ring tines, drawbars and snare |
| Wurlie — Wurlitzer 200A | 8 | Dry, cabinet, tremolo, soft and driven electric-piano variations |
| SiCC — SCC / SCC+ / HuC6280 | 13 | 32-sample waves: bowed, hollow, stepped, folded, stereo and hardware-LFO sounds |
| Consolated — NES / Game Boy APU | 10 | Pulse bass/lead, triangle mallet, editable wave reeds, hats, snares and original DMC woodblock |

This adds 44 newly authored sounds and native preset versions of OPL's 14 original bank voices. The native **Short wire hat** has a longer decay than its original hardware-bank initialization patch. One existing **LA Original saw** preset remains separate: lawful ROMs were unavailable, so it has not been loudness-matched.

Copy the family folders from `Presets/` into the active score library's `Presets` directory (normally `~/Documents/ossia/score/packages/default/Presets`) and restart score to discover them. Use **Save current preset** for your own sounds. Native recall and undo restore musical controls, including Volume; LA recall retains the recipient's ROM selection.

### Loudness and gain

The reference target is **−26 LUFS integrated**, with peak-safe attenuation for sharp transients. Calibration uses each actual core's 48 kHz stereo output, not peak normalization alone: six two-second notes across MIDI 48–72 at velocities 100–127, followed by a six-note velocity-127 chord. A separate pair of six-note velocity-127 chords, including the upper octave, checks peak headroom. OPL drums use their native percussion key/channel; existing fixed-channel and Sega-noise presets retain their intended routing.

Re-rendering all 80 saved presets measured **−27.22 to −25.95 LUFS** (1.27 LU spread), with worst reference/stress true peak **−1.10 dBTP** and no clipped samples. Every reference render was distinct. Native recall/undo/redo, 80 panel captures, and JSON/binary document reopen were checked; the 15 engine/document/qualification targets passed. These are rendered-audio measurements, not a physical-device listening or hardware-accuracy claim.

Only saved post-render Volume is adjusted for matching. Four weak candidate patches were refined before calibration: OPL hat decay, YM wood envelope duration, SID snare body and SID reed waveform. There is no compressor, limiter or hidden automatic gain. A different register mix, voicing, MIDI velocity, imported bank or denser polyphony can change loudness and clip; the reference is not an all-input peak guarantee. Hardware instrument choices preserve Volume and do not individually recall these calibrated native preset gains.

All seven families retain the existing stock Volume control, now accepting **0–32** linear gain (1 = unity, 32 ≈ +30.1 dB). Old numeric values, port IDs, init values and native automation ranges/points retain their meaning. **Review existing explicitly domain-mapped Volume cables/addresses:** score maps their source range to the enlarged destination range; for example, a 0–1 source at 0.5 maps to 16. Native automation and domainless numeric sources remain unscaled. This is the stock host mapping behavior, not an alternate hidden gain path.

The bundled preset gains actually span **0.212–12.036×**; none needs 25×. Twenty of 80 exceed 2×: fourteen OPL and six APU presets. Calibration used Volume to compensate for different core/patch output levels and low integrated energy in short percussion; the largest gain is **Short wire hat**. The 32× UI ceiling is excessive for this library, not an emulation requirement. Reducing the numeric domain alone would alter calibrated sounds and explicitly mapped automation, so gain staging needs a separate compatibility-aware correction.

Panels contain musical controls and resource selectors, without inline status, error, warning or information indicators. Model/executor diagnostics are retained internally; no replacement reporting channel is introduced by this add-on.

OPL's Instrument and bank/program controls, and LA's native part bank/program controls, select actual hardware instruments rather than a separate score preset mechanism. They remain controllable.

## AYstral — AY / YM / Sega PSG

Add **AYstral**, connect MIDI In and Stereo Out, and choose a sound through score's preset menu or edit the chip model directly. AY-3-8910 and YM2149 use emu2149; Sega YM7101 uses Nuked-PSG. No unverified SN76489 variants are advertised.

- One panel contains Model/Volume, **Voice allocation**, **Envelope source** and three tone rows. **Global** groups clock and shared noise controls. The shared hardware envelope appears when a tone uses it; **Software ADSR** appears when selected. There are no PSG tabs. Inapplicable controls are hidden; their authored values are retained.
- One physical chip: Mono plays its three tones together; Poly takes a free voice, then the oldest releasing voice, otherwise the oldest voice; MIDI channel maps channels 1/2/3 to tones A/B/C. Mono and MIDI-channel lines use held-note fallback. Repeated notes coalesce while retaining FIFO lifetimes; stolen Poly notes do not resurrect. Stored Poly=0 and MIDI channel=1 values remain unchanged; Mono is appended as value 2, including on restored selectors.
- AY/YM has one shared noise period and one shared hardware envelope period/shape. In **Native** envelope mode, per-channel routing chooses hardware envelope use; every routed note-on retriggers that shared generator. Hardware envelope amplitude bypasses fixed channel level, velocity and CC7. **Software ADSR** owns the envelope instead, leaving AY routing dormant and reversible. Global Volume applies in both modes. Sega has no AY hardware envelope.
- Sega has a separate noise voice on MIDI channel 10 when enabled. Noise can be periodic or white, with clock/512, /1024, /2048, or tone-C coupling. Tone-C coupling uses its period even when that tone is silent. Channel levels map to native four-bit attenuation.
- MIDI supports note lifetimes, sustain, fixed ±2-semitone bend, CC7, CC120/121/123, channel-mode note-off, and system reset. Program/bank changes, pressure, SysEx, and imported bank formats have no patch mapping.
- Clock is editable from 100,000 to 4,000,000 Hz. Original AY/YM presets use 1,773,400 Hz; Sega presets use 3,579,545 Hz. Integer periods quantize pitch and saturate without wrapping: AY period 1–4095 at clock/(16×period), Sega period 1–1023 at clock/(32×period). Minimum pitches at those clocks are approximately 27.07 Hz and 109.35 Hz respectively; lower MIDI notes cannot retain their requested pitch.
- Model/clock changes prepare a replacement core. Assignment and software-envelope-mode changes stop voices; other edits retain native clock phase. Software ADSR times are milliseconds; hardware envelope period/shape are native register values.
- Mono chip output is duplicated to stereo. Volume is saved, sample-offset automatable post-render gain, 0–32 with unity default and exact mute; native presets recall their saved gain. Sega rate conversion integrates native-clock output continuously; no band-limited or analog-board emulation claim is made.

Fourteen original presets are included. Schema-2 JSON/DataStream state retains dormant model-specific settings and Volume; schema-1 documents and legacy Boolean envelope-source presets remain readable. Invalid edits restore the accepted inlet value. Preparation, rejected-control and transient MIDI diagnostics are tracked separately, so recovery does not hide an unresolved resource/configuration error. No firmware/assets are needed; running notes and native phases are not snapshots.

## SIDérant — SID 6581 / 8580

Add **SIDérant**, connect MIDI In and Stereo Out, and choose a sound through score's preset menu. One process covers MOS6581 and CSG8580 through libresidfp, with PAL (985,248 Hz) or NTSC (1,022,730 Hz) clock and native two-pass sinc resampling.

- **Oscillators** provides three selectable native rows: waveform combinations, 12-bit pulse width, four-bit ADSR, filter routing, tuning, sync and ring modulation. Waveform values are a bitmask: triangle 1, saw 2, pulse 4, noise 8; combinations retain the chip's behavior.
- **Filter** contains shared cutoff (0–2047), resonance, LP/BP/HP combinations and voice-3 direct-output disable. **Chip setup** contains clock, filter curve, combined-wave strength and native chip volume; the model is in the header. **Attack/Decay/Release rate** and **Sustain level** are native SID values, not milliseconds.
- **Voice allocation** selects Mono (all three oscillators, last-note priority across input channels), Poly (three independently edited physical voices, oldest-voice stealing), or MIDI channel (channels 1/2/3 to oscillators 1/2/3). Mono and MIDI-channel lines restore held notes; unmapped note-ons are rejected. Stored mode values remain Mono=0, Poly=1, MIDI channel=2.
- Physical sync/ring sources are oscillator 1←3, 2←1, 3←2 in every allocation mode. Both controls remain editable, including after loading Poly presets such as **Three pulse keys**; editing them never changes allocation. In Poly mode, notes and voice stealing change the pitches on these fixed connections, so notes can modulate one another. Ring modulation affects the triangle component, not a pulse-only or saw-only waveform.
- MIDI supports note-on/off, sustain, fixed ±2-semitone bend, CC120/121/123, channel-mode note-off, and system reset. Nonzero velocity opens the hardware gate; it does not invent independent velocity gain. CC7 controls the one shared SID chip-volume register, last message wins. Duplicate note-ons for the same channel/key coalesce physically but retain their outstanding note lifetimes: an earlier note-off cannot release a later retrigger. Bank/program and SysEx mapping are not implemented.
- Pitch combinations outside the 16-bit frequency register report an error and temporarily gate off the affected oscillator, rather than wrap or silently saturate. Correcting tuning/bend reopens only a still-held or sustained gate; released and stolen notes do not resurrect. Diagnostics follow currently owned offending voices and clear on recovery/release without hiding another voice's ongoing error. Untransposed/unbent maximum MIDI notes are 106 PAL and 107 NTSC; tuning and bend can reduce this range.
- Model, clock, filter curve, and combined-wave strength are prepared off audio. The exposed curves are per-instance; upstream global 6581 range/capacitance setters are deliberately unavailable. Other native register edits affect sounding voices. Output rates supported by this adapter are 8–192 kHz.
- Volume is separate post-render gain, 0–32 with unity default; exact mute continues chip clocks and does not alter native chip volume. Native presets recall their saved gain. Mono output is duplicated to stereo.

Thirteen original presets and schema-1 semantic JSON/DataStream state are included; no firmware or external assets are required. State does not serialize oscillator/envelope phase, active notes, or upstream C++ memory.

GoatTracker `.ins` import was assessed but is not exposed. GTI5 includes relocated waveform/pulse/filter/speed tables, gate/hard-restart rules, and tick/tempo/multispeed-dependent macros. A static register patch import would discard that behavior. There is no `.sng`/`.sid` tune player disguised as an instrument.

### PSG/SID execution limits

Both families use bounded 8192-event token storage and preserve earlier render windows without replaying events. Overflow stops voices and silences the token with an error. They share OPL's native transport cleanup, including pause/seek/loop handling. Engine replacements stop existing notes and retire heavyweight state through score's execution queue. Invalid native preset recall is atomic; rejected live configuration edits preserve the accepted engine. Host sample-rate changes require recreated execution.

## LAdrenaline — Roland LA / MT-32

LAdrenaline is not built for now; configure with `-DEMULATION_ENABLE_LA=ON` to add it back.

Add **LAdrenaline**, connect MIDI and stereo audio, and select a matching **control ROM and PCM ROM** on its ROM page. Munt covers MT-32 old/new and CM-32L-compatible ROM pairs within one process; this is not a CM-32P or full CM-64 emulator. No firmware is included, downloaded, or automatically copied into projects.

- ROMs are identified by content SHA-1 and role, not filename. Unsupported images, incompatible pairs, changed expected hashes, and missing files produce explicit diagnostics. Engine creation, ROM I/O and heavy configuration happen off audio.
- A new object is one instrument receiving **MIDI channel 1**. Other melodic parts and rhythm start Off. **Instrument / Layer 2–8** tabs select editing focus, not the sounding part. Enable an optional layer to receive the same MIDI channel and build a layered instrument. All enabled layers share Munt's native **32-partial** budget; a layer is not an extra synthesizer.
- Each layer has native bank/program, level, pan and tuning controls. Every bank retains its program selection; same-timestamp bank/program automation applies the bank first. Menus use real ROM-derived names after preparation. **Voice allocation** selects global melodic Mono, native Poly, or one melodic mono line per input MIDI channel, without rewriting part receive assignments or the shared partial budget. **Routing** retains per-layer receive channel, bend range, reverb and **Native repeat / priority** (the hardware POLY1–4 policies, not the main Mono/Poly selector). MIDI 1–16 / Off display native automation values 0–15 / 16.
- Selecting a **Memory** bank/program exposes that timbre's four-partial editor. All 64 original user timbres remain accessible this way. Selecting a memory-backed layer follows its program; it does not overwrite the layer's sound or another timbre. Factory/ROM-backed sounds hide the memory editor. Legacy independent editing targets are preserved until explicit selection; an unmatched target is not presented as the selected layer's sound. Four stock toggles edit the native partial mask; pitch/filter/amplifier envelopes use stage tables. Factory timbres remain ROM-backed, not extracted into score presets.
- **System** contains master/reverb/rhythm settings; **ROMs** contains the standard resource file selectors. Header Volume is separate automatable post-render gain with exact digital mute. Native bank/program and timbre parameters remain real control inlets.
- Timestamped MIDI and bounded complete/reassembled SysEx7 reach Munt; SysEx8 is unsupported. SysEx assembly is limited to 64 KiB. Selected melodic notes use a local part-addressed extension to Munt's existing queue, preserving native MIDI delay and abort/retry behavior; each layered message incurs native interface serialization. Rhythm routing remains independent, even when it shares a melodic input channel. Transport panic resets sounding partials and restores native writable patch memory without rebuilding the synthesizer or converter.
- Documents save semantic controls, original memory timbres, absolute ROM references and expected hashes—not ROM bytes, active notes, oscillator phases or an emulator-memory snapshot. Runtime MIDI/SysEx performance state must be replayed by the sequence.
- ROM references are read-only external dependencies: project consolidation and Save As do not collect, copy or reanchor them. A missing path remains visible and execution reports the failure. Provide licensed ROMs separately on each machine.
- SLA3 semantic state appends Voice allocation; SLA1/SLA2 state migrates to Poly without changing native routing. Presets contain musical state and ROM provenance hashes, never machine-specific ROM paths. Recall preserves the locally selected pair, including when reading older presets. Missing or mismatched provenance is reported separately from readiness. Documents still retain their external resource references.
- **Original saw** is authored native state, not a manufacturer patch. Missing-ROM diagnostics survive ordinary edits. Changing a ROM reference, or explicitly selecting the same path again after repairing a file, requests preparation; unrelated edits and preset recall do not repeatedly reopen failed resources.

ROM-independent error handling, state and asset-policy regressions run without firmware. Audible rendering, real-ROM MIDI/SysEx, panic and callback qualification require a lawful matching pair and remain explicitly unverified until one is supplied. Set `SCORE_EMULATION_LA_CONTROL_ROM` and `SCORE_EMULATION_LA_PCM_ROM` for the ROM-gated tests; optionally set `SCORE_EMULATION_LA_MISMATCHED_PCM_ROM` for incompatible-pair coverage.

## Wurlie — Wurlitzer 200A

Add **Wurlie**, connect MIDI and stereo audio, and load a native preset or edit its controls. The headless OpenWurli model is ported through the audited Retromulator source: reed mechanics, pickup/amplifier nonlinearity, tremolo, speaker and model correction. It needs no sample library or ROM.

- **Volume** is post-circuit gain, 0–32 with exact digital mute. **Amplifier drive** remains 0–1 and changes the modeled amplifier pot and nonlinear response; it is not a duplicate output fader.
- **Tremolo**, **Speaker**, **Model correction**, and **Velocity** complete the compact panel. Velocity offers Linear, Soft, Hard and Fixed response; Linear preserves input velocity before the model's physical response.
- **Voice allocation** offers global Mono, Poly's 64-reed pool, and sixteen MIDI-channel mono lines. Mono lines use last-note priority across held and sustained candidates, restoring original channel/velocity on fallback. Stealing uses a 5 ms crossfade; repeated notes retrigger one reed while retaining FIFO note-off ownership. Notes outside the physical MIDI 33–96 range use the nearest modeled key while retaining the original MIDI note identity for release.
- MIDI supports CC1 tremolo, CC7 volume, CC11 expression, CC64 sustain, CC70 correction, CC71 speaker, CC75 velocity curve, CC120/121/123 and system reset. CC75 values 0–3 select the four curves directly; higher values select four 32-value ranges. Pitch bend, pressure, program changes and SysEx have no acoustic-model mapping.
- CC121 restores global timbre overrides to the latest authored controls, while resetting channel controllers only on the addressed MIDI channel. It does not reset reeds, the amplifier circuit or tremolo phase. Transient MIDI diagnostics clear on valid input; preparation/circuit failures retain their own diagnostic ownership.
- Supported host rates are 44.1–192 kHz; lower rates are rejected because the original cabinet model includes a 20 kHz low-pass. Schema 2 appends Voice allocation at inlet ID 7; schema-1 documents and older presets restore Poly. Controls, native presets and JSON/DataStream state retain musical settings, not active reed phases.
- Eight original presets are included: **Reed room**, **Dry felt**, **Evening vibrato**, **Bright cabinet**, **Neon pulse**, **Sidewalk bark**, **Velvet afterglow**, and **Studio silver**.

## SiCC — SCC / SCC+ / HuC6280

Add **SiCC**, connect MIDI and stereo audio, and choose **SCC**, **SCC+**, or **HuC6280**. No ROM is required. Wave RAM uses the dedicated **Waveform** port/editor, not Curve or Wavecycle geometry. Draw directly across sample columns; skipped columns interpolate along the stroke. Release commits one undoable edit; Escape cancels its preview. The inspector and instrument panel use the same editor.

| Model | Physical resources | Wave data and shared resources |
|---|---|---|
| SCC | Five mono voices, duplicated to stereo | 32 signed 8-bit samples per wave; channel 5 uses channel 4's wave RAM |
| SCC+ | Five mono voices, duplicated to stereo | Independent 32-sample signed 8-bit wave per channel |
| HuC6280 | Six voices with native stereo attenuation | 32 unsigned 5-bit samples per wave; noise on channels 5/6; LFO uses channel 2 to modulate channel 1 |

- **Voice allocation** offers Mono (enabled physical voices together), Poly (automatic physical-voice allocation) and MIDI channel (fixed MIDI channels 1–5 on SCC/SCC+, 1–6 on HuC6280). Mono lines restore held notes and their original channel/velocity. This is one chip, not unlimited polyphony. Disabled voices and the HuC LFO's channel-2 source are unavailable for notes. Stored Poly=0 and MIDI channel=1 values remain unchanged; Mono is appended as value 2.
- Per-channel tuning belongs to a **physical voice**, not a layer attached to every note. In polyphonic mode, different notes can occupy different voices. Use global tuning for a uniform instrument transposition. **SCC Plus Oboe thirds** and **SCC Octave staircase** now use equal channel tuning; the latter retains its global one-octave-down register.
- Each channel has **one authored 32-sample waveform and one Level**, shared across models. **Channel 1–5/6 tabs** select editing focus in one persistent editor; they do not mute, solo or select the sounding voice. SCC channels 4/5 edit the actual channel-4 wave resource; channel 5's independent wave remains stored for SCC+, and channel 6 remains stored while unavailable on SCC models.
- Samples are normalized bipolar values in `[-1, 1]`, with implicit periodic wrap and no repeated endpoint. Defaults are sampled triangles. Painting snaps to 256 amplitude levels for SCC/SCC+ or 32 for HuC. Model changes retain authored samples; native RAM is quantized for the active chip when the engine is prepared or a wave arrives.
- Waveform ports are ordinary control inlets: they take cables (a multislider's list, for instance) and device addresses (an OSC message with several arguments). A list of numbers is one cycle, a list of lists a bank whose first frame plays; samples are bipolar, clamped to [-1, 1], and resampled in place to the chip's 32-entry wave RAM without allocating. What execution receives is drawn over the edited cycle. In the editor a stroke stages local preview data and publishes its completed value on release; the model prepares the same fixed native arrays and hands settings snapshots to execution, and a snapshot leaves a wave it did not change as execution last set it.
- Channel Level uses normalized 0–1 musical gain. Native channel levels and stereo attenuation remain chip-specific.
- Document and preset schema **4** stores sample-backed ports. Earlier chip curve/sample schemas, recovery archives and waveform-routing migrations are intentionally unsupported. All 13 shipped presets were converted offline while preserving their active-channel native RAM exactly. General score Curve ports and Wavecycle remain unchanged.
- **Volume** is independent automatable post-render gain, with exact mute at zero. HuC left/right attenuation, noise and LFO remain native controls. With LFO enabled, channel 2 exposes its source wave and period rather than misleading note level/tuning controls. Noise channels show noise parameters instead of an unused wave editor. Global/per-channel tuning uses cents; native periods still quantize and clamp pitch.
- **Clock Hz** defaults to 3,579,545 and accepts 100,000–8,000,000. Host rates are integer 8–384 kHz. Model/clock changes prepare an engine off audio and stop voices; ordinary parameter edits retain native state where the hardware permits.
- MIDI supports note lifetimes/stealing, sustain, CC7, CC120/121/123 and ±2-semitone bend. No program bank, SysEx, software ADSR, DDA sample stream or extra virtual chips are exposed.
- Thirteen original score presets include the five earlier sounds (**SCC Triangle choir**, **SCC Plus Reed fifth**, **HuC Stereo triangle**, **HuC Coupled LFO**, **HuC Noise pair**) plus bowed amber, hollow pipe, octave staircase, glass fold, oboe thirds, velvet stereo, stereo brass and wobble reed variations.

The selected HuC core does not claim hardware-verified LFO frequency or cycle-accurate waveform read/write pointer behavior. Live waveform uploads reset the native write position and may restart that oscillator. Presets/documents retain semantic wave/control data, not running chip phases.

## Consolated — NES / Game Boy APU

Add **Consolated** and choose **NES NTSC**, **NES PAL**, **Game Boy DMG**, or **Game Boy Color**. These are standalone APU cores, not game/CPU emulators. No game ROM, game music or manufacturer patches are included or needed.

New objects use **Mono** allocation: MIDI channel 1 plays the header's automatable **Sound** choice (Pulse 1, Pulse 2, Triangle/Wave, Noise, or NES DMC). Role tabs only select controls to edit; switching tabs does not change the sounding role. **Setup → Voice allocation → MIDI channel** retains whole-chip multichannel use and hides the inapplicable Sound selector:

| MIDI channel | NES role | Game Boy role |
|---|---|---|
| 1 | Pulse 1, including native sweep | Pulse 1, including native sweep |
| 2 | Pulse 2, including native sweep | Pulse 2 |
| 3 | Triangle and linear counter | Editable 32-sample, 4-bit wave RAM |
| 4 | Noise | Noise |
| 5 | DMC | Unavailable |

- Role tabs expose native duty, level/envelope, length, sweep, noise-rate and stereo routing where supported. These are heterogeneous physical roles, not generic polyphonic voices, so no misleading Poly option is offered. Mono addresses one selected role; MIDI channel addresses every physical role concurrently through the channels above. Each line uses last-note priority and held/sustained-note fallback with original velocity. Repeated notes retain FIFO note-off ownership. CC120/121/123 and ±2-semitone pitch bend are supported; noise/DMC pitch uses native rate controls instead of note pitch.
- Native lengths are register/table values, not milliseconds. NES length is a 5-bit table index; GB pulse/noise length is a 6-bit preload, wave length 8-bit. Domains follow the active role/model. GB wave Level uses the native **Mute / Full / Half / Quarter** choices.
- NES and Game Boy retain separate authored role banks **and Sound selections**: NES DMC → GB restores the remembered GB sound; returning to NES restores DMC. PAL/NTSC share the NES bank; DMG/CGB share the GB bank. Invalid direct sound/register edits reject atomically. Schema **5** persists allocation, both banks/selections and the dedicated waveform port; earlier APU schemas are intentionally unsupported. Allocation and Sound retain inlet IDs 90/91 and original routing values 0/1; saved selector labels are refreshed without remapping sounds.
- Changing Voice allocation or the active Mono Sound stops voices and clears fallback candidates while retaining MIDI-channel controllers and outstanding source-note releases. An old note-off cannot stop a new same-pitch note on the newly selected sound. Editing the remembered Sound in MIDI-channel mode does not affect its voices.
- **Clock override** lives on Setup; zero selects the nominal rate: 1,789,773 Hz NTSC, 1,662,607 Hz PAL, 4,194,304 Hz GB. A positive override (100,000–20,000,000 Hz) is retained across model changes and undo. Host rates are 8–384 kHz; NES requires an integer host rate.
- Header **Volume** is automatable post-render gain with exact zero mute. NES output is the upstream linear mono mix duplicated to stereo. GB retains independent native channel routing and distinct DMG/CGB behavior/filtering.
- **Import DMC** uses the normal score file-selector/port layout and accepts user-owned raw `.dmc`/`.bin` data, 1–16,384 bytes, prepared off audio. Bytes map at CPU `$C000`; unprovided memory and native wrapping into `$8000–$BFFF` read zero. Address is `$C000 + 64 × register`; length is `16 × register + 1` bytes. Rate, loop and initial 7-bit DAC level remain native controls. This is not a WAV converter or game-ROM loader.
- Imported DMC bytes are embedded in semantic JSON/binary state and native presets. Deleting the source file does not break recall. File import is an authoring action, not audio-thread path automation. The default DMC data is an original delta-bit triangle pattern.
- GB wave RAM uses the dedicated **Waveform** editor: one 32-sample periodic frame, with painting snapped to 16 amplitude levels and a sampled triangle default. Authored normalized samples survive unrelated controls, preset undo and document recall. The model prepares unsigned 4-bit RAM off audio; a cable or address into the Waveform port is resampled to the same RAM on the audio thread, in place. Editing an active wave disables its DAC, writes RAM and retriggers that role. NES exposes Triangle instead of Wave; Game Boy has no DMC page. Model/clock/DMC preparation stops active voices, rather than pretending to be sample-accurate model switching.
- Ten original score presets select their named sound on MIDI channel 1: the earlier **NES Clear pulse** and **GB Triangle wave**, plus NES quarter bass, triangle mallet, needle lead and DMC woodblock, and GB hollow reed, sand hat, pinched pulse and alloy snare. Use score's preset menu/library, not a preset inlet.
- Resource, control, renderer and MIDI errors are independent. Missing/invalid DMC import remains actionable through unrelated edits; recovered render/MIDI errors clear without concealing that resource failure.

No NES expansion chips, GBA FIFO/DMA audio or cycle-perfect hardware claim. Upstream GB envelope/filter limitations remain qualification boundaries. Both new families persist musical settings/assets, not engine-memory snapshots.

## Qualification boundary

The sample-backed waveform cutover was built on Linux and exercised through native
scene/inspector drawing, gesture cancellation/rebinding, bank frame operations,
periodic resampling, undo/redo, JSON/binary state, preset recall and chip engine
rendering. A visible application smoke drew and quantized samples, saved, reopened
and verified exact undo/redo state; the bank widget was also displayed and exercised.
Offline comparison preserved all 2,592 active native RAM samples across the 23
converted Wavetable/APU presets. Two pre-existing tests remain failing in this
checkout: Wavetable paused-window cleanup and APU preset-popup discovery. These
checks do not claim physical-device listening or new callback-allocation qualification.

Windows callback instrumentation observed zero allocation and deallocation in 4,104 recycled-buffer render callbacks across OPL2/3, AY/YM/Sega, SID6581/8580 and Wurlitzer, including first rendered callbacks, mixed block sizes, MIDI, gain and native controls. This covers wrapped allocators in the statically linked callback path, not every DLL or arbitrary future workload. Debug timing under concurrent load is not a worst-case deadline guarantee. Physical-device listening, other platforms, hardware-reference comparisons and ROM-backed LA qualification remain separate gates.

The workflow review exercised all seven native panels (109 captured states), stock control/tab clicks, model changes, native preset undo and document save/reopen. Curve point dragging, inspector Delete/copy/paste/undo, SCC shared-wave selection, HuC LFO/noise and GB wave editing passed; 29 ROM-free renders were finite and nonzero, including changed output after waveform edits. Twelve prewired scenarios passed play/pause/resume/seek/stop and saved-copy reopen/replay through SDL's silent callback. LA correctly remained silent with its missing-ROM diagnostic. This is execution/UI evidence, not physical-device listening or new callback-allocation qualification.

The subsequent panel-cohesion review captured 152 native states across all seven families, exercised layer enable/undo and stock curve gestures, and saved/reopened every process. All 18 available APU model/role combinations rendered finite nonzero output from MIDI channel 1; two additional GB wave renders changed after a real curve drag. Twelve prewired scenarios passed play/pause/resume/seek/stop and saved-copy replay (24 runs) through SDL's silent callback; the ROM-free cases produced nonzero audio and missing-ROM LA stayed silent. These checks do not qualify real-ROM LA layering or physical-device audio.

## Build

Initialize the dependencies:

```sh
git submodule update --init --recursive
```

Place this repository under `score/src/addons/score-addon-emulation`, then configure/build score normally. Native processes require the engine, media, dataflow, curve and scenario plug-ins. JavaScript is additionally required when converting legacy Wavetable waveform bindings.

The local score host also includes a correction to `Process/Dataflow/ControlWidgets.hpp`: programmatic checkbox updates must block `QCheckBox::toggled`, and rejected edits must display the accepted control value. That host fix is required to prevent recursive inspector commands when editing SID Sync/Ring; updating this add-on alone does not patch an older host.

The matching host also fixes library-preset ownership in `Effect/EffectLayer.cpp`: native menu actions own a preset snapshot instead of retaining pointers into the mutable library registry. Background preset indexing during the menu's nested event loop could otherwise select the wrong sound or access freed storage. This shared preset-menu correction is included in the rebuilt score executable, not applied to older hosts by the add-on ZIP.

Waveform editing requires the matching host ports `Dataflow::WaveformInlet` / `Dataflow::WavetableInlet` (`Dataflow/WaveformInlet.*`) and `Process::WavetableData` (`Process/Dataflow/Wavetable.hpp`), documented in those headers. `GraphicsGridLayout.cpp` also fixes row-titled empty tables, reached by unresolved OPL selections. These are shared host corrections; the source add-on ZIP alone does not patch an older score.

The native Wave controls also use `Process::PortFactory::layoutFullItem` from the matching host: it reapplies the standard label/control/inlet arrangement after waveform editor resizing or selected-channel rebinding. Wave and DMC inlets use no synth-specific offsets.

```sh
cmake -S /path/to/score -B /path/to/score-build \
  -DSCORE_TESTING=ON -DEMULATION_BUILD_TESTS=ON -DEMULATION_BUILD_TOOLS=ON
cmake --build /path/to/score-build --target \
  score score_addon_emulation emulation-opl-render \
  Emulation_OplEngineTest Emulation_OplDocumentTest \
  Emulation_PsgEngineTest Emulation_PsgDocumentTest \
  Emulation_SidEngineTest Emulation_SidDocumentTest \
  Emulation_LaEngineTest Emulation_LaDocumentTest \
  Emulation_WurliEngineTest Emulation_WurliDocumentTest \
  Emulation_WavetableEngineTest Emulation_WavetableDocumentTest \
  Emulation_ApuEngineTest Emulation_ApuDocumentTest \
  Emulation_ChipQualificationTest
ctest --test-dir /path/to/score-build -R '^Emulation_' --output-on-failure
```

For the Qt-free cores and OPL renderer:

```sh
cmake -S . -B build-core -DEMULATION_CORE_ONLY=ON -DEMULATION_BUILD_TESTS=OFF
cmake --build build-core
build-core/emulation-opl-render --model opl3 --output opl3.wav
build-core/emulation-opl-render --model opl2 --program 6 --output opl2.wav
build-core/emulation-opl-render --drum --program 36 --output kick.wav
```

The renderer prints onset, peak/RMS, release, clipping, and PCM hash measurements. `--list` enumerates playable patches. Standalone core tests require an installed Catch2 3 package; score builds use score's existing Catch2 dependency.

On MSYS2/Windows, keep the matching toolchain's `bin` directory on `PATH` for CMake's Qt tools and executable DLL dependencies.

### Allocation validation (2026-11-06)

The add-on and all fifteen emulation regression targets built successfully. All seven engine suites passed their available coverage; ROM-backed LA cases were not exercised. The complete Opale, AYstral, SIDérant, LAdrenaline and Wurlie document suites and chip qualification suite passed. SiCC's allocation-migration case and Consolated's semantic-document case passed separately; the previously recorded unrelated paused-window and preset-popup failures were not rerun.

The running application exercised every allocation value, native preset recall, undo/redo and fresh-process document reopen for all seven instruments. Expanded panels were visually inspected with MIDI/audio connectors visible, including Consolated's Setup selector and a mouse-driven switch to MIDI channel. Offline renders covered all fifteen ROM-free chip/model configurations: last-note release returned to the held A4 at 439.45–440.92 Hz (1.465 Hz analysis-bin resolution), with finite, nonzero audio. This is rendered-output evidence, not physical-device listening. LAdrenaline's policy, state and UI are covered, but native ROM-backed audio and the Munt queue extensions still require a lawful control/PCM ROM pair for runtime validation.

## Source packaging and license

```sh
cmake -P cmake/Release.cmake
```

This creates `score-addon-emulation.zip` with selected dependency source and notices, without the upstream bank collections or unused emulator cores. It refuses to overwrite an existing staging directory. Configure a headless build from the extracted archive before distributing it; GitHub's automatic source archive does not expand submodules.

This working repository is local; creating it does not publish a GitHub repository or release asset. Package metadata intentionally has no download URL until a curated release asset is published.

See [DEPENDENCIES.md](DEPENDENCIES.md) for pinned revisions, file-level licenses, original-preset provenance and pin-sensitive integration boundaries. Newly authored add-on code and original patches are GPL-3.0-or-later, except the Wurlitzer engine adaptation, which retains upstream GPL-3.0-only terms. The selected combined configuration can be distributed under GPLv3; dependency licenses are not relabeled. Remaining roadmap and qualification tasks are in `../EMULATION_TASK_LEDGER.md`.
