#pragma once

#include <spdlog/spdlog.h>

#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace uee {

// Named for the subsystem a UEE_LOG level selects.
class LoggerChannel {
public:
    explicit LoggerChannel(std::string name);

    template <typename... Args>
    void trace(spdlog::format_string_t<Args...> format, Args&&... args) const {
        mLogger->trace(format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(spdlog::format_string_t<Args...> format, Args&&... args) const {
        mLogger->info(format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(spdlog::format_string_t<Args...> format, Args&&... args) const {
        mLogger->warn(format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(spdlog::format_string_t<Args...> format, Args&&... args) const {
        mLogger->error(format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void fatal(spdlog::format_string_t<Args...> format, Args&&... args) const {
        mLogger->critical(format, std::forward<Args>(args)...);
    }

    // A call answered without the subsystem behind it.
    template <typename... Args>
    void stub(spdlog::format_string_t<Args...> format, Args&&... args) const {
        if (mLogger->should_log(spdlog::level::warn)) {
            mLogger->warn("stub {}", std::vformat(spdlog::details::to_string_view(format),
                                                   std::make_format_args(args...)));
        }
    }

    bool tracing() const { return mLogger->should_log(spdlog::level::trace); }

private:
    std::shared_ptr<spdlog::logger> mLogger;
};

// What every line is tagged with, normally the guest thread in the processor
// that reached it.
using LogContext = std::function<std::string()>;

// Reads UEE_LOG: a level for everything ("trace"), or a comma separated list of
// channel levels ("iop=trace,sif=debug"). The only place a setting is read.
void configureLogging(LogContext context = {});

}  // namespace uee
