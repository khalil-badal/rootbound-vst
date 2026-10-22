#pragma once
#include <JuceHeader.h>

class InputOutputGain
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void setGainDecibels (float dB);
    void process (juce::AudioBuffer<float>& buffer);

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedGain;
    std::vector<float> gainRamp;
};
