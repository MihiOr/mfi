#include "mfi/transport.hpp"
#include <cassert>
#include <vector>

struct PeerPort {
    bool receiving = false, stuck = false, data = false, signal = false;
    bool peer = false, fell = false, immediateReply = false;
    mutable uint32_t clock = 0;
    std::vector<bool> bits, sent;
    unsigned at = 0;
    uint32_t cycles() const { return clock++; }
    bool readPeerSignal() const { return stuck ? false : peer; }
    bool readData() const { assert(at < bits.size()); return bits[at]; }
    void clearPeerFalling() { fell = false; }
    bool peerFell() const { return fell; }
    void writeData(bool high) { data = high; }
    void writeSignal(bool high) {
        if (signal == high) return;
        signal = high;
        if (stuck) return;
        if (receiving) {
            if (high) { assert(peer); peer = false; }
            else { ++at; peer = at < bits.size(); }
        } else {
            if (high) { assert(!peer); sent.push_back(data); peer = true; }
            else {
                fell = true;
                peer = immediateReply && sent.size() == bits.size();
                if (sent.size() == bits.size()) assert(!data);
            }
        }
    }
};

int main() {
    mfi::Packet packet, decoded;
    packet.count = 2;
    packet.cells[0] = {1, 1, 0};
    packet.cells[1] = {6, 63, 0x12345678};
    mfi::Bits bits;
    assert(mfi::encode(packet, mfi::Role::Master, bits));
    for (bool turnaround : {false, true}) {
        PeerPort port;
        port.immediateReply = turnaround;
        for (unsigned i = 0; i < bits.size; ++i) port.bits.push_back(bits.bit(i));
        mfi::Transport<PeerPort> wire(port);
        assert(wire.send(bits, 0, 10000) == mfi::Transfer::Ok);
        assert(port.sent == port.bits && !port.signal && !port.data);
        assert(port.peer == turnaround);
    }
    for (bool corrupt : {false, true}) {
        PeerPort port;
        port.receiving = port.peer = true;
        for (unsigned i = 0; i < bits.size; ++i) port.bits.push_back(bits.bit(i));
        if (corrupt) port.bits.back() = !port.bits.back();
        mfi::Transport<PeerPort> wire(port);
        assert(wire.receive(decoded, mfi::Role::Master, 0, 10000) ==
               (corrupt ? mfi::Transfer::BadChecksum : mfi::Transfer::Ok));
        assert(port.at == bits.size && !port.signal);
        if (!corrupt) assert(decoded.count == 2 && decoded.cells[1].value == 0x12345678);
    }
    PeerPort stuck;
    stuck.stuck = true;
    stuck.clock = 0xFFFFFFF0U;
    mfi::Transport<PeerPort> wire(stuck);
    assert(wire.send(bits, 0xFFFFFFF0U, 50) == mfi::Transfer::Timeout);
    assert(!stuck.signal && !stuck.data);
    assert(wire.receive(decoded, mfi::Role::Master, stuck.cycles(), 50) == mfi::Transfer::Timeout);
    assert(!stuck.signal && !stuck.data);
    mfi::Bits empty;
    assert(wire.send(empty, 0, 100) == mfi::Transfer::Malformed);
}
