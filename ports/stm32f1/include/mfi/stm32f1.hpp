#pragma once
#include "mfi/transport.hpp"
#include <cstddef>

// Override consistently for every source in the target. Four distinct pins
// on one STM32F1 GPIO bank; these defaults are an example mapping.
#ifndef MFI_GPIO_BASE
#define MFI_GPIO_BASE 0x40010800
#endif
#ifndef MFI_SIGNAL_RX_PIN
#define MFI_SIGNAL_RX_PIN 0
#endif
#ifndef MFI_DATA_TX_PIN
#define MFI_DATA_TX_PIN 1
#endif
#ifndef MFI_DATA_RX_PIN
#define MFI_DATA_RX_PIN 2
#endif
#ifndef MFI_SIGNAL_TX_PIN
#define MFI_SIGNAL_TX_PIN 3
#endif

namespace mfi {
static_assert(MFI_GPIO_BASE >= 0x40010800 && MFI_GPIO_BASE <= 0x40012000 &&
              (MFI_GPIO_BASE - 0x40010800) % 0x400 == 0, "STM32F1 GPIO bank");
static_assert(MFI_SIGNAL_RX_PIN < 16 && MFI_DATA_TX_PIN < 16 &&
              MFI_DATA_RX_PIN < 16 && MFI_SIGNAL_TX_PIN < 16, "GPIO pin range");
static_assert(MFI_SIGNAL_RX_PIN != MFI_DATA_TX_PIN && MFI_SIGNAL_RX_PIN != MFI_DATA_RX_PIN &&
              MFI_SIGNAL_RX_PIN != MFI_SIGNAL_TX_PIN && MFI_DATA_TX_PIN != MFI_DATA_RX_PIN &&
              MFI_DATA_TX_PIN != MFI_SIGNAL_TX_PIN && MFI_DATA_RX_PIN != MFI_SIGNAL_TX_PIN,
              "Four distinct GPIO pins required");

// No vendor headers, HAL calls or board indicators. Clock tree setup is the
// application's responsibility. initialize() enables GPIO, AFIO and DWT.
class Stm32f1Port {
    static volatile uint32_t &reg(uint32_t address) {
        return *reinterpret_cast<volatile uint32_t *>(address);
    }
    static void configure(unsigned pin, uint32_t mode) {
        auto &config = reg(MFI_GPIO_BASE + (pin < 8 ? 0U : 4U));
        const unsigned shift = (pin % 8) * 4;
        config = (config & ~(0xFU << shift)) | (mode << shift);
    }
public:
    static constexpr uint32_t SignalRx = 1U << MFI_SIGNAL_RX_PIN;
    static constexpr uint32_t DataTx = 1U << MFI_DATA_TX_PIN;
    static constexpr uint32_t DataRx = 1U << MFI_DATA_RX_PIN;
    static constexpr uint32_t SignalTx = 1U << MFI_SIGNAL_TX_PIN;
    void initialize() {
        constexpr unsigned bank = (MFI_GPIO_BASE - 0x40010800) / 0x400;
        reg(0x40021018) |= 1U | (1U << (bank + 2)); // RCC APB2: AFIO + GPIO.
        reg(MFI_GPIO_BASE + 0x10) = (SignalRx | DataRx | DataTx | SignalTx) << 16;
        configure(MFI_SIGNAL_RX_PIN, 8); // Input, pull-down through ODR.
        configure(MFI_DATA_RX_PIN, 8);
        configure(MFI_DATA_TX_PIN, 3);   // 50 MHz push-pull output.
        configure(MFI_SIGNAL_TX_PIN, 3);
        const uint32_t exticr = 0x40010008 + (MFI_SIGNAL_RX_PIN / 4) * 4;
        const unsigned shift = (MFI_SIGNAL_RX_PIN % 4) * 4;
        reg(exticr) = (reg(exticr) & ~(0xFU << shift)) | (bank << shift);
        reg(0x40010400) &= ~SignalRx; // Mask EXTI interrupt; retain edge latch.
        reg(0x40010404) &= ~SignalRx;
        reg(0x40010408) &= ~SignalRx;
        reg(0x4001040C) |= SignalRx;
        clearPeerFalling();
        reg(0xE000EDFC) |= 1U << 24; // DEMCR.TRCENA.
        reg(0xE0001000) |= 1U;       // DWT.CYCCNTENA; do not reset shared clock.
    }
    uint32_t cycles() const { return reg(0xE0001004); }
    bool readPeerSignal() const { return reg(MFI_GPIO_BASE + 8) & SignalRx; }
    bool readData() const { return reg(MFI_GPIO_BASE + 8) & DataRx; }
    void writeData(bool high) { reg(MFI_GPIO_BASE + 0x10) = high ? DataTx : DataTx << 16; }
    void writeSignal(bool high) { reg(MFI_GPIO_BASE + 0x10) = high ? SignalTx : SignalTx << 16; }
    void clearPeerFalling() { reg(0x40010414) = SignalRx; }
    bool peerFell() const { return reg(0x40010414) & SignalRx; }
    // Save/restore around a whole conversation, never around individual bits.
    static uint32_t enterCommunication() {
        uint32_t mask;
        asm volatile("mrs %0, primask\ncpsid i" : "=r"(mask) :: "memory");
        return mask;
    }
    static void leaveCommunication(uint32_t mask) {
        asm volatile("msr primask, %0" :: "r"(mask) : "memory");
    }
};

struct SendArgs { const uint8_t *bytes; uint32_t bits, start, budget; };
extern "C" Transfer mfi_receive_asm(Packet *packet, uint32_t senderIsSlave,
                                   uint32_t start, uint32_t budget);
extern "C" Transfer mfi_send_asm(const SendArgs *args);

class Stm32f1FastTransport {
public:
    explicit Stm32f1FastTransport(Stm32f1Port &port) : port_(port) {}
    uint32_t cycles() const { return port_.cycles(); }
    bool pending() const { return port_.readPeerSignal(); }
    void stop() { port_.writeData(false); port_.writeSignal(false); }
    Transfer send(const Bits &bits, uint32_t start, uint32_t budget) {
        if (!bits.size || bits.size > MaxBits) return Transfer::Malformed;
        const SendArgs args{bits.bytes, bits.size, start, budget};
        return mfi_send_asm(&args);
    }
    Transfer receive(Packet &packet, Role sender, uint32_t start, uint32_t budget) {
        return mfi_receive_asm(&packet, sender == Role::Slave, start, budget);
    }
private:
    Stm32f1Port &port_;
};
}
