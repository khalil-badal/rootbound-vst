#include "InputOutputGain.h"

void InputOutputGain::prepare (const juce::dsp::ProcessSpec& spec)
{
    smoothedGain.reset ((double)spec.sampleRate, 0.02);
    smoothedGain.setCurrentAndTargetValue (1.0f);
    gainRamp.resize (spec.maximumBlockSize, 1.0f);
}

void InputOutputGain::setGainDecibels (float dB)
{
    smoothedGain.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-60.0f, 24.0f, dB)));
}

void InputOutputGain::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // Build the gain ramp once, then apply it to every channel.
    if ((int)gainRamp.size() < numSamples)
        gainRamp.resize ((size_t)numSamples);

    for (int i = 0; i < numSamples; ++i)
        gainRamp[(size_t)i] = smoothedGain.getNextValue();

    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), gainRamp.data(), numSamples);
}
