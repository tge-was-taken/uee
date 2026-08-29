#include "log.h"

#include "types.h"

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uee {
namespace {

constexpr spdlog::level::level_enum kDefaultLevel = spdlog::level::warn;

// A channel that is a member is created after the spec is read, so the spec has
// to outlive the call that parsed it.
struct Configured {
    spdlog::level::level_enum fallback = kDefaultLevel;
    std::vector<std::pair<std::string, spdlog::level::level_enum>> channels;
};

Configured& configured() {
    static Configured levels;
    return levels;
}

LogContext& context() {
    static LogContext source;
    return source;
}

class ContextFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg&, const std::tm&,
                spdlog::memory_buf_t& out) override {
        const std::string tag = context() ? context()() : std::string{"-"};
        out.append(tag.data(), tag.data() + tag.size());
    }

    std::unique_ptr<custom_flag_formatter> clone() const override {
        return spdlog::details::make_unique<ContextFlag>();
    }
};

spdlog::level::level_enum levelFor(const std::string& name) {
    for (const auto& [channel, level] : configured().channels) {
        if (channel == name) {
            return level;
        }
    }
    return configured().fallback;
}

// The guest's own output goes to stdout, so a test reading it sees none of this.
std::shared_ptr<spdlog::sinks::sink> sharedSink() {
    static std::shared_ptr<spdlog::sinks::sink> sink =
        std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    return sink;
}

std::shared_ptr<spdlog::logger> makeLogger(const std::string& name) {
    auto logger = std::make_shared<spdlog::logger>(name, sharedSink());
    logger->set_level(levelFor(name));
    spdlog::register_logger(logger);
    return logger;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (!text.empty()) {
        const usize end = text.find(separator);
        parts.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return parts;
}

void applyLevel(std::string_view channel, spdlog::level::level_enum level) {
    if (channel.empty()) {
        configured().fallback = level;
        spdlog::apply_all(
            [level](std::shared_ptr<spdlog::logger> logger) { logger->set_level(level); });
        return;
    }
    configured().channels.emplace_back(channel, level);
    if (auto logger = spdlog::get(std::string(channel))) {
        logger->set_level(level);
    }
}

}  // namespace

LoggerChannel::LoggerChannel(std::string name) {
    auto existing = spdlog::get(name);
    mLogger = existing != nullptr ? existing : makeLogger(name);
}

void configureLogging(LogContext source) {
    context() = std::move(source);

    auto sink = std::static_pointer_cast<spdlog::sinks::stderr_color_sink_mt>(sharedSink());
    sink->set_color(spdlog::level::trace, "\033[90m");
    sink->set_color(spdlog::level::debug, "\033[36m");
    sink->set_color(spdlog::level::info, "\033[32m");
    sink->set_color(spdlog::level::warn, "\033[33m");
    sink->set_color(spdlog::level::err, "\033[1;31m");
    sink->set_color(spdlog::level::critical, "\033[1;97;41m");

    auto formatter = std::make_unique<spdlog::pattern_formatter>();
    formatter->add_flag<ContextFlag>('*').set_pattern("%^%L %-9!n %-7!*%$ %v");
    sink->set_formatter(std::move(formatter));
    const char* spec = std::getenv("UEE_LOG");
    if (spec == nullptr) {
        applyLevel({}, kDefaultLevel);
        return;
    }
    for (std::string_view entry : split(spec, ',')) {
        const usize split_at = entry.find('=');
        const std::string_view channel = split_at == std::string_view::npos
                                             ? std::string_view{}
                                             : entry.substr(0, split_at);
        const std::string_view name =
            split_at == std::string_view::npos ? entry : entry.substr(split_at + 1);
        applyLevel(channel, spdlog::level::from_str(std::string(name)));
    }
}

}  // namespace uee
