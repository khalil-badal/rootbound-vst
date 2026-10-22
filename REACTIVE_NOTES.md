# Rootbound — Onset-Reactive Mode

A 6th grain mode, **Reactive** (`Grain Mode → Reactive`), that detects the notes you
play and fires grain **bursts** locked to each attack, then settles back into the
existing lush wash. Modes 0–4 are untouched.

## New parameters

All four take effect **only when Grain Mode = Reactive**. They still exist and save
in every mode (so automation lanes never break), but are ignored in modes 0–4.

| Param | Range / values | Default | What it does |
|---|---|---|---|
| **Reactivity** | 0–100 % | 50 | Onset sensitivity. Higher = lower detection threshold = fires on softer plays (and more often). Too high on a noisy input = false triggers. |
| **Burst** | 0–100 % | 50 | Grains-per-onset **and** roll tightness in one knob. Low = few grains, loose/scattered spray. High = many grains packed into a tight ratcheted roll (H90 Stutter). |
| **Arp Mode** | Off / Up / Down / Up-Down / Random / Octaves | Off | Pitch pattern stepped through across each burst's grains → arpeggiated/cascading runs (Microcosm Arp, Crystals climb). Off = every repeat at base pitch. |
| **Playback Mode** | Normal / Oct Up / Oct Down / Reverse / Random | Normal | Per-grain playback on the burst (Walrus Qi's 5 modes). Reverse reads the captured window backward with a swell-in. Random picks per grain. |

Reused where natural: **Pitch** (base transpose of burst grains), **Decay** (wash length +
regen), **Diffusion** (wash amount/swirl + evolving LFO depth), **Grain Size** (burst grain
length), **Freeze** (holds — no new onset bursts, but idle-fill still sustains from the
held buffer).

> **UI note:** only the *Grain Mode* dropdown gained the "Reactive" entry. The four new
> params are automatable/host-controllable but do **not** have faceplate knobs yet, so in
> the **standalone** they sit at their defaults (Reactivity 50, Burst 50, Arp Off, Playback
> Normal — already a usable Reactive voice). Say the word and I'll add four knobs to the UI.

## Starter presets (set Grain Mode = Reactive first)

| Preset | Reactivity | Burst | Arp | Playback | Pitch | Decay | Diffusion | Feel |
|---|---|---|---|---|---|---|---|---|
| **Subtle Sparkle** | 40 | 25 | Up | Normal | 0 | 2 s | 30 | A few pitched droplets per note, gentle. |
| **H90 Stutter Roll** | 60 | 90 | Off | Normal | 0 | 1.5 s | 20 | Tight ratcheted machine-gun roll on each attack. |
| **Microcosm Strum Cascade** | 70 | 65 | Up-Down | Oct Up | 0 | 4 s | 45 | Climbing arpeggiated bursts that wash into a big tail. |
| **Qi Reverse Cloud** | 55 | 50 | Off | Reverse | 0 | 6 s | 60 | Backward swells blooming into a huge space. |
| **Full Chaos** | 90 | 100 | Random | Random | +7 | 8 s | 80 | Dense random-pitch/dir bursts + max wash. Hold on. |

## A/B tuning constants (top of `GrainEngine.cpp`, anonymous namespace)

- `kOnsetThreshHi/Lo` — the reactivity threshold endpoints. If it misses soft plays, lower
  `kOnsetThreshLo`; if it double-fires on one pluck, raise `kOnsetRefractoryMs`.
- `kOnsetRefractoryMs` (45) — min ms between onsets. Raise to stop machine-gun re-triggers.
- `kBurstMinGrains/MaxGrains` (2/14) — grains-per-onset range for the Burst knob.
- `kBurstIntervalLooseMs/TightMs` (90/14) — roll spacing range (loose spray ↔ tight ratchet).
- `kBurstTimingJitter` (0.6) — how scattered the loose end is.
- `kReverseBloomProp` (0.40) — reverse-grain swell length.
- `kIdleFillMs/Amp` (140/0.5) — density/level of the between-onset idle texture.
- `kBurstAmp` (1.05) — burst loudness vs the cloud.

## Test-by-ear checklist

1. **Slow single-note passage** — pluck one note every ~2 s. Each pluck should fire a
   distinct burst; between bursts the wash + idle-fill should keep it from going thin/dead.
2. **Fast strummed chord** — strum hard. Multiple onsets should each add a burst without the
   grain array overrunning or notes sticking (burst voices steal the shortest-remaining roll;
   grain pool skips gracefully when full — no overrun, no stuck grains).
3. **Switch all six modes mid-audio** — Cloud→…→Reactive and back while sound is playing.
   No clicks/discontinuities (the mode branch only changes *which* scheduler runs; grains
   already in flight finish through the same render path).
4. **Load an old preset** — confirm modes 0–4 sound identical to before (the new params
   default and are ignored outside Reactive; appended `grainMode` entry preserves indices).
5. **Freeze in Reactive** — engage Freeze; new plucks should NOT trigger fresh bursts (held),
   while the existing texture sustains from the frozen buffer.
6. **Fidelity** — Oct Up / high Pitch bursts: no aliasing fizz (mipmap by `|rate|`); Reverse:
   click-free swell; peaks stay near −3 dBFS (soft knee rarely engages).

## Correctness property (verified by inspection)

With `grainMode` ∈ 0–4, the per-sample loop takes the `if (! reactive)` branch — the exact
existing density-timer code. The onset detector, burst scheduler, arp, reverse and idle-fill
live entirely inside the `else` (Reactive) branch and never execute. The Reactive block-context
math above the loop is computed but has no side effects. So modes 0–4 are byte-for-byte
unchanged.

## Build

```
cmake --build build --config Release --target Rootbound_Standalone
cmake --build build --config Release --target Rootbound_VST3
```

Compiles clean against the existing `CMakeLists.txt` (one pre-existing benign
`C4456` shadow warning in `PluginEditor.cpp`, unrelated to this feature).

## Known scope note

Burst roll timing is driven by the **Burst** knob (the core tight↔loose behavior). Hard
tempo-sync of the stutter to the host grid via `RootboundSync` was left as a follow-up hook
(it would need BPM piped into the engine + a burst-division param) rather than adding another
control this pass — flagged so it's a deliberate, visible decision.
