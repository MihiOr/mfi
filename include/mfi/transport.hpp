#pragma once
#include "mfi/codec.hpp"

namespace mfi {
enum class Transfer : uint32_t { Ok, BadChecksum, Malformed, Timeout };

// Port supplies GPIO access, a free-running uint32_t clock, and a latched
// falling edge on the peer's signal. All waits share one exchange deadline.
template<class Port> class Transport {
public:
    explicit Transport(Port &port) : port_(port) {}
    uint32_t cycles() const { return port_.cycles(); }
    bool pending() const { return port_.readPeerSignal(); }
    void stop() { port_.writeData(false); port_.writeSignal(false); }

    Transfer send(const Bits &bits, uint32_t start, uint32_t budget) {
        if (!bits.size || bits.size > MaxBits) return Transfer::Malformed;
        port_.writeData(bits.bit(0));
        for (unsigned i = 0; i < bits.size; ++i) {
            port_.writeSignal(true);
            while (!port_.readPeerSignal()) if (expired(start, budget)) return timeout();
            port_.clearPeerFalling();
            if (i + 1 == bits.size) {
                // Turn the data emitter off before the receiver can reply.
                port_.writeData(false);
                port_.writeSignal(false);
            } else {
                port_.writeSignal(false);
                port_.writeData(bits.bit(i + 1));
            }
            // The final ACK LOW may already have become response DOL HIGH.
            while (port_.readPeerSignal() && !port_.peerFell())
                if (expired(start, budget)) return timeout();
        }
        return expired(start, budget) ? timeout() : Transfer::Ok;
    }

    Transfer receive(Packet &packet, Role sender, uint32_t start, uint32_t budget) {
        StreamDecoder decoder;
        decoder.reset(sender, packet);
        for (;;) {
            while (!port_.readPeerSignal()) if (expired(start, budget)) return timeout();
            const bool bit = port_.readData();
            port_.writeSignal(true);
            const Decode result = decoder.push(bit);
            while (port_.readPeerSignal()) if (expired(start, budget)) return timeout();
            port_.writeSignal(false);
            if (expired(start, budget)) return timeout();
            if (result == Decode::Valid) return Transfer::Ok;
            if (result == Decode::BadChecksum) return Transfer::BadChecksum;
            if (result == Decode::Invalid) { stop(); return Transfer::Malformed; }
        }
    }
private:
    Port &port_;
    bool expired(uint32_t start, uint32_t budget) const {
        return uint32_t(cycles() - start) >= budget;
    }
    Transfer timeout() { stop(); return Transfer::Timeout; }
};
}
