#include "mfi/stm32f1.hpp"
// Link both entry points without board startup, HAL, or application firmware.
auto tx_anchor = &mfi::mfi_send_asm;
auto rx_anchor = &mfi::mfi_receive_asm;
