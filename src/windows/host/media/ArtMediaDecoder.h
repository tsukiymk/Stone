#pragma once

#include <cstdint>
#include <vector>

namespace stone::windows {
int AvailableArtAudioDecoders();
int64_t OpenArtAudioDecoder(bool opus, int rate, int channels, const std::vector<uint8_t>& config);
std::vector<uint8_t> DecodeArtAudio(int64_t handle, const std::vector<uint8_t>& packet, int64_t& pts, bool eos);
void FlushArtAudioDecoder(int64_t handle);
void CloseArtAudioDecoder(int64_t handle);

struct ArtVideoFrame {
    std::vector<int32_t> pixels;
    int width = 0, height = 0;
    int64_t pts = 0;
};

bool ArtAvcDecoderAvailable();
bool ArtHevcDecoderAvailable();
int64_t OpenArtVideoDecoder(bool hevc, int width, int height, const std::vector<uint8_t>& headers);
std::vector<ArtVideoFrame> DecodeArtVideo(int64_t handle, const std::vector<uint8_t>& packet, int64_t pts, bool eos);
void FlushArtVideoDecoder(int64_t handle);
void CloseArtVideoDecoder(int64_t handle);
}
