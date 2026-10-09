# Library integration

The library has three layers:

- `codec.hpp` defines packets, bit buffers, encoding, decoding, and latched health.
- `transport.hpp` performs the four-phase handshake through a GPIO port.
- `link.hpp` owns a master or slave request/response connection and its fault state.

None of the portable code depends on an operating system, serial output, HAL, dynamic allocation, or a device's application commands. C++14 is required. Buffers are bounded: 63 cells, 127 information characters, and 457 bytes of encoded storage. `Link` retains two packet buffers and one bit buffer; measure `sizeof` and stack usage for the target ABI before allocating many connections.

## Portable GPIO port

Provide the following methods:

```cpp
uint32_t cycles() const;        // Free-running tick counter; works with IRQs masked.
bool readPeerSignal() const;   // Incoming DOL or ACK, depending on direction.
bool readData() const;
void writeData(bool high);     // LOW must turn the local data emitter off.
void writeSignal(bool high);
void clearPeerFalling();       // Clear the hardware falling-edge latch.
bool peerFell() const;         // Falling edge occurred since clearPeerFalling().
```

Create `mfi::Transport<YourPort>` and `mfi::Link<decltype(wire)>`. Keep GPIO methods small and inline where appropriate. `cycles()` is a tick source, not a baud generator; it is consulted while waiting and at transfer completion. The signal handshake controls progress. Port accessors must not print, sleep, or service unrelated work.

Do not substitute a read of the current pin level for `peerFell()`: the ACK LOW phase can disappear between polls during turnaround. The latch also needs to remain functional while its CPU interrupt is masked.

## Link API

```cpp
YourPort port;
mfi::Transport<YourPort> wire(port);
mfi::Link<decltype(wire)> master(wire, mfi::Role::Master);

mfi::Packet request, response;
request.count = 1;
request.cells[0] = {1, 1, 0};
const auto result = master.exchange(request, response, timeoutTicks);
```

`exchange` returns after a complete request/response or fault. Both transfers use the same start timestamp and budget. Check the returned `Exchange` before consuming the response; then validate its state and application schema. `Complete` means a valid response arrived, including a response with terminal state; `disconnected()` will be true in that case.

The slave checks `wire.pending()` in working mode or enters from a DOL rising interrupt:

```cpp
mfi::Link<decltype(wire)> slave(wire, mfi::Role::Slave);
const auto result = slave.serve(
    [](const mfi::Packet &request, mfi::Packet &reply) {
        // Validate commands, then return cached state or an application ACK.
        reply.count = 1;
        reply.cells[0] = {0, 0, 0};
    }, timeoutTicks);
```

`Idle` means no DOL HIGH was present. The callback runs only for a complete CRC-valid request. Use cached values and bounded operations in it; it is part of communication, not the working loop. Encode failure, invalid framing, timeout, and CRC error are separate results. The reference health policy is documented in [protocol.md](protocol.md).

No method clears a terminal latch. Recreate/reinitialize the link only after an explicit recovery decision and when both endpoints are idle. Receiving idle status is not proof that the peer is powered on.

## STM32F1 Thumb-2 backend

`ports/stm32f1/fast.cpp` implements transmission, reception, field parsing, and receive CRC accumulation in Thumb-2. Packet construction and transmit encoding remain C++. The GPIO adapter uses register access and DWT CYCCNT directly; it has no vendor-source dependency.

Enable it in a Cortex-M3 project and link `mfi_stm32f1`. Configure the following CMake variables for **all** sources using the backend:

| Variable | Meaning |
| --- | --- |
| `MFI_GPIO_BASE` | STM32F1 GPIO bank address |
| `MFI_SIGNAL_RX_PIN` | Incoming signal pin number, 0–15 |
| `MFI_DATA_TX_PIN` | Outgoing data emitter pin number |
| `MFI_DATA_RX_PIN` | Incoming data receiver pin number |
| `MFI_SIGNAL_TX_PIN` | Outgoing signal emitter pin number |

All four pins must be distinct and on the same bank. Defaults use GPIOA pins 0, 1, 2 and 3 solely as an example; select unoccupied pins for the application. The adapter sets inputs to pull-down and outputs to push-pull. It enables the relevant GPIO bank, AFIO, and DWT and configures the incoming signal's falling-edge latch. Board startup still owns oscillator, clock-tree, power, vector-table and optical interface setup.

```sh
cmake -S . -B build/arm \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
  -DMFI_BUILD_STM32F1=ON \
  -DMFI_BUILD_TESTS=OFF -DMFI_BUILD_EXAMPLES=OFF
cmake --build build/arm
python -m pip install -r tests/requirements.txt
python tests/arm_transport_tests.py build/arm/transport.elf
```

Use an Arm GNU toolchain with `arm-none-eabi-g++` on PATH. `transport.elf` is a test image with transport entry points, not flashable application firmware. The example object target checks embedded integration without supplying startup code.

### Slave interrupt integration

An application may arm SIGNAL_RX for rising-edge interrupts while idle. On entry, mask that link's EXTI interrupt and disable its rising detection. Keep falling detection enabled for turnaround latching. Call `serve` with a free-running deadline, then restore idle rising detection if the link is still healthy. Clear pending state only at the appropriate boundary, and recheck the input level when rearming so an already-held HIGH request cannot be lost.

The example uses an idle level check, making it independent of a project's interrupt vector layout. For uninterrupted software communication it saves PRIMASK, masks interrupts for the whole exchange, and restores the original mask on return. It also reads the cached input before computing sample age. Choose that execution policy with the rest of the firmware in mind: an entire stalled conversation can consume the two-second deadline.

### Timing and ABI

The assembly checks packet and argument offsets at compile time and follows AAPCS register/stack rules. It uses the same CRC and grammar as the portable codec. CYCCNT wraps modulo 32 bits; budgets must be 1–`0x7FFFFFFF` ticks. Convert the desired timeout from the actual core clock, and do not change that clock during an exchange.

For example, `cyclesPerSecond * 2` represents two seconds if it fits the supported budget. Cached timestamp ages must also be interpreted within the tick counter's wrap interval. The duration field's 100 ns unit does not turn CYCCNT into a 100 ns-resolution clock.

The extracted assembly has configurable pin mapping, but only one mapping per compiled backend. GPIO access order and deadlines are tested in emulation. Target timing, optical setup/hold, and electrical behavior require measurements on the intended hardware.
