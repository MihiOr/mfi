#include "mfi/stm32f1.hpp"

static_assert(offsetof(mfi::Packet, state) == 0, "ASM packet ABI");
static_assert(offsetof(mfi::Packet, infoLength) == 1, "ASM packet ABI");
static_assert(offsetof(mfi::Packet, info) == 2, "ASM packet ABI");
static_assert(offsetof(mfi::Packet, count) == 130, "ASM packet ABI");
static_assert(offsetof(mfi::Packet, cells) == 132, "ASM packet ABI");
static_assert(sizeof(mfi::Cell) == 8 && offsetof(mfi::Cell, command) == 1 &&
              offsetof(mfi::Cell, value) == 4, "ASM cell ABI");
static_assert(sizeof(void*) == 4 && sizeof(mfi::SendArgs) == 16 && offsetof(mfi::SendArgs, budget) == 12, "ASM TX ABI");

// No C/HAL call, virtual dispatch, per-bit profiler or working-loop re-entry.
// The inline field-read macros keep accumulation and CRC in CPU registers.
// DWT deadline checks live in unsuccessful line waits and at packet boundaries.
#define MFI_STRING_1(x) #x
#define MFI_STRING(x) MFI_STRING_1(x)
asm(".equ MFI_GPIO_BASE, " MFI_STRING(MFI_GPIO_BASE) "\n"
    ".equ MFI_DATA_TX_PIN, " MFI_STRING(MFI_DATA_TX_PIN) "\n"
    ".equ MFI_DATA_RX_PIN, " MFI_STRING(MFI_DATA_RX_PIN) "\n"
    ".equ MFI_SIGNAL_TX_PIN, " MFI_STRING(MFI_SIGNAL_TX_PIN) "\n"
    ".equ MFI_SIGNAL_RX_PIN, " MFI_STRING(MFI_SIGNAL_RX_PIN) "\n"
    ".equ MFI_DATA_TX_MASK, (1 << MFI_DATA_TX_PIN)\n"
    ".equ MFI_DATA_TX_OFF, (1 << (MFI_DATA_TX_PIN + 16))\n"
    ".equ MFI_SIGNAL_TX_MASK, (1 << MFI_SIGNAL_TX_PIN)\n"
    ".equ MFI_SIGNAL_TX_OFF, (1 << (MFI_SIGNAL_TX_PIN + 16))\n"
    ".equ MFI_SIGNAL_RX_MASK, (1 << MFI_SIGNAL_RX_PIN)\n"
R"ASM(
.syntax unified
.cpu cortex-m3
.thumb
.section .text.mfi_receive_asm,"ax",%progbits
.align 2
.global mfi_receive_asm
.type mfi_receive_asm,%function
.thumb_func
mfi_receive_asm:
    push {r4-r11, lr}
    sub sp, sp, #12
    str r1, [sp, #0]
    mov r4, r0
    mov r7, r2
    mov r8, r3
    ldr r5, =MFI_GPIO_BASE
    mvn r6, #0
    ldr r9, =0x04c11db7
    movs r0, #0
    strb r0, [r4, #0]
    strb r0, [r4, #1]
    strb r0, [r4, #2]
    strb r0, [r4, #130]
    movs r1, #4
    bl .Lrx_body
    cmp r0, #11
    bne .Lrx_malformed
    ldr r0, [sp, #0]
    cbz r0, .Lrx_count
    movs r1, #3
    bl .Lrx_body
    cmp r0, #2
    bhi .Lrx_malformed
    strb r0, [r4, #0]
    movs r1, #7
    bl .Lrx_body
    strb r0, [r4, #1]
    mov r10, r0
    add r11, r4, #2
    cbz r0, .Lrx_info_done
.Lrx_info:
    movs r1, #8
    bl .Lrx_body
    cmp r0, #127
    bhi .Lrx_malformed
    strb r0, [r11], #1
    subs r10, r10, #1
    bne .Lrx_info
.Lrx_info_done:
    movs r0, #0
    strb r0, [r11]
.Lrx_count:
    movs r1, #6
    bl .Lrx_body
    strb r0, [r4, #130]
    mov r10, r0
    add r11, r4, #132
    cmp r10, #0
    beq .Lrx_footer
.Lrx_cell:
    movs r1, #3
    bl .Lrx_body
    strb r0, [r11, #0]
    movs r2, #0
    strb r2, [r11, #1]
    str r2, [r11, #4]
    ldr r2, [sp, #0]
    cbnz r2, .Lrx_slave_cell
    cmp r0, #7
    beq .Lrx_malformed
    cbz r0, .Lrx_cell_done
    adr r2, .Lrx_master_widths
    ldrb r2, [r2, r0]
    str r2, [sp, #8]
    movs r1, #6
    bl .Lrx_body
    strb r0, [r11, #1]
    ldr r1, [sp, #8]
    cbz r1, .Lrx_cell_done
    b .Lrx_value
.Lrx_slave_cell:
    adr r2, .Lrx_slave_widths
    ldrb r1, [r2, r0]
    cbz r1, .Lrx_cell_done
.Lrx_value:
    bl .Lrx_body
    str r0, [r11, #4]
.Lrx_cell_done:
    add r11, r11, #8
    subs r10, r10, #1
    bne .Lrx_cell
.Lrx_footer:
    movs r1, #32
    bl .Lrx_no_crc
    cmp r0, r6
    ite eq
    moveq r0, #0
    movne r0, #1
    ldr r2, =0xe0001004
    ldr r2, [r2]
    subs r2, r2, r7
    cmp r2, r8
    bhs .Lrx_timeout
.Lrx_return:
    add sp, sp, #12
    pop {r4-r11, pc}
.Lrx_malformed:
    movs r0, #2
    b .Lrx_abort
.Lrx_timeout:
    movs r0, #3
.Lrx_abort:
    mov r2, #MFI_DATA_TX_OFF
    str r2, [r5, #16]
    mov r2, #MFI_SIGNAL_TX_OFF
    str r2, [r5, #16]
    b .Lrx_return

// Inlined bit handshake within each field. r0=value, r1=bits left,
// r6=running CRC. r3/r12 are scratch; outer parser registers stay live.
.macro RX_FIELD crc
    movs r0, #0
.Lrx_high\@:
    ldr r3, [r5, #8]
    tst r3, #MFI_SIGNAL_RX_MASK
    bne .Lrx_sample\@
    ldr r12, =0xe0001004
    ldr r12, [r12]
    sub r12, r12, r7
    cmp r12, r8
    bhs .Lrx_timeout
    b .Lrx_high\@
.Lrx_sample\@:
    ubfx r3, r3, #MFI_DATA_RX_PIN, #1
    mov r12, #MFI_SIGNAL_TX_MASK
    str r12, [r5, #16]
    orr r0, r3, r0, lsl #1
.if \crc
    eor r2, r3, r6, lsr #31
    rsb r2, r2, #0
    and r2, r2, r9
    eor r6, r2, r6, lsl #1
.endif
.Lrx_low\@:
    ldr r3, [r5, #8]
    tst r3, #MFI_SIGNAL_RX_MASK
    beq .Lrx_release\@
    ldr r12, =0xe0001004
    ldr r12, [r12]
    sub r12, r12, r7
    cmp r12, r8
    bhs .Lrx_timeout
    b .Lrx_low\@
.Lrx_release\@:
    mov r12, #MFI_SIGNAL_TX_OFF
    str r12, [r5, #16]
    subs r1, r1, #1
    bne .Lrx_high\@
    bx lr
.endm
.Lrx_body:
    RX_FIELD 1
.Lrx_no_crc:
    RX_FIELD 0
.align 2
.Lrx_master_widths:
    .byte 0,0,1,8,12,16,32,0
.Lrx_slave_widths:
    .byte 0,1,8,12,16,32,16,32
.ltorg
.size mfi_receive_asm, .-mfi_receive_asm

.section .text.mfi_send_asm,"ax",%progbits
.align 2
.global mfi_send_asm
.type mfi_send_asm,%function
.thumb_func
mfi_send_asm:
    push {r4-r11, lr}
    sub sp, sp, #4
    mov r4, r0
    ldr r5, =MFI_GPIO_BASE
    ldr r6, =0x40010414
    ldr r7, =0xe0001004
    ldr r8, [r4, #8]
    ldr r9, [r4, #12]
    ldr r10, [r4, #0]
    ldr r11, [r4, #4]
    ldrb r1, [r10]
    movs r2, #0x80
    tst r1, r2
    ite ne
    movne r0, #MFI_DATA_TX_MASK
    moveq r0, #MFI_DATA_TX_OFF
    str r0, [r5, #16]
.Ltx_raise:
    mov r0, #MFI_SIGNAL_TX_MASK
    str r0, [r5, #16]
.Ltx_ack_high:
    ldr r0, [r5, #8]
    tst r0, #MFI_SIGNAL_RX_MASK
    bne .Ltx_acknowledged
    ldr r0, [r7]
    sub r0, r0, r8
    cmp r0, r9
    bhs .Ltx_timeout
    b .Ltx_ack_high
.Ltx_acknowledged:
    mov r0, #MFI_SIGNAL_RX_MASK
    str r0, [r6]
    subs r11, r11, #1
    beq .Ltx_last
    // The receiver already sampled this bit. Drop DOL, prepare NEXT data
    // during ACK HIGH, and raise DOL as soon as ACK returns LOW.
    mov r0, #MFI_SIGNAL_TX_OFF
    str r0, [r5, #16]
    lsrs r2, r2, #1
    bne .Ltx_stage
    ldrb r1, [r10, #1]!
    movs r2, #0x80
.Ltx_stage:
    tst r1, r2
    ite ne
    movne r0, #MFI_DATA_TX_MASK
    moveq r0, #MFI_DATA_TX_OFF
    str r0, [r5, #16]
    b .Ltx_ack_low
.Ltx_last:
    // Release the half-duplex emitter BEFORE peer can finish ACK and reply.
    mov r0, #MFI_DATA_TX_OFF
    str r0, [r5, #16]
    mov r0, #MFI_SIGNAL_TX_OFF
    str r0, [r5, #16]
.Ltx_ack_low:
    ldr r0, [r5, #8]
    tst r0, #MFI_SIGNAL_RX_MASK
    beq .Ltx_completed_bit
    ldr r0, [r6]
    tst r0, #MFI_SIGNAL_RX_MASK
    bne .Ltx_completed_bit
    ldr r0, [r7]
    sub r0, r0, r8
    cmp r0, r9
    bhs .Ltx_timeout
    b .Ltx_ack_low
.Ltx_completed_bit:
    cmp r11, #0
    bne .Ltx_raise
    ldr r0, [r7]
    sub r0, r0, r8
    cmp r0, r9
    bhs .Ltx_timeout
    movs r0, #0
.Ltx_return:
    add sp, sp, #4
    pop {r4-r11, pc}
.Ltx_timeout:
    mov r0, #MFI_DATA_TX_OFF
    str r0, [r5, #16]
    mov r0, #MFI_SIGNAL_TX_OFF
    str r0, [r5, #16]
    movs r0, #3
    b .Ltx_return
.ltorg
.size mfi_send_asm, .-mfi_send_asm
)ASM");
