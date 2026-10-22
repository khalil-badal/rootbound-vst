#include "RootboundLookAndFeel.h"
#include <BinaryData.h>

// ---------------------------------------------------------------------- helpers --
static juce::Typeface::Ptr makeTypeface (const void* data, int size)
{
    return juce::Typeface::createSystemTypefaceFor (data, (size_t)size);
}

// ----------------------------------------------------------------- constructor --
RootboundLookAndFeel::RootboundLookAndFeel()
{
    interRegular   = makeTypeface (BinaryData::InterRegular_ttf,        BinaryData::InterRegular_ttfSize);
    interMedium    = makeTypeface (BinaryData::InterMedium_ttf,         BinaryData::InterMedium_ttfSize);
    interSemiBold  = makeTypeface (BinaryData::InterSemiBold_ttf,       BinaryData::InterSemiBold_ttfSize);
    interBold      = makeTypeface (BinaryData::InterBold_ttf,           BinaryData::InterBold_ttfSize);
    barlowExtraBold= makeTypeface (BinaryData::BarlowCondensedExtraBold_ttf, BinaryData::BarlowCondensedExtraBold_ttfSize);
    barlowRegular  = makeTypeface (BinaryData::BarlowCondensedRegular_ttf,   BinaryData::BarlowCondensedRegular_ttfSize);
    barlowLight    = makeTypeface (BinaryData::BarlowCondensedLight_ttf,     BinaryData::BarlowCondensedLight_ttfSize);

    // OpenDyslexic — the typeface for all UI text. Loaded once, cached, kept
    // alive for the plugin's lifetime. jassert catches a bad/missing resource in
    // debug builds (createSystemTypefaceFor returns null on failure).
    openDyslexic   = makeTypeface (BinaryData::OpenDyslexicRegular_otf, BinaryData::OpenDyslexicRegular_otfSize);
    jassert (openDyslexic != nullptr);

    setColour (juce::Slider::thumbColourId,             juce::Colour (kCyan));
    setColour (juce::Slider::rotarySliderFillColourId,  juce::Colour (kCyan));
    setColour (juce::TextButton::buttonColourId,        juce::Colour (kBg));
    setColour (juce::TextButton::textColourOffId,       juce::Colour (kText));
    setColour (juce::Label::textColourId,               juce::Colour (kText));
}

// ---------------------------------------------------------- font factory methods --
juce::Font RootboundLookAndFeel::interFont (float height, const char* style)
{
    // These are static calls — they reach into the instance via the global LNF,
    // but we expose them as statics for convenience in PluginEditor.
    // Since we can't access instance members from a static, PluginEditor creates
    // fonts via juce::FontOptions and the typeface name, which JUCE resolves
    // through the registered typefaces.  We therefore just return a named font;
    // the typefaces registered in the constructor make them available globally.
    juce::String name = juce::String ("Inter ") + style;
    return juce::Font (juce::FontOptions().withName (name).withHeight (height));
}

juce::Font RootboundLookAndFeel::barlowFont (float height, const char* style)
{
    juce::String name = juce::String ("Barlow Condensed ") + style;
    return juce::Font (juce::FontOptions().withName (name).withHeight (height));
}

// -------------------------------------------------------- plugin background art --
void RootboundLookAndFeel::drawPluginBackground (juce::Graphics& g, int width, int height)
{
    // Flat dark base
    g.fillAll (juce::Colour (kBg));

    // Radial cyan bloom from the center — subtle, not dramatic
    float cx = (float)width  * 0.5f;
    float cy = (float)height * 0.48f;
    float radius = (float)juce::jmax (width, height) * 0.65f;

    juce::ColourGradient bloom (
        juce::Colour (kCyan).withAlpha (0.055f), cx, cy,
        juce::Colour (kBg).withAlpha (0.0f),     cx, cy + radius,
        true);  // radial
    g.setGradientFill (bloom);
    g.fillRect (0, 0, width, height);
}

// ------------------------------------------------------------ rotary knob drawing --
void RootboundLookAndFeel::drawRotarySlider (juce::Graphics& g,
                                              int x, int y, int width, int height,
                                              float sliderPos,
                                              float rotaryStartAngle,
                                              float rotaryEndAngle,
                                              juce::Slider&)
{
    const float cx  = (float)x + (float)width  * 0.5f;
    const float cy  = (float)y + (float)height * 0.5f;
    const float r   = juce::jmin ((float)width, (float)height) * 0.42f;
    const float arc = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Shadow / depth ring
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillEllipse (cx - r - 3.0f, cy - r - 3.0f, (r + 3.0f) * 2.0f, (r + 3.0f) * 2.0f);

    // Outer rim
    g.setColour (juce::Colour (kKnobRim));
    g.fillEllipse (cx - r - 1.5f, cy - r - 1.5f, (r + 1.5f) * 2.0f, (r + 1.5f) * 2.0f);

    // Knob body with subtle radial gradient (metallic look)
    {
        juce::ColourGradient grad (
            juce::Colour (0xFF253040), cx - r * 0.3f, cy - r * 0.3f,
            juce::Colour (kKnobBody),  cx + r * 0.5f, cy + r * 0.5f,
            false);
        g.setGradientFill (grad);
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
    }

    // Subtle highlight arc (top-left shine)
    {
        juce::Path shine;
        shine.addArc (cx - r * 0.85f, cy - r * 0.85f, r * 1.7f, r * 1.7f,
                      rotaryStartAngle - 0.5f, rotaryStartAngle + 1.8f, true);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.strokePath (shine, juce::PathStrokeType (r * 0.18f));
    }

    // Track (unfilled arc, dark)
    {
        juce::Path trackPath;
        trackPath.addArc (cx - r * 0.88f, cy - r * 0.88f,
                          r * 1.76f, r * 1.76f,
                          rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (juce::Colour (0xFF0A1018));
        g.strokePath (trackPath, juce::PathStrokeType (r * 0.10f,
                      juce::PathStrokeType::curved,
                      juce::PathStrokeType::rounded));
    }

    // Filled arc (cyan progress)
    {
        juce::Path arcPath;
        arcPath.addArc (cx - r * 0.88f, cy - r * 0.88f,
                        r * 1.76f, r * 1.76f,
                        rotaryStartAngle, arc, true);
        // Outer glow
        g.setColour (juce::Colour (kCyan).withAlpha (0.2f));
        g.strokePath (arcPath, juce::PathStrokeType (r * 0.18f,
                      juce::PathStrokeType::curved,
                      juce::PathStrokeType::rounded));
        // Core
        g.setColour (juce::Colour (kCyan).withAlpha (0.9f));
        g.strokePath (arcPath, juce::PathStrokeType (r * 0.09f,
                      juce::PathStrokeType::curved,
                      juce::PathStrokeType::rounded));
    }

    // Indicator dot/line
    {
        float ix = cx + std::sin (arc) * r * 0.60f;
        float iy = cy - std::cos (arc) * r * 0.60f;
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawLine (cx, cy, ix, iy, 1.2f);
        g.setColour (juce::Colours::white);
        g.fillEllipse (ix - 2.5f, iy - 2.5f, 5.0f, 5.0f);
    }

    // Inner shadow circle
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillEllipse (cx - r * 0.45f, cy - r * 0.45f, r * 0.9f, r * 0.9f);
}

// -------------------------------------------------------------- button drawing --
void RootboundLookAndFeel::drawButtonBackground (juce::Graphics& g,
                                                  juce::Button& button,
                                                  const juce::Colour&,
                                                  bool isMouseOver,
                                                  bool isButtonDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.5f);
    float corner = 6.0f;

    bool isToggled = button.getToggleState();

    juce::Colour fillCol = (isButtonDown || isToggled)
                           ? juce::Colour (kCyan).withAlpha (0.18f)
                           : juce::Colour (0xFF0A1218);
    juce::Colour rimCol  = (isMouseOver || isToggled)
                           ? juce::Colour (kCyan)
                           : juce::Colour (kCyan).withAlpha (0.5f);

    if (isMouseOver || isToggled)
    {
        g.setColour (juce::Colour (kCyan).withAlpha (0.12f));
        g.fillRoundedRectangle (bounds.expanded (3.0f), corner + 1.5f);
    }

    g.setColour (fillCol);
    g.fillRoundedRectangle (bounds, corner);

    g.setColour (rimCol);
    g.drawRoundedRectangle (bounds, corner, 1.4f);
}

void RootboundLookAndFeel::drawButtonText (juce::Graphics& g,
                                            juce::TextButton& button,
                                            bool /*isMouseOver*/,
                                            bool /*isButtonDown*/)
{
    auto font = getTextButtonFont (button, button.getHeight());
    g.setFont (font);
    g.setColour (juce::Colour (kText));

    const int yIndent    = juce::jmin (4, button.proportionOfHeight (0.3f));
    const int cornerSize = juce::jmin (button.getHeight(), button.getWidth()) / 2;
    const int leftIndent  = juce::jmin (cornerSize, 2 + cornerSize / (button.isConnectedOnLeft()  ? 4 : 2));
    const int rightIndent = juce::jmin (cornerSize, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));

    g.drawFittedText (button.getButtonText(),
                      leftIndent, yIndent,
                      button.getWidth() - leftIndent - rightIndent,
                      button.getHeight() - yIndent * 2,
                      juce::Justification::centred, 2);
}

// ---------------------------------------------------------------- combo box ------
void RootboundLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
                                          bool /*isButtonDown*/,
                                          int /*buttonX*/, int /*buttonY*/,
                                          int /*buttonW*/, int /*buttonH*/,
                                          juce::ComboBox& /*box*/)
{
    auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float)width, (float)height);
    g.setColour (juce::Colour (0xFF0A1218));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (juce::Colour (kCyan).withAlpha (0.55f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.2f);

    // Arrow chevron
    const float aw = (float)height * 0.28f;
    const float ax = (float)width  - aw * 2.2f;
    const float ay = (float)height * 0.5f;
    juce::Path arrow;
    arrow.startNewSubPath (ax - aw, ay - aw * 0.5f);
    arrow.lineTo          (ax,       ay + aw * 0.5f);
    arrow.lineTo          (ax + aw,  ay - aw * 0.5f);
    g.setColour (juce::Colour (kCyan));
    g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

void RootboundLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (4, 1, box.getWidth() - (int)(box.getHeight() * 0.9f), box.getHeight() - 2);
    label.setFont (dyslexicFont (13.0f));
}

// ---------------------------------------------------------------- OpenDyslexic --
// Central builder: every UI font routes through here. Extra kerning forced to 0
// so OpenDyslexic renders with normal typographic spacing (a quirky sans-serif),
// not the wide accessibility spacing it ships with by default. Falls back to a
// plain sized font if the typeface somehow failed to load.
juce::Font RootboundLookAndFeel::dyslexicFont (float height) const
{
    juce::Font f = (openDyslexic != nullptr)
                     ? juce::Font (juce::FontOptions().withTypeface (openDyslexic).withHeight (height))
                     : juce::Font (juce::FontOptions().withHeight (height));
    f.setExtraKerningFactor (0.0f);   // normal letter spacing, not the wide default
    return f;
}

// ---------------------------------------------------------------- direct font accessors --
// Both accessors now render in OpenDyslexic (the `bold` flag is ignored — only
// the Regular weight is embedded per spec). Heights are unchanged so layout,
// sizing and the overall UI scale are untouched.
juce::Font RootboundLookAndFeel::uiFont (float height, bool /*bold*/) const
{
    return dyslexicFont (height);
}

juce::Font RootboundLookAndFeel::brandFont (float height) const
{
    return dyslexicFont (height);
}

// --------------------------------------------------------------- font overrides --
juce::Font RootboundLookAndFeel::getLabelFont (juce::Label&)
{
    return dyslexicFont (13.0f);
}

juce::Font RootboundLookAndFeel::getTextButtonFont (juce::TextButton&, int /*height*/)
{
    return dyslexicFont (13.0f);
}

juce::Font RootboundLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return dyslexicFont (13.0f);
}

juce::Font RootboundLookAndFeel::getPopupMenuFont()
{
    return dyslexicFont (14.0f);
}
