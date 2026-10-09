#include "mfi/codec.hpp"

namespace mfi {
namespace {
unsigned valueWidth(Role role, unsigned type) {
    static const unsigned master[] = {0, 0, 1, 8, 12, 16, 32};
    static const unsigned slave[] = {0, 1, 8, 12, 16, 32, 16, 32};
    return role == Role::Master ? (type < 7 ? master[type] : 99) : slave[type];
}
bool fits(uint32_t value, unsigned width) {
    return width == 32 || (width == 0 ? value == 0 : value < (1UL << width));
}
uint32_t crcBit(uint32_t crc, bool bit) {
    const uint32_t feedback = (crc >> 31) ^ uint32_t(bit);
    return (crc << 1) ^ (0x04C11DB7U & (0U - feedback));
}
}
bool Bits::bit(unsigned index) const {
    return (bytes[index / 8] & (0x80U >> (index % 8))) != 0;
}
bool Bits::append(uint32_t value, unsigned width) {
    if (width > 32 || size + width > MaxBits) return false;
    for (unsigned i = width; i != 0; --i) {
        const uint8_t mask = uint8_t(0x80U >> (size % 8));
        if ((value >> (i - 1)) & 1U) bytes[size / 8] |= mask;
        else bytes[size / 8] &= uint8_t(~mask);
        ++size;
    }
    return true;
}
uint32_t Bits::read(unsigned offset, unsigned width) const {
    uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i) result = (result << 1) | uint32_t(bit(offset + i));
    return result;
}
uint32_t checksum(const Bits &bits, unsigned count) {
    uint32_t crc = 0xFFFFFFFFU;
    for (unsigned i = 0; i < count; ++i) {
        crc = crcBit(crc, bits.bit(i));
    }
    return crc;
}
bool encode(const Packet &p, Role sender, Bits &bits) {
    bits.size = 0;
    if (p.count > MaxCells || p.infoLength > MaxInfo || unsigned(p.state) > 2) return false;
    bits.append(0xB, 4);
    if (sender == Role::Slave) {
        bits.append(unsigned(p.state), 3);
        bits.append(p.infoLength, 7);
        for (unsigned i = 0; i < p.infoLength; ++i) {
            if (uint8_t(p.info[i]) > 127) return false;
            bits.append(uint8_t(p.info[i]), 8);
        }
    }
    bits.append(p.count, 6);
    for (unsigned i = 0; i < p.count; ++i) {
        const Cell &c = p.cells[i];
        if (c.type > 7) return false;
        const unsigned width = valueWidth(sender, c.type);
        if (width > 32 || !fits(c.value, width)) return false;
        bits.append(c.type, 3);
        if (sender == Role::Master && c.type != 0) {
            if (c.command > 63) return false;
            bits.append(c.command, 6);
        }
        bits.append(c.value, width);
    }
    const uint32_t crc = checksum(bits, bits.size);
    const bool result = bits.append(crc, 32);
    return result;
}
Decode decode(const Bits &bits, Role sender, Packet &p) {
    unsigned pos = 0;
    auto take = [&](unsigned width, uint32_t &value) {
        if (pos + width > bits.size) return false;
        value = bits.read(pos, width);
        pos += width;
        return true;
    };
    uint32_t value;
    if (!take(4, value)) return Decode::Incomplete;
    if (value != 0xB) return Decode::Invalid;
    p.state = State::Ok;
    p.infoLength = 0;
    if (sender == Role::Slave) {
        if (!take(3, value)) return Decode::Incomplete;
        if (value > 2) return Decode::Invalid;
        p.state = State(value);
        if (!take(7, value)) return Decode::Incomplete;
        p.infoLength = uint8_t(value);
        for (unsigned i = 0; i < p.infoLength; ++i) {
            if (!take(8, value)) return Decode::Incomplete;
            if (value > 127) return Decode::Invalid;
            p.info[i] = char(value);
        }
    }
    p.info[p.infoLength] = '\0';
    if (!take(6, value)) return Decode::Incomplete;
    p.count = uint8_t(value);
    for (unsigned i = 0; i < p.count; ++i) {
        if (!take(3, value)) return Decode::Incomplete;
        Cell &c = p.cells[i];
        c.type = uint8_t(value);
        c.command = 0;
        const unsigned width = valueWidth(sender, c.type);
        if (width > 32) return Decode::Invalid;
        if (sender == Role::Master && c.type != 0) {
            if (!take(6, value)) return Decode::Incomplete;
            c.command = uint8_t(value);
        }
        if (!take(width, value)) return Decode::Incomplete;
        c.value = value;
    }
    const unsigned bodyBits = pos;
    if (!take(32, value)) return Decode::Incomplete;
    if (pos != bits.size) return Decode::Invalid;
    return checksum(bits, bodyBits) == value ? Decode::Valid : Decode::BadChecksum;
}

void StreamDecoder::next(Field field, unsigned width) {
    field_ = field;
    remaining_ = width;
    value_ = 0;
}
void StreamDecoder::reset(Role sender, Packet &packet) {
    sender_ = sender;
    packet_ = &packet;
    packet.state = State::Ok;
    packet.infoLength = packet.count = 0;
    packet.info[0] = '\0';
    cell_ = info_ = valueWidth_ = 0;
    crc_ = 0xFFFFFFFFU;
    result_ = Decode::Incomplete;
    next(Field::Preamble, 4);
}
void StreamDecoder::finishCell() {
    ++cell_;
    next(cell_ == packet_->count ? Field::Footer : Field::Type,
         cell_ == packet_->count ? 32U : 3U);
}
Decode StreamDecoder::push(bool bit) {
    if (result_ != Decode::Incomplete) return result_ = Decode::Invalid;
    // Footer bits are the transmitted CRC, never input to the running CRC.
    if (field_ != Field::Footer) crc_ = crcBit(crc_, bit);
    value_ = (value_ << 1) | uint32_t(bit);
    if (--remaining_ != 0) return Decode::Incomplete;
    const uint32_t value = value_;
    switch (field_) {
    case Field::Preamble:
        if (value != 0xBU) return result_ = Decode::Invalid;
        next(sender_ == Role::Slave ? Field::State : Field::Count,
             sender_ == Role::Slave ? 3U : 6U);
        break;
    case Field::State:
        if (value > 2) return result_ = Decode::Invalid;
        packet_->state = State(value);
        next(Field::InfoLength, 7);
        break;
    case Field::InfoLength:
        packet_->infoLength = uint8_t(value);
        next(value ? Field::Info : Field::Count, value ? 8U : 6U);
        break;
    case Field::Info:
        if (value > 127) return result_ = Decode::Invalid;
        packet_->info[info_++] = char(value);
        if (info_ == packet_->infoLength) {
            packet_->info[info_] = '\0';
            next(Field::Count, 6);
        } else next(Field::Info, 8);
        break;
    case Field::Count:
        packet_->count = uint8_t(value);
        next(value ? Field::Type : Field::Footer, value ? 3U : 32U);
        break;
    case Field::Type:
        valueWidth_ = valueWidth(sender_, value);
        if (valueWidth_ > 32) return result_ = Decode::Invalid;
        packet_->cells[cell_] = {uint8_t(value), 0, 0};
        if (sender_ == Role::Master && value != 0) next(Field::Command, 6);
        else if (valueWidth_) next(Field::Value, valueWidth_);
        else finishCell();
        break;
    case Field::Command:
        packet_->cells[cell_].command = uint8_t(value);
        if (valueWidth_) next(Field::Value, valueWidth_);
        else finishCell();
        break;
    case Field::Value:
        packet_->cells[cell_].value = value;
        finishCell();
        break;
    case Field::Footer:
        field_ = Field::End;
        return result_ = value == crc_ ? Decode::Valid : Decode::BadChecksum;
    default:
        return result_ = Decode::Invalid;
    }
    return Decode::Incomplete;
}

void Health::bad() {
    if (badPackets < 3) ++badPackets;
    const State next = badPackets >= 3 ? State::Terminal : State::Warning;
    if (unsigned(next) > unsigned(state)) state = next;
}
void Health::good() { badPackets = 0; }
const char *stateName(State state) {
    switch (state) {
    case State::Ok: return "OK";
    case State::Warning: return "WARNING";
    case State::Terminal: return "TERMINAL";
    }
    return "UNKNOWN";
}
}
