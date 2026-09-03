#pragma once

#include "timers.h"
#include "log.h"

#include <chrono>

namespace uee {

// Wall clock deliberately: it is what the guest's own rate is measured against.
class SpeedMeter {
public:
    explicit SpeedMeter(u64 fieldsPerReport) : mFieldsPerReport(fieldsPerReport) {}

    void countField() {
        if (++mFields < mFieldsPerReport) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - mSince).count();
        if (seconds > 0.0) {
            const double ceiling = double(mFields) / seconds;
            mLog.info("cpu ceiling {:.1f} fps ({:.2f}x real time)", ceiling,
                      ceiling / kFieldsPerSecond);
        }
        mFields = 0;
        mSince = now;
    }

private:
    u64 mFieldsPerReport;
    u64 mFields = 0;
    std::chrono::steady_clock::time_point mSince = std::chrono::steady_clock::now();
    LoggerChannel mLog{"speed"};
};

}  // namespace uee
