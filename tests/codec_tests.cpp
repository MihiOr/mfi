#include "mfi/codec.hpp"
#include <cassert>
#include <cstdio>
#include <random>
#include <string>
using namespace mfi;
std::string binary(const Bits &b) {
    std::string s;
    for (unsigned i = 0; i < b.size; ++i) s += b.bit(i) ? '1' : '0';
    return s;
}
void codecTests() {
    Packet p, decoded;
    Bits b;
    p.count = 1;
    p.cells[0] = {1, 1, 0};
    assert(encode(p, Role::Master, b));
    assert(b.size == 51);
    assert(binary(b).substr(0, 19) == "1011000001001000001");
    assert(decode(b, Role::Master, decoded) == Decode::Valid);
    // Independent standard CRC-32/MPEG-2 check vector: this bitwise convention
    // matches it when the body length happens to be a multiple of eight.
    Bits vector;
    for (char ch : std::string("123456789")) vector.append(ch, 8);
    assert(checksum(vector, vector.size) == 0x0376E6E7U);

    p.count = 2;
    p.cells[0] = {1, 0, 1};
    p.cells[1] = {6, 0, 1204};
    assert(encode(p, Role::Slave, b));
    assert(b.size == 75);
    assert(binary(b).substr(0, 43) == "1011000000000000001000111100000010010110100");
    assert(decode(b, Role::Slave, decoded) == Decode::Valid);
    for (unsigned i = 0; i < b.size; ++i) {
        Bits corrupt = b;
        corrupt.bytes[i / 8] ^= uint8_t(0x80U >> (i % 8));
        assert(decode(corrupt, Role::Slave, decoded) != Decode::Valid);
    }
    for (unsigned i = 0; i < b.size; ++i) {
        Bits partial = b;
        partial.size = i;
        assert(decode(partial, Role::Slave, decoded) == Decode::Incomplete);
    }

    const unsigned mw[] = {0, 0, 1, 8, 12, 16, 32};
    const unsigned sw[] = {0, 1, 8, 12, 16, 32, 16, 32};
    for (Role role : {Role::Master, Role::Slave}) {
        p.count = 63;
        p.state = State::Terminal;
        p.infoLength = 127;
        for (char &ch : p.info) ch = 'A';
        for (unsigned i = 0; i < 63; ++i) {
            unsigned type = i % (role == Role::Master ? 7 : 8);
            unsigned w = role == Role::Master ? mw[type] : sw[type];
            p.cells[i] = {uint8_t(type), 63, w == 32 ? 0xFFFFFFFFU : uint32_t((1ULL << w) - 1)};
        }
        assert(encode(p, role, b));
        assert(decode(b, role, decoded) == Decode::Valid);
        assert(decoded.count == 63);
        for (unsigned i = 0; i < 63; ++i) {
            assert(decoded.cells[i].type == p.cells[i].type);
            assert(decoded.cells[i].value == p.cells[i].value);
        }
    }
    p.count = 1;
    p.cells[0] = {7, 0, 0};
    assert(!encode(p, Role::Master, b));
    p.cells[0] = {2, 1, 2};
    assert(!encode(p, Role::Master, b));
    p.cells[0] = {1, 64, 0};
    assert(!encode(p, Role::Master, b));
    p.count = 64;
    assert(!encode(p, Role::Slave, b));
}

void samePacket(const Packet &a, const Packet &b) {
    assert(a.state == b.state && a.infoLength == b.infoLength && a.count == b.count);
    for (unsigned i = 0; i <= a.infoLength; ++i) assert(a.info[i] == b.info[i]);
    for (unsigned i = 0; i < a.count; ++i) {
        assert(a.cells[i].type == b.cells[i].type);
        assert(a.cells[i].command == b.cells[i].command);
        assert(a.cells[i].value == b.cells[i].value);
    }
}
void streamTests() {
    // Compare the new bit-state decoder to the existing independent whole-buffer
    // parser at EVERY prefix, including structural faults and corrupted CRCs.
    std::mt19937 random(0xC0DEC);
    Packet source, reference, streamed;
    StreamDecoder decoder;
    assert(decoder.push(false) == Decode::Invalid); // must reset before use
    const unsigned widths[2][8] = {{0, 0, 1, 8, 12, 16, 32, 0}, {0, 1, 8, 12, 16, 32, 16, 32}};
    unsigned packets = 0;
    for (Role role : {Role::Master, Role::Slave}) {
        const unsigned r = role == Role::Master ? 0 : 1;
        for (unsigned iteration = 0; iteration < 200; ++iteration) {
            source.count = uint8_t(iteration < 2 ? iteration * 63 : random() % 64);
            source.infoLength = uint8_t(iteration < 2 ? iteration * 127 : random() % 128);
            source.state = State(iteration % 3);
            for (unsigned i = 0; i < source.infoLength; ++i) source.info[i] = char(random() % 128);
            for (unsigned i = 0; i < source.count; ++i) {
                const unsigned type = (i + iteration) % (r ? 8 : 7);
                const unsigned width = widths[r][type];
                source.cells[i] = {uint8_t(type), uint8_t(random() % 64),
                    uint32_t(random()) & (width == 32 ? 0xFFFFFFFFU : uint32_t((1ULL << width) - 1))};
            }
            Bits bits;
            assert(encode(source, role, bits));
            for (unsigned variant = 0; variant < 3; ++variant) {
                Bits test = bits;
                if (variant) {
                    const unsigned index = variant == 1 ? test.size - 1 : random() % test.size;
                    test.bytes[index / 8] ^= uint8_t(0x80U >> (index % 8));
                }
                decoder.reset(role, streamed); // reuse memory, including after errors
                Bits prefix = test;
                Decode status = Decode::Incomplete;
                for (unsigned i = 0; i < test.size; ++i) {
                    prefix.size = i + 1;
                    status = decoder.push(test.bit(i));
                    const auto expected = decode(prefix, role, reference);
                    assert(status == expected);
                    if (status == Decode::Valid) samePacket(reference, streamed);
                }
                if (variant == 0) assert(status == Decode::Valid);
                if (variant == 1) assert(status == Decode::BadChecksum);
                if (variant == 2) assert(status != Decode::Valid);
                if (variant == 0) assert(decoder.push(false) == Decode::Invalid); // trailing bits
                ++packets;
            }
        }
    }
    std::printf("Streaming parser matched bulk reference at every bit of %u valid/corrupt packets\n", packets);
}

void healthTests() {
    Health h;
    h.bad(); assert(h.state == State::Warning);
    h.good(); assert(h.badPackets == 0 && h.state == State::Warning);
    h.bad(); h.bad(); h.bad(); assert(h.state == State::Terminal);
    h.good(); assert(h.state == State::Terminal);
}
int main() {
    codecTests();
    streamTests();
    healthTests();
    std::puts("Codec, CRC and latched health tests passed.");
}
