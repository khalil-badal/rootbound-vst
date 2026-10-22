#include "Saturator.h"

void Saturator::prepare (const juce::dsp::ProcessSpec& spec)
{
    oversampler.initProcessing (spec.maximumBlockSize);
    oversampler.reset();
    // Smoothers run at the OVERSAMPLED rate (they are advanced per upsampled
    // sample in process()), so reset them with the 2x rate for a true 20 ms ramp.
    smoothDrive.reset ((double)spec.sampleRate * 2.0, 0.02);
    smoothDrive.setCurrentAndTargetValue (1.0f);
    smoothBlend.reset ((double)spec.sampleRate * 2.0, 0.02);
    smoothBlend.setCurrentAndTargetValue (0.0f);
}

void Saturator::reset()
{
    oversampler.reset();
    smoothDrive.setCurrentAndTargetValue (smoothDrive.getTargetValue());
    smoothBlend.setCurrentAndTargetValue (smoothBlend.getTargetValue());
}

void Saturator::setCharacter (float c)
{
    // c in [0, 100] → drive in [1, 10]
    float d = 1.0f + (c / 100.0f) * 9.0f;
    smoothDrive.setTargetValue (d);

    // Character 0% must be clean: even drive 1 with its makeup gain added
    // +2.3 dB and tanh colouration (measured). Fade the saturated path in over
    // 0-25%; from 25% up the blend is 1, i.e. identical to before.
    smoothBlend.setTargetValue (juce::jlimit (0.0f, 1.0f, c / 25.0f));
}

void Saturator::process (juce::AudioBuffer<float>& buffer)
{
    auto block = juce::dsp::AudioBlock<float> (buffer);
    auto upBlock = oversampler.processSamplesUp (block);

    const int numSamples  = (int)upBlock.getNumSamples();
    const int numChannels = (int)upBlock.getNumChannels();

    for (int i = 0; i < numSamples; ++i)
    {
        float d = smoothDrive.getNextValue();
        float w = smoothBlend.getNextValue();
        float tanhD = std::tanh (d);
        float norm  = (tanhD > 1e-6f) ? (1.0f / tanhD) : 1.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* data = upBlock.getChannelPointer ((size_t)ch);
            const float x   = data[i];
            const float sat = std::tanh (d * x) * norm;
            data[i] = x + w * (sat - x);   // memoryless blend: no comb, no phase shift
        }
    }

    oversampler.processSamplesDown (block);
}
