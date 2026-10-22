#include "PluginProcessor.h"
#include "PluginEditor.h"

// ---------------------------------------------------------------- parameter layout --
juce::AudioProcessorValueTreeState::ParameterLayout
RootboundAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "inputGain", "Input Gain",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "grainSize", "Grain Size",
        juce::NormalisableRange<float> (10.0f, 500.0f, 1.0f, 0.4f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "grainDensity", "Grain Density",
        juce::NormalisableRange<float> (1.0f, 100.0f, 1.0f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    // Pitch Snap: same ID and -24..+24 st range as the old Pitch knob (sessions
    // still load), but the knob snaps to consonant intervals (PitchSnap.h).
    {
        juce::NormalisableRange<float> pitchRange (
            -24.0f, 24.0f,
            [] (float s, float e, float n) { return s + n * (e - s); },     // linear, as before
            [] (float s, float e, float v) { return (v - s) / (e - s); },
            [] (float, float, float v)     { return RootboundPitch::snap (v); });
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            "pitch", "Pitch Snap", pitchRange, 0.0f,
            juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction ([] (float v, int) { return RootboundPitch::name (v); })));
    }

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "decay", "Decay",
        juce::NormalisableRange<float> (0.1f, 50.0f, 0.01f, 0.35f), 4.0f,   // default 4 s (was 2): longer wash out of the box
        juce::AudioParameterFloatAttributes().withLabel ("s")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "mix", "Mix",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "tone", "Tone",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "character", "Character",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "diffusion", "Diffusion",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 40.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    // "Reactive" appended as index 5 — appending preserves indices 0-4 so saved
    // presets (which only ever used 0-4) still resolve to the same mode.
    juce::StringArray grainModes { "Cloud", "Scatter", "Shimmer", "Pulse", "Swarm", "Reactive" };
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "grainMode", "Grain Mode", grainModes, 0));

    // ---- Reactive-mode parameters (only take effect when grainMode == 5) ----
    // Defaulted so an existing preset (mode 0-4) reproduces today's sound; they
    // still exist/save in every mode so automation lanes never break.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "reactivity", "Reactivity",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "burst", "Burst",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    juce::StringArray arpModes { "Off", "Up", "Down", "Up-Down", "Random", "Octaves" };
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "arpMode", "Arp Mode", arpModes, 0));

    juce::StringArray playbackModes { "Normal", "Oct Up", "Oct Down", "Reverse", "Random" };
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "playbackMode", "Playback Mode", playbackModes, 0));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "freeze", "Freeze", false));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "outputGain", "Output Gain",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    // ---- Tempo-sync parameters ----
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "grainSizeSync", "Grain Size Sync", false));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "grainSizeDivision", "Grain Size Division",
        RootboundSync::divisionNames(), 7));   // default "1/8"

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        "decaySync", "Decay Sync", false));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "decayDivision", "Decay Division",
        RootboundSync::divisionNames(), 13));   // default "1/2"

    // ---- Added later: appended so existing parameter order is untouched ----
    // Sustain: level of the steady two-grain unison bed. 0 = off (today's sound).
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        "sustain", "Sustain",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    // Grain envelope shape. "Auto" (default) is the original behaviour.
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        "grainEnv", "Grain Env", juce::StringArray { "Auto", "Smooth", "Swell", "Pluck" }, 0));

    return { params.begin(), params.end() };
}

// ------------------------------------------------------------------ constructor --
RootboundAudioProcessor::RootboundAudioProcessor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Rootbound", createParameterLayout())
{
    pInputGain    = apvts.getRawParameterValue ("inputGain");
    pGrainSize    = apvts.getRawParameterValue ("grainSize");
    pGrainDensity = apvts.getRawParameterValue ("grainDensity");
    pPitch        = apvts.getRawParameterValue ("pitch");
    pSustain      = apvts.getRawParameterValue ("sustain");
    pGrainEnv     = apvts.getRawParameterValue ("grainEnv");
    pDecay        = apvts.getRawParameterValue ("decay");
    pMix          = apvts.getRawParameterValue ("mix");
    pTone         = apvts.getRawParameterValue ("tone");
    pCharacter    = apvts.getRawParameterValue ("character");
    pDiffusion    = apvts.getRawParameterValue ("diffusion");
    pGrainMode    = apvts.getRawParameterValue ("grainMode");
    pReactivity   = apvts.getRawParameterValue ("reactivity");
    pBurst        = apvts.getRawParameterValue ("burst");
    pArpMode      = apvts.getRawParameterValue ("arpMode");
    pPlaybackMode = apvts.getRawParameterValue ("playbackMode");
    pFreeze       = apvts.getRawParameterValue ("freeze");
    pOutputGain   = apvts.getRawParameterValue ("outputGain");

    pGrainSizeSync     = apvts.getRawParameterValue ("grainSizeSync");
    pGrainSizeDivision = apvts.getRawParameterValue ("grainSizeDivision");
    pDecaySync         = apvts.getRawParameterValue ("decaySync");
    pDecayDivision     = apvts.getRawParameterValue ("decayDivision");
}

RootboundAudioProcessor::~RootboundAudioProcessor() {}

// ----------------------------------------------------------------- prepareToPlay --
void RootboundAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
    spec.numChannels      = 2;

    inputGain  .prepare (spec);
    outputGain .prepare (spec);
    tiltFilter .prepare (spec);
    saturator  .prepare (spec);
    diffuser   .prepare (spec);

    // The grain engine runs 2x-oversampled: prepare it at the doubled rate and
    // block size, and tell it the oversample factor so it keeps its voicing
    // filters at the same Hz cutoffs (see GrainEngine::adjCoef).
    grainOversampler.initProcessing ((size_t) samplesPerBlock);
    grainOversampler.reset();

    juce::dsp::ProcessSpec grainSpec;
    grainSpec.sampleRate       = sampleRate * kGrainOsFactor;
    grainSpec.maximumBlockSize = (juce::uint32)(samplesPerBlock * kGrainOsFactor);
    grainSpec.numChannels      = 2;
    grainEngine.prepare (grainSpec, kGrainOsFactor);

    smoothMix.reset (sampleRate, 0.02);
    smoothMix.setCurrentAndTargetValue (pMix ? pMix->load() / 100.0f : 0.5f);

    preparedBlockSize = juce::jmax (1, samplesPerBlock);
    wetBuffer  .setSize (2, samplesPerBlock);
    upWetBuffer.setSize (2, samplesPerBlock * kGrainOsFactor);
    wetBuffer.clear();
    upWetBuffer.clear();
}

void RootboundAudioProcessor::releaseResources() {}

// ------------------------------------------------------------------ processBlock --
void RootboundAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // Ensure we have mono/stereo output
    for (int ch = getTotalNumInputChannels(); ch < numChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    // Read host tempo (fall back to 120 in Standalone / when unavailable)
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                if (*bpm > 1.0)
                    currentBpm.store (*bpm);
        }
    }

    // Push parameters to DSP
    updateDSP();

    // Process in chunks no larger than the size we prepared for. Hosts may send
    // a bigger block than promised; before, that could overrun the oversampler
    // and the 2x scratch buffer (memory corruption) or force an allocation on
    // the audio thread. Normal-sized blocks take exactly one chunk.
    for (int offset = 0; offset < numSamples; )
    {
        const int n = juce::jmin (preparedBlockSize, numSamples - offset);
        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), numChannels, offset, n);
        processChunk (chunk);
        offset += n;
    }
}

void RootboundAudioProcessor::processChunk (juce::AudioBuffer<float>& buffer)
{
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // Views sized to THIS block. Previously the Diffuser / Tilt / Saturator
    // processed the whole prepared-size wetBuffer every call, so whenever the
    // host sent a shorter block (REAPER does this routinely) they chewed through
    // stale samples and advanced their filter/LFO state by the wrong amount.
    juce::AudioBuffer<float> wet   (wetBuffer.getArrayOfWritePointers(),   2, numSamples);
    juce::AudioBuffer<float> upWet (upWetBuffer.getArrayOfWritePointers(), 2, numSamples * kGrainOsFactor);

    // 1. Input gain
    inputGain.process (buffer);

    // 2. Save dry signal (post-input-gain)
    // We'll use the original buffer as dry; grain engine gets a read-only view.
    juce::AudioBuffer<float> dryBuffer (buffer.getArrayOfWritePointers(),
                                        numChannels, numSamples);

    // 3. Grain engine → wet reverb signal, 2x OVERSAMPLED.
    //    Upsample the granulate-input, run the engine at 2x, then downsample the
    //    wet back. Only the wet path is oversampled; the dry stays at host rate.
    {
        juce::dsp::AudioBlock<float> inBlock (buffer);
        auto upBlock = grainOversampler.processSamplesUp (inBlock);   // 2x input
        const int up2N = (int) upBlock.getNumSamples();

        float* upIns[2] = { upBlock.getChannelPointer (0),
                            upBlock.getNumChannels() > 1 ? upBlock.getChannelPointer (1)
                                                         : upBlock.getChannelPointer (0) };
        juce::AudioBuffer<float> upInput (upIns, 2, up2N);

        grainEngine.process (upInput, upWet);                         // engine at 2x

        // Put the wet back into the oversampler's block, then downsample to 1x.
        for (int ch = 0; ch < (int) upBlock.getNumChannels(); ++ch)
            juce::FloatVectorOperations::copy (upBlock.getChannelPointer ((size_t) ch),
                                               upWet.getReadPointer (ch), up2N);

        juce::dsp::AudioBlock<float> wetOut (wet);
        grainOversampler.processSamplesDown (wetOut);
    }

    // 3b. Allpass diffusion (smears the grain texture into a wash)
    diffuser.process (wet);

    // 4. Tone EQ on wet
    tiltFilter.process (wet);

    // 5. Character saturation on wet
    saturator.process (wet);

    // 6. Mix dry + wet with an EQUAL-POWER (constant-power) crossfade.
    //    A linear crossfade (dry*(1-m) + wet*m) drops ~3 dB in perceived loudness
    //    at the 50% position because the (decorrelated) reverb and dry sum in
    //    power, not amplitude — the classic "hole in the middle" that makes a mix
    //    knob sound quietest halfway. The sin/cos law keeps dry^2+wet^2 = 1 across
    //    the whole sweep, matching commercial reverbs.
    for (int i = 0; i < numSamples; ++i)
    {
        const float mix     = smoothMix.getNextValue();
        const float theta   = mix * juce::MathConstants<float>::halfPi;
        const float dryGain = std::cos (theta);
        const float wetGain = std::sin (theta);

        for (int ch = 0; ch < juce::jmin (numChannels, 2); ++ch)
        {
            const float d = dryBuffer.getSample (ch, i);
            const float w = wet.getSample (ch, i);
            buffer.setSample (ch, i, d * dryGain + w * wetGain);
        }
    }

    // 7. Output gain
    outputGain.process (buffer);

    // 8. Final safety net, right before the signal reaches the host:
    //    - flush any non-finite sample (NaN/Inf) to silence rather than let it
    //      hit the host's mix bus or speakers
    //    - a transparent brick-wall ceiling at +-1, which never engages in
    //      normal operation but protects against an extreme parameter
    //      combination stacking gain through Tone/Character/Output
    //    A bare hard clamp here used to shave any peak flat at +-1, which is a
    //    hard clip: it generates harsh odd harmonics and was audible as
    //    "clipping". Replaced with a soft knee — fully transparent below the
    //    threshold, then a tanh curve that asymptotes toward the ceiling — so
    //    peaks round off musically instead of being sheared. The final clamp
    //    stays purely as an absolute safety net (the knee keeps us under it).
    constexpr float kKnee = 0.85f;   // below this: bit-transparent

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* data = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            float x = data[i];
            if (! std::isfinite (x))
                x = 0.0f;

            const float a = std::abs (x);
            if (a > kKnee)
            {
                const float sign = (x < 0.0f) ? -1.0f : 1.0f;
                const float over = (a - kKnee) / (1.0f - kKnee);
                x = sign * (kKnee + (1.0f - kKnee) * std::tanh (over));
            }

            data[i] = juce::jlimit (-1.0f, 1.0f, x);
        }
    }
}

// ------------------------------------------------------------------- updateDSP --
void RootboundAudioProcessor::updateDSP()
{
    inputGain .setGainDecibels (pInputGain  ->load());
    outputGain.setGainDecibels (pOutputGain ->load());

    const double bpm    = currentBpm.load();
    const double beatMs = 60000.0 / juce::jmax (1.0, bpm);

    // Grain Size: either the free-running ms knob or a tempo-synced division.
    // The GrainEngine only ever sees a final ms value.
    float grainSizeMs;
    if (pGrainSizeSync->load() >= 0.5f)
    {
        double beats = RootboundSync::divisionBeats ((int) pGrainSizeDivision->load());
        grainSizeMs  = juce::jlimit (10.0f, 5000.0f, (float) (beatMs * beats));
    }
    else
    {
        grainSizeMs = pGrainSize->load();
    }

    // Decay: free-running seconds knob or a tempo-synced division.
    float decaySec;
    if (pDecaySync->load() >= 0.5f)
    {
        double beats = RootboundSync::divisionBeats ((int) pDecayDivision->load());
        decaySec     = juce::jlimit (0.1f, 50.0f, (float) (beatMs * beats / 1000.0));
    }
    else
    {
        decaySec = pDecay->load();
    }

    grainEngine.setGrainSizeMs    (grainSizeMs);
    grainEngine.setGrainDensity   (pGrainDensity ->load());
    grainEngine.setPitchSemitones (RootboundPitch::snap (pPitch->load()));   // old in-between values snap too
    grainEngine.setSustain        (pSustain      ->load());
    grainEngine.setGrainEnvelope  ((int)pGrainEnv ->load());
    grainEngine.setDecaySeconds   (decaySec);
    grainEngine.setGrainMode      ((int)pGrainMode   ->load());
    grainEngine.setFrozen         (pFreeze->load() >= 0.5f);

    // Reactive-mode controls (read by the engine only when grainMode == 5).
    grainEngine.setReactivity     (pReactivity  ->load());
    grainEngine.setBurst          (pBurst       ->load());
    grainEngine.setArpMode        ((int)pArpMode      ->load());
    grainEngine.setPlaybackMode   ((int)pPlaybackMode ->load());

    tiltFilter.setTilt    (pTone     ->load());
    saturator .setCharacter (pCharacter->load());

    // Diffusion knob drives both the allpass diffusion stage and the grain
    // engine's smearing modulation (per-grain detune + slow position drift).
    const float diffusion01 = pDiffusion->load() / 100.0f;
    diffuser   .setAmount   (diffusion01);
    grainEngine.setModDepth (diffusion01);

    smoothMix.setTargetValue (pMix->load() / 100.0f);
}

// -------------------------------------------------------------- randomise params --
void RootboundAudioProcessor::randomiseParameters()
{
    juce::Random rng;
    rng.setSeedRandomly();

    auto set = [&](const juce::String& id, float v)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };

    set ("grainSize",    20.0f  + rng.nextFloat() * 180.0f);
    set ("grainDensity", 20.0f  + rng.nextFloat() * 70.0f);
    // Pitch Snap is deliberately NOT randomised: it stays wherever the user set
    // it (Unison by default), so a random patch never transposes the mood.
    set ("decay",         0.5f  + rng.nextFloat() * 9.5f);
    set ("mix",           40.0f + rng.nextFloat() * 40.0f);
    set ("tone",         (rng.nextFloat() - 0.5f) * 1.2f);
    set ("character",     rng.nextFloat() * 70.0f);
    set ("diffusion",     20.0f + rng.nextFloat() * 70.0f);

    // Discrete: choice params store 0-based index as normalised value
    auto setChoice = [&](const juce::String& id, int count)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 ((float)rng.nextInt (count)));
    };
    setChoice ("grainMode",   5);
}

// --------------------------------------------------------------- state save/load --
void RootboundAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void RootboundAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// ---------------------------------------------------------------- createEditor --
juce::AudioProcessorEditor* RootboundAudioProcessor::createEditor()
{
    return new RootboundAudioProcessorEditor (*this);
}

// ---------------------------------------------------------------- entry point --
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RootboundAudioProcessor();
}
