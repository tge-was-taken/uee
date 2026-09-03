// A tag names the routine that settles a detail, prefixed by its module:
//   PADMAN      md5 0fa9dbfe8794e172733189ed945aff45
//   SLUS_216.21 md5 9074ba25f5a100940ed233c9f8f675ab

#include "pad.h"

#include <algorithm>

namespace uee {
namespace {

// The commands PadCmd43ConfigMode @ 0x71a0 and its neighbours send, each in a
// nine byte frame.
constexpr u8 kPoll = 0x42;
constexpr u8 kEnterConfig = 0x43;
constexpr u8 kSetMainMode = 0x44;
constexpr u8 kQueryModel = 0x45;
constexpr u8 kQueryAct = 0x46;
constexpr u8 kQueryComb = 0x47;
constexpr u8 kQueryMode = 0x4C;
constexpr u8 kSetActAlign = 0x4D;
constexpr u8 kSetButtonInfo = 0x4F;
constexpr u8 kSetVrefParam = 0x40;
constexpr u8 kQueryButtonMask = 0x41;

// PadPortOpenStart @ 0x2574 drops a frame whose marker or id is wrong, and
// PadProbeXferStep4 @ 0x1414 wants 0xF3 from a pad it put into config mode.
constexpr u8 kIdle = 0xFF;
constexpr u8 kMarker = 0x5A;
constexpr u8 kConfig = 0xF3;

// Low half counts data halfwords and sizes the poll; high half is the id
// scePadInfoMode @ 0x511e80 reports.
constexpr u8 kDigital = 0x41;
constexpr u8 kAnalog = 0x73;

// One controller with a digital and an analog mode, the analog one second.
constexpr u8 kModelDualShock = 0x03;
constexpr u8 kModes = 0x02;
constexpr u8 kModeOffset = 0x02;
constexpr u8 kFirstMode = 0x01;

// padman_scePadRead @ 0x3d48 passes these through untouched.
constexpr u8 kStickCentre = 0x7F;
constexpr usize kHeaderBytes = 3;

void put(std::span<u8> data, std::initializer_list<u8> bytes) {
    usize i = 0;
    for (const u8 byte : bytes) {
        if (i < data.size()) {
            data[i++] = byte;
        }
    }
}

}  // namespace

u8 DualShock::getModeByte() const {
    if (mInConfig) {
        return kConfig;
    }
    return mAnalog ? kAnalog : kDigital;
}

void DualShock::answer(std::span<const u8> command, std::span<u8> response) {
    std::fill(response.begin(), response.end(), kIdle);
    if (command.size() < kHeaderBytes || response.size() < kHeaderBytes) {
        return;
    }
    response[1] = getModeByte();
    response[2] = kMarker;

    const std::span<const u8> payload = command.subspan(kHeaderBytes);
    const std::span<u8> data = response.subspan(kHeaderBytes);
    std::fill(data.begin(), data.end(), 0);
    const u8 selector = payload.empty() ? 0 : payload[0];

    switch (command[1]) {
        case kPoll: readButtons(data); return;
        case kEnterConfig: enterConfig(selector, data); return;
        case kSetMainMode: setMainMode(selector); return;
        case kQueryModel: reportModel(data); return;
        case kQueryAct: reportActuator(selector, data); return;
        case kQueryComb: reportCombination(data); return;
        case kQueryMode: reportMode(selector, data); return;
        case kSetActAlign: mapMotors(payload, data); return;
        case kQueryButtonMask: reportButtonMask(data); return;
        case kSetButtonInfo:
        case kSetVrefParam: put(data, {0x00, 0x00, 0x00, 0x00, 0x00, kMarker}); return;
        default: return;
    }
}

// The host has no stick or pressure input, so only the buttons come from it.
void DualShock::readButtons(std::span<u8> data) {
    const u16 buttons = mButtons->buttons();
    put(data, {static_cast<u8>(buttons >> 8), static_cast<u8>(buttons), kStickCentre,
               kStickCentre, kStickCentre, kStickCentre});
}

// Asked to enter config mode, the pad still answers as what it was.
void DualShock::enterConfig(u8 selector, std::span<u8> data) {
    if (!mInConfig) {
        readButtons(data);
    }
    mInConfig = selector != 0;
}

// padman_scePadSetMainMode @ 0x45ec asks for one of the modes reportModel lists.
void DualShock::setMainMode(u8 selector) {
    mAnalog = selector != 0;
}

// padman_scePadInfoMode @ 0x447c hands the game what this reports.
void DualShock::reportModel(std::span<u8> data) {
    put(data, {kModelDualShock, kModes, static_cast<u8>(mAnalog ? 0x01 : 0x00), kModeOffset,
               kFirstMode, 0x00});
}

// padman_scePadInfoAct @ 0x4344 reads these. One actuator per motor, the
// second taking a speed rather than a switch.
void DualShock::reportActuator(u8 selector, std::span<u8> data) {
    if (selector == 0) {
        put(data, {0x00, 0x00, 0x01, 0x02, 0x00, 0x0A});
        return;
    }
    put(data, {0x00, 0x00, 0x01, 0x01, 0x01, 0x14});
}

// padman_scePadInfoComb @ 0x453c reads how many actuators can run at once.
void DualShock::reportCombination(std::span<u8> data) {
    put(data, {0x00, 0x00, 0x02, 0x00, 0x01, 0x00});
}

// PadFrameFill5AF3 @ 0x1fa0 keeps bytes 3 to 6, and only when byte 8 is the
// marker. This mask says every button reports a pressure of its own.
void DualShock::reportButtonMask(std::span<u8> data) {
    put(data, {0xFF, 0xFF, 0x03, 0x00, 0x00, kMarker});
}

// PadProbeXferStep6 @ 0x1c78 reads the id out of byte 6.
void DualShock::reportMode(u8 selector, std::span<u8> data) {
    const u8 id = selector == 0 ? kDigital : kAnalog;
    put(data, {0x00, 0x00, 0x00, static_cast<u8>(id >> 4), 0x00, 0x00});
}

// padman_scePadSetActAlign @ 0x484c maps the motors.
void DualShock::mapMotors(std::span<const u8> payload, std::span<u8> data) {
    for (usize i = 0; i < std::size(mMotors); ++i) {
        if (i < data.size()) {
            data[i] = mMotors[i];
        }
        if (i < payload.size()) {
            mMotors[i] = payload[i];
        }
    }
}

}  // namespace uee
