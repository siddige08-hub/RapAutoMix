#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

namespace
{
    // per role: 0 = Vocal, 1 = Beat / Instrument, 2 = Full mix
    constexpr float kRatio[3]   = { 3.5f, 2.5f, 1.8f };
    constexpr float kAttack[3]  = { 8.0f, 20.0f, 30.0f };
    constexpr float kRelease[3] = { 120.0f, 150.0f, 200.0f };
    constexpr float kHp[3]      = { 90.0f, 25.0f, 25.0f };
    constexpr float kSat[3]     = { 0.15f, 0.10f, 0.06f };
    constexpr float kTargetRms  = -10.0f;   // master loudness goal (dBFS RMS, approximate)

    // Target tonal curve in dB relative to 1 kHz, measured per log-spaced band.
    float targetDb (float f, int role)
    {
        if (role == 0)
        {
            if (f < 200.0f)  return -3.0f * std::log2 (200.0f / f);
            if (f < 4000.0f) return 0.0f;
            return -1.8f * std::log2 (f / 4000.0f);
        }
        const float oct = std::log2 (f / 1000.0f);
        return oct < 0.0f ? -0.8f * oct : -1.3f * oct;
    }

    float bandCenter (int b, int n) { return 20.0f * std::pow (1000.0f, ((float) b + 0.5f) / (float) n); }

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout l;
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "role", 1 }, "Source",
                                                       StringArray { "Vocal", "Beat / Instrument", "Full mix" }, 0));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
                                                       StringArray { "Mix", "Mix + Master" }, 0));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "Amount",
                                                      NormalisableRange<float> (0.0f, 100.0f, 1.0f), 70.0f));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "out", 1 }, "Output",
                                                      NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
        StringArray langs;
        langs.add ("English");
        langs.add (String::fromUTF8 ("\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a"));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "lang", 1 }, "Language", langs, 0));
        return l;
    }
}

RapAutoMixProcessor::RapAutoMixProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto& a : autoEq) a = 0.0f;
    bandPow.fill (0.0);
    fifo.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);
}

void RapAutoMixProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
    comp.prepare (spec);
    limiter.prepare (spec);
    for (int c = 0; c < 2; ++c) { hp[c].reset(); lo[c].reset(); mu[c].reset(); pr[c].reset(); ai[c].reset(); }
    lastKey.fill (-999.0f);
}

bool RapAutoMixProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == l.getMainInputChannelSet();
}

void RapAutoMixProcessor::startLearning()
{
    resetRequest = true;
    learning = true;
}

void RapAutoMixProcessor::analyse (const juce::AudioBuffer<float>& b, int nCh)
{
    if (resetRequest.exchange (false))
    {
        bandPow.fill (0.0);
        frames = 0; sumSq = 0.0; sumCount = 0.0; peak = 0.0f; fifoPos = 0;
        clipRun = 0; clipEvents = 0; frameCount = 0; frameFill = 0; frameAcc = 0.0f;
        learnedSamples = 0;
        learnTarget = (int) (8.0 * sr);   // 8 seconds of playback
    }

    const int n = b.getNumSamples();
    for (int i = 0; i < n; ++i)
    {
        float m = 0.0f, mx = 0.0f;
        for (int c = 0; c < nCh; ++c)
        {
            const float x = b.getSample (c, i);
            m += x; sumSq += (double) x * x;
            mx = juce::jmax (mx, std::abs (x));
        }
        peak = juce::jmax (peak, mx);
        if (mx >= 0.995f) { if (++clipRun == 2) ++clipEvents; } else clipRun = 0;   // a run of 2+ samples at the ceiling = one clipping event
        const float mono = m / (float) nCh;
        frameAcc += mono * mono;
        if (++frameFill == 1024)
        {
            if (frameCount < 1024) frameMs[(size_t) frameCount++] = frameAcc / 1024.0f;
            frameAcc = 0.0f; frameFill = 0;
        }
        sumCount += nCh;
        fifo[(size_t) fifoPos++] = mono;
        if (fifoPos == fftSize) { runFrame(); fifoPos = 0; }
    }

    learnedSamples += n;
    if (learnedSamples.load() >= learnTarget.load())
        finishLearning();
}

void RapAutoMixProcessor::runFrame()
{
    float ms = 0.0f;
    for (int i = 0; i < fftSize; ++i) ms += fifo[(size_t) i] * fifo[(size_t) i];
    if (ms / (float) fftSize < 1.0e-6f) return;   // skip silence

    std::fill (fftData.begin(), fftData.end(), 0.0f);
    std::copy (fifo.begin(), fifo.end(), fftData.begin());
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const double logRange = std::log (1000.0);
    for (int k = 1; k < fftSize / 2; ++k)
    {
        const double f = (double) k * sr / (double) fftSize;
        if (f < 20.0 || f >= 20000.0) continue;
        const int b = (int) std::floor ((double) nBands * std::log (f / 20.0) / logRange);
        if (b >= 0 && b < nBands)
            bandPow[(size_t) b] += (double) fftData[(size_t) k] * (double) fftData[(size_t) k];
    }
    ++frames;
}

void RapAutoMixProcessor::finishLearning()
{
    learning = false;
    if (frames < 4 || sumCount <= 0.0) return;   // not enough signal heard

    const int role = (int) apvts.getRawParameterValue ("role")->load();
    const float amt = apvts.getRawParameterValue ("amount")->load() / 100.0f;

    std::array<float, nBands> db {}, err {};
    std::array<bool, nBands> ok {};
    float ref = 0.0f; int rc = 0;

    for (int b = 0; b < nBands; ++b)
    {
        if (bandPow[(size_t) b] > 0.0)
        {
            db[(size_t) b] = 10.0f * std::log10 ((float) (bandPow[(size_t) b] / frames));
            ok[(size_t) b] = true;
            const float f = bandCenter (b, nBands);
            if (f >= 400.0f && f <= 2000.0f) { ref += db[(size_t) b]; ++rc; }
        }
    }
    if (rc == 0) return;
    ref /= (float) rc;

    float gsum[4] = { 0, 0, 0, 0 }; int gcnt[4] = { 0, 0, 0, 0 };
    auto groupOf = [] (float f) {
        if (f < 150.0f) return 0;
        if (f >= 200.0f && f <= 500.0f) return 1;
        if (f >= 2000.0f && f <= 5000.0f) return 2;
        if (f > 8000.0f) return 3;
        return -1;
    };

    for (int b = 0; b < nBands; ++b)
    {
        if (! ok[(size_t) b]) continue;
        const float f = bandCenter (b, nBands);
        err[(size_t) b] = juce::jlimit (-12.0f, 12.0f, targetDb (f, role) - (db[(size_t) b] - ref));
        const int g = groupOf (f);
        if (g >= 0) { gsum[g] += err[(size_t) b]; ++gcnt[g]; }
    }

    const float lim[4][2] = { { -5, 5 }, { -6, 2 }, { -4, 4 }, { -4, 5 } };
    float gain[4];
    for (int g = 0; g < 4; ++g)
    {
        gain[g] = gcnt[g] ? juce::jlimit (lim[g][0], lim[g][1], gsum[g] / (float) gcnt[g]) : 0.0f;
        autoEq[(size_t) g] = gain[g];
    }

    // quality score: how far the tonal balance is from the target, before and after the correction
    float before = 0.0f, after = 0.0f; int cnt = 0;
    for (int b = 0; b < nBands; ++b)
    {
        const float f = bandCenter (b, nBands);
        if (! ok[(size_t) b] || f < 40.0f || f > 16000.0f) continue;
        const int g = groupOf (f);
        const float applied = g >= 0 ? gain[g] * amt : 0.0f;
        before += std::abs (err[(size_t) b]);
        after  += std::abs (err[(size_t) b] - applied);
        ++cnt;
    }
    if (cnt > 0)
    {
        scoreBefore = juce::jlimit (0.0f, 100.0f, 100.0f - 6.0f * before / (float) cnt);
        scoreAfter  = juce::jlimit (0.0f, 100.0f, 100.0f - 6.0f * after  / (float) cnt);
    }

    // dynamics + loudness
    const float rmsDb  = 10.0f * std::log10 ((float) (sumSq / sumCount) + 1.0e-12f);
    const float peakDb = 20.0f * std::log10 (peak + 1.0e-9f);
    const float crest  = peakDb - rmsDb;
    const float strength = juce::jlimit (0.2f, 1.0f, (crest - 8.0f) / 8.0f);

    autoThresh = rmsDb + (role == 0 ? 1.0f : 2.0f);
    autoRatio  = 1.0f + (kRatio[role] - 1.0f) * strength;
    autoDrive  = juce::jlimit (0.0f, 14.0f, kTargetRms - rmsDb);

    // take check: clipping, level, noise floor, low-end excess
    {
        float snr = 99.0f;
        if (frameCount >= 20)
        {
            std::array<float, 1024> tmp = frameMs;
            std::sort (tmp.begin(), tmp.begin() + frameCount);
            auto q = [&] (float pc) {
                const int idx = juce::jlimit (0, frameCount - 1, (int) (pc * (float) (frameCount - 1)));
                return 10.0f * std::log10 (tmp[(size_t) idx] + 1.0e-12f);
            };
            snr = q (0.9f) - q (0.1f);
        }
        tPeak = peakDb; tRms = rmsDb; tSnr = snr; tClips = clipEvents;
        tLowErr = gcnt[0] ? gsum[0] / (float) gcnt[0] : 0.0f;
    }
    learned = true;
}

void RapAutoMixProcessor::updateFilters (int role, float amt)
{
    const float hpF = juce::jmap (amt, 20.0f, kHp[role]);
    const float gl = autoEq[0].load() * amt, gm = autoEq[1].load() * amt;
    const float gp = autoEq[2].load() * amt, ga = autoEq[3].load() * amt;

    const std::array<float, 5> key { { hpF, gl, gm, gp, ga } };
    bool same = true;
    for (size_t i = 0; i < 5; ++i) if (std::abs (key[i] - lastKey[i]) > 1.0e-3f) same = false;
    if (same) return;
    lastKey = key;

    using C = juce::dsp::IIR::Coefficients<float>;
    auto cHp = C::makeHighPass (sr, hpF);
    auto cLo = C::makeLowShelf (sr, 100.0f, 0.7f, juce::Decibels::decibelsToGain (gl));
    auto cMu = C::makePeakFilter (sr, 300.0f, 1.0f, juce::Decibels::decibelsToGain (gm));
    auto cPr = C::makePeakFilter (sr, 3500.0f, 0.9f, juce::Decibels::decibelsToGain (gp));
    auto cAi = C::makeHighShelf (sr, 11000.0f, 0.7f, juce::Decibels::decibelsToGain (ga));
    for (int c = 0; c < 2; ++c)
    {
        hp[c].coefficients = cHp; lo[c].coefficients = cLo; mu[c].coefficients = cMu;
        pr[c].coefficients = cPr; ai[c].coefficients = cAi;
    }
}

void RapAutoMixProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int nCh = juce::jmin (2, buffer.getNumChannels());
    const int n = buffer.getNumSamples();
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, n);

    if (learning.load()) analyse (buffer, nCh);

    const int   role   = (int) apvts.getRawParameterValue ("role")->load();
    const bool  master = (int) apvts.getRawParameterValue ("mode")->load() == 1;
    const float amt    = apvts.getRawParameterValue ("amount")->load() / 100.0f;
    const float outDb  = apvts.getRawParameterValue ("out")->load();

    updateFilters (role, amt);

    // 1) EQ
    for (int c = 0; c < nCh; ++c)
    {
        auto* d = buffer.getWritePointer (c);
        for (int i = 0; i < n; ++i)
        {
            float x = d[i];
            x = hp[c].processSample (x);
            x = lo[c].processSample (x);
            x = mu[c].processSample (x);
            x = pr[c].processSample (x);
            x = ai[c].processSample (x);
            d[i] = x;
        }
    }

    // 2) Compressor (settings learned from the song)
    juce::dsp::AudioBlock<float> block (buffer);
    auto sub = block.getSubsetChannelBlock (0, (size_t) nCh);
    comp.setThreshold (autoThresh.load());
    comp.setRatio (1.0f + (autoRatio.load() - 1.0f) * amt);
    comp.setAttack (kAttack[role]);
    comp.setRelease (kRelease[role]);
    {
        juce::dsp::ProcessContextReplacing<float> ctx (sub);
        comp.process (ctx);
    }

    // 3) Parallel saturation for warmth
    const float mixSat = kSat[role] * amt * 3.0f;
    const float norm = 1.0f / std::tanh (2.0f);
    for (int c = 0; c < nCh; ++c)
    {
        auto* d = buffer.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            d[i] = (1.0f - mixSat) * d[i] + mixSat * std::tanh (2.0f * d[i]) * norm;
    }

    // 4) Stereo width (master only)
    if (master && nCh == 2)
    {
        const float w = 1.0f + 0.12f * amt;
        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]);
            const float s = 0.5f * (l[i] - r[i]) * w;
            l[i] = m + s; r[i] = m - s;
        }
    }

    // 5) Gain, then the limiter when mastering
    const float totalDb = outDb + (master ? autoDrive.load() * amt : 0.0f);
    buffer.applyGain (juce::Decibels::decibelsToGain (totalDb));

    if (master)
    {
        limiter.setThreshold (-1.0f);
        limiter.setRelease (80.0f);
        juce::dsp::ProcessContextReplacing<float> ctx (sub);
        limiter.process (ctx);
    }
}

juce::String RapAutoMixProcessor::buildReport (bool ar) const
{
    auto A  = [] (const char* u) { return juce::String::fromUTF8 (u); };
    auto L  = [ar] (const juce::String& en, const juce::String& arT) { return ar ? arT : en; };
    auto f1 = [] (float v) { return juce::String (v, 1); };

    if (! learned.load())
        return L ("Press Learn, then play the loudest part of the track (8 seconds).",
                  A ("اضغط Learn ثم شغّل أعلى جزء في التراك (8 ثواني)."));

    const int   role   = (int) apvts.getRawParameterValue ("role")->load();
    const bool  master = (int) apvts.getRawParameterValue ("mode")->load() == 1;
    const float amt    = apvts.getRawParameterValue ("amount")->load() / 100.0f;
    const float peak = tPeak.load(), rms = tRms.load(), snr = tSnr.load();
    const int   clips = tClips.load();

    juce::String s;

    // ---- 1) take check ----
    s << L ("1) TAKE CHECK", A ("1) فحص التيك")) << "\n";
    if (clips >= 3)
    {
        s << L ("- CLIPPED: " + juce::String (clips) + " clipping events. Lower the input gain by at least 6 dB and re-record. Clipping can't be repaired afterwards.",
                "- " + A ("مضروب: ") + juce::String (clips) + A (" مواضع ضرب. خفّض الجين 6 dB على الأقل وأعد التسجيل. الضرب ما ينصلح بعد التسجيل.")) << "\n";
    }
    else if (peak < -18.0f)
    {
        const float up = juce::jmin (20.0f, -6.0f - peak);
        if (role == 0 && snr < 30.0f)
            s << L ("- WEAK and NOISY (peak " + f1 (peak) + " dBFS). Re-record with the input gain raised about " + f1 (up) + " dB. Boosting it in software would boost the noise too.",
                    "- " + A ("ضعيف وفيه ضوضاء (أعلى مستوى ") + f1 (peak) + A (" dBFS). أعد التسجيل ورفع الجين حوالي ") + f1 (up) + A (" dB. الرفع بالبرنامج يرفع الضوضاء معه.")) << "\n";
        else
            s << L ("- WEAK but CLEAN (peak " + f1 (peak) + " dBFS). No need to re-record: raise the clip gain about " + f1 (up) + " dB in FL.",
                    "- " + A ("ضعيف لكن نظيف (أعلى مستوى ") + f1 (peak) + A (" dBFS). ما يحتاج إعادة: ارفع جين الكليب حوالي ") + f1 (up) + A (" dB في FL.")) << "\n";
    }
    else if (peak > -1.0f)
    {
        s << L ("- Very close to the ceiling (peak " + f1 (peak) + " dBFS) but not clipped. Lower the input gain about 3 dB next time to stay safe.",
                "- " + A ("قريب جداً من السقف (أعلى مستوى ") + f1 (peak) + A (" dBFS) بدون ضرب. خفّض الجين حوالي 3 dB في المرة الجاية.")) << "\n";
    }
    else
    {
        s << L ("- Level is fine (peak " + f1 (peak) + " dBFS). No re-record needed for level.",
                "- " + A ("المستوى سليم (أعلى مستوى ") + f1 (peak) + A (" dBFS). ما يحتاج إعادة بسبب المستوى.")) << "\n";
    }

    if (role == 0 && snr < 90.0f)
        s << L ("- Estimated signal-to-noise: " + f1 (snr) + " dB (" + (snr >= 45.0f ? "very clean" : snr >= 30.0f ? "acceptable" : "noisy room or high noise") + ").",
                "- " + A ("نسبة الصوت للضوضاء تقريباً: ") + f1 (snr) + " dB ("
                    + A (snr >= 45.0f ? "نظيف جداً" : snr >= 30.0f ? "مقبول" : "غرفة صاخبة أو ضوضاء عالية") + ").") << "\n";

    if (role == 0 && tLowErr.load() < -6.0f)
        s << L ("- Too much energy below ~150 Hz (pops or rumble). Use a pop filter or move back from the mic.",
                "- " + A ("طاقة زايدة تحت 150 هرتز (بوب أو رمبل). استخدم فلتر بوب أو ابعد عن المايك.")) << "\n";

    // ---- 2) what I did and why ----
    s << "\n" << L ("2) WHAT I DID AND WHY", A ("2) إيش سويت وليش")) << "\n";

    const float hpF = juce::jmap (amt, 20.0f, kHp[role]);
    s << L ("- High-pass at " + juce::String ((int) hpF) + " Hz to remove rumble you can't hear but that eats headroom.",
            "- " + A ("فلتر قطع الجهير الخفيض عند ") + juce::String ((int) hpF) + A (" هرتز لإزالة رمبل ما يُسمع لكنه ياخذ مساحة.")) << "\n";

    const char* en[4] = { "low end (100 Hz)", "mud (300 Hz)", "presence (3.5 kHz)", "air (11 kHz)" };
    const char* arN[4] = { "الجهير (100 هرتز)", "الطين (300 هرتز)", "الحضور (3.5 كيلوهرتز)", "الهواء (11 كيلوهرتز)" };
    bool any = false;
    for (int g = 0; g < 4; ++g)
    {
        const float gdb = autoEq[(size_t) g].load() * amt;
        if (std::abs (gdb) < 0.5f) continue;
        any = true;
        s << L ((gdb < 0 ? "- Cut " : "- Boost ") + juce::String (en[g]) + " by " + f1 (std::abs (gdb)) + " dB: "
                    + (gdb < 0 ? "it is louder than the target curve." : "it is weaker than the target curve."),
                "- " + (gdb < 0 ? A ("خفضت ") : A ("رفعت ")) + A (arN[g]) + A (" بمقدار ") + f1 (std::abs (gdb)) + " dB: "
                    + (gdb < 0 ? A ("المنطقة أعلى من منحنى الهدف.") : A ("المنطقة أضعف من منحنى الهدف."))) << "\n";
    }
    if (! any)
        s << L ("- Tone is already close to the target, so the EQ is almost flat.",
                "- " + A ("النغمة قريبة من الهدف أصلاً، فالـ EQ شبه مسطح.")) << "\n";

    const float ratio = 1.0f + (autoRatio.load() - 1.0f) * amt;
    if (ratio < 1.15f)
        s << L ("- Compression barely needed: the take is already even.",
                "- " + A ("الكومبريسور شبه غير مطلوب: التيك متوازن أصلاً.")) << "\n";
    else
        s << L ("- Compressor " + f1 (ratio) + ":1 from " + f1 (autoThresh.load()) + " dB, because peaks sit " + f1 (peak - rms) + " dB above the average level.",
                "- " + A ("كومبريسور ") + f1 (ratio) + ":1 من " + f1 (autoThresh.load()) + A (" dB لأن القمم أعلى من المتوسط بـ ") + f1 (peak - rms) + " dB.") << "\n";

    if (master)
        s << L ("- Master: +" + f1 (autoDrive.load() * amt) + " dB gain toward about -10 dB RMS, then a limiter at -1 dB so it never clips.",
                "- " + A ("ماستر: رفع ") + f1 (autoDrive.load() * amt) + A (" dB باتجاه RMS حوالي -10 dB ثم ليميتر عند -1 dB عشان ما يضرب.")) << "\n";

    s << "\n" << L ("Mix quality: ", A ("جودة الميكس: ")) << juce::String ((int) scoreBefore.load()) << " %  ->  " << juce::String ((int) scoreAfter.load()) << " %\n";
    s << L ("These are estimates from the 8 seconds you played. Your ears decide.",
            A ("هذه تقديرات من الـ 8 ثواني اللي شغلتها. أذنك هي الحكم."));
    return s;
}

juce::AudioProcessorEditor* RapAutoMixProcessor::createEditor() { return new RapAutoMixEditor (*this); }

void RapAutoMixProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("aLow", autoEq[0].load(), nullptr);
    state.setProperty ("aMud", autoEq[1].load(), nullptr);
    state.setProperty ("aPre", autoEq[2].load(), nullptr);
    state.setProperty ("aAir", autoEq[3].load(), nullptr);
    state.setProperty ("aThr", autoThresh.load(), nullptr);
    state.setProperty ("aRat", autoRatio.load(), nullptr);
    state.setProperty ("aDrv", autoDrive.load(), nullptr);
    state.setProperty ("sB", scoreBefore.load(), nullptr);
    state.setProperty ("sA", scoreAfter.load(), nullptr);
    state.setProperty ("learned", learned.load(), nullptr);
    state.setProperty ("tPk", tPeak.load(), nullptr);
    state.setProperty ("tRm", tRms.load(), nullptr);
    state.setProperty ("tSn", tSnr.load(), nullptr);
    state.setProperty ("tLo", tLowErr.load(), nullptr);
    state.setProperty ("tCl", tClips.load(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void RapAutoMixProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        auto vt = juce::ValueTree::fromXml (*xml);
        if (! vt.isValid()) return;
        apvts.replaceState (vt);
        autoEq[0] = (float) vt.getProperty ("aLow", 0.0);
        autoEq[1] = (float) vt.getProperty ("aMud", 0.0);
        autoEq[2] = (float) vt.getProperty ("aPre", 0.0);
        autoEq[3] = (float) vt.getProperty ("aAir", 0.0);
        autoThresh = (float) vt.getProperty ("aThr", 0.0);
        autoRatio  = (float) vt.getProperty ("aRat", 1.0);
        autoDrive  = (float) vt.getProperty ("aDrv", 0.0);
        scoreBefore = (float) vt.getProperty ("sB", 0.0);
        scoreAfter  = (float) vt.getProperty ("sA", 0.0);
        learned = (bool) vt.getProperty ("learned", false);
        tPeak = (float) vt.getProperty ("tPk", -100.0);
        tRms = (float) vt.getProperty ("tRm", -100.0);
        tSnr = (float) vt.getProperty ("tSn", 0.0);
        tLowErr = (float) vt.getProperty ("tLo", 0.0);
        tClips = (int) vt.getProperty ("tCl", 0);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RapAutoMixProcessor(); }
