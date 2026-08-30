#pragma once

#include "host.h"
#include "types.h"

#include <span>

namespace uee {

// The controller side of the SIO2 protocol. PADMAN md5
// 0fa9dbfe8794e172733189ed945aff45 walks a pad into config mode to read what it
// is and to turn its analog mode on, so a pad that only answered a poll would
// never leave PadProbeXferStep1 @ 0xf74.
class DualShock {
public:
    explicit DualShock(PadSource* buttons) : mButtons(buttons) {}

    bool isConnected() const { return mButtons != nullptr; }
    void answer(std::span<const u8> command, std::span<u8> response);

private:
    u8 getModeByte() const;

    void readButtons(std::span<u8> data);
    void enterConfig(u8 selector, std::span<u8> data);
    void setMainMode(u8 selector);
    void reportModel(std::span<u8> data);
    void reportActuator(u8 selector, std::span<u8> data);
    void reportCombination(std::span<u8> data);
    void reportMode(u8 selector, std::span<u8> data);
    void mapMotors(std::span<const u8> payload, std::span<u8> data);

    PadSource* mButtons = nullptr;
    bool mInConfig = false;
    bool mAnalog = false;
    u8 mMotors[2] = {0xFF, 0xFF};
};

}  // namespace uee
