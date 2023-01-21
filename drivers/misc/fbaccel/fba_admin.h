/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/*
 * FBA Admin
 */

int fba_admin_create_context(struct fba_device *dev, int id);
int fba_admin_delete_context_begin(struct fba_device *dev, int id);
int fba_admin_delete_context_end(struct fba_device *dev, int id);

int fba_admin_get_cfg_data(struct fba_device *fba_dev, dma_addr_t addr,
			   uint16_t opcode, uint64_t start, uint64_t end);

int fba_admin_dma_from_device(struct fba_device *fba_dev, uint16_t opcode,
			      void *dst, uint32_t size);

int fba_admin_dma_to_device(struct fba_device *fba_dev, uint16_t opcode,
			    void *dst, uint32_t size);

// Set CLOCK_REALTIME on device
int fba_admin_time_set(struct fba_device *fba_dev, int64_t time);

bool fba_admin_poll_channel(void *obj);
