# Audio and media

AudioTrack in native x64 image ART outputs streaming PCM through Client-owned WASAPI shared-mode audio. The image framework manages Java parameter checks, player lifecycle, and callbacks. Service does not render audio.

## PCM output

Each track's MTA worker thread owns the Windows audio interfaces and a bounded queue. Array and direct-buffer writes are copied first, without pinning Java arrays while waiting for buffer space. Supported inputs are mono/stereo PCM8, PCM16, and floating point; format availability is determined by the Windows output endpoint.

Blocking writes wait for actual buffer consumption. Nonblocking writes return the number accepted or zero. pause, stop, and release interrupt waits. Position and timestamps come from actually submitted/consumed frames, not fabricated wall-clock progress. Per-track gain does not change Windows endpoint or whole-session volume.

Permanent audio focus and session information are limited to the current application. Cross-Client focus arbitration, transient/ducking focus, recording, device migration, multichannel output, static tracks, offload, effects, and independent speed/pitch control are not fully supported. ARM64 images and Windows ARM64 do not automatically have the same output capabilities.

Legacy stream queries used by media players map Android audio attributes to the corresponding volume stream for the Windows route. Android audio policy strategy discovery and inverse policy queries report unsupported. Compressed offload capability queries also report unsupported; a successful query does not provide a compressed audio decoder.

## Opus MediaCodec

When the Windows Opus MFT is available, image MediaCodecList can expose `c2.stone.opus.decoder` for unencrypted Opus mapping family zero, producing 48 kHz mono/stereo PCM16.

The decoder uses bounded input/output slots and original image ByteBuffer/Handler callbacks, preserving pre-skip, timestamps, EOS/drain, and flush behavior. After flush, asynchronous decoders require start again. Previous buffer ownership becomes invalid when the generation changes.

## Video MediaCodec

When the Windows H.264 decoder MFT is available, MediaCodecList exposes `c2.stone.avc.decoder` for unencrypted AVC baseline, main and high profiles, with a 1920×1088 coded-size limit accommodating 1080p cropping. This decoder requires a Stone software Surface; image-native hardware Surfaces and video ByteBuffer output are unsupported.

If an installed synchronous Windows HEVC decoder can negotiate NV12 output, `c2.stone.hevc.decoder` exposes HEVC Main profile with the same dimensions and Surface limits. The host must already have a usable HEVC media extension; Stone does not install or bundle one.

The Client's MTA decoder worker converts NV12 frames to ARGB pixels. Bounded native frame storage avoids placing full video frames in the Java heap. Output buffer release publishes those pixels to the software Surface queue on the main Looper, including timestamped releases. Flush, stop and release cancel pending frames and invalidate prior buffer ownership. Decoder creation alone does not prove that a player has selected and displayed video.

## AAC MediaCodec

The Windows AAC decoder exposes `c2.stone.aac.decoder` for unencrypted AAC-LC with AudioSpecificConfig, mono/stereo PCM16 output and a matching sample rate. Other AAC profiles and channel layouts are rejected. It shares the image buffer/callback lifecycle with Opus, including EOS, flush and stop/reconfigure.

Other video codecs, encoders and DRM are unsupported. PCM output, decoder availability, Surface composition and a player's complete media flow are distinct capabilities.
