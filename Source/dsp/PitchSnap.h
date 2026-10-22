#pragma once
#include <JuceHeader.h>

// Pitch Snap: the centre PITCH knob no longer transposes by arbitrary semitones.
// It snaps to a small set of consonant intervals (unison, 4ths, 5ths, octaves),
// so every position keeps the grains in tune with what you play. Shared by the
// parameter (knob snapping + host text), the processor and the editor readout.
//
// The parameter ID ("pitch") and its -24..+24 st range are unchanged, so old
// sessions load: a stored in-between value is snapped to the nearest interval.
namespace RootboundPitch
{
    inline constexpr float kSteps[] = { -24.0f, -19.0f, -12.0f, -7.0f, -5.0f, 0.0f,
                                          5.0f,   7.0f,  12.0f, 19.0f, 24.0f };
    inline constexpr int   kNumSteps = (int) (sizeof (kSteps) / sizeof (float));

    inline int nearestIndex (float st)
    {
        int best = 0;
        for (int k = 1; k < kNumSteps; ++k)
            if (std::abs (kSteps[k] - st) < std::abs (kSteps[best] - st))
                best = k;
        return best;
    }

    inline float snap (float st) { return kSteps[nearestIndex (st)]; }

    inline juce::String name (float st)
    {
        static const char* names[kNumSteps] = {
            "-2 Oct", "-Oct 5th", "-Oct", "-5th", "-4th", "Unison",
            "+4th", "+5th", "+Oct", "+Oct 5th", "+2 Oct"
        };
        return names[nearestIndex (st)];
    }
}
