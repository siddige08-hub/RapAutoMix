#include "PluginEditor.h"

RapAutoMixEditor::RapAutoMixEditor (RapAutoMixProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    role.addItemList ({ "Vocal", "Beat / Instrument", "Full mix" }, 1);
    mode.addItemList ({ "Mix", "Mix + Master" }, 1);
    juce::StringArray langs;
    langs.add ("English");
    langs.add (juce::String::fromUTF8 ("\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a"));
    lang.addItemList (langs, 1);

    roleA   = std::make_unique<ComboAttach>  (proc.apvts, "role", role);
    modeA   = std::make_unique<ComboAttach>  (proc.apvts, "mode", mode);
    langA   = std::make_unique<ComboAttach>  (proc.apvts, "lang", lang);
    amountA = std::make_unique<SliderAttach> (proc.apvts, "amount", amount);
    outA    = std::make_unique<SliderAttach> (proc.apvts, "out", out);

    amount.setTextValueSuffix (" %");
    out.setTextValueSuffix (" dB");

    auto setup = [this] (juce::Label& l, const juce::String& t) {
        l.setText (t, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (l);
    };
    setup (lRole, "Source"); setup (lMode, "Mode");
    setup (lAmount, "Amount"); setup (lOut, "Output"); setup (lLang, "Language");

    status.setJustificationType (juce::Justification::centred);
    status.setColour (juce::Label::textColourId, juce::Colour (0xffff6a3d));
    status.setFont (juce::Font (juce::FontOptions (16.0f)));

    report.setMultiLine (true, true);
    report.setReadOnly (true);
    report.setCaretVisible (false);
    report.setScrollbarsShown (true);
    report.setFont (juce::Font (juce::FontOptions (14.0f)));
    report.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff1c1f27));
    report.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    report.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2a2e38));

    for (auto* c : std::initializer_list<juce::Component*> { &role, &mode, &lang, &amount, &out, &learn, &status, &report })
        addAndMakeVisible (c);

    learn.onClick = [this] { proc.startLearning(); };

    setSize (520, 620);
    startTimerHz (10);
}

void RapAutoMixEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14161c));
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (22.0f)));
    g.drawText ("Rap Auto Mix", 16, 10, 488, 30, juce::Justification::left);
}

void RapAutoMixEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (40);

    auto row = [&area] (juce::Label& l, juce::Component& c) {
        auto r = area.removeFromTop (30);
        l.setBounds (r.removeFromLeft (90));
        c.setBounds (r);
        area.removeFromTop (6);
    };
    row (lRole, role);
    row (lMode, mode);
    row (lAmount, amount);
    row (lOut, out);
    row (lLang, lang);

    learn.setBounds (area.removeFromTop (36));
    area.removeFromTop (8);
    status.setBounds (area.removeFromTop (26));
    area.removeFromTop (6);
    report.setBounds (area);
}

void RapAutoMixEditor::timerCallback()
{
    const bool ar = lang.getSelectedItemIndex() == 1;
    auto A = [] (const char* u) { return juce::String::fromUTF8 (u); };

    learn.setEnabled (! proc.isLearning());
    if (proc.isLearning())
        status.setText ((ar ? A ("جاري الاستماع... ") : juce::String ("Listening... ")) + juce::String ((int) (proc.getLearnProgress() * 100.0f)) + " %",
                        juce::dontSendNotification);
    else if (proc.hasLearned())
        status.setText ((ar ? A ("جودة الميكس: ") : juce::String ("Mix quality: ")) + juce::String ((int) proc.getScoreBefore()) + " %  ->  "
                        + juce::String ((int) proc.getScoreAfter()) + " %", juce::dontSendNotification);
    else
        status.setText (ar ? A ("اضغط Learn ثم شغّل التراك") : juce::String ("Press Learn, then play the track"), juce::dontSendNotification);

    const auto txt = proc.buildReport (ar);
    if (txt != lastText || ar != lastAr)
    {
        report.setJustification (ar ? juce::Justification::topRight : juce::Justification::topLeft);
        report.setText (txt, false);
        lastText = txt; lastAr = ar;
    }
}
