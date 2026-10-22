#include "Diffuser.h"

// ------------------------------------------------------------- Allpass stage --
void Diffuser::Allpass::prepare (float baseDelaySamples, float modDepthSamples,
                                  float lfoHz, double sampleRate)
{
    baseDelay = baseDelaySamples;
    modDepth  = modDepthSamples;
    size      = (int) std::ceil (baseDelaySamples + modDepthSamples) + 4;
    buf.assign ((size_t) size, 0.0f);
    writePos  = 0;
    lfoPhase  = 0.0f;
    lfoInc    = juce::MathConstants<float>::twoPi * lfoHz / (float) sampleRate;
}

float Diffuser::Allpass::process (float x, float amountModScale)
{
    // Modulated fractional delay length
    lfoPhase += lfoInc;
    if (lfoPhase >= juce::MathConstants<float>::twoPi)
        lfoPhase -= juce::MathConstants<float>::twoPi;

    float delaySamples = baseDelay + modDepth * amountModScale * std::sin (lfoPhase);
    delaySamples = juce::jlimit (1.0f, (float) (size - 2), delaySamples);

    // Fractional read at writePos - delaySamples, 4-point Hermite (cubic).
    // Linear interpolation here was a lowpass INSIDE the allpass recursion: it
    // broke the allpass property (measured -4.6 dB energy loss and a -3.8 dB
    // 8 kHz tilt at full Diffusion). Cubic keeps the network ~lossless.
    float readPos = (float) writePos - delaySamples;
    while (readPos < 0.0f) readPos += (float) size;
    int   i0   = (int) readPos;
    float frac = readPos - (float) i0;
    const int im1 = (i0 - 1 + size) % size;
    const int i1  = (i0 + 1) % size;
    const int i2  = (i0 + 2) % size;
    const float ym1 = buf[(size_t) im1], y0 = buf[(size_t) i0];
    const float y1  = buf[(size_t) i1],  y2 = buf[(size_t) i2];
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    float delayed = ((c3 * frac + c2) * frac + c1) * frac + y0;

    // Standard all-pass: w[n] = x + g*w[n-D];  y = -g*w[n] + w[n-D]
    float wn = x + g * delayed;
    float y  = -g * wn + delayed;

    buf[(size_t) writePos] = wn;
    writePos = (writePos + 1) % size;
    return y;
}

// ------------------------------------------------------------------ Diffuser --
void Diffuser::prepare (const juce::dsp::ProcessSpec& spec)
{
    const double sr = spec.sampleRate;

    // Decorrelated, roughly-prime base delays (designed around 48 kHz; scaled to
    // the actual rate). A mix of short and medium taps (up to ~23 ms) over 6
    // stages gives a denser, clearly audible smear. Each stage has its own LFO.
    const float baseL[kStages] = { 142.0f, 277.0f, 437.0f, 619.0f, 853.0f, 1031.0f };
    const float baseR[kStages] = { 191.0f, 331.0f, 503.0f, 691.0f, 911.0f, 1117.0f };
    const float lfoHz[kStages] = { 0.50f, 0.73f, 0.91f, 1.13f, 0.62f, 0.84f };

    const float scale   = (float) (sr / 48000.0);
    const float modSamp = 12.0f * scale;  // up to ~12 samples of delay modulation

    for (int s = 0; s < kStages; ++s)
    {
        left [s].prepare (baseL[s] * scale, modSamp, lfoHz[s],         sr);
        right[s].prepare (baseR[s] * scale, modSamp, lfoHz[s] * 1.07f, sr);
    }

    smoothAmount.reset (sr, 0.03);
    smoothAmount.setCurrentAndTargetValue (0.0f);
}

void Diffuser::reset()
{
    for (int s = 0; s < kStages; ++s)
    {
        std::fill (left [s].buf.begin(), left [s].buf.end(), 0.0f);
        std::fill (right[s].buf.begin(), right[s].buf.end(), 0.0f);
        left [s].lfoPhase = 0.0f;
        right[s].lfoPhase = 0.0f;
    }
}

void Diffuser::setAmount (float a)
{
    smoothAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, a));
}

void Diffuser::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numChannels == 0) return;

    float* dataL = buffer.getWritePointer (0);
    float* dataR = (numChannels > 1) ? buffer.getWritePointer (1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const float amount = smoothAmount.getNextValue();

        // EQUAL-POWER crossfade dry -> diffused. The diffused signal is largely
        // decorrelated from the dry, so they sum in power: the old linear
        // crossfade lost -2.8 dB at Diffusion 34% and -5.3 dB at 70% (measured),
        // i.e. turning Diffusion up quietly made the wet weaker.
        const float th = amount * juce::MathConstants<float>::halfPi;
        const float gd = std::cos (th);
        const float gw = std::sin (th);

        float dryL = dataL[i];
        float wetL = dryL;
        for (int s = 0; s < kStages; ++s)
            wetL = left[s].process (wetL, amount);
        dataL[i] = dryL * gd + wetL * gw;

        if (dataR != nullptr)
        {
            float dryR = dataR[i];
            float wetR = dryR;
            for (int s = 0; s < kStages; ++s)
                wetR = right[s].process (wetR, amount);
            dataR[i] = dryR * gd + wetR * gw;
        }
    }
}
