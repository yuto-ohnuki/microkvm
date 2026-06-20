#ifndef UART_H
#define UART_H

#include <stdint.h>

/* 8250 UART register offsets from base address */
#define UART_BASE       0x3F8
#define UART_THR        0       /* Transmit Holding Register (write) */
#define UART_RBR        0       /* Receive Buffer Register (read) */
#define UART_IER        1       /* Interrupt Enable Register */
#define UART_IIR        2       /* Interrupt Identification Register (read) */
#define UART_LCR        3       /* Line Control Register */
#define UART_MCR        4       /* Modem Control Register */
#define UART_LSR        5       /* Line Status Register */

/* LCR flags */
#define UART_LCR_DLAB   0x80    /* Divisor Latch Access Bit */

/* IER flags */
#define UART_IER_RDI    0x01    /* Received Data Available interrupt */
#define UART_IER_THRI   0x02    /* Transmitter Holding Register Empty interrupt */

/* IIR values (identify which interrupt is pending) */
#define UART_IIR_NO_INT 0x01    /* No interrupt pending */
#define UART_IIR_THRI   0x02    /* THR Empty */
#define UART_IIR_RDI    0x04    /* Received Data Available */

/* LSR flags */
#define UART_LSR_DR     0x01    /* Data Ready (RBR has data) */
#define UART_LSR_THRE   0x20    /* THR Empty (ready to accept new data) */
#define UART_LSR_TEMT   0x40    /* Transmitter Empty (shift register empty) */

struct uart8250 {
    uint8_t ier, iir, lcr, mcr, lsr, dll, dlm;
    uint8_t rbr;
    int pending_thre;
    int pending_rdi;
};

void uart_init(struct uart8250 *u);
void uart_out(struct uart8250 *u, uint16_t port, uint8_t val, int vmfd);
uint8_t uart_in(struct uart8250 *u, uint16_t port);
void uart_rx(struct uart8250 *u, uint8_t c, int vmfd);

#endif