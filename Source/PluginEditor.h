#pragma once
#include "PluginProcessor.h"

class RapAutoMixEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit RapAutoMixEditor (RapAutoMixProcessor&);
    ~RapAutoMixEditor() override { stopTimer(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    using ComboAttach  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;

    RapAutoMixProcessor& proc;
    juce::ComboBox role, mode, lang;
    juce::Slider amount { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Slider out    { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton learn { "Learn (8 sec)" };
    juce::Label status, lRole, lMode, lAmount, lOut, lLang;
    juce::TextEditor report;
    juce::String lastText;
    bool lastAr = false;
    std::unique_ptr<ComboAttach> roleA, modeA, langA;
    std::unique_ptr<SliderAttach> amountA, outA;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RapAutoMixEditor)
};
