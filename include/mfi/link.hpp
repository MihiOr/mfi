#pragma once
#include "mfi/transport.hpp"

namespace mfi {
enum class Exchange { Idle, Complete, BadChecksum, Malformed, Timeout,
                      Disconnected, WrongRole, EncodeError };

// One instance owns one point-to-point connection. Call only from its owner:
// a master task or a slave handler. Application work resumes after return.
template<class Wire> class Link {
public:
    Link(Wire &wire, Role role) : wire_(wire), role_(role) {}
    bool disconnected() const { return disconnected_; }
    const Health &health() const { return health_; }
    void disconnect() { health_.state = State::Terminal; disconnected_ = true; wire_.stop(); }

    Exchange exchange(const Packet &request, Packet &response, uint32_t budget) {
        if (role_ != Role::Master) return Exchange::WrongRole;
        if (disconnected_) return Exchange::Disconnected;
        if (!validBudget(budget) || !encode(request, Role::Master, bits_)) return Exchange::EncodeError;
        const uint32_t start = wire_.cycles();
        Transfer status = wire_.send(bits_, start, budget);
        if (status == Transfer::Ok) status = wire_.receive(response, Role::Slave, start, budget);
        const Exchange result = outcome(status);
        if (status == Transfer::Ok && response.state == State::Terminal) disconnect();
        return result;
    }

    // buildResponse(request, response) reads cached application state. It must
    // not block, print, allocate, or start another exchange on the same link.
    template<class Handler> Exchange serve(Handler buildResponse, uint32_t budget) {
        if (role_ != Role::Slave) return Exchange::WrongRole;
        if (disconnected_) return Exchange::Disconnected;
        if (!validBudget(budget)) return Exchange::EncodeError;
        if (!wire_.pending()) return Exchange::Idle;
        const uint32_t start = wire_.cycles();
        const Transfer received = wire_.receive(request_, Role::Master, start, budget);
        if (received == Transfer::Timeout || received == Transfer::Malformed) return outcome(received);
        response_ = Packet{};
        if (received == Transfer::BadChecksum) {
            health_.bad();
            response_.infoLength = 12;
            const char message[] = "BAD_CHECKSUM";
            for (unsigned i = 0; i <= 12; ++i) response_.info[i] = message[i];
        } else {
            health_.good();
            buildResponse(request_, response_);
        }
        if (unsigned(response_.state) < unsigned(health_.state)) response_.state = health_.state;
        if (!encode(response_, Role::Slave, bits_)) { disconnect(); return Exchange::EncodeError; }
        const Transfer sent = wire_.send(bits_, start, budget);
        if (sent != Transfer::Ok) return outcome(sent);
        if (response_.state == State::Terminal) disconnect();
        return received == Transfer::BadChecksum ? Exchange::BadChecksum : Exchange::Complete;
    }
private:
    Wire &wire_;
    Role role_;
    bool disconnected_ = false;
    Health health_;
    Bits bits_;
    Packet request_, response_;
    static bool validBudget(uint32_t budget) { return budget && budget <= 0x7FFFFFFFU; }
    Exchange outcome(Transfer result) {
        switch (result) {
        case Transfer::Ok: health_.good(); return Exchange::Complete;
        case Transfer::BadChecksum:
            health_.bad();
            if (health_.state == State::Terminal) disconnect();
            return Exchange::BadChecksum;
        case Transfer::Malformed: disconnect(); return Exchange::Malformed;
        case Transfer::Timeout: disconnect(); return Exchange::Timeout;
        }
        disconnect(); return Exchange::Malformed;
    }
};
}
