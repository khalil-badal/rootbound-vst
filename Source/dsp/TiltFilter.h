#pragma once
#include <JuceHeader.h>

// Single-knob tilt EQ: low-shelf + high-shelf pair centred at 800 Hz.
// tilt in [-1, +1]: negative = darker, positive = brighter.
class TiltFilter
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec);
    void setTilt (float tilt);          // -1 to +1
    void process (juce::AudioBuffer<float>& buffer);

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double s1 = 0, s2 = 0;

        double tick (double x)
        {
            double y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            return y;
        }

        void reset() { s1 = s2 = 0.0; }
    };

    static void calcLowShelf  (Biquad& bq, double fs, double f0, double gainDb);
    static void calcHighShelf (Biquad& bq, double fs, double f0, double gainDb);

    // Two biquad sections per channel (low + high shelf)
    Biquad lowL,  lowR;
    Biquad highL, highR;

    double sampleRate = 44100.0;
    float  currentTilt = 0.0f;
};
