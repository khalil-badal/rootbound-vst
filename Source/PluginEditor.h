#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "gui/RootboundLookAndFeel.h"

class RootboundAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit RootboundAudioProcessorEditor (RootboundAudioProcessor&);
    ~RootboundAudioProcessorEditor() override;

    void paint         (juce::Graphics&) override;
    void resized       () override;
    void mouseDown     (const juce::MouseEvent&) override;   // right-click size menu
    void timerCallback () override { repaint(); }

private:
    RootboundAudioProcessor& processor;
    RootboundLookAndFeel     laf;

    // ---- Left column: grain texture knobs ----
    juce::Slider sizeKnob, densityKnob, diffusionKnob, sustainKnob;
    juce::Label  sizeLabel, densityLabel, diffusionLabel, sustainLabel;

    // ---- Right column: color / I/O knobs ----
    juce::Slider toneKnob, characterKnob, inputKnob, outputKnob;
    juce::Label  toneLabel, characterLabel, inputLabel, outputLabel;

    // ---- Center column: engine knobs ----
    juce::Slider pitchKnob, decayKnob, mixKnob;
    juce::Label  pitchLabel, decayLabel, mixLabel;

    // ---- Center: tempo sync row ----
    juce::ComboBox syncRateBox;
    juce::TextButton syncButton;
    bool syncUIState = false;

    // ---- Reactive band: onset-mode controls (active only in Grain Mode = Reactive) ----
    juce::Slider   reactivityKnob, burstKnob;
    juce::Label    reactivityLabel, burstLabel;
    juce::ComboBox arpBox, playbackBox;
    juce::Label    arpLabel, playbackLabel;

    // ---- Footer: mode selector + action buttons ----
    juce::ComboBox   grainModeBox, grainEnvBox;
    juce::Label      grainModeLabel, grainEnvLabel;
    juce::TextButton freezeButton, randomButton;

    // ---- APVTS attachments ----
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<SliderAtt> attSize, attDensity, attDiffusion, attSustain;
    std::unique_ptr<ComboAtt>  attGrainEnv;
    std::unique_ptr<SliderAtt> attTone, attCharacter, attInput, attOutput;
    std::unique_ptr<SliderAtt> attPitch, attMix;
    std::unique_ptr<SliderAtt> attDecay;        // reattached on sync toggle
    std::unique_ptr<ComboAtt>  attSyncRate;     // reattached on sync toggle
    std::unique_ptr<ButtonAtt> attSync;
    std::unique_ptr<ButtonAtt> attFreeze;
    std::unique_ptr<ComboAtt>  attGrainMode;
    std::unique_ptr<SliderAtt> attReactivity, attBurst;
    std::unique_ptr<ComboAtt>  attArp, attPlayback;

    void rebuildSyncAttachments (bool synced);
    void applySyncState();
    void updateReactiveEnablement();   // dim the reactive band outside Reactive mode
    void paintFaceplate (juce::Graphics& g);   // decorative panels + HUD linework
    void setGuiScale (float newScale);         // Small / Medium / Large presets
    static int valueY (const juce::Slider& s)  { return s.getBottom() + 2; }

    // UI size preset (1.0 = Small/default, 1.25 = Medium, 1.5 = Large).
    float guiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RootboundAudioProcessorEditor)
};
