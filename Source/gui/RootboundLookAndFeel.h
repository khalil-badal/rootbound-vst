#pragma once
#include <JuceHeader.h>

class RootboundLookAndFeel : public juce::LookAndFeel_V4
{
public:
    RootboundLookAndFeel();

    // Colours
    static constexpr uint32_t kCyan      = 0xFF00D4FF;
    static constexpr uint32_t kCyanDim   = 0x8800A8CC;
    static constexpr uint32_t kBg        = 0xFF080D10;
    static constexpr uint32_t kKnobBody  = 0xFF141C22;
    static constexpr uint32_t kKnobRim   = 0xFF1E2C36;
    static constexpr uint32_t kText      = 0xFFEEF1F8;  // near-white

    // Font factories — use embedded typefaces
    static juce::Font interFont  (float height, const char* style = "Regular");
    static juce::Font barlowFont (float height, const char* style = "ExtraBold");

    // Draw the plugin background. Call this first in PluginEditor::paint().
    void drawPluginBackground (juce::Graphics& g, int width, int height);

    // LookAndFeel overrides
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                                const juce::Colour& backgroundColour,
                                bool isMouseOver, bool isButtonDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool isMouseOver, bool isButtonDown) override;

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox& box) override;

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

    // Direct font accessors (use stored Typeface::Ptr to avoid name-lookup issues)
    juce::Font uiFont  (float height, bool bold = false) const;
    juce::Font brandFont (float height) const;

private:
    // Build a Font from the embedded OpenDyslexic typeface at the given height.
    // All UI text routes through here so the whole GUI renders in OpenDyslexic.
    // Extra kerning is forced to 0 so it reads as a normal (if quirky) sans-
    // serif rather than the wide accessibility spacing OpenDyslexic ships with.
    juce::Font dyslexicFont (float height) const;

    // Embedded typefaces (kept alive for the plugin's lifetime)
    juce::Typeface::Ptr openDyslexic;
    juce::Typeface::Ptr interRegular, interMedium, interSemiBold, interBold;
    juce::Typeface::Ptr barlowExtraBold, barlowRegular, barlowLight;
};
