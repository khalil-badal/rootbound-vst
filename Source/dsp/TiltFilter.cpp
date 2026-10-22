#include "TiltFilter.h"

using std::cos;
using std::sin;
using std::sqrt;
using std::pow;

void TiltFilter::calcLowShelf (Biquad& bq, double fs, double f0, double gainDb)
{
    double A  = pow (10.0, gainDb / 40.0);
    double w0 = 2.0 * juce::MathConstants<double>::pi * f0 / fs;
    double S  = 1.0;
    double alpha = sin (w0) / 2.0 * sqrt ((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);

    double b0 = A * ((A + 1.0) - (A - 1.0) * cos (w0) + 2.0 * sqrt (A) * alpha);
    double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos (w0));
    double b2 = A * ((A + 1.0) - (A - 1.0) * cos (w0) - 2.0 * sqrt (A) * alpha);
    double a0 = (A + 1.0) + (A - 1.0) * cos (w0) + 2.0 * sqrt (A) * alpha;
    double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos (w0));
    double a2 = (A + 1.0) + (A - 1.0) * cos (w0) - 2.0 * sqrt (A) * alpha;

    bq.b0 = b0 / a0;  bq.b1 = b1 / a0;  bq.b2 = b2 / a0;
    bq.a1 = a1 / a0;  bq.a2 = a2 / a0;
}

void TiltFilter::calcHighShelf (Biquad& bq, double fs, double f0, double gainDb)
{
    double A  = pow (10.0, gainDb / 40.0);
    double w0 = 2.0 * juce::MathConstants<double>::pi * f0 / fs;
    double S  = 1.0;
    double alpha = sin (w0) / 2.0 * sqrt ((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);

    double b0 = A * ((A + 1.0) + (A - 1.0) * cos (w0) + 2.0 * sqrt (A) * alpha);
    double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos (w0));
    double b2 = A * ((A + 1.0) + (A - 1.0) * cos (w0) - 2.0 * sqrt (A) * alpha);
    double a0 = (A + 1.0) - (A - 1.0) * cos (w0) + 2.0 * sqrt (A) * alpha;
    double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos (w0));
    double a2 = (A + 1.0) - (A - 1.0) * cos (w0) - 2.0 * sqrt (A) * alpha;

    bq.b0 = b0 / a0;  bq.b1 = b1 / a0;  bq.b2 = b2 / a0;
    bq.a1 = a1 / a0;  bq.a2 = a2 / a0;
}

void TiltFilter::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    lowL.reset();  lowR.reset();
    highL.reset(); highR.reset();
    setTilt (0.0f);
}

void TiltFilter::setTilt (float tilt)
{
    currentTilt = tilt;
    // Positive tilt → bright: low shelf cuts, high shelf boosts
    // Negative tilt → dark:   low shelf boosts, high shelf cuts
    double gainDb = (double)tilt * 12.0;

    calcLowShelf  (lowL,  sampleRate, 300.0,  -gainDb);
    calcLowShelf  (lowR,  sampleRate, 300.0,  -gainDb);
    calcHighShelf (highL, sampleRate, 3000.0,  gainDb);
    calcHighShelf (highR, sampleRate, 3000.0,  gainDb);
}

void TiltFilter::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();

    if (buffer.getNumChannels() >= 1)
    {
        auto* data = buffer.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
            data[i] = (float)(highL.tick (lowL.tick ((double)data[i])));
    }

    if (buffer.getNumChannels() >= 2)
    {
        auto* data = buffer.getWritePointer (1);
        for (int i = 0; i < numSamples; ++i)
            data[i] = (float)(highR.tick (lowR.tick ((double)data[i])));
    }
}
