// SIFCMD md5 bcdaf8bead75a7362a82c95bbbf479c7: the RPC half, kept in guest memory
// the way the module keeps it, so a module that reads its own records sees them.

#include "iop_kernel.h"
#include "iop_run.h"
#include "log.h"

namespace uee {
namespace {

LoggerChannel log("sifcmd");

// SIFCMD: sceSifSetRpcQueue @ 0x1088 fills a SifRpcDataQueue_t.
constexpr u32 kQueueThreadId = 0x00;
constexpr u32 kQueueActive = 0x04;
constexpr u32 kQueueServers = 0x08;
constexpr u32 kQueueFirstRequest = 0x0C;
constexpr u32 kQueueLastRequest = 0x10;
constexpr u32 kQueueNext = 0x14;
constexpr u32 kQueueWords = 6;

// SIFCMD: sceSifRegisterRpc @ 0x1130 fills a SifRpcServerData_t, and the call
// handler @ 0xe08 fills in the request.
constexpr u32 kServerId = 0x00;
constexpr u32 kServerFunc = 0x04;
constexpr u32 kServerBuffer = 0x08;
constexpr u32 kServerSize = 0x0C;
constexpr u32 kServerClientFunc = 0x10;
constexpr u32 kServerClientBuffer = 0x14;
constexpr u32 kServerClient = 0x1C;
constexpr u32 kServerPacket = 0x20;
constexpr u32 kServerNumber = 0x24;
constexpr u32 kServerReceive = 0x28;
constexpr u32 kServerReceiveSize = 0x2C;
constexpr u32 kServerReceiveMode = 0x30;
constexpr u32 kServerNextServer = 0x38;
constexpr u32 kServerNextRequest = 0x3C;
constexpr u32 kServerQueue = 0x40;

// SIFCMD: sceSifBindRpc @ 0xd08 and sceSifCallRpc @ 0xed4 fill a
// SifRpcClientData_t.
constexpr u32 kClientPacket = 0x00;
constexpr u32 kClientRpcId = 0x04;
constexpr u32 kClientThread = 0x08;
constexpr u32 kClientCommand = 0x10;
constexpr u32 kClientBuffer = 0x14;
constexpr u32 kClientClientBuffer = 0x18;
constexpr u32 kClientEndFunction = 0x1C;
constexpr u32 kClientEndParameter = 0x20;
constexpr u32 kClientServer = 0x24;

// The packet both sides use, with the command header SIFCMD's send fills in.
constexpr u32 kPacketBytes = 0x40;
constexpr u32 kPacketHeaderSizes = 0x00;
constexpr u32 kPacketHeaderDestination = 0x04;
constexpr u32 kPacketHeaderCommand = 0x08;
constexpr u32 kPacketHeaderOption = 0x0C;
// The pool marks a packet in use in the word the protocol calls rec_id.
constexpr u32 kPacketFlags = 0x10;
constexpr u32 kPacketAddress = 0x14;
constexpr u32 kPacketRpcId = 0x18;
constexpr u32 kPacketClient = 0x1C;
constexpr u32 kPacketArgument = 0x20;
constexpr u32 kPacketSendSize = 0x24;
constexpr u32 kPacketReceive = 0x28;
constexpr u32 kPacketReceiveSize = 0x2C;
constexpr u32 kPacketReceiveMode = 0x30;
constexpr u32 kPacketServer = 0x34;
// What an end packet carries in the call packet's places.
constexpr u32 kEndCommand = kPacketArgument;
constexpr u32 kEndServer = kPacketSendSize;
constexpr u32 kEndBuffer = kPacketReceive;
constexpr u32 kEndClientBuffer = kPacketReceiveSize;

constexpr u32 kPacketInUse = 0x2;
constexpr u32 kPoolPackets = 32;

constexpr u32 kCommandEnd = 0x80000008;
constexpr u32 kCommandBind = 0x80000009;
constexpr u32 kCommandCall = 0x8000000A;

constexpr u32 kServerBytes = 0x44;

// The servers answered here get what a small module would give itself.
constexpr u32 kHostServerBuffer = 0x1000;
constexpr u32 kHostServerStack = 0x4000;
constexpr s32 kHostServerPriority = 0x40;

constexpr u32 kNoWait = 0x1;
constexpr u32 kNoThread = 0xFFFFFFFFu;
constexpr u32 kNoPacket = 0xFFFFFFFFu;

}  // namespace

// SIFCMD: the pool @ 0x818 hands out the first free packet with a fresh rpc
// id, and a client later checks the id is still its own.
u32 IopKernel::allocatePacket() {
    if (mPacketPool == 0) {
        mPacketPool = allocateMemory(kPoolPackets * kPacketBytes);
    }
    for (u32 i = 0; i < kPoolPackets; ++i) {
        const u32 packet = mPacketPool + i * kPacketBytes;
        const u32 flags = mMemory.read32(packet + kPacketFlags);
        if ((flags & kPacketInUse) != 0) {
            continue;
        }
        mRpcId = mRpcId + 1 == 0 ? 1 : mRpcId + 1;
        mMemory.write32(packet + kPacketFlags, flags | kPacketInUse);
        mMemory.write32(packet + kPacketRpcId, mRpcId);
        mMemory.write32(packet + kPacketAddress, packet);
        return packet;
    }
    return 0;
}

void IopKernel::freePacket(u32 packet) {
    if (packet - mPacketPool < kPoolPackets * kPacketBytes) {
        mMemory.write32(packet + kPacketFlags,
                        mMemory.read32(packet + kPacketFlags) & ~kPacketInUse);
    }
}

std::vector<u32> IopKernel::readPacket(u32 packet) const {
    std::vector<u32> words(kPacketBytes / 4);
    for (u32 i = 0; i < words.size(); ++i) {
        words[i] = mMemory.read32(packet + i * 4);
    }
    return words;
}

// SIFCMD: sceSifSendCmd @ 0x3e4 puts the sizes and command in the header and
// sends the data ahead of the packet.
void IopKernel::sendToEe(u32 command, std::vector<u32> words, u32 dataSource,
                         u32 dataDestination, u32 dataSize) {
    words[kPacketHeaderSizes / 4] = kPacketBytes | (dataSize << 8);
    words[kPacketHeaderDestination / 4] = dataDestination;
    words[kPacketHeaderCommand / 4] = command;
    words[kPacketHeaderOption / 4] = 0;
    mToEe.push_back({std::move(words), dataSource, dataDestination, dataSize});
}

std::vector<SifPacket> IopKernel::takePacketsForEe() {
    return std::exchange(mToEe, {});
}

// SIFCMD: sceSifSetRpcQueue @ 0x1088
void IopKernel::sifSetRpcQueue() {
    linkQueue(argument(0), argument(1));
}

void IopKernel::linkQueue(u32 queue, u32 thread) {
    mMemory.write32(queue + kQueueThreadId, thread);
    for (u32 i = 1; i < kQueueWords; ++i) {
        mMemory.write32(queue + i * 4, 0);
    }
    if (mRpcQueues == 0) {
        mRpcQueues = queue;
        return;
    }
    u32 last = mRpcQueues;
    while (mMemory.read32(last + kQueueNext) != 0) {
        last = mMemory.read32(last + kQueueNext);
    }
    mMemory.write32(last + kQueueNext, queue);
}

// SIFCMD: sceSifRegisterRpc @ 0x1130. A module registering an id a server
// answered here holds takes it over, as the game's own build of a module does.
void IopKernel::sifRegisterRpc() {
    const u32 server = argument(0);
    const u32 queue = argument(6);
    const u32 id = argument(1);
    const u32 held = findRpcServer(id);
    if (const auto host = mHostServers.find(held); host != mHostServers.end()) {
        log.info("a module takes server {:08x} over from {}", id, host->second.server->name());
        unlinkServer(mMemory.read32(held + kServerQueue), held);
        mHostServers.erase(host);
    }
    mMemory.write32(server + kServerId, argument(1));
    mMemory.write32(server + kServerFunc, argument(2));
    mMemory.write32(server + kServerBuffer, argument(3));
    mMemory.write32(server + kServerNextRequest, 0);
    mMemory.write32(server + kServerNextServer, 0);
    mMemory.write32(server + kServerClientFunc, argument(4));
    mMemory.write32(server + kServerClientBuffer, argument(5));
    mMemory.write32(server + kServerQueue, queue);
    if (mMemory.read32(queue + kQueueServers) == 0) {
        mMemory.write32(queue + kQueueServers, server);
        return;
    }
    u32 last = mMemory.read32(queue + kQueueServers);
    while (mMemory.read32(last + kServerNextServer) != 0) {
        last = mMemory.read32(last + kServerNextServer);
    }
    mMemory.write32(last + kServerNextServer, server);
}

// SIFCMD: sceSifRemoveRpc @ 0x1210 answers the server it unlinked when that was
// the first, its predecessor otherwise, and zero when it was not there.
void IopKernel::sifRemoveRpc() {
    returnValue(unlinkServer(argument(1), argument(0)));
}

u32 IopKernel::unlinkServer(u32 queue, u32 server) {
    u32 at = mMemory.read32(queue + kQueueServers);
    if (at == server) {
        mMemory.write32(queue + kQueueServers, mMemory.read32(at + kServerNextServer));
        return at;
    }
    while (at != 0 && mMemory.read32(at + kServerNextServer) != server) {
        at = mMemory.read32(at + kServerNextServer);
    }
    if (at != 0) {
        mMemory.write32(at + kServerNextServer, mMemory.read32(server + kServerNextServer));
    }
    return at;
}

// SIFCMD: sceSifRemoveRpcQueue @ 0x12a8, answering the same way.
void IopKernel::sifRemoveRpcQueue() {
    const u32 queue = argument(0);
    u32 at = mRpcQueues;
    if (at == queue) {
        mRpcQueues = mMemory.read32(at + kQueueNext);
        returnValue(at);
        return;
    }
    while (at != 0 && mMemory.read32(at + kQueueNext) != queue) {
        at = mMemory.read32(at + kQueueNext);
    }
    if (at != 0) {
        mMemory.write32(at + kQueueNext, mMemory.read32(queue + kQueueNext));
    }
    returnValue(at);
}

// SIFCMD: the search @ 0xbe8 the bind handler runs, across every queue. The
// addresses come from guest memory and the EE, and a read outside the IOP's RAM
// would fault the next module to load, so those end the walk.
u32 IopKernel::findRpcServer(u32 id) const {
    for (u32 queue = mRpcQueues; queue != 0 && queue < kIopRamSize;
         queue = mMemory.read32(queue + kQueueNext)) {
        for (u32 server = mMemory.read32(queue + kQueueServers); server != 0 && server < kIopRamSize;
             server = mMemory.read32(server + kServerNextServer)) {
            if (mMemory.read32(server + kServerId) == id) {
                return server;
            }
        }
    }
    return 0;
}

bool IopKernel::isRpcServer(u32 server) const {
    return server != 0 && server < kIopRamSize &&
           findRpcServer(mMemory.read32(server + kServerId)) == server;
}

u32 IopKernel::getRpcServerBuffer(u32 server) const {
    return mMemory.read32(server + kServerBuffer);
}

u32 IopKernel::getRpcServerClientBuffer(u32 server) const {
    return mMemory.read32(server + kServerClientBuffer);
}

// SIFCMD: the call handler @ 0xe08. It links the server behind the last one
// through the field its server list uses, and sceSifGetNextRequest follows a
// different one, so a queue holds one request at a time.
void IopKernel::receiveRpcCall(const SifRpcCall& call) {
    const u32 server = call.server;
    const u32 queue = mMemory.read32(server + kServerQueue);
    if (mMemory.read32(queue + kQueueFirstRequest) == 0) {
        mMemory.write32(queue + kQueueFirstRequest, server);
    } else {
        mMemory.write32(mMemory.read32(queue + kQueueLastRequest) + kServerNextServer, server);
    }
    mMemory.write32(queue + kQueueLastRequest, server);
    mMemory.write32(server + kServerPacket, call.packet);
    mMemory.write32(server + kServerClient, call.client);
    mMemory.write32(server + kServerNumber, call.number);
    mMemory.write32(server + kServerSize, call.sendSize);
    mMemory.write32(server + kServerReceive, call.receive);
    mMemory.write32(server + kServerReceiveSize, call.receiveSize);
    mMemory.write32(server + kServerReceiveMode, call.receiveMode);
    mUnanswered.insert(server);
    if (const auto host = mHostServers.find(server); host != mHostServers.end()) {
        host->second.sendSource = call.sendSource;
    }
    const u32 thread = mMemory.read32(queue + kQueueThreadId);
    if (static_cast<s32>(thread) >= 0 && mMemory.read32(queue + kQueueActive) == 0) {
        wakeThread(thread);
    }
}

// SIFCMD: sceSifGetNextRequest @ 0x1340
u32 IopKernel::takeRequest(u32 queue) {
    const u32 server = mMemory.read32(queue + kQueueFirstRequest);
    if (server == 0) {
        mMemory.write32(queue + kQueueActive, 0);
        return 0;
    }
    mMemory.write32(queue + kQueueActive, 1);
    mMemory.write32(queue + kQueueFirstRequest, mMemory.read32(server + kServerNextRequest));
    return server;
}

void IopKernel::sifGetNextRequest() {
    returnValue(takeRequest(argument(0)));
}

// SIFCMD: sceSifExecRequest @ 0x13a4 calls the handler with the request number,
// the buffer the caller wrote into and its size. A caller that waits takes the
// end command; one that polls finds the end packet written over its own.
void IopKernel::execRequest(u32 server) {
    u32 reply = 0;
    if (const auto host = mHostServers.find(server); host != mHostServers.end()) {
        const s32 result = answerOnHost(server, host->second);
        const u32 receive = mMemory.read32(server + kServerReceive);
        if (mMemory.read32(server + kServerReceiveSize) >= 4) {
            mMainMemory.write32(receive, static_cast<u32>(result));
        }
    } else {
        const u32 arguments[3] = {mMemory.read32(server + kServerNumber),
                                  mMemory.read32(server + kServerBuffer),
                                  mMemory.read32(server + kServerSize)};
        reply = callGuest(*mMachine, mMemory.read32(server + kServerFunc),
                          mIop.cpu.gpr[reg::gp], arguments);
    }
    mUnanswered.erase(server);
    sendReply(server, reply);
}

// The handlers answered here write straight into the EE's memory, as cdvdfsv's
// transfers do, and return a status word the caller finds at its receive buffer.
s32 IopKernel::answerOnHost(u32 server, const HostServer& host) {
    const RpcCall call{mMemory.read32(server + kServerNumber),
                       host.sendSource,
                       mMemory.read32(server + kServerSize),
                       mMemory.read32(server + kServerBuffer),
                       mMemory.read32(server + kServerReceive),
                       mMemory.read32(server + kServerReceiveSize)};
    return host.server->call(host.id, call);
}

void IopKernel::sendReply(u32 server, u32 reply) {
    const u32 size = reply != 0 ? mMemory.read32(server + kServerReceiveSize) : 0;
    const u32 receive = mMemory.read32(server + kServerReceive);

    std::vector<u32> packet(kPacketBytes / 4);
    packet[kEndCommand / 4] = kCommandCall;
    packet[kPacketClient / 4] = mMemory.read32(server + kServerClient);
    if (mMemory.read32(server + kServerReceiveMode) != 0) {
        sendToEe(kCommandEnd, std::move(packet), reply, receive, size);
        return;
    }
    for (u32 i = 0; i < size; ++i) {
        mMainMemory.write8(receive + i, mMemory.read8(reply + i));
    }
    const u32 polled = mMemory.read32(server + kServerPacket);
    for (u32 i = 0; i < packet.size(); ++i) {
        mMainMemory.write32(polled + i * 4, packet[i]);
    }
}

void IopKernel::sifExecRequest() {
    execRequest(argument(0));
}

// SIFCMD: sceSifRpcLoop @ 0x1520 takes requests until none is left, then sleeps
// until the call handler wakes it. It never returns.
void IopKernel::sifRpcLoop() {
    const u32 queue = argument(0);
    const usize self = mCurrent;
    for (;;) {
        while (const u32 server = takeRequest(queue)) {
            execRequest(server);
        }
        sleepCurrentThread();
        waitUntilRunning(*mMachine, self);
    }
}

void IopKernel::addHostServer(u32 id, SifServer& server, u32 loopEntry) {
    const u32 queue = allocateMemory(kQueueWords * 4);
    const u32 record = allocateMemory(kServerBytes);
    const u32 buffer = allocateMemory(kHostServerBuffer);
    const u32 thread = addThread(loopEntry, kHostServerPriority, kHostServerStack, 0, 0);
    linkQueue(queue, thread);
    for (u32 i = 0; i < kServerBytes; i += 4) {
        mMemory.write32(record + i, 0);
    }
    mMemory.write32(record + kServerId, id);
    mMemory.write32(record + kServerBuffer, buffer);
    mMemory.write32(record + kServerQueue, queue);
    mMemory.write32(queue + kQueueServers, record);
    mHostServers[record] = {&server, id, 0};
    runThread(*threadFor(thread), queue, 0);
    reschedule();
}

// SIFCMD: sceSifBindRpc @ 0xd08. The EE's answer fills in the server, at
// receiveRpcEnd.
void IopKernel::sifBindRpc() {
    const u32 client = argument(0);
    const u32 id = argument(1);
    const u32 mode = argument(2);
    mMemory.write32(client + kClientCommand, 0);
    mMemory.write32(client + kClientServer, 0);
    const u32 packet = allocatePacket();
    if (packet == 0) {
        returnValue(kNoPacket);
        return;
    }
    mMemory.write32(client + kClientPacket, packet);
    mMemory.write32(client + kClientRpcId, mMemory.read32(packet + kPacketRpcId));
    mMemory.write32(packet + kPacketClient, client);
    mMemory.write32(packet + kPacketArgument, id);
    returnValue(0);
    if ((mode & kNoWait) != 0) {
        mMemory.write32(client + kClientThread, kNoThread);
        sendToEe(kCommandBind, readPacket(packet), 0, 0, 0);
        return;
    }
    mMemory.write32(client + kClientThread, makeHandle(mCurrent, mThreads[mCurrent].sequence));
    sendToEe(kCommandBind, readPacket(packet), 0, 0, 0);
    mThreads[mCurrent].awaitingEe = true;
    sleepCurrentThread();
}

// SIFCMD: sceSifCallRpc @ 0xed4. The send buffer goes ahead of the packet, into
// the buffer the EE's server named when it was bound.
void IopKernel::sifCallRpc() {
    const u32 client = argument(0);
    const u32 number = argument(1);
    const u32 mode = argument(2);
    const u32 send = argument(3);
    const u32 sendSize = argument(4);
    const u32 receive = argument(5);
    const u32 receiveSize = argument(6);
    const u32 endFunction = argument(7);
    const u32 endParameter = argument(8);
    const u32 packet = allocatePacket();
    if (packet == 0) {
        returnValue(kNoPacket);
        return;
    }
    mMemory.write32(client + kClientPacket, packet);
    mMemory.write32(client + kClientRpcId, mMemory.read32(packet + kPacketRpcId));
    mMemory.write32(client + kClientEndFunction, endFunction);
    mMemory.write32(client + kClientEndParameter, endParameter);
    mEndFunctionGp[client] = mIop.cpu.gpr[reg::gp];
    mMemory.write32(packet + kPacketClient, client);
    mMemory.write32(packet + kPacketArgument, number);
    mMemory.write32(packet + kPacketSendSize, sendSize);
    mMemory.write32(packet + kPacketReceive, receive);
    mMemory.write32(packet + kPacketReceiveSize, receiveSize);
    mMemory.write32(packet + kPacketServer, mMemory.read32(client + kClientServer));
    const u32 destination = mMemory.read32(client + kClientBuffer);
    returnValue(0);
    if ((mode & kNoWait) != 0) {
        mMemory.write32(packet + kPacketReceiveMode, endFunction != 0 ? 1 : 0);
        mMemory.write32(client + kClientThread, kNoThread);
        sendToEe(kCommandCall, readPacket(packet), send, destination, sendSize);
        return;
    }
    mMemory.write32(packet + kPacketReceiveMode, 1);
    mMemory.write32(client + kClientThread, makeHandle(mCurrent, mThreads[mCurrent].sequence));
    sendToEe(kCommandCall, readPacket(packet), send, destination, sendSize);
    mThreads[mCurrent].awaitingEe = true;
    sleepCurrentThread();
}

// SIFCMD: sceSifCheckStatRpc @ 0x1054, whether the client's packet is still out.
void IopKernel::sifCheckStatRpc() {
    const u32 client = argument(0);
    const u32 packet = mMemory.read32(client + kClientPacket);
    const bool pending = packet != 0 &&
                         mMemory.read32(client + kClientRpcId) ==
                             mMemory.read32(packet + kPacketRpcId) &&
                         (mMemory.read32(packet + kPacketFlags) & kPacketInUse) != 0;
    returnValue(pending ? 1 : 0);
}

// SIFCMD: the end handler @ 0x994, for the EE's answer to a bind or a call. The
// end function runs as the interrupt it is on the IOP.
void IopKernel::receiveRpcEnd(std::span<const u32> packet) {
    const u32 client = packet[kPacketClient / 4];
    const u32 command = packet[kEndCommand / 4];
    if (command == kCommandCall) {
        const u32 endFunction = mMemory.read32(client + kClientEndFunction);
        if (endFunction != 0) {
            mPendingHandlers.push_back({endFunction, mMemory.read32(client + kClientEndParameter),
                                        mEndFunctionGp[client]});
        }
    } else if (command == kCommandBind) {
        mMemory.write32(client + kClientServer, packet[kEndServer / 4]);
        mMemory.write32(client + kClientBuffer, packet[kEndBuffer / 4]);
        mMemory.write32(client + kClientClientBuffer, packet[kEndClientBuffer / 4]);
    } else {
        log.stub("end packet for command {:08x}", command);
    }
    const u32 thread = mMemory.read32(client + kClientThread);
    if (static_cast<s32>(thread) >= 0) {
        if (IopThread* waiting = threadFor(thread)) {
            waiting->awaitingEe = false;
        }
        wakeThread(thread);
    }
    freePacket(mMemory.read32(client + kClientPacket));
    mMemory.write32(client + kClientPacket, 0);
}

}  // namespace uee
