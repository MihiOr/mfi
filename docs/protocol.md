# MFI wire specification

## Link model

An MFI connection has two endpoints and three fibers. Each endpoint has DATA_TX, DATA_RX, SIGNAL_TX and SIGNAL_RX electrical interfaces to its optics. DATA_TX and DATA_RX share one optical data path. The two signal paths are simplex, in opposite directions.

The master initiates exactly one request, the slave returns exactly one response, and then the link is idle. There is no unsolicited slave transmission, arbitration, address field, shared clock, or start delay. Roles are assigned per link, independently of the electronic unit's role on other links.

All local emitters are off while idle. At request start the master stages the first data bit and raises SIGNAL_TX. At response start the slave does the same. A data value of zero is still a valid bit because DOL, not light on DATA, signals availability.

## Bit handshake

| Phase | Sender | Receiver |
| --- | --- | --- |
| Stage | Set DATA to the bit | Wait for DOL HIGH |
| Present | DOL HIGH; hold DATA | Sample DATA, ACK HIGH |
| Consumed | Observe ACK HIGH; DOL LOW; stage next DATA | Observe DOL LOW |
| Ready | Wait for ACK LOW; raise DOL for next bit | ACK LOW |

The sender's SIGNAL_TX is DOL and SIGNAL_RX is ACK. The receiver's SIGNAL_RX is DOL and SIGNAL_TX is ACK. Both signal fibers change meaning when data direction reverses.

For an ordinary bit the sender can stage the next value after lowering DOL, while the receiver is completing its ACK HIGH phase. The current bit has already been sampled. The final bit is different: DATA_TX must go LOW before DOL goes LOW so the response cannot overlap the preceding transmitter.

The receiver must reliably observe DOL HIGH before sampling, and DOL LOW before lowering ACK. The sender must reliably observe ACK HIGH, then ACK LOW, before advancing. At request/response turnaround, ACK LOW may become response DOL HIGH before the original sender next polls it. A latched falling edge records that completed LOW phase; the pending HIGH is then received as the next frame's first DOL. The reference STM32 port uses EXTI pending-bit latching even while that interrupt is masked. A port without an equivalent latch must provide another validated way to preserve this event.

No minimum HIGH or LOW duration is encoded in the protocol. Components must still meet their propagation, input sampling, and edge-detection requirements. Effective throughput includes both optical propagation directions and the endpoint's processing time for every bit.

## Frames

All fields and all values are transmitted MSB first. Frames are not byte-aligned. If encoded bits are held in a byte array, unused low bits of its final byte are storage padding and must not be transmitted.

### Master header

```text
1011[4] | CELL_COUNT[6]
```

### Slave header

```text
1011[4] | STATE[3] | INFO_LENGTH[7] | INFO[8 × INFO_LENGTH] | CELL_COUNT[6]
```

| STATE | Meaning |
| --- | --- |
| `000` | OK |
| `001` | Warning |
| `010` | Terminal |
| `011`–`111` | Invalid |

INFO is 0–127 ASCII characters, each encoded in eight bits with its high bit zero. There is no terminator on the wire. A zero byte is a permitted ASCII value; use INFO_LENGTH when interpreting it.

### Master cells

| TYPE | Wire fields | Cell length |
| --- | --- | --- |
| `000` | TYPE; ping/heartbeat | 3 bits |
| `001` | TYPE + COMMAND[6] | 9 bits |
| `010` | TYPE + COMMAND[6] + VALUE[1] | 10 bits |
| `011` | TYPE + COMMAND[6] + VALUE[8] | 17 bits |
| `100` | TYPE + COMMAND[6] + VALUE[12] | 21 bits |
| `101` | TYPE + COMMAND[6] + VALUE[16] | 25 bits |
| `110` | TYPE + COMMAND[6] + VALUE[32] | 41 bits |
| `111` | Reserved; invalid | — |

COMMAND ranges from 0 to 63. Its meaning and allowed TYPE belong to the application schema. The transport validates grammar and CRC; the application validates the command and its value before applying it.

### Slave cells

| TYPE | Wire fields | Cell length |
| --- | --- | --- |
| `000` | TYPE; ping back | 3 bits |
| `001` | TYPE + VALUE[1] | 4 bits |
| `010` | TYPE + VALUE[8] | 11 bits |
| `011` | TYPE + VALUE[12] | 15 bits |
| `100` | TYPE + VALUE[16] | 19 bits |
| `101` | TYPE + VALUE[32] | 35 bits |
| `110` | TYPE + DURATION[16], 100 ns ticks | 19 bits |
| `111` | TYPE + DURATION[32], 100 ns ticks | 35 bits |

Slave cells have no COMMAND. The request's application schema defines response order and meaning. Duration ranges are exactly 0–6.5535 ms and 0–429.4967295 s respectively. The 100 ns unit specifies representation; it does not promise a timer's accuracy or resolution.

VALUE is a raw unsigned bit field at the codec interface. Signed two's-complement values, fixed-point scaling, units, and reserved values are application decisions.

### Footer and CRC

The footer is a 32-bit CRC, MSB first:

```text
polynomial  = 0x04C11DB7
initial     = 0xFFFFFFFF
reflection  = none
final XOR   = 0
input       = pre-header + complete sender header + all cells
```

For every body bit `b`:

```cpp
uint32_t feedback = (crc >> 31) ^ uint32_t(b);
crc = (crc << 1) ^ (feedback ? 0x04C11DB7U : 0U);
```

There are no appended zero bits, byte-padding bits, or CRC-footer bits in the input. For byte-aligned bodies this convention matches CRC-32/MPEG-2: `123456789` gives `0x0376E6E7`.

The largest master frame is 2625 bits. The largest slave frame is 3273 bits (127 information bytes and 63 32-bit value cells). The library allocates a conservative common capacity of 3651 bits, or 457 bytes, for both roles.

## Digital-input example

Application command `1` requests a cached one-bit input and its sample age. The request contains **one** cell; the response contains **two** cells:

```text
Request:  1011 | 000001 | 001 000001 | CRC[32]                  (51 bits)
Response: 1011 | 000 | 0000000 | 000010
               | 001 1 | 110 0000010010110100 | CRC[32]        (75 bits)
```

The duration is 1204 ticks, or 120.4 microseconds. The sample and timestamp are captured during working mode; the response reads cached state. The CRC is calculated over the actual preceding bits, not written as a placeholder.

## Confirmation, validation, and recovery

ACK HIGH confirms that a **bit was sampled**. It does not confirm a valid CRC or successful application action. Only a complete CRC-validated response can carry application acceptance. Applications can use a plain ping-back cell as their positive ACK convention, or define richer response cells.

The codec returns `Incomplete`, `Valid`, `BadChecksum`, or `Invalid`. Values from an incomplete or rejected packet must not be applied. CRC detects transmission corruption; it provides no authentication or encryption.

The reference `Link` wrapper applies these rules:

- A timeout or malformed frame immediately latches terminal and turns local DATA_TX and SIGNAL_TX off.
- A complete request with a bad CRC does not reach the application handler. The slave returns a warning response with `BAD_CHECKSUM` and no payload cells.
- Three consecutive bad CRCs latch terminal. A valid frame resets the consecutive-error count; it does not clear a latched warning or terminal severity.
- A terminal slave response disconnects the master link. No retries occur automatically.
- Explicit reset/reinitialization must happen with both endpoints idle. The wrapper has no automatic reset method.

The request and response share one deadline. An application typically sets two seconds. A timeout is a fault limit, not a pacing delay. The clock must continue running inside communication, including when interrupts are masked. Budgets are nonzero unsigned ticks no larger than `0x7FFFFFFF`; modular subtraction handles counter wrap within that interval.

Retries of commands that have side effects require an application transaction ID and duplicate policy: a lost response does not prove the request was never applied. MFI itself does not add transaction IDs.
