#include <Arduino.h>
#include <driver/i2s.h>
#include <cstring>

#include <AudioGeneratorMP3.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioOutput.h>

#include "SoundHandler.h"

static constexpr size_t AUDIO_URL_SIZE = 201;
static constexpr size_t AUDIO_BUFFER_SIZE = 4096;
static constexpr float AUDIO_GAIN = 0.65f;

// Set from the hook-switch ISR to silence playback immediately.
static volatile bool audioMuted = false;

// I2S clock currently programmed, so the tune can restore the stream rate.
static uint32_t currentI2sRate = SAMPLE_RATE;

class ExistingI2SOutput : public AudioOutput {
public:
    ExistingI2SOutput() {
        SetRate(SAMPLE_RATE);
        SetBitsPerSample(16);
        SetChannels(2);
        SetGain(AUDIO_GAIN);
    }

    bool begin() override {
        sampleCount = 0;
        return true;
    }

    bool SetRate(int rate) override {
        if (rate <= 0) return false;
        hertz = rate;
        if (i2s_set_clk(I2S_PORT, (uint32_t)rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) != ESP_OK) {
            return false;
        }
        currentI2sRate = (uint32_t)rate;
        return true;
    }

    bool SetBitsPerSample(int bits) override {
        if (bits != 16) return false;
        bps = bits;
        return true;
    }

    bool SetChannels(int channelCount) override {
        if (channelCount != 1 && channelCount != 2) return false;
        channels = channelCount;
        return true;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        samples[sampleCount++] = (int16_t)(sample[0] * AUDIO_GAIN);
        samples[sampleCount++] = (int16_t)((channels == 1 ? sample[0] : sample[1]) * AUDIO_GAIN);
        if (sampleCount >= SAMPLE_CAPACITY) {
            return writeBuffer();
        }
        return true;
    }

    void flush() override {
        if (sampleCount > 0) writeBuffer();
    }

    bool stop() override {
        flush();
        return true;
    }

private:
    static constexpr size_t SAMPLE_CAPACITY = 512;
    int16_t samples[SAMPLE_CAPACITY];
    size_t sampleCount = 0;

    bool writeBuffer() {
        if (sampleCount == 0) return true;
        if (audioMuted) {
            memset(samples, 0, sampleCount * sizeof(int16_t));
        }
        size_t bytesToWrite = sampleCount * sizeof(int16_t);
        size_t bytesWritten = 0;
        esp_err_t result = i2s_write(I2S_PORT, samples, bytesToWrite, &bytesWritten, portMAX_DELAY);
        if (audioMuted) {
            i2s_zero_dma_buffer(I2S_PORT);
        }
        sampleCount = 0;
        return result == ESP_OK && bytesWritten == bytesToWrite;
    }
};

static char pendingAudioUrl[AUDIO_URL_SIZE] = {};
static bool audioPending = false;

// --- Confirmation tune ---------------------------------------------------
// Two soft descending notes (E5, C5) with a bell-like decay, synthesised once
// into a PCM buffer. Kept quiet and well below the stream level.
static constexpr uint16_t TUNE_NOTES[] = {659, 523};
static constexpr uint16_t TUNE_NOTE_MS[] = {240, 280};
static constexpr uint16_t TUNE_GAP_MS = 45;
// Silence on both sides of the chime. The lead-in makes the tune start after
// the beeps have died away (no clipped start), the matching tail keeps it from
// cutting straight back into the stream, so the cue sounds the same length at
// both ends.
static constexpr uint16_t TUNE_LEAD_MS = 120;
static constexpr uint16_t TUNE_TAIL_MS = 120;
static constexpr uint32_t TUNE_ATTACK_MS = 20;
static constexpr uint32_t TUNE_DECAY_TAIL_MS = 15;
static constexpr size_t TUNE_NOTE_COUNT = sizeof(TUNE_NOTES) / sizeof(TUNE_NOTES[0]);
static constexpr size_t TUNE_LEAD_FRAMES = SAMPLE_RATE * TUNE_LEAD_MS / 1000;
static constexpr size_t TUNE_TAIL_FRAMES = SAMPLE_RATE * TUNE_TAIL_MS / 1000;
static constexpr size_t TUNE_FRAMES =
    TUNE_LEAD_FRAMES + (SAMPLE_RATE * TUNE_NOTE_MS[0] / 1000) +
    (SAMPLE_RATE * TUNE_GAP_MS / 1000) + (SAMPLE_RATE * TUNE_NOTE_MS[1] / 1000) +
    TUNE_TAIL_FRAMES;
static constexpr size_t TUNE_CHUNK_FRAMES = 128;
static constexpr int16_t TUNE_AMPLITUDE = (int16_t)(0.16f * 32767.0f);

static int16_t tuneBuffer[TUNE_FRAMES * 2];
static bool tuneBufferReady = false;
static bool tuneActive = false;
static size_t tuneFrame = 0;
static uint32_t tuneRestoreRate = SAMPLE_RATE;
static uint32_t tuneHoldStart = 0;
// How long the chime may repeat while the announcement is still being fetched,
// so a failed TTS never turns into an endless jingle. Set by the caller.
static uint32_t tuneHoldMaxMs = 0;

static void buildTuneBuffer() {
    const uint32_t attackFrames = SAMPLE_RATE * TUNE_ATTACK_MS / 1000;
    const uint32_t tailFrames = SAMPLE_RATE * TUNE_DECAY_TAIL_MS / 1000;
    // Lead-in and tail stay silent.
    memset(tuneBuffer, 0, sizeof(tuneBuffer));
    size_t frame = TUNE_LEAD_FRAMES;

    for (size_t note = 0; note < TUNE_NOTE_COUNT; note++) {
        const uint32_t noteFrames = SAMPLE_RATE * TUNE_NOTE_MS[note] / 1000;
        // Bell-like decay: soft attack, then a smooth exponential falloff.
        const float decayTau = noteFrames * 0.45f;
        for (uint32_t i = 0; i < noteFrames; i++, frame++) {
            float envelope = expf(-(float)i / decayTau);
            if (i < attackFrames) {
                envelope *= (float)i / attackFrames;
            }
            if (i > noteFrames - tailFrames) {
                envelope *= (float)(noteFrames - i) / tailFrames;
            }
            const float phase = 2.0f * PI * TUNE_NOTES[note] * i / SAMPLE_RATE;
            const int16_t sample = (int16_t)(sinf(phase) * TUNE_AMPLITUDE * envelope);
            tuneBuffer[frame * 2] = sample;
            tuneBuffer[frame * 2 + 1] = sample;
        }

        const uint32_t gapFrames =
            (note + 1 < TUNE_NOTE_COUNT) ? (SAMPLE_RATE * TUNE_GAP_MS / 1000) : 0;
        for (uint32_t i = 0; i < gapFrames; i++, frame++) {
            tuneBuffer[frame * 2] = 0;
            tuneBuffer[frame * 2 + 1] = 0;
        }
    }
    tuneBufferReady = true;
}

// Writes the next chunk of the tune. Returns true while more remains.
static bool writeTuneChunk() {
    const size_t remaining = TUNE_FRAMES - tuneFrame;
    const size_t frames = remaining < TUNE_CHUNK_FRAMES ? remaining : TUNE_CHUNK_FRAMES;
    size_t bytesWritten = 0;
    i2s_write(I2S_PORT, &tuneBuffer[tuneFrame * 2], frames * 2 * sizeof(int16_t),
              &bytesWritten, portMAX_DELAY);
    tuneFrame += frames;
    return tuneFrame < TUNE_FRAMES;
}

static void finishTune() {
    tuneActive = false;
    i2s_set_clk(I2S_PORT, tuneRestoreRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

void playConfirmationTune(uint32_t holdMs) {
    if (!tuneBufferReady) buildTuneBuffer();
    tuneHoldMaxMs = holdMs;

    // Take over the I2S clock for the tune; playback resumes at its own rate
    // afterwards (MP3 decode is paused while the tune is active).
    tuneRestoreRate = currentI2sRate;
    i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    i2s_zero_dma_buffer(I2S_PORT);
    tuneFrame = 0;
    tuneHoldStart = millis();
    tuneActive = true;
}

// --- Digit beep ----------------------------------------------------------
// One short, quiet pip per successfully detected digit.
static constexpr uint16_t BEEP_HZ = 880;
static constexpr uint16_t BEEP_MS = 60;
static constexpr size_t BEEP_FRAMES = SAMPLE_RATE * BEEP_MS / 1000;
static constexpr size_t BEEP_CHUNK_FRAMES = 64;
static constexpr int16_t BEEP_AMPLITUDE = (int16_t)(0.14f * 32767.0f);

static int16_t beepBuffer[BEEP_FRAMES * 2];
static bool beepBufferReady = false;
static bool beepActive = false;
static size_t beepFrame = 0;
static uint32_t beepRestoreRate = SAMPLE_RATE;

static void buildBeepBuffer() {
    const uint32_t attackFrames = SAMPLE_RATE * 5 / 1000;
    const uint32_t tailFrames = SAMPLE_RATE * 15 / 1000;
    const float decayTau = BEEP_FRAMES * 0.5f;

    for (uint32_t i = 0; i < BEEP_FRAMES; i++) {
        float envelope = expf(-(float)i / decayTau);
        if (i < attackFrames) {
            envelope *= (float)i / attackFrames;
        }
        if (i > BEEP_FRAMES - tailFrames) {
            envelope *= (float)(BEEP_FRAMES - i) / tailFrames;
        }
        const float phase = 2.0f * PI * BEEP_HZ * i / SAMPLE_RATE;
        const int16_t sample = (int16_t)(sinf(phase) * BEEP_AMPLITUDE * envelope);
        beepBuffer[i * 2] = sample;
        beepBuffer[i * 2 + 1] = sample;
    }
    beepBufferReady = true;
}

// Writes the next chunk of the beep. Returns true while more remains.
static bool writeBeepChunk() {
    const size_t remaining = BEEP_FRAMES - beepFrame;
    const size_t frames = remaining < BEEP_CHUNK_FRAMES ? remaining : BEEP_CHUNK_FRAMES;
    size_t bytesWritten = 0;
    i2s_write(I2S_PORT, &beepBuffer[beepFrame * 2], frames * 2 * sizeof(int16_t),
              &bytesWritten, portMAX_DELAY);
    beepFrame += frames;
    return beepFrame < BEEP_FRAMES;
}

static void finishBeep() {
    beepActive = false;
    i2s_set_clk(I2S_PORT, beepRestoreRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

void playDigitBeep() {
    // Never cut off the confirmation chime with a digit pip.
    if (tuneActive) return;
    if (!beepBufferReady) buildBeepBuffer();

    beepRestoreRate = currentI2sRate;
    i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    i2s_zero_dma_buffer(I2S_PORT);
    beepFrame = 0;
    beepActive = true;
}

static AudioFileSourceHTTPStream* httpSource = nullptr;
static AudioFileSourceBuffer* audioBuffer = nullptr;
static AudioGeneratorMP3* mp3 = nullptr;
static ExistingI2SOutput* audioOutput = nullptr;

static void cleanupAudio() {
    if (mp3) {
        if (mp3->isRunning()) mp3->stop();
        delete mp3; mp3 = nullptr;
    }
    if (audioBuffer) { delete audioBuffer; audioBuffer = nullptr; }
    if (httpSource) { httpSource->close(); delete httpSource; httpSource = nullptr; }
    if (audioOutput) { audioOutput->stop(); delete audioOutput; audioOutput = nullptr; }
}

void IRAM_ATTR audioMuteImmediately() {
    audioMuted = true;
}

void audioResetMute() {
    audioMuted = false;
}

void setupSound() {
    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 8;
    config.dma_buf_len = 256;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_BCLK;
    pins.ws_io_num = PIN_LRC;
    pins.data_out_num = PIN_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    esp_err_t result = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
    if (result != ESP_OK) {
        Serial.printf("Audio I2S init failed: %d\n", result);
        return;
    }
    result = i2s_set_pin(I2S_PORT, &pins);
    if (result != ESP_OK) {
        Serial.printf("Audio I2S pin setup failed: %d\n", result);
        i2s_driver_uninstall(I2S_PORT);
        return;
    }
    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("Audio: I2S ready");
}

bool queueAudioUrl(const char* url) {
    if (!url || url[0] == '\0' || strlen(url) >= AUDIO_URL_SIZE) return false;
    strlcpy(pendingAudioUrl, url, sizeof(pendingAudioUrl));
    audioPending = true;
    return true;
}

void stopAudio() {
    cleanupAudio();
    audioPending = false;
    pendingAudioUrl[0] = '\0';
    if (tuneActive) {
        finishTune();
        i2s_zero_dma_buffer(I2S_PORT);
    }
    if (beepActive) {
        finishBeep();
        i2s_zero_dma_buffer(I2S_PORT);
    }
}

bool isAudioPlaying() { return mp3 && mp3->isRunning(); }

// Opens the pending track: HTTP source, read buffer, I2S output and decoder
// object are created, but decoding is not started until the main playback path
// calls begin(). The HTTP connect blocks, so it is only run while the I2S DMA
// buffer is full - the already queued audio keeps sounding for ~130 ms.
static bool prepareAudio() {
    if (mp3) return true;
    if (!audioPending) return false;

    audioPending = false;
    audioMuted = false;
    httpSource = new AudioFileSourceHTTPStream();
    if (httpSource) {
        httpSource->useHTTP10();   // avoid chunked transfer encoding
    }
    if (!httpSource || !httpSource->open(pendingAudioUrl) || !httpSource->isOpen()) {
        Serial.println("Audio HTTP connection failed");
        cleanupAudio();
        return false;
    }
    audioBuffer = new AudioFileSourceBuffer(httpSource, AUDIO_BUFFER_SIZE);
    audioOutput = new ExistingI2SOutput();
    mp3 = new AudioGeneratorMP3();
    return true;
}

void handleAudio() {
    // If the hook went down, cancel everything immediately instead of letting
    // the tune/beep/MP3 keep producing sound until the main loop cleans up.
    if (audioMuted) {
        if (mp3) cleanupAudio();
        if (tuneActive) { finishTune(); i2s_zero_dma_buffer(I2S_PORT); }
        if (beepActive) { finishBeep(); i2s_zero_dma_buffer(I2S_PORT); }
        audioPending = false;
        pendingAudioUrl[0] = '\0';
        // Cancellation is done; allow future playback to start normally.
        audioMuted = false;
        return;
    }

    // The confirmation tune has priority; normal playback waits for it.
    if (tuneActive) {
        if (writeTuneChunk()) return;

        // One full pass of the chime is done. Rather than fall silent, keep
        // repeating it until the announcement from Home Assistant has been
        // fetched and can play, so the hand-over has no dead air. The fetch is
        // attempted at this quiet boundary, where a blocking connect is least
        // audible.
        if (!mp3) prepareAudio();
        if (mp3 == nullptr && millis() - tuneHoldStart < tuneHoldMaxMs) {
            tuneFrame = 0;
            return;
        }
        finishTune();
    }

    // Short digit pips also take priority over normal playback.
    if (beepActive) {
        if (writeBeepChunk()) return;
        finishBeep();
    }

    if (!mp3) {
        if (!prepareAudio()) return;
    }
    if (!mp3->isRunning()) {
        if (!mp3->begin(audioBuffer, audioOutput)) {
            Serial.println("Audio MP3 decoder failed to start");
            cleanupAudio();
            return;
        }
    }
    if (!mp3->loop()) cleanupAudio();
}
