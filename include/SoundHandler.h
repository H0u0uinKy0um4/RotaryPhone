#pragma once
#include <Arduino.h>

#define I2S_PORT      I2S_NUM_0
#define PIN_BCLK      2
#define PIN_LRC       1
#define PIN_DOUT      3

// Rate the I2S driver is installed with. It is re-programmed automatically to
// the real sample rate of the stream that is being played.
#define SAMPLE_RATE   16000


// Initialise the I2S output.
void setupSound();

// Request playback of an MP3 stream. Non blocking: the URL is queued and
// picked up by handleAudio() in the main loop.
// Returns false when the URL is invalid or the request could not be queued.
bool queueAudioUrl(const char* url);

// Stop the track that is currently playing and drop anything still queued.
void stopAudio();

// Play a soft two-note confirmation chime.
// The chime repeats until the queued track (the number announcement) can be
// played, so there is no silent gap between the two. `holdMs` limits how long
// it may keep repeating when nothing arrives.
// Non blocking: driven by handleAudio() like normal playback.
void playConfirmationTune(uint32_t holdMs);

// Play a very short, quiet beep confirming one dialed digit.
// Non blocking: driven by handleAudio() like normal playback.
void playDigitBeep();

// Call frequently from the main loop to drive playback.
void handleAudio();

// Set output gain (0.0 .. 1.0, scaled by the base gain).
void setAudioGain(float gain);
float getAudioGain();

// Immediately silence the I2S output (safe to call from an ISR).
void audioMuteImmediately();

// Clear the mute flag set by audioMuteImmediately(), allowing new playback.
// Call when the phone is picked up again.
void audioResetMute();

// True while a track is being played.
bool isAudioPlaying();
