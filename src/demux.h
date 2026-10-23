#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace demux {

struct Error {
    int av_code;
    std::string message;

    [[nodiscard]] bool interrupted() const { return av_code == AVERROR_EXIT; }
    [[nodiscard]] bool eof() const { return av_code == AVERROR_EOF; }
    [[nodiscard]] bool timed_out() const { return av_code == AVERROR(ETIMEDOUT); }
};

template <class T> using Result = std::expected<T, Error>;

// Lets another thread or a signal handler abort FFmpeg's blocking calls (connect, read).
// Set `stop` to abort. `closing` is raised by Input's deleter so the RTSP TEARDOWN can still
// be sent after a stop request; nobody else needs to touch it.
struct Interrupt {
    std::atomic_bool stop{false};
    std::atomic_bool closing{false};

    static int poll(void* self) noexcept {
        const auto& irq = *static_cast<const Interrupt*>(self);
        return irq.stop.load(std::memory_order_relaxed) &&
               !irq.closing.load(std::memory_order_relaxed);
    }
};
static_assert(std::atomic_bool::is_always_lock_free, "must be safe to set from a signal handler");

struct InputCloser {
    Interrupt* irq = nullptr;

    void operator()(AVFormatContext* ctx) const noexcept {
        if (irq != nullptr) {
            irq->closing = true; // let TEARDOWN through even if stop was requested
        }
        avformat_close_input(&ctx);
    }
};

struct PacketFreer {
    void operator()(AVPacket* pkt) const noexcept { av_packet_free(&pkt); }
};

using Input = std::unique_ptr<AVFormatContext, InputCloser>;
using Packet = std::unique_ptr<AVPacket, PacketFreer>;

[[nodiscard]] Packet make_packet(); // null on allocation failure

enum class Transport : std::uint8_t { tcp, udp };

struct Options {
    Transport transport = Transport::tcp;
    std::chrono::milliseconds io_timeout{5000};
    bool low_latency = true;
    bool video_only = true;
};

using AvOptions = std::vector<std::pair<std::string, std::string>>;
[[nodiscard]] AvOptions to_av_options(const Options& options);

// Connects, reads the SDP and probes the streams. `irq` must outlive the returned Input.
// The packets read while probing are replayed first, as a burst (about 0.7-0.9 s of video in a
// few ms), usually starting at the camera's first keyframe. Their pts may be missing or negative.
[[nodiscard]] Result<Input> open(std::string_view url, const Options& options, Interrupt& irq);

// A snapshot for display and logging. Treat it as hints: width/height/extradata exist only if the
// probe window contained a keyframe. The decoder should read the stream's codecpar directly.
struct StreamInfo {
    int index;
    AVMediaType media;
    AVCodecID codec;
    int width;                           // 0 when the probe saw no keyframe
    int height;                          // 0 when the probe saw no keyframe
    int profile;                         // AV_PROFILE_UNKNOWN (-99) when unknown
    int level;                           // AV_LEVEL_UNKNOWN (-99) when unknown
    AVRational time_base;                // RTP clock for RTSP, e.g 1/90000 for video
    AVRational frame_rate;               // 0/0 when unknown
    int64_t bit_rate;                    // 0 when unknown
    std::vector<std::uint8_t> extradata; // SPS/PPS(/VPS): from SDP sprop-parameter-sets if the
                                         // camera sends them, else extracted from in-band NAL
                                         // units while probing. Copied.
};

[[nodiscard]] std::vector<StreamInfo> describe(const AVFormatContext& ctx);

[[nodiscard]] std::optional<int> best_video_stream(AVFormatContext& ctx);

[[nodiscard]] std::string_view codec_name(AVCodecID id);
[[nodiscard]] std::string_view media_name(AVMediaType type);

[[nodiscard]] constexpr bool is_supported_video(AVCodecID id) {
    return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC;
}

// Blocks for the next packet. `pkt` is unreferenced first, so its data is valid only until the
// next call. Returns AVERROR_EXIT (Error::interrupted) once `stop` is set.
[[nodiscard]] Result<void> read_packet(AVFormatContext& ctx, AVPacket& pkt);

} // namespace demux