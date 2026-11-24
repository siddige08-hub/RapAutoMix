#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>

class RapAutoMixProcessor : public juce::AudioProcessor
{
public:
    RapAutoMixProcessor();
    ~RapAutoMixProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Rap Auto Mix"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    // UI side
    void startLearning();
    bool isLearning() const { return learning.load(); }
    bool hasLearned() const { return learned.load(); }
    float getLearnProgress() const { return (float) learnedSamples.load() / (float) juce::jmax (1, learnTarget.load()); }
    float getScoreBefore() const { return scoreBefore.load(); }
    float getScoreAfter() const { return scoreAfter.load(); }
    juce::String buildReport (bool arabic) const;   // coach text, call from the message thread

private:
    static constexpr int fftOrder = 13;
    static constexpr int fftSize  = 1 << fftOrder;
    static constexpr int nBands   = 24;

    void analyse (const juce::AudioBuffer<float>&, int nCh);
    void runFrame();
    void finishLearning();
    void updateFilters (int role, float amt);

    double sr = 44100.0;

    // learned settings (written by audio thread, read by audio thread, saved in state)
    std::array<std::atomic<float>, 4> autoEq;   // low shelf, mud, presence, air (dB)
    std::atomic<float> autoThresh { 0.0f }, autoRatio { 1.0f }, autoDrive { 0.0f };
    std::atomic<float> scoreBefore { 0.0f }, scoreAfter { 0.0f };
    std::atomic<float> tPeak { -100.0f }, tRms { -100.0f }, tSnr { 0.0f }, tLowErr { 0.0f };
    std::atomic<int> tClips { 0 };
    std::atomic<bool> learning { false }, learned { false }, resetRequest { false };
    std::atomic<int> learnedSamples { 0 }, learnTarget { 1 };

    // analysis state (audio thread only)
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fifo, fftData;
    int fifoPos = 0, frames = 0;
    std::array<double, nBands> bandPow;
    double sumSq = 0.0, sumCount = 0.0;
    float peak = 0.0f;
    int clipRun = 0, clipEvents = 0, frameCount = 0, frameFill = 0;
    float frameAcc = 0.0f;
    std::array<float, 1024> frameMs {};

    // DSP
    juce::dsp::IIR::Filter<float> hp[2], lo[2], mu[2], pr[2], ai[2];
    std::array<float, 5> lastKey { { -999.f, -999.f, -999.f, -999.f, -999.f } };
    juce::dsp::Compressor<float> comp;
    juce::dsp::Limiter<float> limiter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RapAutoMixProcessor)
};
