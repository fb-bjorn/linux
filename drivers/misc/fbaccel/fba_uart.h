/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/*
 * FBA SAIL UART support
 */

int __init fba_uart_driver_init(void);
void fba_uart_driver_exit(void);

struct fba_uart_dev;

struct fba_uart_dev *fba_uart_register(struct fba_device *fba,
				       struct device *parent, int minor,
				       const char *name, int id, int mbox_start,
				       int msi_irq_rx, int msi_irq_tx);
void fba_uart_unregister(struct fba_uart_dev *ssp);
