/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates */
#pragma once
#include "fba_device.h"

/*
 * FBA SAIL logger support
 */
int fba_logger_driver_init(void);
void fba_logger_driver_exit(void);

struct fba_logger_dev;

struct fba_logger_dev *fba_logger_register(struct fba_device *dev);
void fba_logger_unregister(struct fba_logger_dev *dev);
