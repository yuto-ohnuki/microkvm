#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include "uart.h"

/* Raise IRQ 4 (COM1) via in-kernel PIC with edge trigger (level 0→1→0) */
static void inject_irq4(int vmfd) {
    struct kvm_irq_level irq = {
        .irq = 4,
        .level = 1
    };
    ioctl(vmfd, KVM_IRQ_LINE, &irq);
    irq.level = 0;
    ioctl(vmfd, KVM_IRQ_LINE, &irq);
}

/* Raise THRE interrupt if IER allows and not already pending */
static void uart_maybe_raise_thre(struct uart8250 *u, int vmfd) {
    if ((u->ier & UART_IER_THRI) && !u->pending_thre) {
        u->pending_thre = 1;
        u->iir = UART_IIR_THRI;
        inject_irq4(vmfd);
    }
}

/* Initialize UART to idle state (transmitter empty, no pending interrupts) */
void uart_init(struct uart8250 *u) {
    u->iir = UART_IIR_NO_INT;
    u->lsr = UART_LSR_THRE | UART_LSR_TEMT;
}

/* Handle guest write to UART I/O port */
void uart_out(struct uart8250 *u, uint16_t port, uint8_t val, int vmfd) {
    switch (port - UART_BASE) {
    case UART_THR:
        if (u->lcr & UART_LCR_DLAB) {
            u->dll = val;
        } else {
            write(STDOUT_FILENO, &val, 1);
            u->lsr |= UART_LSR_THRE | UART_LSR_TEMT;
            uart_maybe_raise_thre(u, vmfd);
        }
        break;
    case UART_IER:
        if (u->lcr & UART_LCR_DLAB) {
            u->dlm = val;
        } else {
            u->ier = val;
            if (u->lsr & UART_LSR_THRE)
                uart_maybe_raise_thre(u, vmfd);
        }
        break;
    case UART_LCR:
        u->lcr = val;
        break;
    case UART_MCR:
        u->mcr = val;
        break;
    default:
        break;
    }
}

/* Handle guest read from UART I/O port */
uint8_t uart_in(struct uart8250 *u, uint16_t port) {
    switch (port - UART_BASE) {
    case UART_RBR:
        if (u->lcr & UART_LCR_DLAB) return u->dll;
        u->lsr &= ~UART_LSR_DR;
        u->pending_rdi = 0;
        return u->rbr;
    case UART_IER:
        return (u->lcr & UART_LCR_DLAB) ? u->dlm : u->ier;
    case UART_IIR:
        if (u->pending_rdi)
            return UART_IIR_RDI;
        if (u->pending_thre) {
            u->pending_thre = 0;
            return UART_IIR_THRI;
        }
        return UART_IIR_NO_INT;
    case UART_LCR:
        return u->lcr;
    case UART_MCR:
        return u->mcr;
    case UART_LSR:
        return u->lsr | UART_LSR_THRE | UART_LSR_TEMT;
    default:
        return 0;
    }
}

/* Deliver a byte from host to guest (RX path) */
void uart_rx(struct uart8250 *u, uint8_t c, int vmfd) {
    u->rbr = c;
    u->lsr |= UART_LSR_DR;
    if (u->ier & UART_IER_RDI) {
        u->pending_rdi = 1;
        inject_irq4(vmfd);
    }
}