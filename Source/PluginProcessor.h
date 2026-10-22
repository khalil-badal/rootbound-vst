#pragma once
#include <JuceHeader.h>
#include "dsp/GrainEngine.h"
#include "dsp/TiltFilter.h"
#include "dsp/Saturator.h"
#include "dsp/Diffuser.h"
#include "dsp/InputOutputGain.h"
#include "dsp/PitchSnap.h"

// Shared tempo-sync division table (used by both processor and editor).
// Ordered ascending by duration so a knob sweep goes short -> long.
// Values are in quarter-note beats (assumes 4/4; "Bar" = 4 beats).
namespace RootboundSync
{
    inline const juce::StringArray& divisionNames()
    {
        static const juce::StringArray names {
            "1/32T","1/32","1/16T","1/32.","1/16","1/8T","1/16.","1/8",
            "1/4T","1/8.","1/4","1/2T","1/4.","1/2","Bar T","1/2.","1 Bar","Bar ."
        };
        return names;
    }

    inline double divisionBeats (int idx)
    {
        static const double beats[] = {
            0.083333, 0.125, 0.166667, 0.1875, 0.25, 0.333333, 0.375, 0.5,
            0.666667, 0.75, 1.0, 1.333333, 1.5, 2.0, 2.666667, 3.0, 4.0, 6.0
        };
        idx = juce::jlimit (0, (int) (sizeof (beats) / sizeof (double)) - 1, idx);
        return beats[idx];
    }
}

class RootboundAudioProcessor : public juce::AudioProcessor
{
public:
    RootboundAudioProcessor();
    ~RootboundAudioProcessor() override;

    void prepareToPlay  (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock   (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Rootbound"; }
    bool   acceptsMidi()   const override { return false; }
    bool   producesMidi()  const override { return false; }
    bool   isMidiEffect()  const override { return false; }
    // Decay can be automated up to 50s (see createParameterLayout); report the
    // actual current value so hosts that use this to decide when to stop
    // calling processBlock (freeze/bounce/tail detection) don't truncate a
    // long decay tail.
    double getTailLengthSeconds() const override
    {
        return pDecay != nullptr ? (double) pDecay->load() : 20.0;
    }

    int  getNumPrograms()  override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Called by the Random button in the editor
    void randomiseParameters();

    // Current host tempo (falls back to 120 in Standalone / when unavailable)
    double getCurrentBpm() const { return currentBpm.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void updateDSP();
    void processChunk (juce::AudioBuffer<float>& buffer);   // <= preparedBlockSize samples
    int  preparedBlockSize = 512;

    GrainEngine    grainEngine;
    Diffuser       diffuser;
    TiltFilter     tiltFilter;
    Saturator      saturator;
    InputOutputGain inputGain, outputGain;

    // 2x oversampling wrapped around the grain engine ONLY, for the cleanest
    // pitched-grain resampling (the engine runs at 2x the host rate internally).
    static constexpr int kGrainOsFactor = 2;
    juce::dsp::Oversampling<float> grainOversampler
        { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR };
    juce::AudioBuffer<float> upWetBuffer;   // 2x wet scratch (engine output)

    // Pre-allocated wet buffer (avoids audio-thread allocation)
    juce::AudioBuffer<float> wetBuffer;

    // Raw parameter pointers (set once in constructor, read in processBlock)
    std::atomic<float>* pInputGain    = nullptr;
    std::atomic<float>* pGrainSize    = nullptr;
    std::atomic<float>* pGrainDensity = nullptr;
    std::atomic<float>* pPitch        = nullptr;
    std::atomic<float>* pSustain      = nullptr;
    std::atomic<float>* pGrainEnv     = nullptr;
    std::atomic<float>* pDecay        = nullptr;
    std::atomic<float>* pMix          = nullptr;
    std::atomic<float>* pTone         = nullptr;
    std::atomic<float>* pCharacter    = nullptr;
    std::atomic<float>* pDiffusion    = nullptr;
    std::atomic<float>* pGrainMode    = nullptr;
    std::atomic<float>* pReactivity   = nullptr;
    std::atomic<float>* pBurst        = nullptr;
    std::atomic<float>* pArpMode      = nullptr;
    std::atomic<float>* pPlaybackMode = nullptr;
    std::atomic<float>* pFreeze       = nullptr;
    std::atomic<float>* pOutputGain   = nullptr;

    // Tempo-sync parameters
    std::atomic<float>* pGrainSizeSync     = nullptr;
    std::atomic<float>* pGrainSizeDivision = nullptr;
    std::atomic<float>* pDecaySync         = nullptr;
    std::atomic<float>* pDecayDivision     = nullptr;

    std::atomic<double> currentBpm { 120.0 };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothMix;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RootboundAudioProcessor)
};
