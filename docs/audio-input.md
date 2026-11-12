# Polling audio input

The runtime connects Ocarina's polling `nn::audio` input calls to SDL3's default recording device. Select the
microphone or audio interface as the operating system's default input and enable the game's microphone control.
Both a hummed note and an instrument received by that input go through the game's own note recognition.

At `OpenInputStream` the runtime checks that a default input exists and prepares a buffer. It opens the recording
device at `StartInputStream` and releases it at stop or close. After a stop, the game can still read or purge the
buffered frames and start capture again. Recordings stay in memory. SDL converts the input device's format to the
requested PCM format and sample rate.
The capture callback uses a bounded ring: unread overflow drops the oldest complete frames instead of retaining
an unlimited recording while the game is paused. Stop, purge and close are distinct operations. Purging an unopened
or failed stream is safe, including the null-stream path that previously crashed in `InputStreamImpl::Purge`.

This implements the non-event polling overload used by Ocarina 1.1.1. Event-based streams, per-device selection,
hot-unplug recovery and packed 24-bit PCM are not implemented. Statistics return a neutral 24-byte structure.
An unsupported format or missing default device returns an error at open. If the device disappears or access is
denied when capture starts, start returns an input error and the game can retry. Input follows the operating system's
default recording device; there is no new runtime menu setting.

## Validation

`python tests/audio_input_check.py --toolchain C:/msys64/ucrt64` builds and runs headless checks using SDL's dummy
audio driver. They require no game files or physical microphone. They check that idle streams leave the device
closed, that a denied start can be retried, and that stopping one stream releases its device and preserves unread
frames. They also cover ring wrap and overflow, frame counts, repeated open/close, unsupported formats and
purge/read after a failed open. On Linux, omit `--toolchain` and install SDL3's development package and pkg-config.

A separate local real-device check received mono signed PCM16 at 48 kHz, verified the 4800-frame ring remained
bounded, and opened and closed the device three times. In an Ocarina 1.1.1 Windows session built from the 0.5.0
runtime, the player enabled the ocarina microphone control and confirmed that it worked without the previous crash.
The game's actual requested format was mono signed PCM16 at 48 kHz, capacity 4800 frames. The experimental session
does not establish compatibility with other game updates, drivers or input devices.
