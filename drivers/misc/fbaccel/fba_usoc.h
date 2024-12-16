/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/*
 * fbaccel subdevice driver that exports UltraSoC registers
 * mapped over PCI to a userspace UltraSoC proxy.
 */

int __init fba_usoc_driver_init(void);
void fba_usoc_driver_exit(void);

struct fba_usoc_dev;

struct fba_usoc_dev *
fba_usoc_register(struct pci_dev *pdev, struct device *parent, int minor,
		  const char *name, uint64_t offset, int id, void __iomem *regs,
		  unsigned long vm_pgoff, int msix_irq_ds, int msix_irq_us);
void fba_usoc_unregister(struct fba_usoc_dev *dev);
