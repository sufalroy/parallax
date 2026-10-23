#include "demux.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <format>
#include <span>
#include <thread>

namespace demux {
namespace {

Error make_error(std::string_view what, int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> reason{};
    av_strerror(code, reason.data(), reason.size());
    return Error{.av_code = code, .message = std::format("{}: {}", what, reason.data())};
}

struct Dict {
    AVDictionary* d = nullptr;
    Dict() = default;
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;
    ~Dict() { av_dict_free(&d); }
};
} // namespace

Packet make_packet() {
    return Packet{av_packet_alloc()};
}

AvOptions to_av_options(const Options& options) {
    const auto timeout_us =
        std::chrono::duration_cast<std::chrono::microseconds>(options.io_timeout).count();

    AvOptions out;
    out.emplace_back("rtsp_transport", options.transport == Transport::tcp ? "tcp" : "udp");
    // Microseconds. Applies to connect and read/write, and is the only timeout for UDP.
    out.emplace_back("timeout", std::to_string(timeout_us));

    if (options.video_only) {
        out.emplace_back("allowed_media_types", "video"); // rtsp demuxer: no SETUP for other tracks
    }
    if (options.low_latency) {
        // Deliberately no fflags=nobuffer: it drops the packets read while probing, including the
        // keyframe the camera sends first. Measured: the first keyframe arrives ~2.5 s sooner
        // without it, as part of a short burst of replayed packets.
        out.emplace_back("analyzeduration", "500000"); // microseconds, default 5 s
        out.emplace_back("probesize", "262144");       // bytes, default 5000000
    }
    return out;
}

Result<Input> open(std::string_view url, const Options& options, Interrupt& irq) {
    Dict opts;
    for (const auto& [key, value] : to_av_options(options)) {
        if (const int rc = av_dict_set(&opts.d, key.c_str(), value.c_str(), 0); rc < 0) {
            return std::unexpected(make_error("av_dict_set", rc));
        }
    }

    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr) {
        return std::unexpected(make_error("avformat_alloc_context", AVERROR(ENOMEM)));
    }
    raw->interrupt_callback = {.callback = Interrupt::poll, .opaque = &irq};

    const std::string url_z{url};
    // On failure FFmpeg frees `raw` itself.
    if (const int rc = avformat_open_input(&raw, url_z.c_str(), nullptr, &opts.d); rc < 0) {
        return std::unexpected(make_error("avformat_open_input", rc));
    }

    Input input{raw, InputCloser{&irq}}; // RAII owns the context from here on

    // Options the demuxer did not consume are left in the dictionary: a typo or an unsupported key.
    if (const AVDictionaryEntry* entry = av_dict_iterate(opts.d, nullptr)) {
        return std::unexpected(
            Error{.av_code = AVERROR_OPTION_NOT_FOUND,
                  .message = std::format("unrecognized option: {}={}", entry->key, entry->value)});
    }

    if (const int rc = avformat_find_stream_info(input.get(), nullptr); rc < 0) {
        return std::unexpected(make_error("avformat_find_stream_info", rc));
    }
    return input;
}

std::vector<StreamInfo> describe(const AVFormatContext& ctx) {
    std::vector<StreamInfo> out;
    out.reserve(ctx.nb_streams);
    for (const AVStream* stream : std::span<AVStream* const>(ctx.streams, ctx.nb_streams)) {
        const AVCodecParameters& par = *stream->codecpar;
        out.push_back(StreamInfo{
            .index = stream->index,
            .media = par.codec_type,
            .codec = par.codec_id,
            .width = par.width,
            .height = par.height,
            .profile = par.profile,
            .level = par.level,
            .time_base = stream->time_base,
            .frame_rate = stream->avg_frame_rate,
            .bit_rate = par.bit_rate,
            .extradata =
                std::vector<std::uint8_t>(par.extradata, par.extradata + par.extradata_size),
        });
    }
    return out;
}

std::optional<int> best_video_stream(AVFormatContext& ctx) {
    const int idx = av_find_best_stream(&ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    return idx >= 0 ? std::optional<int>{idx} : std::nullopt;
}

std::string_view codec_name(AVCodecID id) {
    return avcodec_get_name(id);
}

std::string_view media_name(AVMediaType type) {
    const char* name = av_get_media_type_string(type);
    return name != nullptr ? name : "unknown";
}

Result<void> read_packet(AVFormatContext& ctx, AVPacket& pkt) {
    av_packet_unref(&pkt);

    const AVIOInterruptCB& icb = ctx.interrupt_callback;
    int rc;
    while ((rc = av_read_frame(&ctx, &pkt)) == AVERROR(EAGAIN)) {
        // Nothing to deliver yet (e.g. an RTCP packet was consumed). Same pause as ffplay/ffmpeg,
        // and the stop flag is checked here because no blocking I/O happens in this loop.
        if (icb.callback != nullptr && icb.callback(icb.opaque) != 0) {
            return std::unexpected(make_error("av_read_frame", AVERROR_EXIT));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (rc < 0) {
        return std::unexpected(make_error("av_read_frame", rc));
    }
    return {};
}

} // namespace demux