#include "mfi/codec.hpp"
#include <cstdio>

int main() {
    // Application command 1: read a cached digital input.
    mfi::Packet request;
    request.count = 1;
    request.cells[0] = {1, 1, 0};
    mfi::Bits wire;
    if (!mfi::encode(request, mfi::Role::Master, wire)) return 1;
    std::printf("Request: %u bits\n", wire.size);

    mfi::Packet received;
    if (mfi::decode(wire, mfi::Role::Master, received) != mfi::Decode::Valid) return 2;
    mfi::Packet response;
    response.count = 2;
    response.cells[0] = {1, 0, 1};      // Digital value.
    response.cells[1] = {6, 0, 1204};   // Age: 1204 * 100 ns = 120.4 us.
    if (!mfi::encode(response, mfi::Role::Slave, wire)) return 3;
    if (mfi::decode(wire, mfi::Role::Slave, received) != mfi::Decode::Valid) return 4;
    std::printf("Response: %u bits, value=%lu, age=%lu ns\n", wire.size,
                static_cast<unsigned long>(received.cells[0].value),
                static_cast<unsigned long>(received.cells[1].value * 100U));
}
