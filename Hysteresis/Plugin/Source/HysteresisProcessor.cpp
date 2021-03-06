#include "HysteresisProcessor.h"

enum
{
    reset = 1048576,
    numSteps = 500,
};

HysteresisProcessor::HysteresisProcessor()
{
    overSample.reset (new dsp::Oversampling<float> (2, 1, dsp::Oversampling<float>::filterHalfBandPolyphaseIIR));

    for (int ch = 0; ch < 2; ++ch)
    {
        drive[ch].reset  (numSteps);
        width[ch].reset  (numSteps);
        sat[ch].reset    (numSteps);
        makeup[ch].reset (numSteps);
    }
}

float HysteresisProcessor::calcMakeup()
{
    return (1.0f + 0.6f * width[0].getTargetValue()) / (0.5f + 1.5f * (1.0f - sat[0].getTargetValue()));
}

void HysteresisProcessor::setDrive (float newDrive)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        drive[ch].setTargetValue (newDrive);
    }
}

void HysteresisProcessor::setWidth (float newWidth)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        width[ch].setTargetValue (newWidth);
        makeup[ch].setTargetValue (calcMakeup());
    }
}

void HysteresisProcessor::setSaturation (float newSaturation)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        sat[ch].setTargetValue (newSaturation);
        makeup[ch].setTargetValue (calcMakeup());
    }
}

void HysteresisProcessor::toggleOnOff (bool shouldBeOn)
{
    if (shouldBeOn == isOn)
        return;

    isChanging = true;
}

void HysteresisProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        drive[ch].skip (numSteps);
        width[ch].skip (numSteps);
        sat[ch].skip (numSteps);
        makeup[ch].skip (numSteps);

        hProcs[ch].setSampleRate ((float) (sampleRate * overSamplingFactor));
        hProcs[ch].cook (drive[ch].getCurrentValue(), width[ch].getCurrentValue(), sat[ch].getCurrentValue());
        hProcs[ch].reset();
    }

    overSample->initProcessing (samplesPerBlock);

    dcBlocker[0].reset (sampleRate);
    dcBlocker[0].calcCoefs (dcFreq, 0.707f);
    dcBlocker[1].reset (sampleRate);
    dcBlocker[1].calcCoefs (dcFreq, 0.707f);
     
    // dcLower[0].reset (sampleRate);
    // dcLower[0].calcCoefs (dcShelfFreq, 0.707f, Decibels::decibelsToGain (-12.0f));
    // dcLower[1].reset (sampleRate);
    // dcLower[1].calcCoefs (dcShelfFreq, 0.707f, Decibels::decibelsToGain (-12.0f));

    fadeBuffer.setSize (2, samplesPerBlock);
    resetCount = 0;
}

void HysteresisProcessor::releaseResources()
{
    overSample->reset();
}

void HysteresisProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& /*midi*/)
{
    if (! isOn && ! isChanging) //bypass
        return;

    if (isChanging || resetCount + buffer.getNumSamples() >= reset)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            fadeBuffer.copyFrom (ch, 0, buffer, ch, 0, buffer.getNumSamples());
    }

    dsp::AudioBlock<float> block (buffer);
    dsp::AudioBlock<float> osBlock;
    
    osBlock = overSample->processSamplesUp (block);

    float* ptrArray[] = { osBlock.getChannelPointer(0), osBlock.getChannelPointer(1) };
    AudioBuffer<float> osBuffer (ptrArray, 2, static_cast<int> (osBlock.getNumSamples()));

    for (int channel = 0; channel < osBuffer.getNumChannels(); ++channel)
    {
        auto* x = osBuffer.getWritePointer (channel);
        for (int samp = 0; samp < osBuffer.getNumSamples(); samp++)
        {
            if (drive[channel].isSmoothing() || width[channel].isSmoothing() || sat[channel].isSmoothing())
                hProcs[channel].cook (drive[channel].getNextValue(), width[channel].getNextValue(), sat[channel].getNextValue());

            x[samp] = hProcs[channel].process (x[samp]) * makeup[channel].getNextValue();
        }
    }

    overSample->processSamplesDown (block);

    resetCount += buffer.getNumSamples();
    if (resetCount >= reset && ! isChanging)
    {
        resetCount = 0;

        dsp::AudioBlock<float> fadeBlock (fadeBuffer);
        dsp::AudioBlock<float> osFadeBlock;

        osFadeBlock = overSample->processSamplesUp (fadeBlock);

        float* fadePtrArray[] = { osFadeBlock.getChannelPointer(0), osFadeBlock.getChannelPointer(1) };
        AudioBuffer<float> osFadeBuffer (fadePtrArray, 2, static_cast<int> (osFadeBlock.getNumSamples()));

        for (int channel = 0; channel < osFadeBuffer.getNumChannels(); ++channel)
        {
            hProcs[channel].reset();

            auto* x = osFadeBuffer.getWritePointer (channel);
            for (int samp = 0; samp < osFadeBuffer.getNumSamples(); samp++)
            {
                x[samp] = hProcs[channel].process (x[samp]) * makeup[channel].getCurrentValue();
            }
        }

        overSample->processSamplesDown (fadeBlock);

        buffer.applyGainRamp (0, buffer.getNumSamples(), 1.0f, 0.0f);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.addFromWithRamp (ch, 0, fadeBuffer.getReadPointer (ch), buffer.getNumSamples(), 0.0f, 1.0f);
    }

    // for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    // {
    //     auto* x = buffer.getWritePointer (channel);
    //     for (int samp = 0; samp < buffer.getNumSamples(); samp++)
    //     {
    //         // x[samp] = dcBlocker[channel].processSample (x[samp]);
    //         // x[samp] = dcLower[channel].processSample (x[samp]);
    //     }
    // }

    if (isChanging)
    {
        if (! isOn) //fading in
        {
            buffer.applyGainRamp (0, buffer.getNumSamples(), 0.0f, 1.0f);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.addFromWithRamp (ch, 0, fadeBuffer.getReadPointer (ch), buffer.getNumSamples(), 1.0f, 0.0f);
        }
        else if (isOn) //fading out
        {
            buffer.applyGainRamp (0, buffer.getNumSamples(), 1.0f, 0.0f);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.addFromWithRamp (ch, 0, fadeBuffer.getReadPointer (ch), buffer.getNumSamples(), 0.0f, 1.0f);
        }

        isChanging = false;
        isOn = ! isOn;
    }
}
