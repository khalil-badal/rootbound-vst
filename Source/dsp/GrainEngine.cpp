#include "GrainEngine.h"

// ============================================================================
//  Cosmic Web rework — changelog
// ============================================================================
//  TORN OUT: reverse, completely. All reverse/morph grain behavior, the reverse
//  read-offset math, reverse-bloom envelopes, and both APVTS params ("reverse"
//  and "reverseMode") are gone — engine, parameters, and UI. Forward only.
//
//  THE PROBLEM this pass fixes: every prior "granular" ingredient was voiced to
//  smooth itself back into a wash, so grains never became audible as discrete
//  events. Fixes, mapped to the six causes:
//
//   1. ONSET DECOUPLED FROM GRAIN LENGTH. Density now maps directly to the GAP
//      between grain onsets (exponential: ~420 ms at Density 1 -> ~4 ms at 100),
//      independent of grain size. Low density = countable droplets with audible
//      space between them; high density = fused cloud. (Was: onset = len/N, which
//      forced seamless overlap and made pointillism impossible.)
//   2. NO MORE FLAT POWER-NORM. Cloud gain uses an exponent BELOW 0.5 so level
//      surges with density and breathes; peaks caught by the limiter, not
//      pre-flattened to constant RMS.
//   3. PITCHED, RE-SCATTERING FEEDBACK (Crystals cascade). A shimmer grain can
//      spawn an octave-HIGHER child (bounded depth, decaying amp); grain output
//      is written back into the capture buffer, so later grains re-pitch it and
//      octaves CLIMB over time instead of converging to a flat drone.
//   4. INTRA-GRAIN RETRIGGER. A pitched-up grain replays its source span within
//      its own length at the pitch ratio (octave up = span twice), each loop
//      Hann-windowed (click-free) -> crystalline internal stutter, pitch as a
//      repeat-speed like Cosmic Web.
//   5. RICHER, SELF-PROPAGATING SHIMMER. Musical-pitch share raised (0.30 base /
//      0.60 in Shimmer), quantized to octaves/fifths/+2-oct, propagating through
//      the cascade into a glassy climbing halo.
//   6. PERCUSSIVE ATTACK for short grains (<80 ms): near-instant attack so they
//      TICK as discrete events; long grains keep the soft bloom.
//
//  The FDN is kept only as a light post-air sweetener, SCALED BY DENSITY so it
//  never blurs sparse pointillistic grains — it just glues dense clouds.
//
//  KNOBS NOW:
//   * GRAIN SIZE : short = ticking shards, long = blooming voices.
//   * DENSITY    : sparse countable droplets -> thick moving cloud (real gaps!).
//   * PITCH SNAP : base interval of every cloud grain, snapped to unison / 4ths /
//                  5ths / octaves (PitchSnap.h). The Sustain bed stays at unison.
//   * SUSTAIN    : level of the steady two-grain unison bed under the cloud.
//   * GRAIN ENV  : grain envelope shape (Auto / Smooth / Swell / Pluck).
//   * DECAY      : spray depth + regeneration + cascade probability (how far the
//                  ice climbs and how long the cloud self-sustains).
//   * DIFFUSION  : scatter width + detune + evolving-LFO depth.
//   * FREEZE     : stops buffer writes; grains loop the held moment forever.
// ============================================================================

// ============================================================================
//  === Onset-Reactive rework — changelog ===
// ============================================================================
//  WHY: the engine spawned grains purely on a density timer and never reacted
//  to the notes played, so it sounded like "a good reverb" but never "went
//  crazy when I play." Added a 6th grain mode, "Reactive" (grainMode == 5),
//  that detects onsets in the input and fires grain BURSTS locked to each
//  attack — then settles back into the existing lush wash.
//
//  MAPPING TO THE REFERENCE UNITS:
//   * Eventide H90 Stutter/Glitch  -> onset-triggered ratcheted rolls whose
//     tightness/spread is the `burst` knob (tight ratchet .. loose scatter);
//     Glitch pitched bursts        -> the arp + octave/reverse playback modes.
//   * Walrus Qi "Phrase Sample"    -> bursts are ANCHORED to the just-captured
//     note (read anchor = writePos at the onset), replaying what you played;
//     the 5 Qi playback modes       -> Normal / Oct Up / Oct Down / Reverse /
//     Random on each burst grain.
//   * Hologram Microcosm Strum/Arp -> successive plucks chain into pointillistic
//     bursts; the arp table turns repeats into climbing/cascading runs.
//
//  HOW IT STAYS ADDITIVE (the critical property): the per-sample loop branches
//  on modeThisBlock. In modes 0-4 it runs the EXACT existing density-timer path
//  — not one line of onset/burst code executes. Only when grainMode == 5 does
//  the onset detector + burst scheduler + idle-fill run, REPLACING (not
//  layering on) the density timer. New params default so old presets (modes
//  0-4) reproduce today's sound bit-for-similar.
//
//  REUSE: burst grains are the same Grain struct through the same render loop,
//  anti-alias mipmap, FDN wash, WashDiffuser, DC-blocked regen and softLimiter.
//  Reverse is a per-grain negative read rate inside a burst voice only.
// ============================================================================

// Bench builds (ROOTBOUND_PROBE) can override a few voicing constants from
// environment variables for tuning sweeps; the plugin always gets constexpr.
#if ROOTBOUND_PROBE
 #include <cstdlib>
 #define RB_TUNABLE const float
 #define RB_TUNE(name, def) (std::getenv (name) ? (float) std::atof (std::getenv (name)) : (def))
#else
 #define RB_TUNABLE constexpr float
 #define RB_TUNE(name, def) (def)
#endif

namespace
{
    // ---- Tuning constants (A/B these by ear) ----------------------------------
    // Scheduler: Density -> gap between grain onsets (the sparse<->dense sweep):
    constexpr float kOnsetMaxSeconds    = 0.42f;  // gap at Density 1 (sparse)     [0.25..0.6]
    constexpr float kOnsetMinSeconds    = 0.004f; // gap at Density 100 (dense)    [0.002..0.008]

    // Level dynamics: kNormExp < 0.5 lets dense clouds surge louder than sparse.
    constexpr float kOutputTrim         = 0.78f;  // overall cloud gain            [0.5..1.0]
    constexpr float kNormExp            = 0.34f;  // <0.5 => dense surges; 0.5=flat [0.3..0.5]

    // Spray window (position spread across the capture buffer):
    constexpr float kSprayBaseSeconds   = 0.18f;  // min spread even at short Decay [0.1..0.5]
    constexpr float kSprayDecayFraction = 0.40f;  // Decay -> spray depth scaling   [0.2..0.6]
    constexpr float kSprayDiffusionBase = 0.70f;  // spray width at Diffusion 0     [0.4..0.9]
    constexpr float kSprayDiffusionRange= 0.80f;  // extra width at Diffusion 1     [0.5..1.2]

    // Playability: front-load the grain read distribution so most grains read
    // the FRESHEST audio (immediate response to what you just played), while a
    // minority still reach back across the spray window for the smeared tail.
    // This fixes the laggy feel without shrinking the spray/decay character.
    constexpr float kFreshShare         = 0.80f;  // fraction of grains that read very recent [0.5..0.9]
    constexpr float kFreshDepth         = 0.08f;  // "fresh" = within this fraction of the window [0.04..0.20]
    // Recent-compression for the spread modes (Scatter/Swarm) that don't use the
    // front-load draw: pulls their read positions toward recent for playability
    // while keeping their distribution SHAPE. 1 = unchanged, lower = tighter.
    constexpr float kSpreadModeResponse = 0.58f;  // Scatter/Swarm recent pull      [0.4..1.0]

    // Per-grain STEREO WIDTH. Grains read L and R separately and blend toward the
    // true stereo (mono core + width*side), so the cloud is wide/cinematic but
    // stays mono-safe (never collapses to one channel). 0 = mono, 1 = full stereo.
    constexpr float kStereoWidth        = 0.55f;  // grain stereo width            [0.0..0.9]

    // Cloud self-sustain (buffer regeneration feedback):
    constexpr float kRegenMax           = 0.85f;  // hard ceiling for stability     [0.78..0.90]
    constexpr float kRegenKnee          = 1.50f;  // Decay(s) where regen ~= 0.5*max [1.0..2.5]
    // Regen highpass corner: raised from a pure DC blocker (~38 Hz) to ~120 Hz so
    // low-mid energy stops ACCUMULATING in the sustaining loop (the "muddy /
    // cluttered" buildup). Higher pole = lower corner; 0.984 ~= 120 Hz @ 48k.
    constexpr float kRegenHpPole        = 0.984f; // regen loop highpass pole (de-mud)

    // ---- Anti-aliasing (mipmap band-limiting of the capture buffer) --------
    // A grain read at rate R shifts its spectrum up by R; anything above
    // Nyquist/R folds back as aliasing. So we band-limit BEFORE the read, with
    // two mip levels (each 4-pole / 24 dB per octave) selected by grain rate.
    // Cutoffs are chosen so the passband edge stays under Nyquist/R for the
    // fastest rate that uses each level, with the 4-pole slope killing the rest.
    constexpr float kMipRate1           = 2.20f;  // rates <= this use mip 1
    constexpr float kMip1Coef           = 0.66f;  // mip 1 LP coef (crystalline shards) [0.45..0.75]
    constexpr float kMip2Coef           = 0.36f;  // mip 2 LP coef (crystalline shards) [0.20..0.45]

    // Hard ceiling on grain playback rate. Was 8x (+36 st), which is both
    // musically pointless (pure shrill fizz) and impossible to keep alias-free.
    // 4.5x ~= +26 semitones: two octaves of shimmer, cleanly band-limitable.
    constexpr float kMaxGrainRate       = 4.50f;  // max playback rate            [3.0..6.0]
    constexpr float kMaxShimmerSemis    = 24.0f;  // cascade pitch cap (+2 octaves)

    // Output "air" high-shelf (adds the polished branded top once aliasing is
    // gone). kAirCoef sets the shelf corner (one-pole HF split); kAirAmount the
    // lift. Applied to the wet signal before the safety limiter.
    // Air: kept MODEST on purpose — the "expensive" character is dark/smooth, not
    // bright. Grain sparkle comes from the crystalline shimmer shards (mips), not
    // a broadband HF boost, so this stays low to avoid a cheap/harsh top.
    constexpr float kAirCoef            = 0.55f;  // ~6 kHz HF split @ 48k         [0.4..0.7]
    constexpr float kAirAmount          = 0.22f;  // air lift amount (subtle)      [0.0..0.4]

    // Wet-sum headroom. grain + wash used to be summed raw, so peaks routinely
    // exceeded 1.0 and got hard-clipped downstream (harsh odd harmonics). The
    // sum is now gain-compensated for how much wash is mixed in, then trimmed,
    // so nominal peaks sit near -3 dBFS and the safety limiter rarely engages.
    constexpr float kWetTrim            = 0.80f;  // final wet trim               [0.6..1.0]
    RB_TUNABLE kWashSumComp        = RB_TUNE ("WSC", 0.55f);  // wash contribution to sum norm [0.4..0.8]

    // Crystals cascade (pitched self-propagating shimmer):
    constexpr float kShimmerShareBase   = 0.30f;  // pitched-grain share, all modes [0.15..0.4]
    constexpr float kShimmerShareCosmic = 0.60f;  // pitched share in Shimmer mode  [0.4..0.75]
    constexpr float kShimmerAmp         = 0.60f;  // level of pitched grains (halo) [0.45..0.75]
    constexpr float kCascadeProbBase    = 0.55f;  // base prob shimmer grain -> child [0.3..0.75]
    constexpr float kCascadeAmp         = 0.68f;  // child amp multiplier           [0.55..0.8]
    constexpr int   kMaxCascadeGen      = 3;      // cascade depth cap (stability)  [2..4]
    constexpr float kChildLenScale      = 0.62f;  // child grain length vs parent   [0.5..0.8]

    // Envelope: short grains tick, long grains bloom:
    constexpr float kPercussiveThreshSec= 0.080f; // grains shorter than this tick  [0.04..0.12]
    constexpr float kPercAttackSeconds  = 0.0006f;// percussive attack (~0.6 ms)    [0.0003..0.0015]
    constexpr float kAttackProportion   = 0.11f;  // long-grain attack prop. (lower = snappier onset) [0.08..0.25]

    // Always-on per-grain randomization:
    constexpr float kBaselineDetuneCents= 6.0f;   // detune spread at Diffusion 0   [3..10]
    constexpr float kModDetuneCents     = 25.0f;  // extra detune at Diffusion 1    [15..35]
    constexpr float kOnsetJitter        = 0.40f;  // +/- fraction of onset spacing  [0.2..0.5]
    constexpr float kAmpJitterFloor     = 0.55f;  // min per-grain amp mult (lower = more alive) [0.4..0.75]

    // Swarm tight-cluster spread (absolute, seconds):
    constexpr float kSwarmSpreadSeconds = 0.15f;  // cluster half-width (s)         [0.08..0.30]

    // Evolving modulation LFOs (sub-Hz, decorrelated); depth scaled by Diffusion:
    constexpr float kOnsetLfoHz         = 0.070f;
    constexpr float kDurLfoHz           = 0.110f;
    constexpr float kFiltLfoHz          = 0.130f;
    constexpr float kOnsetLfoDepth      = 0.50f;  // trigger-rate drift @ Diff 1    [0.2..0.7]
    constexpr float kDurLfoDepth        = 0.30f;  // grain-duration drift @ Diff 1  [0.15..0.5]
    constexpr float kFiltLfoDepth       = 0.50f;  // grain-filter drift @ Diff 1    [0.3..0.8]

    // Blackhole-style unifying reverb wash. Present at ALL densities (sparse
    // grains need gluing MOST) and driven by Diffusion, so it fills the gaps
    // between discrete grains with a continuous lush tail instead of leaving
    // silence (which read as grains "stopping").
    // Voiced for the Eventide Blackhole "expensive" character: BIG, DARK, smooth,
    // deeply modulated — never bright/cheap. Bigger space + longer tail + darker
    // damping + deeper swirl + a pre-delay that separates the dry from the wash.
    constexpr float kReverbMaxRT60      = 11.0f;  // cap on wash decay (s) — huge   [4..16]
    RB_TUNABLE kWashMixBase        = RB_TUNE ("WMB", 0.60f);  // wash level at Diffusion 0  [0.3..0.9]
    RB_TUNABLE kWashMixRange       = RB_TUNE ("WMR", 1.00f);  // extra wash level at Diffusion 1 [0.5..1.4]
    constexpr float kWashModMaxSamples  = 34.0f;  // max delay-mod depth (deep swirl) [12..56]
    constexpr float kWashModBase        = 0.42f;  // baseline swirl even at Diff 0  [0.1..0.6]
    constexpr float kReverbSizeScale    = 1.75f;  // delay-line length multiplier (galaxy size) [1.2..2.4]
    RB_TUNABLE kWashDirectSend     = RB_TUNE ("WDS", 0.25f);  // input -> wash send (every note gets a tail) [0.1..0.4]
    RB_TUNABLE kWashInLpCoef       = RB_TUNE ("WLP", 0.35f);  // LP on wash input (~3.3 kHz; FDN damping still darkens the tail) [0.12..0.5]
    constexpr float kWashDamping        = 0.42f;  // FDN in-loop damping (higher = darker tail) [0.2..0.6]
    constexpr float kWashPreDelayMs     = 28.0f;  // pre-delay before the wash (space/clarity) [0..60]

    // Musical intervals (semitones) grains snap to:
    constexpr float kShimmerIntervalsBase[]   = { 12.0f, 7.0f, 12.0f, 19.0f };
    constexpr float kShimmerIntervalsCosmic[] = { 12.0f, 24.0f, 19.0f, 7.0f, 12.0f, 24.0f };

    constexpr float kNoForcedPitch = 1.0e9f;      // sentinel: "decide shimmer normally"

    // ---- Onset-Reactive layer (grainMode == 5) --------------------------------
    // Onset detector: instant-attack fast follower vs slow follower; onset when
    // (fast-slow) flux exceeds a reactivity-mapped threshold, past a refractory.
    constexpr float kOnsetFastRelMs     = 60.0f;  // fast follower release          [30..120]
    constexpr float kOnsetSlowMs        = 120.0f; // slow follower time             [80..250]
    constexpr float kOnsetThreshHi      = 0.060f; // threshold @ reactivity 0 (insensitive) [0.03..0.12]
    constexpr float kOnsetThreshLo      = 0.004f; // threshold @ reactivity 100 (sensitive)  [0.002..0.01]
    constexpr float kOnsetFloor         = 0.003f; // min level to consider an onset (noise gate)
    constexpr float kOnsetRefractoryMs  = 45.0f;  // min gap between onsets (debounce) [30..80]

    // Burst roll: `burst` knob maps grains-per-onset and roll tightness.
    constexpr int   kBurstMinGrains     = 2;      // grains/onset @ burst 0
    constexpr int   kBurstMaxGrains     = 14;     // grains/onset @ burst 100        [8..20]
    constexpr float kBurstIntervalLooseMs = 90.0f;// grain spacing @ burst 0 (loose) [60..140]
    constexpr float kBurstIntervalTightMs = 14.0f;// grain spacing @ burst 100 (ratchet) [8..25]
    constexpr float kBurstTimingJitter  = 0.60f;  // timing scatter at loose end     [0.3..0.9]
    constexpr float kBurstPosJitterMs   = 8.0f;   // per-grain read jitter around the anchor [4..16]
    constexpr float kBurstAmp           = 1.05f;  // burst grain base amp (a touch hotter than cloud)
    constexpr float kReverseBloomProp   = 0.40f;  // reverse-grain attack proportion (swell-in) [0.25..0.6]

    // Idle fill: sparse cloud grains between onsets so slow passages / freeze
    // aren't dead silent (the wash still carries, this just adds a little life).
    constexpr float kIdleFillMs         = 140.0f; // spacing of idle grains          [90..250]
    constexpr float kIdleFillAmp        = 0.50f;  // idle grain amp (well under bursts) [0.3..0.7]

    // Arp tables (semitone patterns stepped through per burst grain).
    constexpr int   kArpUp[]     = { 0, 7, 12, 19, 24 };
    constexpr int   kArpDown[]   = { 0, -5, -12, -17, -24 };
    constexpr int   kArpUpDown[] = { 0, 7, 12, 19, 24, 19, 12, 7 };
    constexpr int   kArpOct[]    = { 0, 12, 24, 12 };
    constexpr int   kArpRandom[] = { -12, -7, -5, 0, 7, 12, 19, 24 };

    // Scatter/Swarm "lead" grains: this share of spawns reads the freshest audio
    // (the front-loaded draw) instead of the mode's spread/cluster position, so
    // a note you play is heard promptly. Measured before: Scatter median 155 ms
    // (max 518 ms) note->first wet energy vs ~30 ms in Cloud. The rest of the
    // spawns keep the mode's spaced/cluster character.
    constexpr float kLeadShare          = 0.40f;  // Scatter/Swarm lead-grain share [0.2..0.6]

    // Sustain layer (two-grain unison bed, see GrainEngine.h). Window length
    // follows Grain Size within these bounds; each window starts a short, random
    // distance behind the write head (Diffusion widens the scatter).
    constexpr float kSustainGain        = 0.50f;  // bed level at Sustain 100%      [0.3..0.8]
    constexpr float kSustainMinMs       = 60.0f;  // shortest bed window (no buzz)   [40..100]
    constexpr float kSustainMaxMs       = 500.0f; // longest bed window              [300..800]
    constexpr float kSustainBaseMs      = 25.0f;  // min distance behind the input   [15..40]
    constexpr float kSustainJitterMs    = 15.0f;  // read scatter at Diffusion 0     [5..30]
    constexpr float kSustainJitterRange = 60.0f;  // extra scatter at Diffusion 1    [30..100]
    constexpr float kSustainPanSpread   = 0.15f;  // +/- pan per window (gentle width) [0..0.3]

    // Grain Env modes (Auto keeps the original behaviour):
    constexpr float kSwellAttackProp    = 0.80f;  // Swell: slow fade-in, quick fade-out
    constexpr float kPluckAttackSeconds = 0.0015f;// Pluck: near-instant attack at ANY length

    // Raised-cosine lookup table: rise(t) = 0.5*(1 - cos(pi*t)), t in [0,1].
    // Replaces two std::cos calls per grain per sample (attack/release window +
    // retrigger Hann) — the dominant CPU cost at 2x oversampling. Linear
    // interpolation over 4096 points: max error ~1e-7 (far below audibility).
    // Built at DLL load (static init), never on the audio thread.
    struct RaisedCosineTable
    {
        static constexpr int N = 4096;
        float v[N + 2];
        RaisedCosineTable()
        {
            for (int k = 0; k <= N + 1; ++k)
                v[k] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi
                                                 * juce::jmin (1.0f, (float) k / (float) N)));
        }
        float rise (float t) const noexcept
        {
            const float x = juce::jlimit (0.0f, 1.0f, t) * (float) N;
            const int   i = (int) x;
            return v[i] + (x - (float) i) * (v[i + 1] - v[i]);
        }
    };
    const RaisedCosineTable kRaisedCos;
}

GrainEngine::GrainEngine()
{
    rng.setSeedRandomly();
}

// Re-derive a one-pole coefficient / pole so it keeps the SAME cutoff frequency
// at a different sample rate. exp = hostRate/internalRate (1.0 at host rate,
// 0.5 at 2x). For a leaky one-pole y+=a(x-y): a' = 1 - (1-a)^exp. For a highpass
// pole p: p' = p^exp. Both preserve the -3 dB corner in Hz.
static inline float adjCoef (float a, float exp) { return 1.0f - std::pow (1.0f - a, exp); }
static inline float adjPole (float p, float exp) { return std::pow (p, exp); }

// ------------------------------------------------------------------ prepare --
void GrainEngine::prepare (const juce::dsp::ProcessSpec& spec, int oversampleFactor)
{
    sampleRate = (float)spec.sampleRate;

    // Voicing filters made sample-rate-independent so oversampling doesn't
    // brighten the (deliberately dark) Blackhole tail.
    coefExp      = 1.0f / (float) juce::jmax (1, oversampleFactor);
    mip1CoefA    = adjCoef (kMip1Coef,     coefExp);
    mip2CoefA    = adjCoef (kMip2Coef,     coefExp);
    airCoefA     = adjCoef (kAirCoef,      coefExp);
    washInLpA    = adjCoef (kWashInLpCoef, coefExp);
    regenHpPoleA = adjPole (kRegenHpPole,  coefExp);

    bufferSize = (int)(kMaxBufferSeconds * sampleRate);
    bufL.assign ((size_t)bufferSize, 0.0f);
    bufR.assign ((size_t)bufferSize, 0.0f);
    bufM1L.assign ((size_t)bufferSize, 0.0f);
    bufM1R.assign ((size_t)bufferSize, 0.0f);
    bufM2L.assign ((size_t)bufferSize, 0.0f);
    bufM2R.assign ((size_t)bufferSize, 0.0f);
    for (int i = 0; i < 4; ++i) { m1L[i] = m1R[i] = m2L[i] = m2R[i] = 0.0f; }
    airLpL = airLpR = 0.0f;
    washFeedLpL = washFeedLpR = 0.0f;
    washSendSm  = 0.0f;

    // Wash pre-delay line (max ~120 ms).
    preDelaySize  = juce::jmax (1, (int)(0.12f * sampleRate));
    preDelayL.assign ((size_t) preDelaySize, 0.0f);
    preDelayR.assign ((size_t) preDelaySize, 0.0f);
    preDelayPos   = 0;
    preDelaySamps = juce::jlimit (0, preDelaySize - 1, (int)(kWashPreDelayMs * 0.001f * sampleRate));

    writePos = 0;

    for (auto& g : grains)
        g.active = false;
    numActive = 0;
    for (auto& h : sustainHeads) h = SustainHead{};
    sustainNextHead  = 0;
    sustainCountdown = 0.0f;

    // Blackhole-style wash FDN: 8 mutually-prime-ish delay lengths, scaled up by
    // kReverbSizeScale for a bigger space, and given decorrelated sub-Hz
    // modulation rates for the lush chorus-swirl tail.
    {
        const float scale = (sampleRate / 48000.0f) * kReverbSizeScale;
        const int   base[FDN::N] = { 1933, 2179, 2381, 2687, 2999, 3251, 3517, 3821 };
        int         lengths[FDN::N];
        for (int k = 0; k < FDN::N; ++k)
            lengths[k] = juce::jmax (1, (int) std::round (base[k] * scale));
        fdn.prepare (lengths, sampleRate);
        fdn.setDamping (adjCoef (kWashDamping, coefExp));

        // Decorrelated modulation rates (~0.09–0.5 Hz) so the swirl never phase-locks.
        const float modHz[FDN::N] = { 0.09f, 0.13f, 0.17f, 0.23f, 0.29f, 0.37f, 0.43f, 0.51f };
        for (int k = 0; k < FDN::N; ++k)
            fdn.modInc[k] = juce::MathConstants<float>::twoPi * modHz[k] / sampleRate;
    }

    washDiff.prepare (sampleRate);

    // Reactive layer state.
    onsFastEnv = onsSlowEnv = 0.0f;
    onsRefractory = 0;
    idleFillCountdown = 0.0f;
    for (auto& b : bursts) b = Burst{};

    samplesSinceOnset  = 0.0f;
    scatterIndex       = 0;
    swarmPhase         = 0.0f;
    swarmDriftRate     = 0.0015f;

    globalPosMod = 0.0f;
    lfoPhase     = 0.0f;
    lfoInc       = juce::MathConstants<float>::twoPi * 0.18f / sampleRate;

    onsetLfoPhase = durLfoPhase = filtLfoPhase = 0.0f;
    onsetLfoInc = juce::MathConstants<float>::twoPi * kOnsetLfoHz / sampleRate;
    durLfoInc   = juce::MathConstants<float>::twoPi * kDurLfoHz   / sampleRate;
    filtLfoInc  = juce::MathConstants<float>::twoPi * kFiltLfoHz  / sampleRate;
    filtLfoVal  = 0.0f;

    const double rampSec = 0.02;
    smoothGrainSize .reset (spec.sampleRate, rampSec);
    smoothDensity   .reset (spec.sampleRate, rampSec);
    smoothPitch     .reset (spec.sampleRate, rampSec);
    smoothDecay     .reset (spec.sampleRate, rampSec);

    smoothGrainSize .setCurrentAndTargetValue (100.0f);
    smoothDensity   .setCurrentAndTargetValue (50.0f);
    smoothPitch     .setCurrentAndTargetValue (0.0f);
    smoothDecay     .setCurrentAndTargetValue (2.0f);
}

void GrainEngine::reset()
{
    std::fill (bufL.begin(), bufL.end(), 0.0f);
    std::fill (bufR.begin(), bufR.end(), 0.0f);
    std::fill (bufM1L.begin(), bufM1L.end(), 0.0f);
    std::fill (bufM1R.begin(), bufM1R.end(), 0.0f);
    std::fill (bufM2L.begin(), bufM2L.end(), 0.0f);
    std::fill (bufM2R.begin(), bufM2R.end(), 0.0f);
    for (int i = 0; i < 4; ++i) { m1L[i] = m1R[i] = m2L[i] = m2R[i] = 0.0f; }
    airLpL = airLpR = 0.0f;
    washFeedLpL = washFeedLpR = 0.0f;
    washSendSm  = 0.0f;
    std::fill (preDelayL.begin(), preDelayL.end(), 0.0f);
    std::fill (preDelayR.begin(), preDelayR.end(), 0.0f);
    preDelayPos = 0;
    writePos          = 0;
    samplesSinceOnset = 0.0f;
    for (auto& g : grains)
        g.active = false;
    numActive = 0;
    for (auto& h : sustainHeads) h = SustainHead{};
    sustainNextHead  = 0;
    sustainCountdown = 0.0f;
    fdn.clear();
    washDiff.clear();

    onsFastEnv = onsSlowEnv = 0.0f;
    onsRefractory = 0;
    idleFillCountdown = 0.0f;
    for (auto& b : bursts) b = Burst{};

    regenHpXL = regenHpYL = 0.0f;
    regenHpXR = regenHpYR = 0.0f;
}

// ---------------------------------------------------------- parameter setters --
void GrainEngine::setGrainSizeMs    (float ms)  { smoothGrainSize .setTargetValue (ms);  }
void GrainEngine::setGrainDensity   (float pct)  { smoothDensity   .setTargetValue (pct); }
// Pitch arrives already snapped to an interval (see PitchSnap.h). It is applied
// immediately, not ramped: a grain takes its pitch once, at spawn, so a ramp only
// produced off-interval grains during the 20 ms glide between two steps.
void GrainEngine::setPitchSemitones (float st)   { smoothPitch     .setCurrentAndTargetValue (st); }
void GrainEngine::setDecaySeconds   (float s)    { smoothDecay     .setTargetValue (s);   }
void GrainEngine::setGrainMode      (int mode)   { grainMode.store (mode); }
void GrainEngine::setFrozen         (bool f)     { frozen   .store (f);    }
void GrainEngine::setModDepth       (float d)    { modDepth .store (juce::jlimit (0.0f, 1.0f, d)); }
void GrainEngine::setReactivity     (float pct)  { reactivity  .store (juce::jlimit (0.0f, 100.0f, pct)); }
void GrainEngine::setBurst          (float pct)  { burstAmt    .store (juce::jlimit (0.0f, 100.0f, pct)); }
void GrainEngine::setArpMode        (int mode)   { arpMode     .store (mode); }
void GrainEngine::setPlaybackMode   (int mode)   { playbackMode.store (mode); }
void GrainEngine::setSustain        (float pct)  { sustainAmt  .store (juce::jlimit (0.0f, 100.0f, pct) / 100.0f); }
void GrainEngine::setGrainEnvelope  (int mode)   { envMode     .store (juce::jlimit (0, 3, mode)); }

// ------------------------------------------------------------------ helpers --
int GrainEngine::findFreeGrain() const
{
    for (int i = 0; i < kMaxGrains; ++i)
        if (!grains[i].active) return i;
    return -1;
}

// Hermite (Catmull-Rom) 4-point interpolation.
float GrainEngine::interpolate (const std::vector<float>& buf, float pos) const
{
    int p1 = (int)std::floor (pos);
    float frac = pos - (float)p1;

    float ym1, y0, y1, y2;
    if (p1 >= 1 && p1 + 2 < bufferSize)
    {
        // Fast path (virtually every call): all four taps are in range, so skip
        // the four integer modulo operations. Same taps, same result.
        const float* b = buf.data() + p1;
        ym1 = b[-1]; y0 = b[0]; y1 = b[1]; y2 = b[2];
    }
    else
    {
        auto wrap = [this] (int p) { return ((p % bufferSize) + bufferSize) % bufferSize; };
        ym1 = buf[(size_t) wrap (p1 - 1)];
        y0  = buf[(size_t) wrap (p1)];
        y1  = buf[(size_t) wrap (p1 + 1)];
        y2  = buf[(size_t) wrap (p1 + 2)];
    }

    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

float GrainEngine::grainWindow (int progress, int length, int attackLen)
{
    if (length <= 1) return 1.0f;
    // Up to length-1 (was length/2) so the Swell envelope can use a long attack;
    // every pre-existing caller already passes <= length/2, so they're unchanged.
    attackLen = juce::jlimit (1, juce::jmax (1, length - 1), attackLen);

    // Same raised-cosine attack/release as before, via the lookup table.
    if (progress < attackLen)
        return kRaisedCos.rise ((float) progress / (float) attackLen);           // 0.5(1 - cos πt)

    int   rel = juce::jmax (1, length - attackLen);
    float t   = (float) (progress - attackLen) / (float) rel;
    return kRaisedCos.rise (1.0f - t);                                          // 0.5(1 + cos πt)
}

// Grain Env selector. Window energy is the same for every split of attack vs
// release (a raised-cosine rise + fall always integrates to length/2), so
// switching modes changes the shape, not the level.
int GrainEngine::envelopeAttack (float grainLenSamples, int length) const
{
    switch (envMode.load())
    {
        case 1:  return juce::jmax (1, length / 2);                                  // Smooth: symmetric Hann
        case 2:  return juce::jlimit (1, length - 1, (int) (kSwellAttackProp * length)); // Swell
        case 3:  return juce::jlimit (1, length / 2, (int) (kPluckAttackSeconds * sampleRate)); // Pluck
        default: break;                                                              // Auto (original)
    }
    if (grainLenSamples < kPercussiveThreshSec * sampleRate)
        return juce::jmax (1, (int) (kPercAttackSeconds * sampleRate));
    return juce::jlimit (1, length / 2, (int) (kAttackProportion * grainLenSamples));
}

// ---------------------------------------------------------------- spawnGrain --
void GrainEngine::spawnGrain (float grainLenSamples, int gen, float ampScale, float forcedSemis)
{
    int idx = findFreeGrain();
    if (idx < 0) return;

    Grain& g   = grains[idx];
    const int  mode = grainMode.load();
    const float modD = modDepth.load();
    const bool  isChild = (forcedSemis != kNoForcedPitch);

    // --- WIDE position spray window ---------------------------------------
    // Forward-only now: a grain at rate r>1 would overtake the head, so the
    // floor is L*(r-1); at unity pitch that's ~0. Everything above the floor is
    // the spray span (Decay sets depth, Diffusion widens).
    const float pr     = blockPitchRate;
    const float margin = 0.003f * sampleRate;
    const float minOffset = grainLenSamples * juce::jmax (0.0f, pr - 1.0f) + margin;

    const float baseSpray  = kSprayBaseSeconds * sampleRate;
    const float decaySpray = blockDecayInSamples * kSprayDecayFraction;
    float sprayDepth = (baseSpray + decaySpray)
                       * (kSprayDiffusionBase + kSprayDiffusionRange * modD);

    const float maxSpray = (float) bufferSize - minOffset - grainLenSamples - 4.0f;
    sprayDepth = juce::jlimit (1.0f, juce::jmax (1.0f, maxSpray), sprayDepth);

    const float winLo   = minOffset;
    const float winHi   = juce::jmin (winLo + sprayDepth, (float)(bufferSize - 1));
    const float winSpan = juce::jmax (1.0f, winHi - winLo);

    // Recent-biased random draw: a large share (kFreshShare) of grains land in
    // the freshest slice of the window (immediate response = playability), while
    // the rest spread quadratically across the spray for the tail texture. This
    // keeps the leading edge tracking the input tightly regardless of how deep
    // Decay pushes the spray window.
    auto recentDraw = [this]
    {
        float u = rng.nextFloat();
        if (u < kFreshShare)
            return (u / kFreshShare) * kFreshDepth;                       // fresh cluster
        float v = (u - kFreshShare) / (1.0f - kFreshShare);
        return kFreshDepth + v * v * (1.0f - kFreshDepth);                // tail reach
    };

    float offset = winLo + recentDraw() * winSpan;   // default / child

    if (! isChild)
    {
        switch (mode)
        {
            case 0: // Cloud – recent-biased fill
                offset = winLo + recentDraw() * winSpan;
                break;

            case 1: // Scatter – evenly-spaced zones across the wide window
            {
                int   zones    = 8;
                float zoneSize = winSpan / (float) zones;
                float center   = (float)(scatterIndex % zones) * zoneSize + zoneSize * 0.5f;
                offset = winLo + center + (rng.nextFloat() - 0.5f) * zoneSize * 0.6f;
                // Pull toward recent for playability (keeps the zoned shape).
                offset = winLo + (offset - winLo) * kSpreadModeResponse;
                // Lead grains: a share reads the freshest audio so the note is
                // heard promptly; the zoned spray continues around them.
                if (rng.nextFloat() < kLeadShare)
                    offset = winLo + recentDraw() * winSpan;
                ++scatterIndex;
                break;
            }

            case 2: // Shimmer – recent-biased position; identity is the pitch share
                offset = winLo + recentDraw() * winSpan;
                break;

            case 3: // Pulse – recent-biased position (rigid timing in scheduler)
                offset = winLo + recentDraw() * winSpan;
                break;

            case 4: // Swarm – tight cluster around a drifting point
            {
                float center       = winLo + swarmPhase * winSpan;
                float narrowSpread = juce::jmin (winSpan * 0.5f, kSwarmSpreadSeconds * sampleRate);
                offset = center + (rng.nextFloat() - 0.5f) * 2.0f * narrowSpread;
                // Pull toward recent for playability (keeps the drifting cluster).
                offset = winLo + (offset - winLo) * kSpreadModeResponse;
                // Lead grains (see kLeadShare): prompt response to new notes.
                if (rng.nextFloat() < kLeadShare)
                    offset = winLo + recentDraw() * winSpan;
                break;
            }
        }
    }

    offset += globalPosMod;
    offset = juce::jlimit (winLo, winHi, offset);

    // Gentle distance taper (far-back grains a touch quieter).
    float decayAmp = std::exp (-0.7f * (offset - winLo) / winSpan);

    // --- pitch: baseline detune + musical shimmer interval ----------------
    float detuneCents = (rng.nextFloat() - 0.5f) * 2.0f
                        * (kBaselineDetuneCents + kModDetuneCents * modD);

    float musicalSemis = 0.0f;
    if (isChild)
    {
        musicalSemis = forcedSemis;   // cascade child: forced octave-up shard
    }
    else
    {
        const float shimmerShare = (mode == 2) ? kShimmerShareCosmic : kShimmerShareBase;
        if (rng.nextFloat() < shimmerShare)
        {
            if (mode == 2)
            {
                constexpr int n = (int)(sizeof (kShimmerIntervalsCosmic) / sizeof (float));
                musicalSemis = kShimmerIntervalsCosmic[rng.nextInt (n)];
            }
            else
            {
                constexpr int n = (int)(sizeof (kShimmerIntervalsBase) / sizeof (float));
                musicalSemis = kShimmerIntervalsBase[rng.nextInt (n)];
            }
        }
    }
    const bool isShimmerGrain = (musicalSemis != 0.0f);

    float rate = blockPitchRate * std::pow (2.0f, (detuneCents + musicalSemis * 100.0f) / 1200.0f);
    // Rate capped so every grain stays inside what the mipmap can band-limit
    // cleanly (was 8x, which aliased no matter how the buffer was filtered).
    rate = juce::jlimit (0.25f, kMaxGrainRate, rate);

    // Read start = writePos - offset (forward).
    float bufStart = (float)(writePos - (int) offset);
    while (bufStart < 0.0f)               bufStart += (float)bufferSize;
    while (bufStart >= (float)bufferSize)  bufStart -= (float)bufferSize;

    // --- intra-grain retrigger --------------------------------------------
    // Pitched-up grains replay their source span within the grain length at the
    // pitch ratio: loopLen (output samples per loop) = length / rate, so rate=2
    // (octave) replays the span twice. Each loop is Hann-windowed in the render
    // loop, so the retrigger is click-free. Non-pitched grains: one loop.
    const int   length  = juce::jmax (4, (int) grainLenSamples);
    const bool  retrig  = (rate > 1.05f);
    const float loopLen = retrig ? juce::jmax (16.0f, (float) length / rate)
                                 : (float) length;

    // --- envelope: Grain Env mode (Auto = percussive tick for short grains,
    //     bloom for long, exactly as before) --------------------------------
    const int attackLen = envelopeAttack (grainLenSamples, length);

    // --- amplitude: per-grain jitter, NO flat normalization ---------------
    float ampJitter = kAmpJitterFloor + (1.0f - kAmpJitterFloor) * rng.nextFloat();
    float amp = decayAmp * ampJitter * ampScale * (isShimmerGrain ? kShimmerAmp : 1.0f);

    // --- pan: full-width per-grain ----------------------------------------
    float pan   = rng.nextFloat();
    float angle = pan * 0.5f * juce::MathConstants<float>::pi;

    // --- evolving grain filter (distance + filter LFO) --------------------
    // Eased (0.6 -> 0.4) so grains stay clearer/airier — the anti-alias dark
    // buffer, not heavy haze, is what now tames the pitched grains, so we don't
    // need to darken everything to hide grit.
    float distance01 = juce::jlimit (0.0f, 1.0f, (offset - winLo) / winSpan);
    float hazeAmt    = distance01 * 0.4f * (0.4f + 0.6f * modD) + filtLfoVal;
    hazeAmt          = juce::jlimit (0.0f, 0.7f, hazeAmt);
    if (isShimmerGrain) hazeAmt *= 0.4f;      // keep shards crystalline/bright

    g.readPos    = bufStart;
    g.loopStart  = bufStart;
    g.rate       = rate;
    g.loopLen    = loopLen;
    g.loopPhase  = 0.0f;
    g.length     = length;
    g.progress   = 0;
    g.amplitude  = amp;
    g.gainL      = std::cos (angle);
    g.gainR      = std::sin (angle);
    g.attackLen  = attackLen;
    g.retrig     = retrig;
    // Anti-alias mip selection: pick the band-limited mirror whose cutoff sits
    // below Nyquist/rate for this grain's playback rate. Unpitched grains read
    // the full-band buffer so they stay crisp.
    g.mip        = (rate <= 1.05f)      ? 0
                 : (rate <= kMipRate1)  ? 1
                                        : 2;
    g.gen        = gen;
    g.active     = true;
    activeList[numActive++] = idx;   // idx came from findFreeGrain(), so never a duplicate
    g.hazeLp     = 0.0f;
    g.hazeCoef   = 1.0f - hazeAmt;

    // --- Crystals cascade -------------------------------------------------
    // A shimmer grain seeds an octave-HIGHER child (bounded depth, decaying amp)
    // so pitched shards accumulate into a climbing glassy halo. Probability
    // scales with Decay (via blockCascadeProb) so longer Decay climbs higher.
    if (isShimmerGrain && gen < kMaxCascadeGen && rng.nextFloat() < blockCascadeProb)
    {
        // Next octave up, but capped: an uncapped chain reached +60 semitones
        // (32x), which is pure shrill fizz and un-band-limitable.
        float childSemis = juce::jmin (musicalSemis + 12.0f, kMaxShimmerSemis);
        spawnGrain (grainLenSamples * kChildLenScale,
                    gen + 1, ampScale * kCascadeAmp, childSemis);
    }
}

// -------------------------------------------------------------- spawnBurstGrain --
// Reactive burst voice grain. Reads the most-recent captured window — the anchor
// is `writePos - length - margin`, so the whole grain (forward, reverse, or
// intra-grain-retriggered) always reads BEHIND the write head and can never
// touch not-yet-written samples (RT-safe) while still replaying the note you
// just played. Shares the Grain render path, so it flows through the same
// mipmap / window / wash / limiter as every other grain.
void GrainEngine::spawnBurstGrain (float semis, float octaveMul, bool reverse,
                                   float ampScale, float grainLenSamples)
{
    int idx = findFreeGrain();
    if (idx < 0) return;                       // pool full: skip (graceful cap)

    Grain& g = grains[idx];

    const int   length = juce::jmax (4, (int) grainLenSamples);

    // Pitch = base PITCH knob * arp interval * playback-octave, plus a little
    // per-grain detune for life. Magnitude clamped to what the mipmap covers.
    const float detuneCents = (rng.nextFloat() - 0.5f) * 2.0f * kBaselineDetuneCents;
    float rateMag = blockPitchRate
                    * std::pow (2.0f, (detuneCents + semis * 100.0f) / 1200.0f)
                    * octaveMul;
    rateMag = juce::jlimit (0.25f, kMaxGrainRate, rateMag);

    // Anchor the read window behind the head so all reads are already written.
    const float margin  = 0.005f * sampleRate;
    const float posJit  = (rng.nextFloat() - 0.5f) * 2.0f * kBurstPosJitterMs * 0.001f * sampleRate;
    float anchor = (float) writePos - (float) length - margin + posJit;
    while (anchor < 0.0f)               anchor += (float) bufferSize;
    while (anchor >= (float) bufferSize) anchor -= (float) bufferSize;

    // Reverse reads the same window backward (swell into the note); forward
    // starts at the anchor. Retrigger only for pitched-up FORWARD grains.
    const bool  retrig  = (! reverse && rateMag > 1.05f);
    const float loopLen = retrig ? juce::jmax (16.0f, (float) length / rateMag)
                                 : (float) length;
    float readStart = reverse ? (anchor + (float) length) : anchor;
    while (readStart >= (float) bufferSize) readStart -= (float) bufferSize;

    // Envelope: reverse grains bloom (slow swell in); pitched-up/forward grains
    // tick or bloom by length, exactly like the cloud grains.
    int attackLen;
    if (reverse)
        attackLen = juce::jlimit (1, length - 1, (int)(kReverseBloomProp * length));
    else
        attackLen = envelopeAttack (grainLenSamples, length);

    const float pan   = rng.nextFloat();
    const float angle = pan * 0.5f * juce::MathConstants<float>::pi;

    g.readPos    = readStart;
    g.loopStart  = anchor;
    g.rate       = reverse ? -rateMag : rateMag;
    g.loopLen    = loopLen;
    g.loopPhase  = 0.0f;
    g.length     = length;
    g.progress   = 0;
    g.amplitude  = ampScale * kBurstAmp;
    g.gainL      = std::cos (angle);
    g.gainR      = std::sin (angle);
    g.attackLen  = attackLen;
    g.retrig     = retrig;
    g.mip        = (rateMag <= 1.05f)     ? 0     // anti-alias by |rate|
                 : (rateMag <= kMipRate1) ? 1
                                          : 2;
    g.gen        = 0;
    g.active     = true;
    activeList[numActive++] = idx;
    g.hazeLp     = 0.0f;
    g.hazeCoef   = 1.0f;                          // bursts stay upfront/crisp (no distance haze)
}

// ------------------------------------------------------------------- process --
void GrainEngine::process (const juce::AudioBuffer<float>& inputBuffer,
                            juce::AudioBuffer<float>&       wetBuffer)
{
    const int numSamples  = inputBuffer.getNumSamples();
    const int numInCh     = inputBuffer.getNumChannels();

    wetBuffer.clear();
    if (bufferSize == 0) return;

    // Advance the smoothers by the WHOLE block. These ramps are configured in
    // samples (20 ms), but were previously advanced one step per BLOCK, so a
    // knob change took ~1900 blocks (measured 2-5 s, up to ~20 s to settle at
    // a 512 buffer) to arrive. skip(n) == n x getNextValue(): now 20 ms.
    const float grainLenMs = smoothGrainSize.skip (numSamples);
    const float densityPct = smoothDensity  .skip (numSamples);
    const float pitchSt    = smoothPitch    .skip (numSamples);
    const float decaySec   = smoothDecay    .skip (numSamples);

    const float decayInSamples = decaySec * sampleRate;

    // --- evolving modulation LFOs (advance once per block) ---
    const float evoDepth = modDepth.load();
    auto advance = [numSamples] (float& phase, float inc)
    {
        phase += inc * (float) numSamples;
        if (phase >= juce::MathConstants<float>::twoPi)
            phase -= juce::MathConstants<float>::twoPi;
        return std::sin (phase);
    };
    const float onsetLfo = advance (onsetLfoPhase, onsetLfoInc);
    const float durLfo   = advance (durLfoPhase,   durLfoInc);
    const float filtLfo  = advance (filtLfoPhase,  filtLfoInc);
    filtLfoVal = filtLfo * kFiltLfoDepth * evoDepth * 0.5f;

    // Grain duration drifts with its LFO (bounded).
    float grainLenSamples = grainLenMs * sampleRate / 1000.0f;
    grainLenSamples *= (1.0f + durLfo * kDurLfoDepth * evoDepth);
    grainLenSamples  = juce::jmax (4.0f, grainLenSamples);

    // --- SCHEDULER: Density -> onset GAP, decoupled from grain length ---
    // Exponential map so the sparse<->dense sweep is perceptually even. At low
    // density the gap is hundreds of ms (discrete, countable droplets); at high
    // density a few ms (fused cloud).
    const float d01 = juce::jlimit (0.0f, 1.0f, (densityPct - 1.0f) / 99.0f);
    float onsetInterval = kOnsetMaxSeconds
                          * std::pow (kOnsetMinSeconds / kOnsetMaxSeconds, d01)
                          * sampleRate;
    // Trigger-rate drift (evolving), Pulse mode excluded below via jitter gate.
    onsetInterval *= (1.0f + onsetLfo * kOnsetLfoDepth * evoDepth);
    onsetInterval  = juce::jmax (2.0f, onsetInterval);

    // --- dynamic (non-flat) output gain ---
    // Overlap = grainLen / onsetGap. Normalizing by overlap^kNormExp with
    // kNormExp < 0.5 lets dense clouds surge louder than sparse ones while
    // staying bounded; the limiter catches peaks.
    const float overlap  = juce::jmax (1.0f, grainLenSamples / onsetInterval);
    const float cloudGain = kOutputTrim / std::pow (overlap, kNormExp);

    // Blackhole wash: long tail driven by Decay (huge cap), present at ALL
    // densities and driven by Diffusion so it fills the gaps between discrete
    // grains with continuous lush reverb (this is what stops grains sounding
    // like they abruptly "stop"). Modulation depth (the swirl) rises with
    // Diffusion, with a baseline so the tail is never static even at Diffusion 0.
    fdn.setFeedback (juce::jmin (decaySec, kReverbMaxRT60), sampleRate);
    // Depth is in SAMPLES, voiced at the host rate; divide by coefExp so the swirl
    // keeps the same depth in time when the engine runs oversampled (it was
    // silently halved at 2x).
    fdn.setModDepth ((kWashModMaxSamples / coefExp) * (kWashModBase + (1.0f - kWashModBase) * evoDepth));
    const float washMix = kWashMixBase + kWashMixRange * evoDepth;
    juce::ignoreUnused (d01);

    // Regeneration self-sustain (curved by Decay, capped for stability).
    const float regen = juce::jlimit (0.0f, kRegenMax,
                            kRegenMax * (decaySec / (decaySec + kRegenKnee)));

    // Per-block context for spawnGrain / cascade.
    blockPitchRate     = std::pow (2.0f, pitchSt / 12.0f);
    blockDecayInSamples = decayInSamples;
    blockCascadeProb   = kCascadeProbBase * (decaySec / (decaySec + kRegenKnee));

    const int  modeThisBlock = grainMode.load();
    const bool isFrozen      = frozen.load();

    // Swarm drift.
    swarmPhase += swarmDriftRate;
    if (swarmPhase >= 1.0f) swarmPhase -= 1.0f;

    // Global position drift (smear), depth from Diffusion.
    lfoPhase += lfoInc * (float) numSamples;
    if (lfoPhase >= juce::MathConstants<float>::twoPi)
        lfoPhase -= juce::MathConstants<float>::twoPi;
    globalPosMod = std::sin (lfoPhase) * evoDepth * grainLenSamples * 0.5f;

    // Onset spacing for the NEXT grain, re-jittered per spawn (Pulse stays rigid).
    float nextOnset = onsetInterval;

    // ---- Sustain layer block context ----
    const float susTarget  = kSustainGain * sustainAmt.load();
    const bool  susRun     = (susTarget > 0.0f || sustainGainSm > 1.0e-5f);
    const int   susLen     = juce::jmax (8, (int) (juce::jlimit (kSustainMinMs, kSustainMaxMs, grainLenMs)
                                                   * 0.001f * sampleRate));
    const int   susBase    = (int) (kSustainBaseMs * 0.001f * sampleRate);
    const float susJit     = (kSustainJitterMs + kSustainJitterRange * evoDepth) * 0.001f * sampleRate;
    const float susSmCoef  = 1.0f - std::exp (-1.0f / (0.02f * sampleRate));   // 20 ms de-zipper
    if (! susRun)
    {
        // Idle: park the heads so turning Sustain up restarts them cleanly.
        sustainHeads[0].active = sustainHeads[1].active = false;
        sustainCountdown = 0.0f;
        sustainGainSm    = 0.0f;
    }

    // ---- Reactive-mode (grainMode == 5) block context ----
    // Computed regardless (cheap), but the per-sample onset/burst code below only
    // runs when `reactive` is true, so modes 0-4 are byte-for-byte unchanged.
    const bool  reactive = (modeThisBlock == 5);
    const float react01  = juce::jlimit (0.0f, 1.0f, reactivity.load() / 100.0f);
    const float onsThresh = kOnsetThreshHi + (kOnsetThreshLo - kOnsetThreshHi) * react01;
    const float onsFastRelCoef = 1.0f - std::exp (-1.0f / (kOnsetFastRelMs * 0.001f * sampleRate));
    const float onsSlowCoef     = 1.0f - std::exp (-1.0f / (kOnsetSlowMs     * 0.001f * sampleRate));
    const int   refractorySamps = (int)(kOnsetRefractoryMs * 0.001f * sampleRate);

    const float b01 = juce::jlimit (0.0f, 1.0f, burstAmt.load() / 100.0f);
    const int   burstGrains   = (int) std::round (kBurstMinGrains + (kBurstMaxGrains - kBurstMinGrains) * b01);
    const float burstInterval = (kBurstIntervalLooseMs + (kBurstIntervalTightMs - kBurstIntervalLooseMs) * b01)
                                * 0.001f * sampleRate;
    const float burstJitter   = kBurstTimingJitter * (1.0f - b01);
    const int   arp    = arpMode.load();
    const int   pbMode = playbackMode.load();

    // Arp table lookup (semitone for the given step).
    auto arpSemis = [this, arp] (int step) -> float
    {
        switch (arp)
        {
            case 1: { constexpr int n = (int)(sizeof (kArpUp)     / sizeof (int)); return (float) kArpUp    [((step % n) + n) % n]; }
            case 2: { constexpr int n = (int)(sizeof (kArpDown)   / sizeof (int)); return (float) kArpDown  [((step % n) + n) % n]; }
            case 3: { constexpr int n = (int)(sizeof (kArpUpDown) / sizeof (int)); return (float) kArpUpDown[((step % n) + n) % n]; }
            case 4: { constexpr int n = (int)(sizeof (kArpRandom) / sizeof (int)); return (float) kArpRandom[rng.nextInt (n)]; }
            case 5: { constexpr int n = (int)(sizeof (kArpOct)    / sizeof (int)); return (float) kArpOct   [((step % n) + n) % n]; }
            default: return 0.0f;   // Off
        }
    };

    // Playback-mode -> (octave multiplier, reverse flag), Random picks per grain.
    auto playback = [this, pbMode] (float& octaveMul, bool& reverse)
    {
        int m = (pbMode == 4) ? rng.nextInt (4) : pbMode;
        switch (m)
        {
            case 1: octaveMul = 2.0f; reverse = false; break;   // Oct Up
            case 2: octaveMul = 0.5f; reverse = false; break;   // Oct Down
            case 3: octaveMul = 1.0f; reverse = true;  break;   // Reverse
            default: octaveMul = 1.0f; reverse = false; break;  // Normal
        }
    };

    for (int i = 0; i < numSamples; ++i)
    {
        float inL = (numInCh > 0) ? inputBuffer.getSample (0, i) : 0.0f;
        float inR = (numInCh > 1) ? inputBuffer.getSample (1, i) : inL;

        if (! reactive)
        {
            // ---- Modes 0-4: the existing free-running density timer (unchanged) ----
            samplesSinceOnset += 1.0f;
            if (samplesSinceOnset >= nextOnset)
            {
                spawnGrain (grainLenSamples, 0, 1.0f, kNoForcedPitch);
                samplesSinceOnset -= nextOnset;

                float jit = (modeThisBlock == 3)   // Pulse: rigid grid, no jitter
                              ? 0.0f
                              : (rng.nextFloat() - 0.5f) * 2.0f * kOnsetJitter;
                nextOnset = juce::jmax (2.0f, onsetInterval * (1.0f + jit));
            }
        }
        else
        {
            // ---- Mode 5 (Reactive): onset detector -> burst rolls + idle fill ----
            const float rect = std::abs (0.5f * (inL + inR));
            if (rect > onsFastEnv) onsFastEnv = rect;                       // instant attack
            else                   onsFastEnv += onsFastRelCoef * (rect - onsFastEnv);
            onsSlowEnv += onsSlowCoef * (rect - onsSlowEnv);
            if (onsRefractory > 0) --onsRefractory;

            const float flux = onsFastEnv - onsSlowEnv;
            if (! isFrozen && flux > onsThresh && onsFastEnv > kOnsetFloor && onsRefractory <= 0)
            {
                // Claim a burst voice (free slot, else steal the one with the
                // fewest grains left so a long stale roll yields to a fresh note).
                int slot = -1, fewest = (1 << 30);
                for (int b = 0; b < kMaxBursts; ++b)
                {
                    if (! bursts[b].active) { slot = b; break; }
                    if (bursts[b].remaining < fewest) { fewest = bursts[b].remaining; slot = b; }
                }
                Burst& bv   = bursts[slot];
                bv.active    = true;
                bv.remaining = burstGrains;
                bv.interval  = juce::jmax (2.0f, burstInterval);
                bv.jitter    = burstJitter;
                bv.countdown = 0.0f;          // first grain fires immediately (on the transient)
                bv.arpIndex  = 0;
                bv.amp       = 1.0f;
                bv.grainLen  = grainLenSamples;
                onsRefractory = refractorySamps;
            }

            // Service active burst voices: emit due grains one at a time.
            for (int b = 0; b < kMaxBursts; ++b)
            {
                Burst& bv = bursts[b];
                if (! bv.active) continue;
                bv.countdown -= 1.0f;
                if (bv.countdown <= 0.0f)
                {
                    float octaveMul; bool reverse;
                    playback (octaveMul, reverse);
                    const float semis = arpSemis (bv.arpIndex);
                    spawnBurstGrain (semis, octaveMul, reverse, bv.amp, bv.grainLen);

                    ++bv.arpIndex;
                    if (--bv.remaining <= 0) { bv.active = false; }
                    else
                    {
                        const float jt = 1.0f + (rng.nextFloat() - 0.5f) * 2.0f * bv.jitter;
                        bv.countdown += juce::jmax (2.0f, bv.interval * jt);
                    }
                }
            }

            // Sparse idle fill so slow passages / freeze aren't dead silent
            // between bursts (reads recent/held audio via the normal spawn path).
            idleFillCountdown -= 1.0f;
            if (idleFillCountdown <= 0.0f)
            {
                spawnGrain (grainLenSamples, 0, kIdleFillAmp, kNoForcedPitch);
                idleFillCountdown = kIdleFillMs * 0.001f * sampleRate;
            }
        }

        // --- render active grains ---
        float outL = 0.0f, outR = 0.0f;

        // Iterate only the ACTIVE grains (compact index list) instead of
        // scanning all kMaxGrains slots every sample — at 2x oversampling that
        // scan alone was ~25M slot checks/s. Dead grains are swap-removed below.
        for (int j = 0; j < numActive; )
        {
            Grain& g = grains[activeList[j]];

            // Pitched-up grains read the band-limited buffer (anti-alias); the
            // fundamental/forward grains read full-band and stay crisp. The grain
            // is summed to MONO and then PANNED across the field via gainL/gainR,
            // so grains always dance across BOTH speakers regardless of whether
            // the input is mono or one channel is silent (fixes left-only grains).
            const std::vector<float>& srcL = (g.mip == 0) ? bufL : (g.mip == 1 ? bufM1L : bufM2L);
            const std::vector<float>& srcR = (g.mip == 0) ? bufR : (g.mip == 1 ? bufM1R : bufM2R);
            float sL = interpolate (srcL, g.readPos);
            float sR = interpolate (srcR, g.readPos);

            // Evolving grain filter (distance/LFO haze) on the mono core.
            float s = 0.5f * (sL + sR);
            if (g.hazeCoef < 0.999f)
            {
                g.hazeLp += g.hazeCoef * (s - g.hazeLp);
                float d = (s * g.hazeCoef + g.hazeLp * (1.0f - g.hazeCoef)) - s;
                sL += d; sR += d;   // apply the haze delta to both channels
                s  += d;
            }

            // Stereo width: mono core (mono-safe) + a blend toward the true L/R
            // stereo of the buffer, for a wide/cinematic cloud that never
            // collapses to one channel even on mono input.
            float wL = s + kStereoWidth * (sL - s);
            float wR = s + kStereoWidth * (sR - s);

            // Outer envelope (attack/bloom + release) over the whole grain.
            float outer = grainWindow (g.progress, g.length, g.attackLen);

            // Inner envelope: Hann per retrigger loop (click-free stutter). For
            // non-retriggering grains this is ~1 (outer controls the shape).
            float inner = 1.0f;
            if (g.retrig)
            {
                // Full Hann 0.5(1-cos 2πph) via the raised-cosine table (no
                // per-sample std::cos): symmetric about ph = 0.5.
                float ph = g.loopPhase / g.loopLen;                 // 0..1
                inner = kRaisedCos.rise (ph <= 0.5f ? 2.0f * ph : 2.0f * (1.0f - ph));
            }

            float amp = g.amplitude * outer * inner;
            outL += wL * amp * g.gainL;
            outR += wR * amp * g.gainR;

            // Advance read + retrigger loop.
            g.readPos   += g.rate;
            g.loopPhase += 1.0f;
            if (g.retrig && g.loopPhase >= g.loopLen)
            {
                g.loopPhase -= g.loopLen;
                g.readPos    = g.loopStart;    // retrigger: replay the span
            }
            while (g.readPos < 0.0f)              g.readPos += (float)bufferSize;
            while (g.readPos >= (float)bufferSize) g.readPos -= (float)bufferSize;

            ++g.progress;
            if (g.progress >= g.length)
            {
                g.active = false;
                activeList[j] = activeList[--numActive];   // swap-remove; re-check slot j
            }
            else
            {
                ++j;
            }
        }

        float grainL = outL * cloudGain;
        float grainR = outR * cloudGain;

        // --- Sustain layer: two unison heads, half a window apart (COLA) ---
        float susL = 0.0f, susR = 0.0f;
        if (susRun)
        {
            sustainGainSm += susSmCoef * (susTarget - sustainGainSm);

            sustainCountdown -= 1.0f;
            if (sustainCountdown <= 0.0f)
            {
                SustainHead& h = sustainHeads[sustainNextHead];
                sustainNextHead ^= 1;

                // Start a short random distance behind the input. When frozen the
                // write head stops, so start a full window further back: the head
                // then never runs past the frozen write point into old audio.
                int off = susBase + (int) (rng.nextFloat() * susJit);
                if (isFrozen) off += susLen;
                int rp = writePos - off;
                while (rp < 0) rp += bufferSize;

                const float pan   = 0.5f + (rng.nextFloat() - 0.5f) * 2.0f * kSustainPanSpread;
                const float angle = pan * 0.5f * juce::MathConstants<float>::pi;
                h.readPos  = rp;
                h.progress = 0;
                h.length   = susLen;
                h.gainL    = std::cos (angle) * juce::MathConstants<float>::sqrt2;   // unity at centre
                h.gainR    = std::sin (angle) * juce::MathConstants<float>::sqrt2;
                h.active   = true;

                sustainCountdown += 0.5f * (float) susLen;   // next head at the half-way point
            }

            for (auto& h : sustainHeads)
            {
                if (! h.active) continue;
                const float ph = (float) h.progress / (float) h.length;
                const float w  = kRaisedCos.rise (ph <= 0.5f ? 2.0f * ph : 2.0f * (1.0f - ph));   // Hann
                const float hl = bufL[(size_t) h.readPos];
                const float hr = bufR[(size_t) h.readPos];
                const float m  = 0.5f * (hl + hr);
                susL += (m + kStereoWidth * (hl - m)) * w * h.gainL;
                susR += (m + kStereoWidth * (hr - m)) * w * h.gainR;

                if (++h.readPos >= bufferSize) h.readPos = 0;
                if (++h.progress >= h.length)  h.active = false;
            }
            susL *= sustainGainSm;
            susR *= sustainGainSm;
        }

        // Cloud + bed: this is what is heard and what feeds the wash. The regen
        // write further down uses the cloud alone (grainL/R), so the bed never
        // raises the feedback loop's gain.
        const float layerL = grainL + susL;
        const float layerR = grainR + susR;

        // Wash PRE-DELAY: the reverb tail is fed a delayed copy of the grain
        // signal, so the dry/grains are heard first and the huge wash blooms in
        // behind them (the "expensive" separation). Grains themselves are not
        // delayed, so playability/response is unaffected.
        int prd = preDelayPos - preDelaySamps;
        if (prd < 0) prd += preDelaySize;
        float washInL = preDelayL[(size_t) prd];
        float washInR = preDelayR[(size_t) prd];
        washSendSm += susSmCoef * ((isFrozen ? 0.0f : kWashDirectSend) - washSendSm);
        preDelayL[(size_t) preDelayPos] = layerL + washSendSm * inL;
        preDelayR[(size_t) preDelayPos] = layerR + washSendSm * inR;
        if (++preDelayPos >= preDelaySize) preDelayPos = 0;

        // Blackhole wash: feed the reverb a DARKENED (one-pole LP) then heavily
        // DIFFUSED signal, so the tail is smooth/dark and dense rather than
        // bright/metallic — the allpass diffusion is what makes it read as
        // Blackhole rather than a plain FDN.
        washFeedLpL += washInLpA * (washInL - washFeedLpL);
        washFeedLpR += washInLpA * (washInR - washFeedLpR);
        float diffL = washFeedLpL, diffR = washFeedLpR;
        washDiff.process (diffL, diffR);
        float washL = 0.0f, washR = 0.0f;
        fdn.process (diffL, diffR, washL, washR);

        // "Air" high-shelf on the GRAIN path ONLY (clarity/sparkle on the grains
        // without brightening the wash into metallic territory).
        airLpL += airCoefA * (layerL - airLpL);
        airLpR += airCoefA * (layerR - airLpR);
        float gAirL = layerL + kAirAmount * (layerL - airLpL);
        float gAirR = layerR + kAirAmount * (layerR - airLpR);

        // Gain-staged wet sum: normalise for how much wash is being added (so
        // raising Diffusion doesn't push the sum past full scale), then trim for
        // headroom. Peaks now sit near -3 dBFS, so the safety limiter only
        // catches genuine transients instead of shaving the whole signal.
        const float sumNorm = kWetTrim / (1.0f + kWashSumComp * washMix);
       #if ROOTBOUND_PROBE
        probeGrainE += (double) (gAirL * gAirL + gAirR * gAirR) * sumNorm * sumNorm;
        probeWashE  += (double) (washL * washL + washR * washR) * washMix * washMix * sumNorm * sumNorm;
        probeInE    += (double) (inL * inL + inR * inR);
       #endif
        float wetL = softLimit ((gAirL + washMix * washL) * sumNorm);
        float wetR = softLimit ((gAirR + washMix * washR) * sumNorm);

        // Regeneration write: input + DC-blocked grain output. Because grain
        // output contains pitched/cascaded shards, the buffer keeps re-pitching
        // and re-scattering itself (climbing ice) rather than smoothing to a
        // flat drone. Frozen => stop writing (infinite held sustain).
        if (!isFrozen)
        {
            float hpL = grainL - regenHpXL + regenHpPoleA * regenHpYL;
            regenHpXL = grainL; regenHpYL = hpL;
            float hpR = grainR - regenHpXR + regenHpPoleA * regenHpYR;
            regenHpXR = grainR; regenHpYR = hpR;

            float writtenL = inL + regen * hpL;
            float writtenR = inR + regen * hpR;
            bufL[(size_t)writePos] = writtenL;
            bufR[(size_t)writePos] = writtenR;

            // Anti-alias mipmap: two band-limited mirrors, each a 4-pole (24
            // dB/oct) lowpass, so pitched grains can read a copy whose content
            // is already below Nyquist/rate and cannot fold.
            auto lp4 = [] (float* st, float coef, float in)
            {
                st[0] += coef * (in    - st[0]);
                st[1] += coef * (st[0] - st[1]);
                st[2] += coef * (st[1] - st[2]);
                st[3] += coef * (st[2] - st[3]);
                return st[3];
            };
            bufM1L[(size_t)writePos] = lp4 (m1L, mip1CoefA, writtenL);
            bufM1R[(size_t)writePos] = lp4 (m1R, mip1CoefA, writtenR);
            bufM2L[(size_t)writePos] = lp4 (m2L, mip2CoefA, writtenL);
            bufM2R[(size_t)writePos] = lp4 (m2R, mip2CoefA, writtenR);

            writePos = (writePos + 1) % bufferSize;
        }

        if (wetBuffer.getNumChannels() > 0) wetBuffer.setSample (0, i, wetL);
        if (wetBuffer.getNumChannels() > 1) wetBuffer.setSample (1, i, wetR);
    }
}

// ------------------------------------------------------------------ softLimit --
// A transparent SAFETY catch, not a tone/dynamics stage: fully linear below the
// threshold so grain transients keep their natural dynamics (peaks burst
// through instead of being pinned/"squashed"), with only a gentle tanh knee
// above the threshold to stop true overs. Threshold raised to 0.92 so it now
// only engages on genuine peaks rather than sitting on the sustained cloud.
float GrainEngine::softLimit (float x)
{
    const float t = 0.92f;
    float a = std::abs (x);
    if (a <= t) return x;

    float sign = (x < 0.0f) ? -1.0f : 1.0f;
    float over = (a - t) / (1.0f - t);
    return sign * (t + (1.0f - t) * std::tanh (over));
}

// ============================================================================
//  WashDiffuser — dense input allpass diffusion (Blackhole smoothness)
// ============================================================================
void GrainEngine::WashDiffuser::prepare (double sampleRate)
{
    const float scale = (float) (sampleRate / 48000.0);
    // Decorrelated, roughly-prime allpass delays (~7–24 ms) per channel.
    const int   dl[kStages] = {  337,  481,  719, 1103 };
    const int   dr[kStages] = {  389,  523,  787, 1153 };
    const float gg[kStages] = { 0.70f, 0.70f, 0.68f, 0.66f };

    for (int s = 0; s < kStages; ++s)
    {
        int nl = juce::jmax (1, (int) std::round (dl[s] * scale));
        int nr = juce::jmax (1, (int) std::round (dr[s] * scale));
        left [s].buf.assign ((size_t) nl, 0.0f); left [s].size = nl; left [s].pos = 0; left [s].g = gg[s];
        right[s].buf.assign ((size_t) nr, 0.0f); right[s].size = nr; right[s].pos = 0; right[s].g = gg[s];
    }
}

void GrainEngine::WashDiffuser::clear()
{
    for (int s = 0; s < kStages; ++s)
    {
        std::fill (left [s].buf.begin(), left [s].buf.end(), 0.0f); left [s].pos = 0;
        std::fill (right[s].buf.begin(), right[s].buf.end(), 0.0f); right[s].pos = 0;
    }
}

void GrainEngine::WashDiffuser::process (float& l, float& r)
{
    for (int s = 0; s < kStages; ++s) l = left [s].process (l);
    for (int s = 0; s < kStages; ++s) r = right[s].process (r);
}

// ============================================================================
//  FDN — Blackhole-style modulated reverb wash
// ============================================================================
void GrainEngine::FDN::prepare (const int* lengths, double /*sampleRate*/)
{
    for (int k = 0; k < N; ++k)
    {
        size[k] = juce::jmax (1, lengths[k]);
        line[k].assign ((size_t) size[k], 0.0f);
        pos[k]     = 0;
        lpState[k] = 0.0f;
    }
}

void GrainEngine::FDN::clear()
{
    for (int k = 0; k < N; ++k)
    {
        std::fill (line[k].begin(), line[k].end(), 0.0f);
        pos[k]     = 0;
        lpState[k] = 0.0f;
        modPhase[k]= 0.0f;
    }
    dcBlockXL = dcBlockYL = 0.0f;
    dcBlockXR = dcBlockYR = 0.0f;
}

// Modulated fractional read of line k: reads slightly ahead of the write index
// by a slowly-modulated offset, so the effective delay time wobbles (the lush
// Blackhole chorus-swirl). Linear interpolation is plenty for a reverb tap.
float GrainEngine::FDN::readLine (int k) const
{
    const int sz = size[k];
    // Offset in [0, modDepthSamp], always >= 0 so we read valid (older) samples.
    float mod = modDepthSamp * (0.5f + 0.5f * std::sin (modPhase[k]));
    float idx = (float) pos[k] + mod;
    while (idx >= (float) sz) idx -= (float) sz;

    int   i0   = (int) idx;
    float frac = idx - (float) i0;
    int   i1   = (i0 + 1 >= sz) ? 0 : i0 + 1;
    return line[k][(size_t) i0] * (1.0f - frac) + line[k][(size_t) i1] * frac;
}

void GrainEngine::FDN::setFeedback (float decaySec, double sampleRate)
{
    double meanLen = 0.0;
    for (int k = 0; k < N; ++k) meanLen += (double) size[k];
    meanLen /= (double) N;

    const float meanSec = (float) (meanLen / sampleRate);
    const float dec     = juce::jmax (0.05f, decaySec);
    // Clamp well below unity (0.985, was 0.997) so the loop keeps a stability
    // margin — no runaway build-up even with long Decay + modulation.
    fb = juce::jlimit (0.0f, 0.985f, std::pow (0.001f, meanSec / dec));
}

void GrainEngine::FDN::process (float inL, float inR, float& outL, float& outR)
{
    // Recirculation reads: UNMODULATED, fixed delay. Modulating the feedback
    // path in a near-unity loop can spike/blow up (channel-specific pops that
    // can damage monitors), so the loop stays clean and only the OUTPUT taps
    // below are modulated for the lush swirl.
    float d[N];
    for (int k = 0; k < N; ++k)
        d[k] = line[k][(size_t) pos[k]];

    // Output taps: modulated (swirled) fractional reads — these do NOT feed back.
    float o[N];
    for (int k = 0; k < N; ++k)
    {
        o[k] = readLine (k);
        modPhase[k] += modInc[k];
        if (modPhase[k] >= juce::MathConstants<float>::twoPi)
            modPhase[k] -= juce::MathConstants<float>::twoPi;
    }

    float rawOutL = (o[0] - o[2] + o[4] - o[6]) * 0.5f;
    float rawOutR = (o[1] - o[3] + o[5] - o[7]) * 0.5f;

    outL = rawOutL - dcBlockXL + 0.995f * dcBlockYL;
    dcBlockXL = rawOutL; dcBlockYL = outL;

    outR = rawOutR - dcBlockXR + 0.995f * dcBlockYR;
    dcBlockXR = rawOutR; dcBlockYR = outR;

    float m[N], mLow[N];
    for (int k = 0; k < N; ++k)
    {
        lpState[k] += dampCoef * (d[k] - lpState[k]);
        m[k]    = d[k];
        mLow[k] = lpState[k];
    }
    for (int len = 1; len < N; len <<= 1)
        for (int i = 0; i < N; i += (len << 1))
            for (int j = i; j < i + len; ++j)
            {
                { const float x = m[j],    y = m[j + len];    m[j]    = x + y; m[j + len]    = x - y; }
                { const float x = mLow[j], y = mLow[j + len]; mLow[j] = x + y; mLow[j + len] = x - y; }
            }

    const float norm = 0.35355339f;
    for (int k = 0; k < N; ++k)
    {
        const float inj = ((k & 1) == 0 ? inL : inR) * 0.5f;

        const float lowBand  = mLow[k];
        const float highBand = m[k] - lowBand;
        const float shaped   = lowBand * fb + highBand * (fb * fbHighRatio);

        float written = inj + shaped * norm;

        if (! std::isfinite (written))
        {
            written    = 0.0f;
            lpState[k] = 0.0f;
        }

        // Hard safety clamp: even a freak transient can't run the loop away
        // (monitor protection). Well above normal signal, so it never colours.
        written = juce::jlimit (-4.0f, 4.0f, written);

        line[k][(size_t) pos[k]] = written;
        if (++pos[k] >= size[k]) pos[k] = 0;
    }
}
