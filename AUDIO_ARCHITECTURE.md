# Audio Architecture (Working Version)

## What works

This firmware uses **ESP8266Audio** with a **custom `AudioOutput` subclass
(`ExistingI2SOutput`)** that drives the I2S peripheral directly via
`i2s_write()`. The MP3 decoder runs in the **main loop** (`handleAudio()`),
not in a separate FreeRTOS task.

## Why this works on ESP32-C3

- **Single core, no task contention.** The ESP32-C3 has one core. A separate
  audio FreeRTOS task competes with the WiFi/TCP stack and the Arduino loop for
  CPU time, causing buffer underruns ("slow stream, dropouts"). Running decode
  + I2S write in the main loop lets WiFi and MQTT stay responsive naturally.
- **Direct I2S writes.** Writing PCM samples directly with `i2s_write()` and
  `portMAX_DELAY` gives the DMA engine a steady, back-pressured stream instead
  of relying on a library's internal DMA callback timing.
- **Chunked HTTP is handled by the source.** `AudioFileSourceHTTPStream` parses
  chunked transfer encoding correctly, so the MP3 bytes fed to the decoder are
  clean (no stray chunk-size hex leaking into the stream).
- **Small, fixed buffer (4 KB).** Large buffers on a no-PSRAM C3 just delay
  underruns and waste heap; a modest buffer plus main-loop polling keeps
  latency and memory use predictable.

## Why the other approaches failed

- **ESP32-audioI2S (nopsram fork):** streams and decodes in a background task.
  On the single-core C3 this starved WiFi/TCP, producing "slow stream, dropouts
  are possible" and eventual stalls.
- **ESP8266Audio with the library's own `AudioOutputI2S`:** its internal I2S
  handling did not match this board's pin/timing setup and produced crackling.
- **Increasing buffers / TX power / CPU clock:** masked symptoms but never
  fixed the core contention; audio still degraded after the first second.

## Rule: stay on this audio style

Do **not** reintroduce a FreeRTOS audio task, do **not** switch back to
`AudioOutputI2S`, and do **not** replace the custom `ExistingI2SOutput`.
Keep playback driven by `handleAudio()` in `loop()`, with the direct I2S
driver initialized in `setupSound()`. Any future audio work should build on this
pattern.
