#include "demux.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace {

struct Cli {
    std::string url;
    demux::Options options;
    std::int64_t max_packets = 300;
    bool verbose = false;
};

constexpr std::string_view kUsage =
    "usage: probe <rtsp-url> [--udp] [--all] [--packets N] [--verbose]";

std::optional<Cli> parse_args(std::span<char* const> argv) {
    Cli cli;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--udp") {
            cli.options.transport = demux::Transport::udp;
        } else if (arg == "--all") {
            cli.options.video_only = false;
        } else if (arg == "--verbose") {
            cli.verbose = true;
        } else if (arg == "--packets") {
            if (++i >= argv.size()) {
                return std::nullopt;
            }

            std::string_view vv = argv[i];
            auto [ptr, ec] = std::from_chars(vv.data(), vv.data() + vv.size(), cli.max_packets);
            if (ec != std::errc{} || ptr != vv.data() + vv.size() || cli.max_packets <= 0) {
                return std::nullopt;
            }
        } else if (cli.url.empty() && !arg.starts_with("--")) {
            cli.url = arg;
        } else {
            return std::nullopt;
        }
    }
    if (cli.url.empty()) {
        return std::nullopt;
    }
    return cli;
}

std::string redact(std::string_view url) {
    constexpr auto npos = std::string_view::npos;
    const auto scheme = url.find("://");
    const auto start = scheme == npos ? 0 : scheme + 3;
    const auto at = url.find('@', start);
    const auto slash = url.find('/', start);
    if (at == npos || (slash != npos && at > slash)) {
        return std::string{url};
    }
    return std::string{url.substr(0, start)} + std::string{url.substr(at + 1)};
}

demux::Interrupt g_irq;

void on_signal(int /* */) {
    g_irq.stop = true;
}

struct StreamStats {
    std::int64_t packets = 0;
    std::int64_t bytes = 0;
    std::int64_t keyframes = 0;

    void add(const AVPacket& pkt) {
        ++packets;
        bytes += pkt.size;
        if ((pkt.flags & AV_PKT_FLAG_KEY) != 0) {
            ++keyframes;
        }
    }
};

std::string rational(AVRational rr) {
    return rr.den == 0 ? "unknown" : std::format("{}/{}", rr.num, rr.den);
}

std::string frame_rate(AVRational rr) {
    return rr.den == 0 || rr.num == 0 ? "unknown" : std::format("{:.2f} fps", av_q2d(rr));
}

std::string hex_prefix(std::span<const std::uint8_t> bytes, std::size_t max = 24) {
    std::string out;
    const std::size_t nn = std::min(bytes.size(), max);
    for (std::size_t i = 0; i < nn; ++i) {
        if (i != 0) {
            out += ' ';
        }
        std::format_to(std::back_inserter(out), "{:02x}", bytes[i]);
    }
    if (bytes.size() > nn) {
        out += " ...";
    }
    return out;
}

std::optional<std::int64_t> pts_ms(const AVPacket& pkt, AVRational time_base) {
    if (pkt.pts == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return av_rescale_q(pkt.pts, time_base, AVRational{1, 1000});
}

std::string level_text(AVCodecID id, int level) {
    if (level == AV_LEVEL_UNKNOWN) {
        return "?";
    }
    const int dd = id == AV_CODEC_ID_HEVC ? 30 : 10;
    return std::format("{}.{}", level / dd, (level % dd) * 10 / dd);
}

void print_streams(std::span<const demux::StreamInfo> streams) {
    std::println("streams: {}", streams.size());
    for (const auto& ss : streams) {
        std::print(
            " #{} {} {}", ss.index, demux::media_name(ss.media), demux::codec_name(ss.codec));
        if (ss.media == AVMEDIA_TYPE_VIDEO) {
            const char* profile = avcodec_profile_name(ss.codec, ss.profile);
            std::print(" {}x{} {} profile={}",
                       ss.width,
                       ss.height,
                       frame_rate(ss.frame_rate),
                       profile != nullptr ? profile : "?");

            std::print(" level={}", level_text(ss.codec, ss.level));
            if (!demux::is_supported_video(ss.codec)) {
                std::print("  [not H.264/H.265]");
            }
        }
        std::print(" time_base={}", rational(ss.time_base));
        if (ss.bit_rate > 0) {
            std::print(" bitrate={}kbps", ss.bit_rate / 1000);
        }
        std::println();
        if (!ss.extradata.empty()) {
            std::println("    extradata {}B: {}", ss.extradata.size(), hex_prefix(ss.extradata));
        }
    }
}

void print_summary(std::span<const demux::StreamInfo> streams,
                   std::span<const StreamStats> stats,
                   std::chrono::duration<double> elapsed) {
    const double secs = std::max(elapsed.count(), 1e-3);
    std::println("\nsummary after {:.2f}s", elapsed.count());
    for (std::size_t i = 0; i < stats.size(); ++i) {
        const StreamStats& ss = stats[i];
        std::println("  #{} {}: {} packets ({:.1f}/s), {:.0f} kbit/s, {} keyframes",
                     streams[i].index,
                     demux::codec_name(streams[i].codec),
                     ss.packets,
                     static_cast<double>(ss.packets) / secs,
                     static_cast<double>(ss.bytes) * 8.0 / 1000.0 / secs,
                     ss.keyframes);
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto cli = parse_args(std::span<char* const>(argv, static_cast<std::size_t>(argc)));

    if (!cli) {
        std::println(stderr, "{}", kUsage);
        return 2;
    }

    av_log_set_level(cli->verbose ? AV_LOG_DEBUG : AV_LOG_WARNING);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string shown_url = redact(cli->url);
    std::println("connecting to {} ({}, timeout {} ms)",
                 shown_url,
                 cli->options.transport == demux::Transport::tcp ? "tcp" : "udp",
                 cli->options.io_timeout.count());

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now(); // wall(ms) below counts from here, so it includes open()

    auto opened = demux::open(cli->url, cli->options, g_irq);
    const auto loop_start = Clock::now();
    if (!opened) {
        if (g_irq.stop) { // Ctrl-C while connecting is not an error
            return 0;
        }
        std::println(stderr, "error: {}", opened.error().message);
        return 1;
    }
    demux::Input input = std::move(*opened);
    AVFormatContext& ctx = *input;

    std::println("open took {} ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(loop_start - t0).count());

    if (cli->verbose) {
        av_dump_format(&ctx, 0, shown_url.c_str(), 0);
    }
    const auto streams = demux::describe(ctx);
    print_streams(streams);

    std::vector<StreamStats> stats(streams.size());
    const demux::Packet packet = demux::make_packet();
    if (!packet) {
        std::println(stderr, "error: out of memory");
        return 1;
    }
    std::println("\n{:>9} {:>3} {:>12} {:>8} {:>4}", "wall(ms)", "str", "pts(ms)", "bytes", "key");
    int exit_code = 0;

    for (std::int64_t nn = 0; nn < cli->max_packets; ++nn) {
        const auto rr = demux::read_packet(ctx, *packet);
        const auto wall = Clock::now() - t0;
        if (!rr) {
            const demux::Error& err = rr.error();
            if (err.eof()) {
                std::println(stderr, "note: server closed the stream");
            } else if (!g_irq.stop) { // a stop request is not an error
                std::println(stderr, "error: {}", err.message);
                exit_code = 1;
            }
            break;
        }

        const AVPacket& pkt = *packet;
        const auto idx = static_cast<std::size_t>(pkt.stream_index);
        if (idx >= stats.size()) {
            continue;
        }
        stats[idx].add(pkt);

        const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall).count();
        std::print("{:>9} {:>3} ", wall_ms, idx);
        if (const auto pts = pts_ms(pkt, ctx.streams[idx]->time_base)) {
            std::print("{:>12}", *pts);
        } else {
            std::print("{:>12}", '-');
        }
        std::println(" {:>8} {:>4}", pkt.size, (pkt.flags & AV_PKT_FLAG_KEY) != 0 ? "K" : "");
    }

    const std::chrono::duration<double> elapsed = Clock::now() - loop_start;
    print_summary(streams, stats, elapsed);

    input.reset(); // sends TEARDOWN; the deleter lets it through even after Ctrl-C
    return exit_code;
}