#include "mfi/link.hpp"
#include <cassert>
#include <vector>

struct Wire {
    uint32_t clock = 100;
    bool requested = true, stopped = false;
    mfi::Transfer rx = mfi::Transfer::Ok, tx = mfi::Transfer::Ok;
    mfi::Packet input;
    std::vector<mfi::Packet> output;
    std::vector<uint32_t> starts, budgets;
    uint32_t cycles() const { return clock; }
    bool pending() const { return requested; }
    void stop() { stopped = true; }
    mfi::Transfer send(const mfi::Bits &bits, uint32_t start, uint32_t budget) {
        starts.push_back(start); budgets.push_back(budget); clock += 20;
        mfi::Packet packet;
        auto role = master ? mfi::Role::Master : mfi::Role::Slave;
        assert(mfi::decode(bits, role, packet) == mfi::Decode::Valid);
        output.push_back(packet);
        return tx;
    }
    mfi::Transfer receive(mfi::Packet &packet, mfi::Role, uint32_t start, uint32_t budget) {
        starts.push_back(start); budgets.push_back(budget); clock += 30;
        packet = input;
        return rx;
    }
    bool master = false;
};

int main() {
    Wire master; master.master = true;
    mfi::Link<Wire> link(master, mfi::Role::Master);
    mfi::Packet request, response;
    request.count = 1; request.cells[0] = {1, 1, 0};
    master.input.count = 1; master.input.cells[0] = {0, 0, 0};
    assert(link.exchange(request, response, 200) == mfi::Exchange::Complete);
    assert(master.starts[0] == master.starts[1] && master.budgets[0] == master.budgets[1]);
    assert(link.serve([](const mfi::Packet &, mfi::Packet &) {}, 200) == mfi::Exchange::WrongRole);
    assert(link.exchange(request, response, 0) == mfi::Exchange::EncodeError);
    master.rx = mfi::Transfer::Timeout;
    assert(link.exchange(request, response, 200) == mfi::Exchange::Timeout);
    assert(link.disconnected() && master.stopped && link.health().state == mfi::State::Terminal);
    const auto calls = master.starts.size();
    assert(link.exchange(request, response, 200) == mfi::Exchange::Disconnected);
    assert(master.starts.size() == calls); // No automatic retry.

    Wire slave; slave.input = request;
    mfi::Link<Wire> target(slave, mfi::Role::Slave);
    unsigned callbacks = 0;
    auto handle = [&callbacks](const mfi::Packet &q, mfi::Packet &r) {
        ++callbacks; assert(q.count == 1 && q.cells[0].command == 1);
        r.count = 1; r.cells[0] = {1, 0, 1};
    };
    slave.requested = false;
    assert(target.serve(handle, 200) == mfi::Exchange::Idle && callbacks == 0);
    slave.requested = true;
    assert(target.serve(handle, 200) == mfi::Exchange::Complete && callbacks == 1);
    assert(slave.starts[0] == slave.starts[1] && slave.output.back().cells[0].value == 1);
    slave.rx = mfi::Transfer::BadChecksum;
    assert(target.serve(handle, 200) == mfi::Exchange::BadChecksum && callbacks == 1);
    assert(slave.output.back().state == mfi::State::Warning && slave.output.back().count == 0);
    slave.rx = mfi::Transfer::Ok;
    assert(target.serve(handle, 200) == mfi::Exchange::Complete && callbacks == 2);
    assert(target.health().state == mfi::State::Warning); // Severity stays latched.
    slave.rx = mfi::Transfer::BadChecksum;
    for (unsigned i = 0; i < 3; ++i) assert(target.serve(handle, 200) == mfi::Exchange::BadChecksum);
    assert(target.disconnected() && slave.stopped && slave.output.back().state == mfi::State::Terminal);
    assert(target.serve(handle, 200) == mfi::Exchange::Disconnected);

    // Separate links on one unit can own different roles and independent faults.
    Wire second; second.master = true;
    mfi::Link<Wire> independent(second, mfi::Role::Master);
    assert(!independent.disconnected());
    assert(independent.exchange(request, response, 200) == mfi::Exchange::Complete);
}
