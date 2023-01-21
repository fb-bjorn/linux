// SPDX-License-Identifier: GPL-2.0
/*
 * FBA UART driver
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates
 *
 * Based originally on SiFive UART driver (sifive.c):
 * Copyright (C) 2018 Paul Walmsley <paul@pwsan.com>
 * Copyright (C) 2018-2019 SiFive
 *
 * Which states it was based partially on:
 * - drivers/tty/serial/pxa.c
 * - drivers/tty/serial/amba-pl011.c
 * - drivers/tty/serial/uartlite.c
 * - drivers/tty/serial/omap-serial.c
 * - drivers/pwm/pwm-sifive.c
 */
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/tty_flip.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/nospec.h>

#include "fba_device.h"
#include "fba_util.h"
#include "fba_uart.h"

/* Register offsets, in multiples of mailbox stride */
#define FBA_UART_TX_CTRL_OFFS 0
#define FBA_UART_RX_CTRL_OFFS 1
#define FBA_UART_IEN_OFFS 2
#define FBA_UART_REMOTE_CTRL_OFFS 3 // remote tail + head
#define FBA_UART_TX_FIFO_OFFS 4
#define FBA_UART_RX_FIFO_OFFS 0x12

/* FBA_UART_MAX_PORTS: maximum number of FBA UARTs */
#define FBA_UART_MAX_PORTS 16

/* FBA_UART_NAME: our driver's name that we pass to the operating system */
#define FBA_UART_NAME "fba_uart"

/* FBA_UART_TTY_PREFIX: tty name prefix for FBA serial ports */
#define FBA_UART_TTY_PREFIX "ttyFBA"

/* FBA_UART_TX_FIFO_DEPTH: depth of the TX FIFO (in bytes) */
#define FBA_UART_TX_FIFO_DEPTH 14

/* FBA_UART_RX_FIFO_DEPTH: depth of the RX FIFO (in bytes) */
#define FBA_UART_RX_FIFO_DEPTH FBA_UART_TX_FIFO_DEPTH

#define FBA_UART_REMOTE_TAIL(x) (((x) >> 16) & 0xFFFF)
#define FBA_UART_REMOTE_HEAD(x) ((x)&0xFFFF)

#define FBA_UART_TX_IEN 1
#define FBA_UART_RX_IEN 2
#define FBA_UART_DEV_ENABLE 4

/**
 * fba_uart_dev - driver-specific data extension to struct uart_port
 */
struct fba_uart_dev {
	struct fba_device *fba;
	struct uart_port port;
	struct device *dev;
	int mbox_start;
	unsigned char ier;
	int minor;
	int id;
	int msi_irq_rx;
	int msi_irq_tx;
	uint16_t tx_tail;
	uint16_t rx_head;
};

#define port_to_fba_uart_dev(p) (container_of((p), struct fba_uart_dev, port))

static void fba_uart_stop_tx(struct uart_port *port);

static inline void fbau_write_mbox(struct fba_uart_dev *fbau, u32 val, u32 mbox)
{
	set_mailbox(fbau->fba, fbau->mbox_start + mbox, val);
}

static inline u32 fbau_read_mbox(struct fba_uart_dev *fbau, u32 mbox)
{
	return get_mailbox(fbau->fba, fbau->mbox_start + mbox);
}

static inline bool fba_uart_tx_full(struct fba_uart_dev *fbau)
{
	u32 val = fbau_read_mbox(fbau, FBA_UART_REMOTE_CTRL_OFFS);

	return (((fbau->tx_tail + 1) % FBA_UART_TX_FIFO_DEPTH) ==
		FBA_UART_REMOTE_HEAD(val));
}

static inline bool fba_uart_rx_empty(struct fba_uart_dev *fbau)
{
	u32 val = fbau_read_mbox(fbau, FBA_UART_REMOTE_CTRL_OFFS);

	return (FBA_UART_REMOTE_TAIL(val) == fbau->rx_head);
}

/**
 * fbau_transmit_chars() - enqueue multiple bytes onto the TX FIFO
 * @fbau: pointer to a struct fba_uart_dev
 *
 * Transfer up to a TX FIFO size's worth of characters from the Linux serial
 * transmit buffer to the device's UART TX FIFO.
 */
static void fbau_transmit_chars(struct fba_uart_dev *fbau)
{
	struct circ_buf *xmit = &fbau->port.state->xmit;
	int count;

	if (fbau->port.x_char) {
		fbau->port.x_char = 0;
		return;
	}

	if (uart_circ_empty(xmit) || uart_tx_stopped(&fbau->port))
		return;

	count = 0;
	while (!fba_uart_tx_full(fbau)) {
		fbau_write_mbox(fbau, xmit->buf[xmit->tail],
				FBA_UART_TX_FIFO_OFFS + fbau->tx_tail);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		fbau->tx_tail = (fbau->tx_tail + 1) % FBA_UART_TX_FIFO_DEPTH;
		fbau->port.icount.tx++;
		count++;
		if (uart_circ_empty(xmit))
			break;
	}

	if (count)
		fbau_write_mbox(fbau, fbau->tx_tail, FBA_UART_TX_CTRL_OFFS);

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(&fbau->port);
}

/**
 * fbau_enable_device() - enable device
 * @fbau: pointer to a struct fba_uart_dev
 */
static void fbau_enable_device(struct fba_uart_dev *fbau)
{
	fbau->ier |= FBA_UART_DEV_ENABLE;
	fbau_write_mbox(fbau, fbau->ier, FBA_UART_IEN_OFFS);
}

/**
 * fbau_disable_device() - disable device
 * @fbau: pointer to a struct fba_uart_dev
 */
static void fbau_disable_device(struct fba_uart_dev *fbau)
{
	fbau->ier &= ~FBA_UART_DEV_ENABLE;
	fbau_write_mbox(fbau, fbau->ier, FBA_UART_IEN_OFFS);
}

/**
 * fbau_enable_txint() - enable transmit not full interrupts
 * @fbau: pointer to a struct fba_uart_dev
 *
 * Enable interrupt generation when the transmit FIFO has space
 * on the FBA UART referred to by @fbau.
 */
static void fbau_enable_txint(struct fba_uart_dev *fbau)
{
	if (fbau->ier & FBA_UART_TX_IEN)
		return;

	fbau->ier |= FBA_UART_TX_IEN;
	fbau_write_mbox(fbau, fbau->ier, FBA_UART_IEN_OFFS);
}

/**
 * fbau_enable_rxint() - enable receive interrupts
 * @fbau: pointer to a struct fba_uart_dev
 *
 * Enable interrupt generation when the receive FIFO is not empty
 * on the FBA UART referred to by @fbau.
 */
static void fbau_enable_rxint(struct fba_uart_dev *fbau)
{
	if (fbau->ier & FBA_UART_RX_IEN)
		return;

	fbau->ier |= FBA_UART_RX_IEN;
	fbau_write_mbox(fbau, fbau->ier, FBA_UART_IEN_OFFS);
}

/**
 * fbau_disable_txint() - disable transmit not full interrupts
 * @fbau: pointer to a struct fba_uart_dev
 */
static void fbau_disable_txint(struct fba_uart_dev *fbau)
{
	if (!(fbau->ier & FBA_UART_TX_IEN))
		return;

	fbau->ier &= ~FBA_UART_TX_IEN;
	fbau_write_mbox(fbau, fbau->ier, FBA_UART_IEN_OFFS);
}

/**
 * fbau_disable_rxint() - disable receive not empty interrupts
 * @fbau: pointer to a struct fba_uart_dev
 */
static void fbau_disable_rxint(struct fba_uart_dev *fbau)
{
	if (!(fbau->ier & FBA_UART_RX_IEN))
		return;

	fbau->ier &= ~FBA_UART_RX_IEN;
	fbau_write_mbox(fbau, fbau->ier, FBA_UART_IEN_OFFS);
}

/**
 * fbau_receive_chars() - receive multiple bytes from the UART
 * @fbau: pointer to a struct fba_uart_dev
 *
 * Receive up to an RX FIFO's worth of bytes from the FBA UART referred
 * to by @fbau and pass them up to the Linux serial layer.
 *
 * Context: Expects fbau->port.lock to be held by caller.
 */
static void fbau_receive_chars(struct fba_uart_dev *fbau)
{
	unsigned char ch;
	int i;

	i = 0;
	while (!fba_uart_rx_empty(fbau)) {
		ch = fbau_read_mbox(fbau,
				    FBA_UART_RX_FIFO_OFFS + fbau->rx_head);
		fbau->port.icount.rx++;
		uart_insert_char(&fbau->port, 0, 0, ch, TTY_NORMAL);
		fbau->rx_head = (fbau->rx_head + 1) % FBA_UART_RX_FIFO_DEPTH;
		i++;
	}

	if (i)
		fbau_write_mbox(fbau, fbau->rx_head, FBA_UART_RX_CTRL_OFFS);

	spin_unlock(&fbau->port.lock);
	tty_flip_buffer_push(&fbau->port.state->port);
	spin_lock(&fbau->port.lock);
}

/*
 * Linux serial API functions
 */

static void fba_uart_stop_tx(struct uart_port *port)
{
	struct fba_uart_dev *fbau = port_to_fba_uart_dev(port);

	fbau_disable_txint(fbau);
}

static void fba_uart_stop_rx(struct uart_port *port)
{
	struct fba_uart_dev *fbau = port_to_fba_uart_dev(port);

	fbau_disable_rxint(fbau);
}

static void fba_uart_start_tx(struct uart_port *port)
{
	struct fba_uart_dev *fbau = port_to_fba_uart_dev(port);

	fbau_transmit_chars(fbau);

	fbau_enable_txint(fbau);
}

static irqreturn_t fba_uart_pci_tx_isr(int irq, void *dev_id)
{
	struct fba_uart_dev *fbau = dev_id;

	spin_lock(&fbau->port.lock);
	fbau_transmit_chars(fbau);
	spin_unlock(&fbau->port.lock);

	return IRQ_HANDLED;
}

static irqreturn_t fba_uart_pci_rx_isr(int irq, void *dev_id)
{
	struct fba_uart_dev *fbau = dev_id;

	spin_lock(&fbau->port.lock);
	fbau_receive_chars(fbau);
	spin_unlock(&fbau->port.lock);

	return IRQ_HANDLED;
}

static unsigned int fba_uart_tx_empty(struct uart_port *port)
{
	return TIOCSER_TEMT;
}

static unsigned int fba_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_CTS | TIOCM_DSR;
}

static void fba_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static int fba_uart_startup(struct uart_port *port)
{
	struct fba_uart_dev *fbau = port_to_fba_uart_dev(port);

	fbau_enable_rxint(fbau);
	fbau_enable_device(fbau);

	return 0;
}

static void fba_uart_shutdown(struct uart_port *port)
{
	struct fba_uart_dev *fbau = port_to_fba_uart_dev(port);

	fbau_disable_device(fbau);
	fbau_disable_rxint(fbau);
	fbau_disable_txint(fbau);
}

static void fba_uart_set_termios(struct uart_port *port,
				 struct ktermios *termios, struct ktermios *old)
{
	struct fba_uart_dev *fbau = port_to_fba_uart_dev(port);
	unsigned long flags;

	if ((termios->c_cflag & CSIZE) != CS8)
		dev_err_once(fbau->port.dev, "only 8-bit words supported\n");
	if (termios->c_iflag & (INPCK | PARMRK))
		dev_err_once(fbau->port.dev, "parity checking not supported\n");
	if (termios->c_iflag & BRKINT)
		dev_err_once(fbau->port.dev, "BREAK detection not supported\n");

	spin_lock_irqsave(&fbau->port.lock, flags);
	fbau->port.read_status_mask = 0;
	spin_unlock_irqrestore(&fbau->port.lock, flags);
}

static void fba_uart_config_port(struct uart_port *port, int flags)
{
	struct fba_uart_dev *fbau = port_to_fba_uart_dev(port);

	if (flags & UART_CONFIG_TYPE)
		fbau->port.type = PORT_FBA_UART;
}

static int fba_uart_verify_port(struct uart_port *port,
				struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_FBA_UART)
		return -EINVAL;

	return 0;
}

static const char *fba_uart_type(struct uart_port *port)
{
	return port->type == PORT_FBA_UART ? "fba_uart" : NULL;
}

static struct uart_driver fba_uart_uart_driver;

static const struct uart_ops fba_uart_uops = {
	.tx_empty = fba_uart_tx_empty,
	.set_mctrl = fba_uart_set_mctrl,
	.get_mctrl = fba_uart_get_mctrl,
	.stop_tx = fba_uart_stop_tx,
	.start_tx = fba_uart_start_tx,
	.stop_rx = fba_uart_stop_rx,
	.startup = fba_uart_startup,
	.shutdown = fba_uart_shutdown,
	.set_termios = fba_uart_set_termios,
	.type = fba_uart_type,
	.config_port = fba_uart_config_port,
	.verify_port = fba_uart_verify_port,
};

static struct uart_driver fba_uart_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = FBA_UART_NAME,
	.dev_name = FBA_UART_TTY_PREFIX,
	.nr = FBA_UART_MAX_PORTS,
};

struct fba_uart_dev *fba_uart_register(struct fba_device *fba,
				       struct device *parent, int minor,
				       const char *name, int id, int mbox_start,
				       int msi_irq_rx, int msi_irq_tx)
{
	struct fba_uart_dev *fbau;
	struct pci_dev *pdev = fba->pdev;
	int ret;

	fbau = devm_kzalloc(&pdev->dev, sizeof(*fbau), GFP_KERNEL);
	if (!fbau)
		return ERR_PTR(-ENOMEM);

	fbau->dev = get_device(&pdev->dev);

	fbau->mbox_start = mbox_start;

	fbau->minor = minor;
	fbau->id = id;
	fbau->fba = fba;
	fbau->msi_irq_rx = msi_irq_rx;
	fbau->msi_irq_tx = msi_irq_tx;

	fbau->port.dev = parent;
	fbau->port.type = PORT_FBA_UART;
	fbau->port.iotype = UPIO_MEM;
	fbau->port.fifosize = FBA_UART_TX_FIFO_DEPTH;
	fbau->port.ops = &fba_uart_uops;
	fbau->port.line = fbau->minor;

	ret = fba_request_irq(pdev, msi_irq_tx, fba_uart_pci_tx_isr, NULL, fbau,
			      "%s%d-uart-tx", name, fbau->id);
	if (ret) {
		dev_err(&pdev->dev, "fba_request_irq failed for tx irq\n");
		goto err_put_device;
	}

	ret = fba_request_irq(pdev, msi_irq_rx, fba_uart_pci_rx_isr, NULL, fbau,
			      "%s%d-uart-rx", name, fbau->id);
	if (ret) {
		dev_err(&pdev->dev, "fba_request_irq failed for rx irq\n");
		goto err_free_irqs;
	}

	ret = uart_add_one_port(&fba_uart_uart_driver, &fbau->port);
	if (ret != 0) {
		dev_err(&pdev->dev, "could not add uart: %d\n", ret);
		goto err_free_irqs;
	}

	return fbau;

err_free_irqs:
	fba_free_irq(pdev, fbau->msi_irq_rx, fbau);
	fba_free_irq(pdev, fbau->msi_irq_tx, fbau);
err_put_device:
	put_device(fbau->dev);
	devm_kfree(&pdev->dev, fbau);
	return ERR_PTR(ret);
}

void fba_uart_unregister(struct fba_uart_dev *fbau)
{
	uart_remove_one_port(&fba_uart_uart_driver, &fbau->port);

	// disable device interrupts
	fbau_write_mbox(fbau, 0, FBA_UART_IEN_OFFS);

	fba_free_irq(fbau->fba->pdev, fbau->msi_irq_rx, fbau);
	fba_free_irq(fbau->fba->pdev, fbau->msi_irq_tx, fbau);

	put_device(fbau->dev);
}

int __init fba_uart_driver_init(void)
{
	return uart_register_driver(&fba_uart_uart_driver);
}

void fba_uart_driver_exit(void)
{
	uart_unregister_driver(&fba_uart_uart_driver);
}
