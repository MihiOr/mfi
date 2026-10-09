#pragma once
#include <cstdint>

namespace mfi {
constexpr unsigned MaxCells = 63;
constexpr unsigned MaxInfo = 127;
constexpr unsigned MaxBits = 4 + 3 + 7 + 8 * MaxInfo + 6 + MaxCells * 41 + 32;
enum class Role { Master, Slave };
enum class State : uint8_t { Ok, Warning, Terminal };
struct Cell { uint8_t type = 0; uint8_t command = 0; uint32_t value = 0; };
struct Packet {
    State state = State::Ok;
    uint8_t infoLength = 0;
    char info[MaxInfo + 1] = {};
    uint8_t count = 0;
    Cell cells[MaxCells] = {};
};
struct Bits {
    uint8_t bytes[(MaxBits + 7) / 8] = {};
    unsigned size = 0;
    bool bit(unsigned index) const;
    bool append(uint32_t value, unsigned width);
    uint32_t read(unsigned offset, unsigned width) const;
};
// MSB first, poly 0x04C11DB7, init 0xFFFFFFFF, no reflection/xor/padding.
uint32_t checksum(const Bits &bits, unsigned count);
bool encode(const Packet &packet, Role sender, Bits &bits);

enum class Decode { Incomplete, Valid, BadChecksum, Invalid };
Decode decode(const Bits &bits, Role sender, Packet &packet);

// Incremental decoder. The transport feeds one acknowledged bit at a time.
// The packet becomes usable only after push() returns Valid at the footer end.
class StreamDecoder {
public:
    void reset(Role sender, Packet &packet);
    Decode push(bool bit);
private:
    enum class Field { Preamble, State, InfoLength, Info, Count, Type, Command, Value, Footer, End };
    void next(Field field, unsigned width);
    void finishCell();
    Role sender_ = Role::Master;
    Packet *packet_ = nullptr;
    Field field_ = Field::End;
    Decode result_ = Decode::Invalid;
    uint32_t value_ = 0, crc_ = 0xFFFFFFFFU;
    unsigned remaining_ = 0, cell_ = 0, info_ = 0, valueWidth_ = 0;
};

// Errors latch until an explicit local/manual reset; a good packet only resets
// the consecutive bad-packet counter, never the latched severity.
struct Health {
    State state = State::Ok;
    unsigned badPackets = 0;
    void bad();
    void good();
};

const char *stateName(State state);
}
