#pragma once
#include <JuceHeader.h>

// Modulated all-pass diffusion stage (Schroeder/Dattorro style).
// Smears transients into a smooth, continuous wash. Placed after the grain
// engine and before Tone/Character. Stereo with decorrelated L/R delay lengths
// for width, and slowly-modulated delay times for Blackhole-like movement.
class Diffuser
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setAmount (float a);   // 0..1 : wet/dry blend + modulation depth
    void process (juce::AudioBuffer<float>& buffer);

private:
    static constexpr int kStages = 6;

    struct Allpass
    {
        std::vector<float> buf;
        int   size      = 0;
        int   writePos  = 0;
        float g         = 0.70f;
        float baseDelay = 0.0f;   // samples
        float modDepth  = 0.0f;   // samples of delay modulation
        float lfoPhase  = 0.0f;
        float lfoInc    = 0.0f;

        void  prepare (float baseDelaySamples, float modDepthSamples,
                       float lfoHz, double sampleRate);
        float process (float x, float amountModScale);
    };

    Allpass left[kStages];
    Allpass right[kStages];

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothAmount;
};
