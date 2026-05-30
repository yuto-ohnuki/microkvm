#ifndef UART_H
#define UART_H

#include <stdint.h>

#define UART_BASE       0x3F8
#define UART_THR        0
#define UART_RBR        0
#define UART_IER        1
#define UART_IIR        2
#define UART_LCR        3
#define UART_MCR        4
#define UART_LSR        5

#define UART_LCR_DLAB   0x80
#define UART_IER_RDI    0x01
#define UART_IER_THRI   0x02
#define UART_IIR_NO_INT 0x01
#define UART_IIR_THRI   0x02
#define UART_IIR_RDI    0x04
#define UART_LSR_DR     0x01
#define UART_LSR_THRE   0x20
#define UART_LSR_TEMT   0x40

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