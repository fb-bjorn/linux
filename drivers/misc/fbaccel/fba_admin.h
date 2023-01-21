/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once

/*
 * FBA Admin
 */

int fba_admin_get_cfg_data(struct fba_device *fba_dev, dma_addr_t addr,
			   uint16_t opcode, uint64_t start, uint64_t end);
