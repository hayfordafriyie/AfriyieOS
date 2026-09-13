// SPDX-License-Identifier: MIT
// AfriyieOS — x86_64 COM1 serial console
//
// The primary debugging channel. Everything the kernel says, it says here
// first. See docs/AfriyieOS-Blueprint.md section 12.2.

#include "x86_64.h"
#include "afriyie/io.h"
#include "afriyie/log.h"

// -----------------------------------------------------------------------------
// 16550 UART register offsets from the port base
// -----------------------------------------------------------------------------
#define UART_DATA         0   // R/W: data (DLAB=0), divisor low (DLAB=1)
#define UART_IER          1   // R/W: interrupt enable (DLAB=0), divisor high (DLAB=1)
#define UART_FCR          2   // W:   FIFO control
#define UART_LCR          3   // R/W: line control
#define UART_MCR          4   // R/W: modem control
#define UART_LSR          5   // R:   line status
#define UART_MSR          6   // R:   modem status
#define UART_SCR          7   // R/W: scratch

#define UART_LSR_DATA_READY  0x01
#define UART_LSR_THR_EMPTY   0x20

static af_u16 s_port  = AF_COM1_PORT;
static bool  s_ready = false;

bool af_x86_serial_is_ready(void)
{
    return s_ready;
}

void af_x86_serial_init(af_u16 port, af_u32 baud)
{
    s_port = port;

    if (baud == 0) {
        baud = 115200;
    }

    af_outb((af_u16)(port + UART_IER), 0x00);   // interrupts off while we configure

    // DLAB=1 so that offsets 0 and 1 address the divisor latch.
    af_outb((af_u16)(port + UART_LCR), 0x80);

    // Divisor = 115200 / baud, from the UART's fixed 1.8432 MHz clock.
    af_u16 divisor = (af_u16)(115200 / baud);
    if (divisor == 0) {
        divisor = 1;
    }
    af_outb((af_u16)(port + UART_DATA), (af_u8)(divisor & 0xFF));
    af_outb((af_u16)(port + UART_IER),  (af_u8)((divisor >> 8) & 0xFF));

    // 8 data bits, no parity, one stop bit; DLAB back to 0.
    af_outb((af_u16)(port + UART_LCR), 0x03);

    // Enable and clear the FIFOs, 14-byte trigger level.
    af_outb((af_u16)(port + UART_FCR), 0xC7);

    // DTR + RTS + OUT2 (OUT2 is required for IRQ delivery on a real 16550).
    af_outb((af_u16)(port + UART_MCR), 0x0B);

    // Loopback self-test: if the scratch register does not read back what we
    // wrote, there is no UART here and every later write would hang waiting for
    // a transmitter that never drains.
    af_outb((af_u16)(port + UART_SCR), 0xA5);
    if (af_inb((af_u16)(port + UART_SCR)) != 0xA5) {
        s_ready = false;
        return;
    }

    // Enable the FIFOs and disable loopback.
    af_outb((af_u16)(port + UART_MCR), 0x0B);
    s_ready = true;
}

void af_x86_serial_putc(char c)
{
    if (!s_ready) {
        return;
    }

    // Wait for the transmit holding register to drain. Bounded, so a broken
    // UART cannot hang the kernel forever inside a log call.
    af_u32 spins = 100000;
    while (spins-- > 0) {
        if ((af_inb((af_u16)(s_port + UART_LSR)) & UART_LSR_THR_EMPTY) != 0) {
            break;
        }
    }

    af_outb((af_u16)(s_port + UART_DATA), (af_u8)c);
}

void af_x86_serial_write(const char *text, af_size len)
{
    if (text == NULL) {
        return;
    }
    for (af_size i = 0; i < len; i++) {
        // Translate a bare newline into CRLF so terminals render it correctly.
        if (text[i] == '\n') {
            af_x86_serial_putc('\r');
        }
        af_x86_serial_putc(text[i]);
    }
}
