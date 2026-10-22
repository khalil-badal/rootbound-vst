#include "PluginEditor.h"

namespace
{
    // Shared UI text sizes (OpenDyslexic). Enlarged for legibility — face,
    // weight, caps and spacing are unchanged; only the point size grew.
    constexpr float kFontKnobLabel  = 13.0f;   // GRAIN SIZE, PITCH, ...
    constexpr float kFontReadout    = 13.0f;   // value readouts (ms, s, %, dB) — most critical
    constexpr float kFontSection    = 12.0f;   // GRAIN MODE section label
    constexpr float kFontSubtitle   = 14.0f;   // GRANULAR REVERB
    constexpr float kFontTitle      = 26.0f;   // ROOTBOUND
    constexpr float kFontBrandSmall = 15.0f;   // KHYLE AUDIO
    constexpr float kFontBpm        = 11.0f;   // BPM hint

    // Shared layout geometry — used by BOTH paint() (dividers, readouts) and
    // resized() (component placement) so they can never drift out of sync.
    constexpr int kWindowW = 620;
    constexpr int kWindowH = 596;
    constexpr int kHeaderH = 56;
    constexpr int kFooterH = 56;   // tall enough for a label + dropdown stacked
    constexpr int kBandH   = 96;   // Reactive controls band (label + knob + readout)
    constexpr int kColW    = 158;  // left/right column width

    // Decorative panel geometry (used only by paint() for the faceplate look;
    // controls are still laid out by column centre in resized()).
    constexpr int kPanelGapX = 12;  // horizontal gap between panels
    constexpr int kPanelPadY = 4;   // vertical inset of panels from header/footer

    // Faceplate palette (existing colourway only — cyan / blue-gray / white).
    const juce::Colour kSteel  (0xFF3A4A5C);   // blue-gray linework
    const juce::Colour kSteelD (0xFF1E2C36);   // dim blue-gray

    // ---- faceplate linework helpers (all purely decorative) ----------------

    // L-shaped corner bracket: corner at (x,y), arms along (ax,ay) unit dirs.
    void cornerBracket (juce::Graphics& g, float x, float y, float ax, float ay,
                        float len, float thick, juce::Colour c)
    {
        g.setColour (c);
        g.drawLine (x, y, x + ax * len, y, thick);
        g.drawLine (x, y, x, y + ay * len, thick);
    }

    // Rounded rect with the TOP two corners chamfered at 45° (side panels).
    void chamferPanel (juce::Graphics& g, juce::Rectangle<float> r, float ch,
                       float rad, juce::Colour c, float thick)
    {
        const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
        juce::Path p;
        p.startNewSubPath (x + ch, y);
        p.lineTo (x + w - ch, y);
        p.lineTo (x + w, y + ch);
        p.lineTo (x + w, y + h - rad);
        p.quadraticTo (x + w, y + h, x + w - rad, y + h);
        p.lineTo (x + rad, y + h);
        p.quadraticTo (x, y + h, x, y + h - rad);
        p.lineTo (x, y + ch);
        p.closeSubPath();
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thick));
    }

    // Very faint dot-grid surface texture inside a rect.
    void dotGrid (juce::Graphics& g, juce::Rectangle<float> r, float spacing,
                  float dotR, juce::Colour c)
    {
        g.setColour (c);
        for (float yy = r.getY() + spacing * 0.5f; yy < r.getBottom(); yy += spacing)
            for (float xx = r.getX() + spacing * 0.5f; xx < r.getRight(); xx += spacing)
                g.fillEllipse (xx - dotR, yy - dotR, dotR * 2.0f, dotR * 2.0f);
    }

    // Repeated micro tick marks along an edge (calibration detail).
    void microTicks (juce::Graphics& g, float x, float y, int n, float step,
                     float len, bool vertical, float thick, juce::Colour c)
    {
        g.setColour (c);
        for (int i = 0; i < n; ++i)
        {
            if (vertical) { float yy = y + i * step; g.drawLine (x, yy, x + len, yy, thick); }
            else          { float xx = x + i * step; g.drawLine (xx, y, xx, y + len, thick); }
        }
    }

    // Horizontal broken/segmented HUD line (frames without enclosing).
    void segLine (juce::Graphics& g, float x1, float x2, float y, float dash,
                  float gap, float thick, juce::Colour c)
    {
        g.setColour (c);
        for (float xx = x1; xx < x2; xx += dash + gap)
            g.drawLine (xx, y, juce::jmin (xx + dash, x2), y, thick);
    }

    // A short circuit-trace: horizontal run, 45° step, then horizontal again.
    void traceStep (juce::Graphics& g, float x1, float y1, float run, float step,
                    bool up, bool leftToRight, float thick, juce::Colour c)
    {
        const float dir = leftToRight ? 1.0f : -1.0f;
        const float dy  = up ? -step : step;
        const float xA = x1 + dir * run;
        const float xB = xA + dir * step;
        juce::Path p;
        p.startNewSubPath (x1, y1);
        p.lineTo (xA, y1);
        p.lineTo (xB, y1 + dy);
        p.lineTo (xB + dir * run, y1 + dy);
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::mitered,
                                               juce::PathStrokeType::butt));
    }
}

// ============================================================= PluginEditor ====
RootboundAudioProcessorEditor::RootboundAudioProcessorEditor (RootboundAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&laf);
    setSize (kWindowW, kWindowH);

    // ----------------------------------------------------------------- knobs --
    auto setupKnob = [&](juce::Slider& s, juce::Label& lbl, const juce::String& text)
    {
        s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        addAndMakeVisible (s);

        lbl.setText (text, juce::dontSendNotification);
        lbl.setFont (laf.uiFont (kFontKnobLabel));
        lbl.setColour (juce::Label::textColourId, juce::Colour (RootboundLookAndFeel::kText));
        lbl.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (lbl);
    };

    // Left column
    setupKnob (sizeKnob,      sizeLabel,      "GRAIN SIZE");
    setupKnob (densityKnob,   densityLabel,   "GRAIN DENSITY");
    setupKnob (diffusionKnob, diffusionLabel, "DIFFUSION");
    setupKnob (sustainKnob,   sustainLabel,   "SUSTAIN");

    // Right column
    setupKnob (toneKnob,      toneLabel,      "TONE");
    setupKnob (characterKnob, characterLabel, "CHARACTER");
    setupKnob (inputKnob,     inputLabel,     "INPUT");
    setupKnob (outputKnob,    outputLabel,    "OUTPUT");

    // Center column
    setupKnob (pitchKnob, pitchLabel, "PITCH SNAP");
    setupKnob (decayKnob, decayLabel, "DECAY");
    setupKnob (mixKnob,   mixLabel,   "MIX");

    // --------------------------------------------------------------- sync row --
    syncRateBox.addItemList (RootboundSync::divisionNames(), 1);
    addAndMakeVisible (syncRateBox);

    syncButton.setButtonText ("SYNC");
    syncButton.setClickingTogglesState (true);
    addAndMakeVisible (syncButton);

    // ---------------------------------------------------------- footer combos --
    auto setupCombo = [&](juce::ComboBox& box, juce::Label& lbl,
                          const juce::String& labelText,
                          const juce::StringArray& items)
    {
        lbl.setText (labelText, juce::dontSendNotification);
        lbl.setFont (laf.uiFont (kFontSection));
        lbl.setColour (juce::Label::textColourId,
                       juce::Colour (RootboundLookAndFeel::kText).withAlpha (0.7f));
        lbl.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (lbl);

        box.addItemList (items, 1);
        addAndMakeVisible (box);
    };

    setupCombo (grainModeBox,   grainModeLabel,   "GRAIN MODE",
                { "Cloud", "Scatter", "Shimmer", "Pulse", "Swarm", "Reactive" });
    setupCombo (grainEnvBox,    grainEnvLabel,    "GRAIN ENV",
                { "Auto", "Smooth", "Swell", "Pluck" });

    // ---- Reactive band controls (dimmed unless Grain Mode = Reactive) ----
    setupKnob (reactivityKnob, reactivityLabel, "REACTIVITY");
    setupKnob (burstKnob,      burstLabel,      "BURST");
    setupCombo (arpBox,      arpLabel,      "ARP MODE",
                { "Off", "Up", "Down", "Up-Down", "Random", "Octaves" });
    setupCombo (playbackBox, playbackLabel, "PLAYBACK",
                { "Normal", "Oct Up", "Oct Down", "Reverse", "Random" });

    // ---------------------------------------------------------- footer buttons --
    freezeButton.setButtonText ("FREEZE");
    freezeButton.setClickingTogglesState (true);
    addAndMakeVisible (freezeButton);

    randomButton.setButtonText ("RANDOM");
    randomButton.onClick = [this] { processor.randomiseParameters(); };
    addAndMakeVisible (randomButton);

    // --------------------------------------------------------------- attachments --
    auto& avts = processor.apvts;

    // Left
    attSize      = std::make_unique<SliderAtt> (avts, "grainSize",    sizeKnob);
    attDensity   = std::make_unique<SliderAtt> (avts, "grainDensity", densityKnob);
    attDiffusion = std::make_unique<SliderAtt> (avts, "diffusion",    diffusionKnob);
    attSustain   = std::make_unique<SliderAtt> (avts, "sustain",      sustainKnob);

    // Right
    attTone      = std::make_unique<SliderAtt> (avts, "tone",       toneKnob);
    attCharacter = std::make_unique<SliderAtt> (avts, "character",  characterKnob);
    attInput     = std::make_unique<SliderAtt> (avts, "inputGain",  inputKnob);
    attOutput    = std::make_unique<SliderAtt> (avts, "outputGain", outputKnob);

    // Center
    attPitch = std::make_unique<SliderAtt> (avts, "pitch", pitchKnob);
    attMix   = std::make_unique<SliderAtt> (avts, "mix",   mixKnob);

    // Footer
    attFreeze    = std::make_unique<ButtonAtt> (avts, "freeze",    freezeButton);
    attGrainMode = std::make_unique<ComboAtt>  (avts, "grainMode", grainModeBox);
    attGrainEnv  = std::make_unique<ComboAtt>  (avts, "grainEnv",  grainEnvBox);

    // Reactive band
    attReactivity = std::make_unique<SliderAtt> (avts, "reactivity",   reactivityKnob);
    attBurst      = std::make_unique<SliderAtt> (avts, "burst",        burstKnob);
    attArp        = std::make_unique<ComboAtt>  (avts, "arpMode",      arpBox);
    attPlayback   = std::make_unique<ComboAtt>  (avts, "playbackMode", playbackBox);

    // Dim the reactive band when not in Reactive mode so its scope is obvious.
    grainModeBox.onChange = [this] { updateReactiveEnablement(); };
    updateReactiveEnablement();

    // Sync — build initial decay/size attachments then wire the button
    rebuildSyncAttachments (false);
    syncButton.onStateChange = [this] { applySyncState(); };
    attSync = std::make_unique<ButtonAtt> (avts, "grainSizeSync", syncButton);
    applySyncState();

    startTimerHz (15);   // drives repaint so value readouts stay live
}

RootboundAudioProcessorEditor::~RootboundAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

// ---------------------------------------------------------- GUI size presets ----
void RootboundAudioProcessorEditor::setGuiScale (float newScale)
{
    guiScale = newScale;
    // Resizing the editor re-runs resized() (which scales all children) and
    // repaints; the host / standalone window follows the new editor size.
    setSize (juce::roundToInt (kWindowW * guiScale),
             juce::roundToInt (kWindowH * guiScale));
}

void RootboundAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;

    auto isScale = [this] (float v) { return std::abs (guiScale - v) < 0.01f; };

    juce::PopupMenu m;
    m.addSectionHeader ("UI Size");
    m.addItem (1, "Small",  true, isScale (1.0f));
    m.addItem (2, "Medium", true, isScale (1.25f));
    m.addItem (3, "Large",  true, isScale (1.5f));

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                     [this] (int result)
                     {
                         if      (result == 1) setGuiScale (1.0f);
                         else if (result == 2) setGuiScale (1.25f);
                         else if (result == 3) setGuiScale (1.5f);
                     });
}

// -------------------------------------------------------- sync helpers ----------
void RootboundAudioProcessorEditor::rebuildSyncAttachments (bool synced)
{
    auto& avts = processor.apvts;
    attDecay.reset();
    attSyncRate.reset();

    if (synced)
    {
        attDecay    = std::make_unique<SliderAtt> (avts, "decayDivision",     decayKnob);
        attSyncRate = std::make_unique<ComboAtt>  (avts, "grainSizeDivision", syncRateBox);
    }
    else
    {
        attDecay = std::make_unique<SliderAtt> (avts, "decay", decayKnob);
        // syncRateBox shows grainSizeDivision even when unsynced for preview,
        // but the division value only feeds the processor when sync is on.
        attSyncRate = std::make_unique<ComboAtt> (avts, "grainSizeDivision", syncRateBox);
    }
}

void RootboundAudioProcessorEditor::applySyncState()
{
    const bool on = syncButton.getToggleState();
    if (on == syncUIState) return;
    syncUIState = on;

    // Mirror decaySync to the single SYNC button
    if (auto* p = processor.apvts.getParameter ("decaySync"))
        p->setValueNotifyingHost (on ? 1.0f : 0.0f);

    rebuildSyncAttachments (on);
    repaint();
}

void RootboundAudioProcessorEditor::updateReactiveEnablement()
{
    // Grain Mode "Reactive" is index 5. The band controls stay interactive in
    // every mode (the params exist/save always), but dim when not in Reactive
    // so it's obvious they only take effect there.
    const bool  on = (grainModeBox.getSelectedItemIndex() == 5);
    const float a  = on ? 1.0f : 0.4f;
    juce::Component* controls[] = { &reactivityKnob, &burstKnob, &arpBox, &playbackBox,
                                    &reactivityLabel, &burstLabel, &arpLabel, &playbackLabel };
    for (auto* c : controls)
        c->setAlpha (a);
}

// --------------------------------------------------------------- paint ----------
void RootboundAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Everything is drawn in BASE coordinates (kWindowW × kWindowH); the whole
    // paint is scaled by the current GUI-size preset via one Graphics transform.
    g.addTransform (juce::AffineTransform::scale ((float) getWidth() / (float) kWindowW));

    // Background: dark base + radial cyan bloom
    laf.drawPluginBackground (g, kWindowW, kWindowH);

    // Decorative faceplate: bordered panels, center texture, HUD linework.
    // Painted in the background layer (behind all child controls).
    paintFaceplate (g);

    // Header branding
    g.setFont (laf.brandFont (kFontTitle));
    g.setColour (juce::Colours::white);
    g.drawText ("ROOTBOUND", 0, 2, kWindowW, 32, juce::Justification::centred);

    g.setFont (laf.brandFont (kFontSubtitle));
    g.setColour (juce::Colour (RootboundLookAndFeel::kText).withAlpha (0.55f));
    g.drawText ("GRANULAR REVERB", 0, 34, kWindowW, 20, juce::Justification::centred);

    // Khyle Audio — upper right, same typeface as ROOTBOUND but smaller
    g.setFont (laf.brandFont (kFontBrandSmall));
    g.setColour (juce::Colour (RootboundLookAndFeel::kText).withAlpha (0.65f));
    g.drawText ("KHYLE AUDIO", kWindowW - 150, 6, 142, 20, juce::Justification::centredRight);

    // Value readout helper
    auto drawVal = [&](juce::Slider& s, const juce::String& unit, int cx, int y)
    {
        float v = (float)s.getValue();
        juce::String txt;

        if      (unit == "dB")  txt = juce::String (v, 1) + " dB";
        else if (unit == "ms")  txt = juce::String (v, 0) + " ms";
        else if (unit == "st")  txt = RootboundPitch::name (v);   // Pitch Snap interval
        else if (unit == "s")   txt = juce::String (v, 2) + " s";
        else if (unit == "%")   txt = juce::String (juce::roundToInt (v)) + " %";
        else if (unit == "tone")
        {
            int pct = juce::roundToInt (v * 100.0f);
            txt = (pct >= 0 ? "+" : "") + juce::String (pct) + " %";
        }
        else txt = juce::String (v, 1);

        g.setFont (laf.uiFont (kFontReadout));
        g.setColour (juce::Colour (RootboundLookAndFeel::kText).withAlpha (0.7f));
        g.drawText (txt, cx - 46, y, 92, 18, juce::Justification::centred);
    };

    auto drawSyncable = [&](juce::Slider& s, const juce::String& unit, int cx, int y)
    {
        if (syncUIState)
        {
            int idx = juce::jlimit (0, RootboundSync::divisionNames().size() - 1,
                                    juce::roundToInt ((float)s.getValue()));
            g.setFont (laf.uiFont (kFontReadout, true));
            g.setColour (juce::Colour (RootboundLookAndFeel::kText));
            g.drawText (RootboundSync::divisionNames()[idx],
                        cx - 46, y, 92, 18, juce::Justification::centred);
        }
        else
        {
            drawVal (s, unit, cx, y);
        }
    };

    // Readout x-centres derived from the same shared constants as resized(),
    // so they always track the knob columns.
    const int lcx = kColW / 2;               // left column   (79)
    const int ccx = kWindowW / 2;            // centre column (310)
    const int rcx = kWindowW - kColW / 2;    // right column  (541)

    // Left column readouts
    drawSyncable (sizeKnob,      "ms",   lcx, valueY (sizeKnob));
    drawVal      (densityKnob,   "%",    lcx, valueY (densityKnob));
    drawVal      (diffusionKnob, "%",    lcx, valueY (diffusionKnob));
    drawVal      (sustainKnob,   "%",    lcx, valueY (sustainKnob));

    // Right column readouts
    drawVal (toneKnob,      "tone", rcx, valueY (toneKnob));
    drawVal (characterKnob, "%",    rcx, valueY (characterKnob));
    drawVal (inputKnob,     "dB",   rcx, valueY (inputKnob));
    drawVal (outputKnob,    "dB",   rcx, valueY (outputKnob));

    // Center readouts
    drawVal      (pitchKnob, "st", ccx, valueY (pitchKnob));
    drawSyncable (decayKnob, "s",  ccx, valueY (decayKnob));
    drawVal      (mixKnob,   "%",  ccx, valueY (mixKnob));

    // Reactive-band knob readouts
    drawVal (reactivityKnob, "%", reactivityKnob.getBounds().getCentreX(), valueY (reactivityKnob));
    drawVal (burstKnob,      "%", burstKnob.getBounds().getCentreX(),      valueY (burstKnob));

    // BPM hint next to sync button when engaged
    if (syncUIState)
    {
        double bpm = processor.getCurrentBpm();
        g.setFont (laf.uiFont (kFontBpm));
        g.setColour (juce::Colour (RootboundLookAndFeel::kText).withAlpha (0.55f));
        g.drawText (juce::String (juce::roundToInt (bpm)) + " BPM",
                    syncButton.getRight() + 6, syncButton.getY(),
                    60, syncButton.getHeight(), juce::Justification::centredLeft);
    }
}

// ------------------------------------------------------------- faceplate --------
void RootboundAudioProcessorEditor::paintFaceplate (juce::Graphics& g)
{
    const int W = kWindowW;   // base coords; paint() applies the size-preset scale
    const int H = kWindowH;
    const juce::Colour cyan  (RootboundLookAndFeel::kCyan);
    const juce::Colour white (RootboundLookAndFeel::kText);

    const float pTop = (float)(kHeaderH + kPanelPadY);
    const float pBot = (float)(H - kFooterH - kBandH - kPanelPadY);   // above the Reactive band
    const float half = kPanelGapX * 0.5f;
    const float edge = 6.0f;

    juce::Rectangle<float> Lp (edge, pTop, (kColW - half) - edge, pBot - pTop);
    juce::Rectangle<float> Cp (kColW + half, pTop,
                               (float)(W - kColW - half) - (kColW + half), pBot - pTop);
    juce::Rectangle<float> Rp ((float)(W - kColW + half), pTop,
                               (float)(W - edge) - (W - kColW + half), pBot - pTop);

    // ---- SIDE PANELS: chamfered-top, layered border, faint texture ----
    auto sidePanel = [&] (juce::Rectangle<float> r, bool leftSide)
    {
        chamferPanel (g, r, 14.0f, 8.0f, kSteel.withAlpha (0.55f), 1.2f);
        chamferPanel (g, r.reduced (3.0f), 11.0f, 7.0f, kSteelD.withAlpha (0.45f), 0.8f);

        dotGrid (g, r.reduced (10.0f), 12.0f, 0.6f, white.withAlpha (0.03f));


        const float tx = leftSide ? r.getRight() - 6.0f : r.getX() + 2.0f;
        microTicks (g, tx, r.getCentreY() - 24.0f, 5, 12.0f, 4.0f, true, 0.8f,
                    kSteel.withAlpha (0.4f));
    };
    sidePanel (Lp, true);
    sidePanel (Rp, false);

    // ---- CENTER PANEL: layered border, texture, corner brackets + nodes ----
    g.setColour (kSteel.withAlpha (0.6f));
    g.drawRoundedRectangle (Cp, 9.0f, 1.3f);
    g.setColour (kSteelD.withAlpha (0.5f));
    g.drawRoundedRectangle (Cp.reduced (3.0f), 8.0f, 0.8f);

    dotGrid (g, Cp.reduced (10.0f), 11.0f, 0.7f, white.withAlpha (0.045f));

    {
        const float bi = 11.0f, bl = 15.0f, bt = 1.3f;
        const juce::Colour bc = cyan.withAlpha (0.4f);
        cornerBracket (g, Cp.getX() + bi,     Cp.getY() + bi,      +1, +1, bl, bt, bc);
        cornerBracket (g, Cp.getRight() - bi, Cp.getY() + bi,      -1, +1, bl, bt, bc);
        cornerBracket (g, Cp.getX() + bi,     Cp.getBottom() - bi, +1, -1, bl, bt, bc);
        cornerBracket (g, Cp.getRight() - bi, Cp.getBottom() - bi, -1, -1, bl, bt, bc);
    }


    // short diagonal accents just inside the top corners (the « / » marks)
    {
        g.setColour (cyan.withAlpha (0.3f));
        const float ax = Cp.getX() + 30.0f, ay = Cp.getY() + 26.0f;
        g.drawLine (ax, ay, ax + 14.0f, ay - 8.0f, 1.0f);
        g.drawLine (ax + 8.0f, ay + 6.0f, ax + 22.0f, ay - 2.0f, 1.0f);
        const float bx = Cp.getRight() - 30.0f, by = Cp.getY() + 26.0f;
        g.drawLine (bx, by, bx - 14.0f, by - 8.0f, 1.0f);
        g.drawLine (bx - 8.0f, by + 6.0f, bx - 22.0f, by - 2.0f, 1.0f);
    }

    segLine (g, Cp.getX() + 20.0f, Cp.getRight() - 20.0f, Cp.getBottom() - 8.0f,
             6.0f, 5.0f, 0.7f, kSteel.withAlpha (0.35f));

    // ---- HEADER HUD LINES (left & right of the title) ----
    {
        const float hy = 44.0f;
        const juce::Colour hc = kSteel.withAlpha (0.45f);
        traceStep  (g, Lp.getX() + 4.0f, hy, 90.0f, 8.0f, true, true,  0.9f, hc);
        microTicks (g, Lp.getX() + 20.0f, hy + 4.0f, 3, 7.0f, 4.0f, false, 0.8f, hc);

        traceStep  (g, Rp.getRight() - 4.0f, hy, 90.0f, 8.0f, true, false, 0.9f, hc);
        microTicks (g, Rp.getRight() - 24.0f, hy + 4.0f, 3, 7.0f, 4.0f, false, 0.8f, hc);
    }

    // ---- REACTIVE BAND PANEL (holds the onset-mode controls) ----
    {
        const float bTop = (float)(H - kFooterH - kBandH + kPanelPadY);
        const float bBot = (float)(H - kFooterH - kPanelPadY);
        juce::Rectangle<float> Bp (edge, bTop, (float)(W - edge) - edge, bBot - bTop);
        g.setColour (kSteel.withAlpha (0.5f));
        g.drawRoundedRectangle (Bp, 8.0f, 1.1f);
        g.setColour (kSteelD.withAlpha (0.4f));
        g.drawRoundedRectangle (Bp.reduced (3.0f), 7.0f, 0.7f);
    }

    // ---- FOOTER HUD LINE (segmented, spanning under the panels) ----
    {
        const float fy = (float)(H - kFooterH) + 2.0f;
        segLine (g, Lp.getX() + 4.0f, Rp.getRight() - 4.0f, fy, 7.0f, 6.0f, 0.7f,
                 kSteel.withAlpha (0.3f));
        cornerBracket (g, Lp.getX() + 4.0f,     fy + 4.0f, +1, +1, 8.0f, 0.9f, kSteel.withAlpha (0.4f));
        cornerBracket (g, Rp.getRight() - 4.0f, fy + 4.0f, -1, +1, 8.0f, 0.9f, kSteel.withAlpha (0.4f));
    }
}

// --------------------------------------------------------------- resized --------
void RootboundAudioProcessorEditor::resized()
{
    // Lay everything out in BASE coordinates; the current size preset is applied
    // by scaling each child (and, in paint(), the Graphics) by this factor.
    const float s = (float) getWidth() / (float) kWindowW;
    const int W = kWindowW;
    const int H = kWindowH;

    const int headerH = kHeaderH;
    const int footerH = kFooterH;
    const int colW    = kColW;    // left and right column width
    const int centreX = W / 2;

    const int contentTop = headerH;
    const int contentBot = H - footerH - kBandH;   // knob columns sit above the Reactive band
    const int contentH   = contentBot - contentTop;

    // Compact knob-group metrics: label above, knob, then value readout below
    // (the readout is drawn in paint() at valueY = knob.getBottom()+2).
    const int knobDia  = 48;
    const int pitchDia = 54;
    const int labelH   = 18;
    const int labelGap = 1;
    const int labelW   = 150;
    const int readoutH = 18;
    const int interGap = 8;   // tight vertical gap between knob groups

    // Full vertical footprint of a group: label + knob + readout beneath it.
    auto fullGroupH = [&](int dia) { return labelH + labelGap + dia + 2 + readoutH; };

    // Place a group with its top edge at groupTop: label, then knob under it.
    auto placeKnob = [&](juce::Slider& s, juce::Label& lbl, int cx, int groupTop, int dia)
    {
        lbl.setBounds (cx - labelW / 2, groupTop, labelW, labelH);
        s  .setBounds (cx - dia / 2, groupTop + labelH + labelGap, dia, dia);
    };

    // ------- LEFT COLUMN (4 knobs, same stack as the right), vertically centred -------
    {
        const int cx = colW / 2;   // 79
        const int stackH = 4 * fullGroupH (knobDia) + 3 * interGap;
        int y = contentTop + (contentH - stackH) / 2;
        placeKnob (sizeKnob,      sizeLabel,      cx, y, knobDia); y += fullGroupH (knobDia) + interGap;
        placeKnob (densityKnob,   densityLabel,   cx, y, knobDia); y += fullGroupH (knobDia) + interGap;
        placeKnob (diffusionKnob, diffusionLabel, cx, y, knobDia); y += fullGroupH (knobDia) + interGap;
        placeKnob (sustainKnob,   sustainLabel,   cx, y, knobDia);
    }

    // ------- RIGHT COLUMN (4 knobs), vertically centred -------
    {
        const int cx = W - colW / 2;   // 541
        const int stackH = 4 * fullGroupH (knobDia) + 3 * interGap;
        int y = contentTop + (contentH - stackH) / 2;
        placeKnob (toneKnob,      toneLabel,      cx, y, knobDia); y += fullGroupH (knobDia) + interGap;
        placeKnob (characterKnob, characterLabel, cx, y, knobDia); y += fullGroupH (knobDia) + interGap;
        placeKnob (inputKnob,     inputLabel,     cx, y, knobDia); y += fullGroupH (knobDia) + interGap;
        placeKnob (outputKnob,    outputLabel,    cx, y, knobDia);
    }

    // ------- CENTER COLUMN (Pitch, Decay, Mix + Sync row), vertically centred -------
    {
        const int syncH   = 28;
        const int syncGap = 4;   // sync sits close under Mix so it reads as attached
        const int stackH  = fullGroupH (pitchDia) + interGap
                          + fullGroupH (knobDia)  + interGap
                          + fullGroupH (knobDia)  + syncGap + syncH;
        int y = contentTop + (contentH - stackH) / 2;

        placeKnob (pitchKnob, pitchLabel, centreX, y, pitchDia); y += fullGroupH (pitchDia) + interGap;
        placeKnob (decayKnob, decayLabel, centreX, y, knobDia);  y += fullGroupH (knobDia)  + interGap;
        placeKnob (mixKnob,   mixLabel,   centreX, y, knobDia);  y += fullGroupH (knobDia)  + syncGap;

        // Sync row attached directly beneath Mix.
        const int availW   = W - 2 * colW - 28;
        const int rateW    = juce::roundToInt (availW * 0.62f);
        const int syncBtnW = 54;
        const int gap      = 8;
        const int totalW   = rateW + gap + syncBtnW;
        const int sx       = centreX - totalW / 2;
        syncRateBox.setBounds (sx, y, rateW, syncH);
        syncButton .setBounds (sx + rateW + gap, y, syncBtnW, syncH);
    }

    // ------- REACTIVE BAND (between the knob panels and the footer) -------
    // Four evenly-spaced controls: REACTIVITY + BURST knobs, ARP MODE + PLAYBACK
    // dropdowns. Dimmed (via updateReactiveEnablement) unless Grain Mode=Reactive.
    {
        const int bandTop = H - footerH - kBandH;
        const int pad     = 14;
        const int usable  = W - 2 * pad;
        const int colBand = usable / 4;
        const int bandDia = 40;
        const int lblH    = 16;
        const int lblW    = 140;
        const int lblTop  = bandTop + 8;   // clears the band panel's top border

        auto centreOf = [&] (int k) { return pad + colBand * k + colBand / 2; };

        auto placeBandKnob = [&] (juce::Slider& s, juce::Label& lbl, int cx)
        {
            lbl.setBounds (cx - lblW / 2, lblTop, lblW, lblH);
            s  .setBounds (cx - bandDia / 2, lblTop + lblH + 1, bandDia, bandDia);
        };
        auto placeBandCombo = [&] (juce::ComboBox& box, juce::Label& lbl, int cx)
        {
            lbl.setBounds (cx - lblW / 2, lblTop, lblW, lblH);
            box.setBounds (cx - lblW / 2, lblTop + lblH + 2, lblW, 28);
        };

        placeBandKnob  (reactivityKnob, reactivityLabel, centreOf (0));
        placeBandKnob  (burstKnob,      burstLabel,      centreOf (1));
        placeBandCombo (arpBox,         arpLabel,        centreOf (2));
        placeBandCombo (playbackBox,    playbackLabel,   centreOf (3));
    }

    // ------- FOOTER -------
    // The GRAIN MODE label+dropdown are stacked ENTIRELY below the footer's
    // top edge (fTop, where the horizontal divider is drawn in paint()), with
    // an explicit top pad — a label positioned by "fy - lblH" previously
    // floated above fTop and got cut through by the divider line.
    {
        const int fTop = H - footerH;
        const int btnH = 28;
        const int padTop = 8;
        const int padSide = 14;
        const int lblH = 14;
        const int labelGap = 2;

        const int modeWide = 180;
        grainModeLabel.setBounds (padSide, fTop + padTop, modeWide, lblH);
        grainModeBox  .setBounds (padSide, fTop + padTop + lblH + labelGap, modeWide, btnH);

        // GRAIN ENV sits between GRAIN MODE and the action buttons.
        const int envX = padSide + modeWide + 14;
        const int envW = 160;
        grainEnvLabel.setBounds (envX, fTop + padTop, envW, lblH);
        grainEnvBox  .setBounds (envX, fTop + padTop + lblH + labelGap, envW, btnH);

        // Action buttons aligned to the same row as the dropdown, right-aligned.
        const int btnY = fTop + padTop + lblH + labelGap;
        const int btnW = 100;
        const int gap  = 12;
        int btnsRight = W - padSide;
        randomButton .setBounds (btnsRight - btnW, btnY, btnW, btnH);
        freezeButton .setBounds (btnsRight - btnW * 2 - gap, btnY, btnW, btnH);
    }

    // Apply the current size preset: every child is laid out in base coords
    // above, then scaled by `s` so the whole UI (knobs, labels, text) grows
    // uniformly with the Small/Medium/Large selection.
    const auto scaleXform = juce::AffineTransform::scale (s);
    for (auto* child : getChildren())
        child->setTransform (scaleXform);
}
