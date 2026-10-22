#pragma once
#include <JuceHeader.h>

// Soft-saturation stage for the Character knob.
// Uses tanh waveshaping with 2x oversampling to suppress aliasing.
class Saturator
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void setCharacter (float c);   // 0–100
    void process (juce::AudioBuffer<float>& buffer);
    void reset();

private:
    // 2x oversampling, 2 channels, polyphase IIR filter
    juce::dsp::Oversampling<float> oversampler { 2, 1,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR };

    float drive     = 1.0f;
    float normGain  = 1.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothDrive;
    // Clean->saturated blend: 0 at Character 0% (fully transparent), reaching 1
    // at Character 25% and above (where behaviour is unchanged).
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothBlend;
};
