#pragma once
#include <JuceHeader.h>

// ============================================================
//  GrainEngine — Cosmic-Web-style granular cloud
// ============================================================
//
//  Grain Modes (grainMode 0-5):
//   0  Cloud    – uniform random scatter across the wide spray window
//   1  Scatter  – evenly-spaced zones across the window (spaced, wide)
//   2  Shimmer  – heaviest musical-pitch share: the ice-crystal halo
//   3  Pulse    – rigid onset grid (rhythmic; exempt from onset jitter)
//   4  Swarm    – tight cluster around a slowly drifting point (thickening)
//   5  Reactive – ONSET-TRIGGERED bursts: detects the notes you play and fires
//                 arpeggiated / pitched / reversible grain rolls locked to each
//                 attack (H90 Stutter/Glitch, Qi Phrase Sample, Microcosm Strum).
//
//  Modes 0-4 are the free-running forward granular cloud. Reverse playback is
//  reintroduced ONLY inside Reactive burst voices (per-grain), never as a
//  global mode. See the Onset-Reactive changelog in GrainEngine.cpp.
// ============================================================

class GrainEngine
{
public:
    GrainEngine();

    // oversampleFactor tells the engine how much its internal rate (spec.sampleRate)
    // has been raised above the host rate, so its voicing filters can be made
    // sample-rate-independent (the tail stays exactly as dark whether run at 1x
    // or oversampled). 1 = run at host rate.
    void prepare   (const juce::dsp::ProcessSpec& spec, int oversampleFactor = 1);
    void reset();

    // Called once per block from PluginProcessor.
    // Reads from inputBuffer, writes wet-only output to wetBuffer.
    void process (const juce::AudioBuffer<float>& inputBuffer,
                  juce::AudioBuffer<float>&       wetBuffer);

    // Parameter setters (thread-safe via SmoothedValue or atomic update)
    void setGrainSizeMs   (float ms);           // 10–500
    void setGrainDensity  (float pct);          // 1–100
    void setPitchSemitones(float st);           // -24–+24
    void setDecaySeconds  (float s);            // 0.1–50
    void setGrainMode     (int mode);           // 0–5 (5 = Reactive)
    void setFrozen        (bool frozen);
    void setModDepth      (float depth);        // 0–1 : detune + scatter + evolving mod
    void setSustain       (float pct);          // 0–100 : steady unison bed under the cloud
    void setGrainEnvelope (int mode);           // 0 Auto / 1 Smooth / 2 Swell / 3 Pluck

    // Reactive-mode controls (only take effect when grainMode == 5). They still
    // exist/save in every mode so automation lanes never break; they are simply
    // not read outside Reactive mode.
    void setReactivity    (float pct);          // 0–100 : onset sensitivity
    void setBurst         (float pct);          // 0–100 : grains/onset + roll tightness
    void setArpMode       (int mode);           // 0 Off / Up / Down / Up-Down / Random / Octaves
    void setPlaybackMode  (int mode);           // 0 Normal / Oct Up / Oct Down / Reverse / Random

   #if ROOTBOUND_PROBE
    // Bench-only energy probes (never compiled into the plugin).
    double probeGrainE = 0.0, probeWashE = 0.0, probeInE = 0.0;
   #endif

private:
    // ---- circular buffer ----
    static constexpr float kMaxBufferSeconds = 22.0f;
    static constexpr int   kMaxGrains        = 256;

    std::vector<float> bufL, bufR;

    // ---- Anti-alias MIPMAP of the capture buffer -------------------------
    // Resampling a grain at rate R shifts its spectrum up by R, so any content
    // above Nyquist/R folds back as aliasing ("gritty digital edge"). The fix
    // is to low-pass to Nyquist/R BEFORE the read. One fixed filter can't serve
    // rates from 1.05x to 4.5x, so we keep two progressively darker mirrors of
    // the capture buffer and each grain reads the level matching its rate:
    //   mip 0 = full band   -> rates <= 1.05x (unpitched; stays crisp)
    //   mip 1 = ~4.5 kHz LP -> rates up to kMipRate1
    //   mip 2 = ~2.4 kHz LP -> rates above that (up to the rate cap)
    // Each mirror is 4-pole (24 dB/oct) — the old single 2-pole (12 dB/oct)
    // was far too shallow to stop folding at 4x+.
    std::vector<float> bufM1L, bufM1R;    // mip 1 (moderately dark)
    std::vector<float> bufM2L, bufM2R;    // mip 2 (very dark)
    float m1L[4] = { 0.0f }, m1R[4] = { 0.0f };   // 4-pole LP state, mip 1
    float m2L[4] = { 0.0f }, m2R[4] = { 0.0f };   // 4-pole LP state, mip 2

    // Sample-rate-independent (oversampling-safe) filter coefficients. The raw
    // constants are calibrated at the host rate; these are re-derived in
    // prepare() from the ACTUAL internal rate so cutoffs stay at the same Hz
    // (and the voicing is identical) whether the engine runs at 1x or 2x.
    float coefExp       = 1.0f;      // hostRate / internalRate
    float mip1CoefA     = 0.60f, mip2CoefA = 0.30f;
    float airCoefA      = 0.55f, washInLpA = 0.28f;
    float regenHpPoleA  = 0.984f;

    // High-shelf "air" lift — applied to the GRAIN path only (keeps grains
    // clear without brightening the reverb wash into metallic territory).
    float airLpL = 0.0f, airLpR = 0.0f;

    // Lowpass on the signal feeding the Blackhole wash, so the reverb tail is
    // dark and smooth (Blackhole is not a bright reverb) rather than metallic.
    float washFeedLpL = 0.0f, washFeedLpR = 0.0f;

    // Direct send of the input into the wash (de-zippered; 0 while frozen so a
    // held Freeze isn't refilled by new playing). Every note now gets a tail,
    // even ones the grains happen to miss.
    float washSendSm = 0.0f;

    // Pre-delay before the wash: separates the dry/grains from the reverb tail
    // (the classic "expensive" reverb gap). Only the WASH is delayed — grains
    // stay immediate, so playability is untouched.
    std::vector<float> preDelayL, preDelayR;
    int preDelaySize = 0, preDelayPos = 0, preDelaySamps = 0;

    int   bufferSize  = 0;
    int   writePos    = 0;

    // ---- Blackhole-style unifying reverb wash (modulated FDN) ----
    // The grain cloud provides discrete pointillistic events; THIS provides the
    // continuous, lush, evolving tail that washes them together and fills the
    // gaps between grain onsets (without it, discrete grains sound like they
    // "stop" between hits). Per-line MODULATION (slow decorrelated LFOs varying
    // the read delay) is the Blackhole signature — chorus-like swirls that keep
    // the tail from sounding static/sterile. Long, diffuse, soft-attack.
    struct FDN
    {
        static constexpr int N = 8;
        std::vector<float> line[N];
        int   size   [N] = { 0 };
        int   pos    [N] = { 0 };
        float lpState[N] = { 0.0f };   // one-pole low/high band splitter state per line
        float dampCoef   = 0.22f;      // band-split corner: 0 = bright .. ->1 = dark
        float fb         = 0.7f;       // global feedback for LOWS (set from Decay)
        float fbHighRatio= 0.18f;      // highs decay much faster than lows (smooth/dark, not metallic)

        // Per-line delay modulation (the Blackhole swirl).
        float modPhase[N] = { 0.0f };
        float modInc  [N] = { 0.0f };
        float modDepthSamp = 0.0f;     // modulation depth in samples (set per block)

        // One-pole DC blocker on the network's stereo output.
        float dcBlockXL = 0.0f, dcBlockYL = 0.0f;
        float dcBlockXR = 0.0f, dcBlockYR = 0.0f;

        void prepare (const int* lengths, double sampleRate);
        void clear();
        void setFeedback (float decaySec, double sampleRate);
        void setDamping  (float coef) { dampCoef = juce::jlimit (0.05f, 0.95f, coef); }
        void setModDepth (float samples) { modDepthSamp = juce::jmax (0.0f, samples); }
        float readLine (int k) const;   // modulated fractional read
        void process (float inL, float inR, float& outL, float& outR);
    };

    FDN fdn;

    // ---- input diffusion for the wash (the Blackhole smoothness) ----
    // A dense allpass network smears the signal feeding the reverb, dissolving
    // transients and metallic modes into a smooth, thick wash before it even
    // reaches the FDN. This is the single biggest "sounds like Blackhole" move.
    struct WashDiffuser
    {
        static constexpr int kStages = 4;
        struct AP
        {
            std::vector<float> buf;
            int   size = 0;
            int   pos  = 0;
            float g    = 0.7f;
            float process (float x)
            {
                float delayed = buf[(size_t) pos];
                float wn = x + g * delayed;
                float y  = -g * wn + delayed;
                buf[(size_t) pos] = wn;
                if (++pos >= size) pos = 0;
                return y;
            }
        };
        AP left[kStages], right[kStages];

        void prepare (double sampleRate);
        void clear();
        void process (float& l, float& r);
    };

    WashDiffuser washDiff;

    // ---- grain voice ----
    // A grain is now a small pitched, self-retriggering voice. For pitched-up
    // grains the source span is replayed within the grain length at the pitch
    // ratio (intra-grain retrigger -> crystalline stutter); each loop iteration
    // is Hann-windowed so the retrigger is click-free.
    struct Grain
    {
        float readPos    = 0.0f;   // current buffer read position
        float loopStart  = 0.0f;   // buffer pos each retrigger loop restarts from
        float rate       = 1.0f;   // playback speed / pitch ratio (always > 0)
        float loopLen    = 0.0f;   // output samples per retrigger loop
        float loopPhase  = 0.0f;   // 0..loopLen, wraps -> retrigger
        int   length     = 0;      // total grain output length
        int   progress   = 0;
        float amplitude  = 0.0f;
        float gainL      = 0.7071f;
        float gainR      = 0.7071f;
        int   attackLen  = 0;      // outer-envelope attack (fast for percussive grains)
        bool  retrig     = false;  // true if this grain retriggers (pitched up)
        int   mip        = 0;      // anti-alias mip level to read (0=full band, 1, 2)
        int   gen        = 0;      // Crystals-cascade generation (0 = root)
        bool  active     = false;
        float hazeLp     = 0.0f;   // one-pole lowpass state for distance/evolving haze
        float hazeCoef   = 1.0f;   // 1 = no filtering .. lower = darker
    };

    Grain grains[kMaxGrains];

    // ---- Sustain layer (the "two-grain bed") -----------------------------
    // Two unison read heads half a window apart, each with a raised-cosine
    // window. Two such windows offset by half their length always sum to 1, so
    // the layer is a STEADY, non-fluttering copy of what you just played (the
    // classic two-grain overlap-add player). Each head re-jumps to a slightly
    // different spot behind the write head on every window, so it reads as a
    // smooth doubled body, not a plain delay. Always at unison: it anchors the
    // pitch-snapped cloud to the note you played. It feeds the output and the
    // wash, but NOT the regen loop (keeps the feedback loop's gain unchanged).
    struct SustainHead
    {
        int   readPos  = 0;        // integer read position (rate is exactly 1)
        int   progress = 0;
        int   length   = 0;
        float gainL    = 0.7071f;
        float gainR    = 0.7071f;
        bool  active   = false;
    };
    SustainHead sustainHeads[2];
    int   sustainNextHead  = 0;
    float sustainCountdown = 0.0f;           // samples until the next head starts
    float sustainGainSm    = 0.0f;           // de-zippered bed gain
    std::atomic<float> sustainAmt { 0.0f };  // 0..1
    std::atomic<int>   envMode    { 0 };     // grain envelope shape (see setGrainEnvelope)

    // Compact list of active grain indices, so the per-sample render touches
    // only live grains instead of scanning every slot (pre-sized; RT-safe).
    int activeList[kMaxGrains] = { 0 };
    int numActive = 0;

    // ---- scheduling ----
    float samplesSinceOnset  = 0.0f;
    float swarmPhase         = 0.0f;  // 0..1 drift phase for Swarm mode
    float swarmDriftRate     = 0.0f;
    int   scatterIndex       = 0;

    // Per-block context read by spawnGrain / the cascade (set at block start).
    float blockDecayInSamples = 0.0f;
    float blockPitchRate      = 1.0f;
    float blockCascadeProb    = 0.0f;

    // Smearing / evolving modulation (driven by the Diffusion knob)
    std::atomic<float> modDepth { 0.0f };  // 0..1
    float globalPosMod = 0.0f;             // slow LFO position offset (samples)
    float lfoPhase     = 0.0f;
    float lfoInc       = 0.0f;

    // Evolving modulation LFOs (GrainMod character): slow, sub-Hz, decorrelated
    // drift on trigger rate, grain duration and grain-filter cutoff. Depth
    // scaled by Diffusion. Advanced once per block.
    float onsetLfoPhase = 0.0f, onsetLfoInc = 0.0f;
    float durLfoPhase   = 0.0f, durLfoInc   = 0.0f;
    float filtLfoPhase  = 0.0f, filtLfoInc  = 0.0f;
    float filtLfoVal    = 0.0f;  // set per block, read in spawnGrain (haze cutoff)

    // DC blocker on the buffer-regeneration feedback path.
    float regenHpXL = 0.0f, regenHpYL = 0.0f;
    float regenHpXR = 0.0f, regenHpYR = 0.0f;

    // ---- parameters (smoothed) ----
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothGrainSize;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothDensity;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothPitch;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothDecay;

    std::atomic<int>  grainMode { 0 };
    std::atomic<bool> frozen    { false };

    // ---- Onset-Reactive layer (grainMode == 5 only) ----------------------
    // Reactive params (read only in mode 5; harmless in modes 0-4).
    std::atomic<float> reactivity  { 50.0f };
    std::atomic<float> burstAmt    { 50.0f };
    std::atomic<int>   arpMode      { 0 };
    std::atomic<int>   playbackMode { 0 };

    // Onset detector state (instant-attack fast follower vs slow follower).
    float onsFastEnv = 0.0f;
    float onsSlowEnv = 0.0f;
    int   onsRefractory = 0;
    float idleFillCountdown = 0.0f;

    // Burst voices: on each onset one is claimed and emits a timed roll of
    // grains anchored to the just-played note. Fixed pool -> no allocation.
    static constexpr int kMaxBursts = 6;
    struct Burst
    {
        bool  active    = false;
        int   remaining = 0;       // grains left to emit
        float countdown = 0.0f;    // samples until the next grain
        float interval  = 0.0f;    // samples between grains (roll tightness)
        float jitter    = 0.0f;    // 0..1 timing scatter
        int   arpIndex  = 0;       // steps through the arp table
        float amp       = 1.0f;
        float grainLen  = 0.0f;    // grain length for this burst (samples)
    };
    Burst bursts[kMaxBursts];

    float sampleRate = 44100.0f;
    juce::Random rng;

    // ---- helpers ----
    int  findFreeGrain() const;
    // Spawns one grain. gen/ampScale/forcedSemis drive the Crystals cascade:
    // a shimmer grain may recursively spawn an octave-up child (gen+1, reduced
    // amp) so pitched shards accumulate into a climbing halo.
    void spawnGrain (float grainLenSamples, int gen, float ampScale, float forcedSemis);
    // Reactive burst grain: reads the most-recent captured window (behind the
    // write head, always RT-safe) at an explicit arp pitch (semis) and octave
    // multiplier, optionally reversed. Shares the Grain struct / window / mipmap
    // / render path with normal grains.
    void spawnBurstGrain (float semis, float octaveMul, bool reverse,
                          float ampScale, float grainLenSamples);
    float interpolate (const std::vector<float>& buf, float pos) const;
    // Asymmetric outer envelope: fast raised-cosine attack (attackLen), long
    // raised-cosine release. Fast attack = percussive tick; long = bloom.
    static float grainWindow (int progress, int length, int attackLen);
    // Attack length of a forward grain under the selected Grain Env mode.
    int envelopeAttack (float grainLenSamples, int length) const;
    static float softLimit  (float x);   // transparent below threshold, ceiling at +-1
};
