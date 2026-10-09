#include "mfi/link.hpp"
#include "mfi/stm32f1.hpp"

// Integration example, compiled as an object library. The application's
// startup code configures clocks and calls initializeMaster/initializeSlave
// on the respective endpoint; neither example supplies a vector table.
namespace master_example {
mfi::Stm32f1Port port;
mfi::Stm32f1FastTransport wire(port);
mfi::Link<mfi::Stm32f1FastTransport> link(wire, mfi::Role::Master);
void initializeMaster() { port.initialize(); }
mfi::Exchange readSample(mfi::Packet &reply, uint32_t cyclesPerSecond) {
    mfi::Packet request;
    request.count = 1;
    request.cells[0] = {1, 1, 0};
    const uint32_t mask = port.enterCommunication();
    const auto result = link.exchange(request, reply, cyclesPerSecond * 2U);
    port.leaveCommunication(mask);
    // Inspect result and reply, or print diagnostics, after returning.
    return result;
}
}

namespace slave_example {
mfi::Stm32f1Port port;
mfi::Stm32f1FastTransport wire(port);
mfi::Link<mfi::Stm32f1FastTransport> link(wire, mfi::Role::Slave);
bool cachedValue = false;
uint32_t sampledAt = 0;
void initializeSlave() { port.initialize(); }
void sampleInWorkingMode(bool value) {
    cachedValue = value;
    sampledAt = port.cycles();
}
mfi::Exchange serviceIfRequested(uint32_t cyclesPerSecond) {
    if (!wire.pending()) return mfi::Exchange::Idle;
    const uint32_t mask = port.enterCommunication();
    const auto result = link.serve([cyclesPerSecond](const mfi::Packet &request, mfi::Packet &reply) {
        if (request.count == 1 && request.cells[0].type == 1 && request.cells[0].command == 1) {
            const uint32_t age = uint32_t(port.cycles() - sampledAt);
            const uint32_t ticks100ns = uint32_t(uint64_t(age) * 10000000ULL / cyclesPerSecond);
            reply.count = 2;
            reply.cells[0] = {1, 0, uint32_t(cachedValue)};
            reply.cells[1] = {7, 0, ticks100ns};
        } else {
            reply.state = mfi::State::Warning; // No application acceptance.
        }
    }, cyclesPerSecond * 2U);
    port.leaveCommunication(mask);
    return result;
}
}
