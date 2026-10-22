// =============================================================================
//  RootboundBench — offline DSP measurement harness (not part of the plugin).
//
//  Runs the REAL GrainEngine / Diffuser / TiltFilter / Saturator classes through
//  the same chain as RootboundAudioProcessor::processBlock (grain engine wrapped
//  in 2x polyphase-IIR oversampling -> diffuser -> tilt -> saturator -> equal-
//  power mix) and prints objective measurements. Build with -DROOTBOUND_BENCH=ON.
//
//  Usage:  RootboundBench [outputDirForWavs]
// =============================================================================
#include <JuceHeader.h>
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <vector>
#include "../dsp/GrainEngine.h"
#include "../dsp/Diffuser.h"
#include "../dsp/TiltFilter.h"
#include "../dsp/Saturator.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;

struct Settings
{
    float grainMs = 58.0f, density = 57.0f, pitch = 0.0f, decay = 2.0f;
    float diffusion = 34.0f, mix = 50.0f, tone = -0.19f, character = 61.0f;
    int   mode = 1;   // Scatter (the user's screenshot settings)
    float sustain = 0.0f;   // new controls default to "off" (today's sound)
    int   env = 0;
};

// Mirror of the processor's wet chain, using the real DSP classes.
struct Chain
{
    GrainEngine eng;
    Diffuser    dif;
    TiltFilter  tilt;
    Saturator   sat;
    juce::dsp::Oversampling<float> os { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR };
    juce::AudioBuffer<float> wet, upWet;
    juce::SmoothedValue<float> mixS;
    int    osFactor = 2, bs = 512;
    double sr = 48000.0;

    void prepare (double sampleRate, int blockSize, int osF)
    {
        sr = sampleRate; bs = blockSize; osFactor = osF;
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) bs, 2 };
        dif.prepare (spec); tilt.prepare (spec); sat.prepare (spec);
        os.initProcessing ((size_t) bs); os.reset();
        juce::dsp::ProcessSpec gs { sr * osF, (juce::uint32) (bs * osF), 2 };
        eng.prepare (gs, osF);
        wet.setSize (2, bs); upWet.setSize (2, bs * osF);
        mixS.reset (sr, 0.02);
    }

    void set (const Settings& s)
    {
        eng.setGrainSizeMs (s.grainMs);  eng.setGrainDensity (s.density);
        eng.setPitchSemitones (s.pitch); eng.setDecaySeconds (s.decay);
        eng.setGrainMode (s.mode);       eng.setModDepth (s.diffusion / 100.0f);
        eng.setSustain (s.sustain);      eng.setGrainEnvelope (s.env);
        dif.setAmount (s.diffusion / 100.0f);
        tilt.setTilt (s.tone);
        sat.setCharacter (s.character);
        mixS.setCurrentAndTargetValue (s.mix / 100.0f);
    }

    void engine (juce::AudioBuffer<float>& in)
    {
        const int n = in.getNumSamples();
        if (osFactor == 1) { eng.process (in, wet); return; }
        juce::dsp::AudioBlock<float> ib (in);
        auto ub = os.processSamplesUp (ib);
        const int n2 = (int) ub.getNumSamples();
        float* p[2] = { ub.getChannelPointer (0), ub.getChannelPointer (1) };
        juce::AudioBuffer<float> ui (p, 2, n2);
        eng.process (ui, upWet);
        for (int c = 0; c < 2; ++c)
            juce::FloatVectorOperations::copy (ub.getChannelPointer ((size_t) c), upWet.getReadPointer (c), n2);
        auto wo = juce::dsp::AudioBlock<float> (wet).getSubBlock (0, (size_t) n);
        os.processSamplesDown (wo);
    }

    // Full chain in place. wetOnly = 100% wet (for IR / decay tests).
    void full (juce::AudioBuffer<float>& io, bool wetOnly = false)
    {
        engine (io);
        dif.process (wet); tilt.process (wet); sat.process (wet);
        const int n = io.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            const float m  = mixS.getNextValue();
            const float th = m * juce::MathConstants<float>::halfPi;
            const float dg = wetOnly ? 0.0f : std::cos (th);
            const float wg = wetOnly ? 1.0f : std::sin (th);
            for (int c = 0; c < 2; ++c)
                io.setSample (c, i, io.getSample (c, i) * dg + wet.getSample (c, i) * wg);
        }
    }
};

// Plucked-guitar-like test material: harmonic plucks every 500 ms.
void makePlucks (juce::AudioBuffer<float>& b, double sr)
{
    b.clear();
    const int   n = b.getNumSamples();
    const float freqs[] = { 110.0f, 146.83f, 196.0f, 246.94f, 329.63f, 220.0f };
    const int   noteLen = (int) (0.5 * sr);
    for (int k = 0; k * noteLen < n; ++k)
    {
        const float f = freqs[k % 6];
        const int st = k * noteLen;
        for (int i = 0; i < noteLen * 3 && st + i < n; ++i)
        {
            const float t = (float) i / (float) sr;
            float s = 0.0f;
            for (int h = 1; h <= 8; ++h)
                s += std::sin (2.0f * (float) kPi * f * (float) h * t) / (float) h * std::exp (-t * (float) h * 0.8f);
            s *= 0.25f * std::exp (-t * 3.0f);
            b.addSample (0, st + i, s);
            b.addSample (1, st + i, s * 0.95f);
        }
    }
}

double ticksToUs (juce::int64 t) { return 1.0e6 * (double) t / (double) juce::Time::getHighResolutionTicksPerSecond(); }

// Run `blocks` of silence to let parameter smoothers + scheduler settle.
void warm (Chain& c, int blocks)
{
    juce::AudioBuffer<float> z (2, c.bs);
    for (int k = 0; k < blocks; ++k) { z.clear(); c.full (z); }
}

void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    if (os == nullptr) return;
    std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (w == nullptr) return;
    os.release();
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

// Candidate Character law for listening A/B: same tanh curve and drive range,
// but UNITY small-signal gain (tanh(d x)/d) instead of the current full-scale-
// peak makeup (tanh(d x)/tanh d), which lifts quiet material by up to +20 dB.
struct ProposedSaturator
{
    juce::dsp::Oversampling<float> os { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR };
    float drive = 1.0f, blend = 0.0f;
    void prepare (int bs) { os.initProcessing ((size_t) bs); os.reset(); }
    void setCharacter (float c) { drive = 1.0f + (c / 100.0f) * 9.0f; blend = juce::jlimit (0.0f, 1.0f, c / 25.0f); }
    void process (juce::AudioBuffer<float>& b)
    {
        juce::dsp::AudioBlock<float> blk (b);
        auto up = os.processSamplesUp (blk);
        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            float* d = up.getChannelPointer (ch);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                d[i] = d[i] + blend * (std::tanh (drive * d[i]) / drive - d[i]);
        }
        os.processSamplesDown (blk);
    }
};

// Render 100%-wet plucks at the user's settings with (a) the current Character
// law, (b) the proposed law, (c) Character 0, all RMS-matched for a fair A/B.
void renderSaturatorAB (const juce::File& outDir)
{
    const double sr = 48000.0; const int bs = 512;
    const int total = (int) (12.0 * sr);
    juce::AudioBuffer<float> src (2, total); makePlucks (src, sr);

    auto render = [&] (int variant)   // 0 = current @61, 1 = proposed @61, 2 = clean (Character 0)
    {
        Settings s; s.mix = 100.0f; if (variant == 2) s.character = 0.0f;
        auto c = std::make_unique<Chain>();
        c->prepare (sr, bs, 2); c->set (s);
        ProposedSaturator ps; ps.prepare (bs); ps.setCharacter (s.character);
        warm (*c, 2100);
        juce::AudioBuffer<float> out (2, total), blk (2, bs);
        for (int pos = 0; pos + bs <= total; pos += bs)
        {
            for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, src, ch, pos, bs);
            c->engine (blk);
            c->dif.process (c->wet); c->tilt.process (c->wet);
            if (variant == 1) ps.process (c->wet); else c->sat.process (c->wet);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, c->wet, ch, 0, bs);
        }
        return out;
    };
    juce::AudioBuffer<float> a = render (0), b = render (1), cl = render (2);
    auto rms = [] (const juce::AudioBuffer<float>& x) { double s = 0; for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < x.getNumSamples(); ++i) s += x.getSample (ch, i) * x.getSample (ch, i); return std::sqrt (s / (2.0 * x.getNumSamples())); };
    auto crest = [] (const juce::AudioBuffer<float>& x, double r) { float p = 0; for (int ch = 0; ch < 2; ++ch) p = juce::jmax (p, x.getMagnitude (ch, 0, x.getNumSamples())); return 20.0 * std::log10 (p / r); };
    const double ra = rms (a), rb = rms (b), rc = rms (cl);
    std::printf ("  Wet RMS  current@61 %6.2f dBFS (crest %5.2f dB) | proposed@61 %6.2f dBFS (crest %5.2f dB) | clean %6.2f dBFS (crest %5.2f dB)\n",
                 20 * std::log10 (ra), crest (a, ra), 20 * std::log10 (rb), crest (b, rb), 20 * std::log10 (rc), crest (cl, rc));
    // Match everything to a common -20 dBFS RMS so loudness can't bias the A/B.
    const double target = std::pow (10.0, -20.0 / 20.0);
    a.applyGain ((float) (target / ra)); b.applyGain ((float) (target / rb)); cl.applyGain ((float) (target / rc));
    writeWav (outDir.getChildFile ("AB_character61_CURRENT.wav"),  a,  sr);
    writeWav (outDir.getChildFile ("AB_character61_PROPOSED.wav"), b,  sr);
    writeWav (outDir.getChildFile ("AB_character0_CLEAN.wav"),     cl, sr);
    std::printf ("  Wrote level-matched (-20 dBFS RMS) A/B renders to %s\n", outDir.getFullPathName().toRawUTF8());
}

// ---------------------------------------------------------------- tests ------
void testCpu (double sr, int bs, int osF, const Settings& s)
{
    auto c = std::make_unique<Chain>();
    c->prepare (sr, bs, osF); c->set (s);
    const int total = (int) (12.0 * sr);
    juce::AudioBuffer<float> src (2, total); makePlucks (src, sr);
    warm (*c, 400);

    std::vector<double> eng, all;
    juce::AudioBuffer<float> blk (2, bs);
    for (int pos = 0; pos + bs <= total; pos += bs)
    {
        for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, src, ch, pos, bs);
        auto t0 = juce::Time::getHighResolutionTicks();
        c->engine (blk);
        auto t1 = juce::Time::getHighResolutionTicks();
        c->dif.process (c->wet); c->tilt.process (c->wet); c->sat.process (c->wet);
        auto t2 = juce::Time::getHighResolutionTicks();
        eng.push_back (ticksToUs (t1 - t0));
        all.push_back (ticksToUs (t2 - t0));
    }
    auto stats = [] (std::vector<double> v, double& mean, double& p99, double& mx)
    {
        std::sort (v.begin(), v.end());
        mean = std::accumulate (v.begin(), v.end(), 0.0) / (double) v.size();
        p99  = v[(size_t) (0.99 * (double) (v.size() - 1))];
        mx   = v.back();
    };
    double em, ep, ex, am, ap, ax;
    stats (eng, em, ep, ex); stats (all, am, ap, ax);
    const double budget = 1.0e6 * bs / sr;
    std::printf ("  CPU %5.0fk/%4d os%dx | engine mean %7.1f us p99 %7.1f max %7.1f | chain mean %7.1f us (%5.1f%% of %6.0f us budget) max %7.1f\n",
                 sr / 1000.0, bs, osF, em, ep, ex, am, 100.0 * am / budget, budget, ax);
}

void testLatency (int osF, const Settings& s, const char* label)
{
    const double sr = 48000.0; const int bs = 256;
    auto c = std::make_unique<Chain>();
    c->prepare (sr, bs, osF); c->set (s);
    warm (*c, 2100);
    juce::Random r (1234);
    std::vector<double> ms;
    juce::AudioBuffer<float> blk (2, bs);
    for (int trial = 0; trial < 25; ++trial)
    {
        // Random silence to randomize scheduler phase, then let tails die.
        const int gap = 20 + r.nextInt (60);
        for (int k = 0; k < gap; ++k) { blk.clear(); c->full (blk, true); }
        blk.clear(); blk.setSample (0, 0, 0.5f); blk.setSample (1, 0, 0.5f);
        int found = -1;
        for (int k = 0; k < 400 && found < 0; ++k)
        {
            c->full (blk, true);
            for (int i = 0; i < bs; ++i)
                if (std::abs (blk.getSample (0, i)) > 2.0e-3f || std::abs (blk.getSample (1, i)) > 2.0e-3f)
                    { found = k * bs + i; break; }
            blk.clear();
        }
        if (found >= 0) ms.push_back (1000.0 * found / sr);
        for (int k = 0; k < (int) (4.0 * sr / bs); ++k) { blk.clear(); c->full (blk, true); }
    }
    std::sort (ms.begin(), ms.end());
    if (ms.empty()) { std::printf ("  Latency %-28s : no wet response detected\n", label); return; }
    std::printf ("  Latency %-28s : click->first wet energy  min %6.1f ms  median %6.1f ms  max %6.1f ms  (n=%d)\n",
                 label, ms.front(), ms[ms.size() / 2], ms.back(), (int) ms.size());
}

void testHeadroom (const Settings& s, const juce::File& outDir, const char* wavName)
{
    const double sr = 48000.0; const int bs = 512;
    auto c = std::make_unique<Chain>();
    c->prepare (sr, bs, 2); c->set (s);
    warm (*c, 2100);
    const int total = (int) (12.0 * sr);
    juce::AudioBuffer<float> src (2, total); makePlucks (src, sr);
    juce::AudioBuffer<float> out (2, total);
    juce::AudioBuffer<float> blk (2, bs);
    double sumW = 0, sumL2 = 0, sumR2 = 0, sumLR = 0, sumM2 = 0;
    float  peakOut = 0, peakWet = 0; int overKnee = 0, overOne = 0;
    for (int pos = 0; pos + bs <= total; pos += bs)
    {
        for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, src, ch, pos, bs);
        c->full (blk);
        for (int i = 0; i < bs; ++i)
        {
            const float l = blk.getSample (0, i), rr = blk.getSample (1, i);
            const float wl = c->wet.getSample (0, i), wr = c->wet.getSample (1, i);
            peakOut = juce::jmax (peakOut, std::abs (l), std::abs (rr));
            peakWet = juce::jmax (peakWet, std::abs (wl), std::abs (wr));
            if (std::abs (l) > 0.85f || std::abs (rr) > 0.85f) ++overKnee;
            if (std::abs (l) > 1.0f  || std::abs (rr) > 1.0f)  ++overOne;
            sumW  += 0.5 * (wl * wl + wr * wr);
            sumL2 += wl * wl; sumR2 += wr * wr; sumLR += wl * wr;
            const double m = 0.5 * (wl + wr); sumM2 += m * m;
            out.setSample (0, pos + i, l); out.setSample (1, pos + i, rr);
        }
    }
    const int nS = (total / bs) * bs;
    const double corr = sumLR / std::sqrt (juce::jmax (1e-20, sumL2 * sumR2));
    const double monoLoss = 10.0 * std::log10 (juce::jmax (1e-20, sumM2) / juce::jmax (1e-20, 0.5 * (sumL2 + sumR2)));
    std::printf ("  Headroom: output peak %6.2f dBFS | wet peak %6.2f dBFS | wet RMS %6.2f dBFS | samples > knee(0.85): %.3f%%  > 1.0: %.3f%%\n",
                 juce::Decibels::gainToDecibels (peakOut), juce::Decibels::gainToDecibels (peakWet),
                 10.0 * std::log10 (juce::jmax (1e-20, sumW / nS)), 100.0 * overKnee / nS, 100.0 * overOne / nS);
    std::printf ("  Stereo  : wet L/R correlation %.3f | mono-sum level change %+.2f dB\n", corr, monoLoss);
    if (outDir.isDirectory())
        writeWav (outDir.getChildFile (wavName), out, sr);
}

void testDiffuser()
{
    const double sr = 48000.0; const int bs = 512;
    const int   order = 15, N = 1 << order;
    for (float a : { 0.0f, 0.34f, 0.7f, 1.0f })
    {
        Diffuser d; d.prepare ({ sr, (juce::uint32) bs, 2 }); d.setAmount (a);
        juce::AudioBuffer<float> b (2, bs);
        for (int k = 0; k < 200; ++k) { b.clear(); d.process (b); }     // settle smoother
        std::vector<float> ir ((size_t) N, 0.0f);
        int got = 0; bool first = true;
        while (got < N)
        {
            b.clear(); if (first) { b.setSample (0, 0, 1.0f); first = false; }
            d.process (b);
            for (int i = 0; i < bs && got < N; ++i) ir[(size_t) got++] = b.getSample (0, i);
        }
        // IR energy: a true (lossless) allpass network preserves exactly 0 dB.
        double energy = 0; for (float v : ir) energy += (double) v * v;

        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) (2 * N), 0.0f);
        std::copy (ir.begin(), ir.end(), data.begin());
        fft.performFrequencyOnlyForwardTransform (data.data());

        // 1/3-octave smoothed band power: reveals real tonal coloration (tilt,
        // HF loss, broad dips) rather than the random fine structure every
        // smeared IR has.
        double bMin = 1e9, bMax = -1e9, b250 = 0, b8k = 0;
        for (double fc = 125.0; fc <= 8001.0; fc *= std::pow (2.0, 1.0 / 3.0))
        {
            const double lo = fc / std::pow (2.0, 1.0 / 6.0), hi = fc * std::pow (2.0, 1.0 / 6.0);
            double p = 0; int cnt = 0;
            for (int k = 1; k < N / 2; ++k)
            {
                const double f = (double) k * sr / N;
                if (f >= lo && f < hi) { p += (double) data[(size_t) k] * data[(size_t) k]; ++cnt; }
            }
            const double bandDb = 10.0 * std::log10 (juce::jmax (1e-20, p / juce::jmax (1, cnt)));
            bMin = juce::jmin (bMin, bandDb); bMax = juce::jmax (bMax, bandDb);
            if (std::abs (fc - 250.0) < 20.0)  b250 = bandDb;
            if (std::abs (fc - 8000.0) < 700.0) b8k  = bandDb;
        }
        std::printf ("  Diffuser amount %.2f : IR energy %+6.2f dB | 1/3-oct bands 125Hz-8kHz: min %+6.2f max %+6.2f dB (spread %5.2f) | 8k vs 250 %+6.2f dB\n",
                     a, 10.0 * std::log10 (energy), bMin, bMax, bMax - bMin, b8k - b250);
    }
}

void testSaturator()
{
    const double sr = 48000.0; const int bs = 512;
    for (float ch : { 0.0f, 25.0f, 61.0f })
    {
        Saturator s; s.prepare ({ sr, (juce::uint32) bs, 2 }); s.setCharacter (ch);
        juce::AudioBuffer<float> b (2, bs);
        double sumIn = 0, sumOut = 0; double phase = 0;
        for (int k = 0; k < 400; ++k)
        {
            for (int i = 0; i < bs; ++i)
            {
                const float x = 0.1f * (float) std::sin (phase); phase += 2.0 * kPi * 1000.0 / sr;
                b.setSample (0, i, x); b.setSample (1, i, x);
            }
            if (k >= 100) for (int i = 0; i < bs; ++i) sumIn += b.getSample (0, i) * b.getSample (0, i);
            s.process (b);
            if (k >= 100) for (int i = 0; i < bs; ++i) sumOut += b.getSample (0, i) * b.getSample (0, i);
        }
        std::printf ("  Saturator Character %3.0f%% : small-signal (-20 dBFS) gain %+.2f dB\n",
                     ch, 10.0 * std::log10 (sumOut / sumIn));
    }
}

void testDecay (float decay)
{
    const double sr = 48000.0; const int bs = 512;
    Settings s; s.decay = decay; s.mix = 100.0f;
    auto c = std::make_unique<Chain>();
    c->prepare (sr, bs, 2); c->set (s);
    warm (*c, 2100);
    const int total = (int) (16.0 * sr);
    std::vector<double> e; e.reserve ((size_t) total);
    std::vector<float>  mono; mono.reserve ((size_t) total);
    juce::AudioBuffer<float> blk (2, bs);
    bool first = true;
    for (int pos = 0; pos + bs <= total; pos += bs)
    {
        blk.clear();
        if (first) { for (int i = 0; i < 48; ++i) { blk.setSample (0, i, 0.5f); blk.setSample (1, i, 0.5f); } first = false; }
        c->full (blk, true);
        for (int i = 0; i < bs; ++i)
        {
            e.push_back (0.5 * (blk.getSample (0, i) * blk.getSample (0, i) + blk.getSample (1, i) * blk.getSample (1, i)));
            mono.push_back (0.5f * (blk.getSample (0, i) + blk.getSample (1, i)));
        }
    }

    // Normalized echo density (Abel & Huang): fraction of samples beyond 1 sigma
    // in a 20 ms window, / erfc(1/sqrt2). ~1.0 = Gaussian-diffuse tail; low
    // values = sparse, discrete, "flutter/metallic" reflections.
    const int W = (int) (0.020 * sr);
    auto nedAt = [&] (int start)
    {
        double m = 0; for (int i = 0; i < W; ++i) m += mono[(size_t) (start + i)]; m /= W;
        double v = 0; for (int i = 0; i < W; ++i) { const double d = mono[(size_t) (start + i)] - m; v += d * d; }
        const double sd = std::sqrt (v / W); if (sd <= 1e-12) return 0.0;
        int cnt = 0; for (int i = 0; i < W; ++i) if (std::abs (mono[(size_t) (start + i)] - m) > sd) ++cnt;
        return ((double) cnt / W) / 0.3173;
    };
    double nedSum = 0; int nedN = 0; double tDiffuse = -1;
    for (int st = 0; st + W < (int) (1.0 * sr); st += W / 2)
    {
        const double ned = nedAt (st);
        const double tt  = (double) st / sr;
        if (tDiffuse < 0 && ned >= 0.9) tDiffuse = tt;
        if (tt >= 0.1 && tt <= 0.6) { nedSum += ned; ++nedN; }
    }
    std::printf ("  Echo density: first reaches NED>=0.9 at %6.1f ms | mean NED 100-600 ms %.2f (1.0 = fully diffuse)\n",
                 1000.0 * tDiffuse, nedN > 0 ? nedSum / nedN : 0.0);
    std::vector<double> edc (e.size());
    double acc = 0; for (size_t i = e.size(); i-- > 0;) { acc += e[i]; edc[i] = acc; }
    const double ref = edc[0];
    auto timeAt = [&] (double dB) { for (size_t i = 0; i < edc.size(); ++i) if (10.0 * std::log10 (edc[i] / ref + 1e-30) <= dB) return (double) i / sr; return -1.0; };
    const double t5 = timeAt (-5), t25 = timeAt (-25), t35 = timeAt (-35);
    const double rt20 = (t5 >= 0 && t25 >= 0) ? 3.0 * (t25 - t5) : -1.0;
    const double rt30 = (t5 >= 0 && t35 >= 0) ? 2.0 * (t35 - t5) : -1.0;
    std::printf ("  Decay knob %4.1f s : measured RT60 (T20) %6.2f s | (T30) %6.2f s\n", decay, rt20, rt30);
}

void testParamResponse (int bs)
{
    // Steady 220 Hz sine, Pitch 0 -> +12 at t=1s. Track energy at 440 vs 220 Hz
    // to see how long the engine's smoothed Pitch takes to actually arrive.
    const double sr = 48000.0;
    Settings s; s.mode = 4; s.diffusion = 0.0f; s.mix = 100.0f; s.density = 80.0f; s.grainMs = 80.0f;
    auto c = std::make_unique<Chain>();
    c->prepare (sr, bs, 2); c->set (s);
    warm (*c, 2100);
    juce::AudioBuffer<float> blk (2, bs);
    double phase = 0;
    const int win = (int) (0.25 * sr);
    double e220 = 0, e440 = 0; int inWin = 0; double t = 0;
    double crossed = -1;
    auto goertzel = [] (double f, double sr_, double x, double& s1, double& s2) { const double coeff = 2.0 * std::cos (2.0 * kPi * f / sr_); const double s0 = x + coeff * s1 - s2; s2 = s1; s1 = s0; };
    double a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    for (int k = 0; t < 40.0; ++k)
    {
        if (t >= 1.0 && s.pitch == 0.0f) { s.pitch = 12.0f; c->eng.setPitchSemitones (12.0f); }
        for (int i = 0; i < bs; ++i)
        {
            const float x = 0.3f * (float) std::sin (phase); phase += 2.0 * kPi * 220.0 / sr;
            blk.setSample (0, i, x); blk.setSample (1, i, x);
        }
        c->full (blk, true);
        for (int i = 0; i < bs; ++i)
        {
            const double y = 0.5 * (blk.getSample (0, i) + blk.getSample (1, i));
            goertzel (220.0, sr, y, a1, a2); goertzel (440.0, sr, y, b1, b2);
            if (++inWin == win)
            {
                const double c220 = 2.0 * std::cos (2.0 * kPi * 220.0 / sr), c440 = 2.0 * std::cos (2.0 * kPi * 440.0 / sr);
                e220 = a1 * a1 + a2 * a2 - c220 * a1 * a2;
                e440 = b1 * b1 + b2 * b2 - c440 * b1 * b2;
                const double tt = t + (double) i / sr;
                if (tt > 1.0 && crossed < 0 && e440 > e220) crossed = tt - 1.0;
                a1 = a2 = b1 = b2 = 0; inWin = 0;
            }
        }
        t += (double) bs / sr;
    }
    if (crossed >= 0)
        std::printf ("  Param response (block %4d): Pitch 0 -> +12 st takes %6.2f s until the octave dominates\n", bs, crossed);
    else
        std::printf ("  Param response (block %4d): Pitch change NOT reached within 39 s\n", bs);
}

// Steady 220 Hz sine through the chain (100% wet). Reports wet RMS and the
// level ripple (std-dev of 20 ms RMS frames, dB) — the "flutter" the Sustain
// bed is meant to smooth out.
void testSteadiness (const Settings& s, float sustain, int env, const char* label)
{
    const double sr = 48000.0; const int bs = 512;
    auto c = std::make_unique<Chain>();
    c->prepare (sr, bs, 2); c->set (s);
    c->eng.setSustain (sustain); c->eng.setGrainEnvelope (env);
    juce::AudioBuffer<float> blk (2, bs);
    double phase = 0;
    const int frame = (int) (0.02 * sr);
    std::vector<double> framesDb; double acc = 0; int inF = 0;
    double total = 0; long long nTot = 0;
    for (double t = 0; t < 8.0; t += (double) bs / sr)
    {
        for (int i = 0; i < bs; ++i)
        {
            const float x = 0.3f * (float) std::sin (phase); phase += 2.0 * kPi * 220.0 / sr;
            blk.setSample (0, i, x); blk.setSample (1, i, x);
        }
        c->full (blk, true);
        if (t < 3.0) continue;   // let the cloud / wash reach steady state
        for (int i = 0; i < bs; ++i)
        {
            const double y = 0.5 * (blk.getSample (0, i) + blk.getSample (1, i));
            acc += y * y; total += y * y; ++nTot;
            if (++inF == frame) { framesDb.push_back (10.0 * std::log10 (acc / frame + 1e-20)); acc = 0; inF = 0; }
        }
    }
    const double mean = std::accumulate (framesDb.begin(), framesDb.end(), 0.0) / (double) framesDb.size();
    double var = 0; for (double v : framesDb) var += (v - mean) * (v - mean);
    std::printf ("  %-34s wet RMS %6.2f dBFS | 20 ms level ripple %5.2f dB (std)\n",
                 label, 10.0 * std::log10 (total / (double) nTot), std::sqrt (var / (double) framesDb.size()));
}

// "How washed is it?" — wet vs dry balance, grain vs wash share, and how long
// the wet stays audible after a note, at the plugin's DEFAULT settings.
void testWash (Settings s, const char* label)
{
    const double sr = 48000.0; const int bs = 512;
    auto c = std::make_unique<Chain>();
    c->prepare (sr, bs, 2); c->set (s);
    warm (*c, 400);
    c->eng.probeGrainE = c->eng.probeWashE = c->eng.probeInE = 0.0;

    // (1) Playing: 12 s of plucks, 100% wet render vs the dry input.
    const int total = (int) (12.0 * sr);
    juce::AudioBuffer<float> src (2, total); makePlucks (src, sr);
    juce::AudioBuffer<float> blk (2, bs);
    double dryE = 0, wetE = 0;
    for (int pos = 0; pos + bs <= total; pos += bs)
    {
        for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, src, ch, pos, bs);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < bs; ++i) dryE += blk.getSample (ch, i) * blk.getSample (ch, i);
        c->full (blk, true);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < bs; ++i) wetE += blk.getSample (ch, i) * blk.getSample (ch, i);
    }
    const double gE = c->eng.probeGrainE, wE = c->eng.probeWashE;
    const double wetVsDry = 10.0 * std::log10 (wetE / dryE);
    // At the Mix knob position, dry*cos vs wet*sin (equal power):
    const double th = s.mix / 100.0 * kPi * 0.5;
    const double wetInMix = wetVsDry + 20.0 * std::log10 (std::sin (th) / std::cos (th));

    // (2) Tail: one pluck (0.5 s), then silence; wet level over time vs the pluck.
    auto c2 = std::make_unique<Chain>(); c2->prepare (sr, bs, 2); c2->set (s); warm (*c2, 400);
    const int tN = (int) (8.0 * sr);
    juce::AudioBuffer<float> one (2, tN); one.clear();
    { juce::AudioBuffer<float> p (2, (int) (0.5 * sr)); makePlucks (p, sr); for (int ch = 0; ch < 2; ++ch) one.copyFrom (ch, 0, p, ch, 0, p.getNumSamples()); }
    double plE = 0; for (int i = 0; i < (int) (0.5 * sr); ++i) plE += one.getSample (0, i) * one.getSample (0, i);
    const double plRms = std::sqrt (plE / (0.5 * sr));
    std::vector<float> wetMono ((size_t) tN);
    for (int pos = 0; pos + bs <= tN; pos += bs)
    {
        for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, one, ch, pos, bs);
        c2->full (blk, true);
        for (int i = 0; i < bs; ++i) wetMono[(size_t) (pos + i)] = 0.5f * (blk.getSample (0, i) + blk.getSample (1, i));
    }
    auto winDb = [&] (double t0, double t1) { double e = 0; int n = 0; for (int i = (int) (t0 * sr); i < (int) (t1 * sr) && i < tN; ++i, ++n) e += wetMono[(size_t) i] * wetMono[(size_t) i]; return 20.0 * std::log10 (std::sqrt (e / juce::jmax (1, n)) / plRms + 1e-12); };

    std::printf ("  %-30s wet vs dry %6.1f dB | in the mix @%2.0f%%: wet %+5.1f dB vs dry | grains %4.0f%% / wash %4.0f%% of wet energy\n",
                 label, wetVsDry, s.mix, wetInMix, 100.0 * gE / (gE + wE), 100.0 * wE / (gE + wE));
    std::printf ("  %-30s tail after the note (wet, re dry pluck): 0-0.5s %6.1f | 0.5-1s %6.1f | 1-2s %6.1f | 2-3s %6.1f | 4-5s %6.1f dB\n",
                 "", winDb (0.0, 0.5), winDb (0.5, 1.0), winDb (1.0, 2.0), winDb (2.0, 3.0), winDb (4.0, 5.0));
}

// Level-matched renders of the new controls (plucks, 100% wet, user settings).
void renderFeatureAB (const juce::File& outDir)
{
    const double sr = 48000.0; const int bs = 512;
    const int total = (int) (12.0 * sr);
    juce::AudioBuffer<float> src (2, total); makePlucks (src, sr);
    auto render = [&] (float sustain, int env, float pitch)
    {
        Settings s; s.mix = 100.0f; s.mode = 0; s.pitch = pitch;
        auto c = std::make_unique<Chain>();
        c->prepare (sr, bs, 2); c->set (s);
        c->eng.setSustain (sustain); c->eng.setGrainEnvelope (env);
        warm (*c, 2100);
        juce::AudioBuffer<float> out (2, total), blk (2, bs);
        for (int pos = 0; pos + bs <= total; pos += bs)
        {
            for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, src, ch, pos, bs);
            c->full (blk, true);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, blk, ch, 0, bs);
        }
        double e = 0; for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < total; ++i) e += out.getSample (ch, i) * out.getSample (ch, i);
        const double r = std::sqrt (e / (2.0 * total));
        std::printf ("    sustain %3.0f env %d pitch %+5.1f : raw wet RMS %6.2f dBFS\n", sustain, env, pitch, 20.0 * std::log10 (r));
        out.applyGain ((float) (std::pow (10.0, -20.0 / 20.0) / r));
        return out;
    };
    struct R { float sus; int env; float pitch; const char* name; };
    const R rs[] = { { 0, 0, 0, "FEAT_cloud_baseline.wav" },   { 60, 0, 0, "FEAT_cloud_sustain60.wav" },
                     { 0, 1, 0, "FEAT_env_smooth.wav" },       { 0, 2, 0, "FEAT_env_swell.wav" },
                     { 0, 3, 0, "FEAT_env_pluck.wav" },
                     { 0, 0, 12, "FEAT_snap_oct_up.wav" },     { 60, 0, 12, "FEAT_snap_oct_up_sustain60.wav" } };
    for (const auto& r : rs)
        writeWav (outDir.getChildFile (r.name), render (r.sus, r.env, r.pitch), sr);
    std::printf ("  Wrote level-matched (-20 dBFS RMS) feature renders to %s\n", outDir.getFullPathName().toRawUTF8());
}
} // namespace

int main (int argc, char* argv[])
{
    juce::FloatVectorOperations::disableDenormalisedNumberSupport();   // as ScopedNoDenormals does in processBlock
    const juce::File outDir = argc > 1 ? juce::File (juce::String (argv[1])) : juce::File();
    if (outDir != juce::File()) outDir.createDirectory();
    Settings s;

    if (argc > 2 && juce::String (argv[2]) == "ab")
    {
        std::printf ("\n=== Character A/B renders (100%% wet, user settings) ===\n");
        renderSaturatorAB (outDir);
        return 0;
    }

    if (argc > 2 && juce::String (argv[2]) == "wash")
    {
        Settings d;   // plugin DEFAULTS (createParameterLayout)
        d.grainMs = 100.0f; d.density = 50.0f; d.pitch = 0.0f; d.decay = 4.0f;
        d.mix = 50.0f; d.tone = 0.0f; d.character = 0.0f; d.diffusion = 40.0f; d.mode = 0;
        std::printf ("\n=== How washed is it? (plugin defaults unless noted) ===\n");
        testWash (d, "Defaults (Cloud)");
        { Settings e = d; e.diffusion = 0.0f;   testWash (e, "Diffusion 0"); }
        { Settings e = d; e.diffusion = 100.0f; testWash (e, "Diffusion 100"); }
        { Settings e = d; e.decay = 2.0f;       testWash (e, "Decay 2 s (old default)"); }
        { Settings e = d; e.density = 20.0f;    testWash (e, "Density 20"); }
        { Settings e = d; e.character = 61.0f;  testWash (e, "Character 61"); }
        { Settings e = d; e.sustain = 60.0f;    testWash (e, "Sustain 60"); }
        return 0;
    }

    // Mix-50% render at the plugin defaults: argv[3] = file tag, argv[4] = decay (s).
    // The "before" voicing is reproduced with the WMB/WMR/WDS/WLP env overrides.
    if (argc > 4 && juce::String (argv[2]) == "washab")
    {
        const double sr = 48000.0; const int bs = 512; const int total = (int) (14.0 * sr);
        Settings d; d.grainMs = 100.0f; d.density = 50.0f; d.pitch = 0.0f; d.decay = (float) std::atof (argv[4]);
        d.mix = 50.0f; d.tone = 0.0f; d.character = 0.0f; d.diffusion = 40.0f; d.mode = 0;
        juce::AudioBuffer<float> src (2, total); makePlucks (src, sr);
        for (int i = (int) (9.0 * sr); i < total; ++i) { src.setSample (0, i, 0.0f); src.setSample (1, i, 0.0f); }  // let the tail ring out
        auto c = std::make_unique<Chain>(); c->prepare (sr, bs, 2); c->set (d); warm (*c, 400);
        juce::AudioBuffer<float> out (2, total), blk (2, bs);
        for (int pos = 0; pos + bs <= total; pos += bs)
        {
            for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, src, ch, pos, bs);
            c->full (blk);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, blk, ch, 0, bs);
        }
        float pk = 0; for (int ch = 0; ch < 2; ++ch) pk = juce::jmax (pk, out.getMagnitude (ch, 0, total));
        const juce::String name = "WASH_" + juce::String (argv[3]) + "_mix50.wav";
        writeWav (outDir.getChildFile (name), out, sr);
        std::printf ("  wrote %s (Mix 50%%, Decay %.1f s, peak %.2f dBFS, not normalised)\n", name.toRawUTF8(), d.decay, 20.0 * std::log10 (pk));
        return 0;
    }

    if (argc > 2 && juce::String (argv[2]) == "feat")
    {
        std::printf ("\n=== Sustain / Grain Env steadiness (220 Hz sine, 100%% wet) ===\n");
        Settings c = s; c.mode = 0;
        testSteadiness (c,   0.0f, 0, "Cloud, Sustain 0 (today)");
        testSteadiness (c,  30.0f, 0, "Cloud, Sustain 30");
        testSteadiness (c,  60.0f, 0, "Cloud, Sustain 60");
        testSteadiness (c, 100.0f, 0, "Cloud, Sustain 100");
        testSteadiness (c,   0.0f, 1, "Cloud, Env Smooth");
        testSteadiness (c,   0.0f, 2, "Cloud, Env Swell");
        testSteadiness (c,   0.0f, 3, "Cloud, Env Pluck");
        { Settings d = c; d.density = 20.0f;
          testSteadiness (d,  0.0f, 0, "Cloud D20, Sustain 0");
          testSteadiness (d, 60.0f, 0, "Cloud D20, Sustain 60"); }
        std::printf ("\n=== CPU, Sustain 0 vs 100 (user settings, 48k / 128, 2x) ===\n");
        testCpu (48000.0, 128, 2, s);
        { Settings d = s; d.sustain = 100.0f; testCpu (48000.0, 128, 2, d); }
        std::printf ("\n=== Feature renders ===\n");
        renderFeatureAB (outDir);
        return 0;
    }

    std::printf ("\n=== CPU (real classes, plucked guitar material, user settings) ===\n");
    testCpu (48000.0, 128, 1, s);
    testCpu (48000.0, 128, 2, s);
    testCpu (48000.0, 512, 1, s);
    testCpu (48000.0, 512, 2, s);
    testCpu (96000.0, 512, 2, s);
    { Settings d = s; d.density = 100.0f; d.mode = 2; d.decay = 8.0f; d.grainMs = 300.0f;
      std::printf ("  -- worst case: Density 100, Shimmer, Decay 8 s, Grain 300 ms --\n");
      testCpu (48000.0, 128, 2, d); }

    {
        juce::dsp::Oversampling<float> o { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR };
        o.initProcessing (512);
        std::printf ("\n  Grain-engine oversampler latency: %.2f samples @ host rate (%.3f ms @ 48k)\n",
                     (double) o.getLatencyInSamples(), 1000.0 * o.getLatencyInSamples() / 48000.0);
    }

    std::printf ("\n=== Perceptual latency (click -> first wet energy, 100%% wet) ===\n");
    testLatency (2, s, "Scatter, user settings, 2x");
    { Settings c = s; c.mode = 0; testLatency (2, c, "Cloud, user settings, 2x"); }
    { Settings c = s; c.mode = 0; c.density = 20.0f; testLatency (2, c, "Cloud, Density 20, 2x"); }

    std::printf ("\n=== Headroom / stereo (user settings, 50%% mix) ===\n");
    testHeadroom (s, outDir, "pluck_user_settings.wav");

    std::printf ("\n=== Processor Diffuser frequency response (single stage chain, impulse) ===\n");
    testDiffuser();

    std::printf ("\n=== Saturator small-signal gain ===\n");
    testSaturator();

    std::printf ("\n=== Decay accuracy (impulse, 100%% wet, Schroeder EDC) ===\n");
    testDecay (2.0f);
    testDecay (6.0f);

    std::printf ("\n=== Parameter response time ===\n");
    testParamResponse (128);
    testParamResponse (512);

    std::printf ("\nDone.\n");
    return 0;
}
