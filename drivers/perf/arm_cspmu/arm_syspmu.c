// SPDX-License-Identifier: GPL-2.0

#include <linux/io.h>
#include <linux/module.h>

#include "arm_cspmu.h"

#define PMEVFILT3R	0x200

static struct attribute *arm_syspmu_format_attrs[] = {
	ARM_CSPMU_FORMAT_EVENT_ATTR,
	ARM_CSPMU_FORMAT_FILTER_ATTR,
	ARM_CSPMU_FORMAT_FILTER2_ATTR,
	ARM_CSPMU_FORMAT_ATTR(filter3, "config3:0-63"),
	NULL,
};

static struct attribute **arm_syspmu_get_format_attrs(const struct arm_cspmu *cspmu)
{
	return devm_kmemdup(cspmu->dev, arm_syspmu_format_attrs,
			    sizeof(arm_syspmu_format_attrs), GFP_KERNEL);
}

static void arm_syspmu_set_ev_filter(struct arm_cspmu *cspmu,
				     const struct perf_event *event)
{
	u32 offset = 8 * event->hw.idx;

	writeq(event->attr.config1, cspmu->base0 + PMEVFILTR + offset);
	writeq(event->attr.config2, cspmu->base0 + PMEVFILT2R + offset);
	writeq(event->attr.config3, cspmu->base0 + PMEVFILT3R + offset);
}

static int arm_syspmu_init_ops(struct arm_cspmu *cspmu)
{
	cspmu->impl.ops.set_ev_filter = arm_syspmu_set_ev_filter;
	cspmu->impl.ops.get_format_attrs = arm_syspmu_get_format_attrs;

	return 0;
}

static const struct arm_cspmu_impl_match arm_syspmu_param = {
//FIXME: This does not actually apply to *all* Arm implementations; we need some ProductIDs defined
	.pmiidr_val	= ARM_CSPMU_IMPL_ID_ARM,
	.module		= THIS_MODULE,
	.impl_init_ops	= arm_syspmu_init_ops
};

static int __init arm_syspmu_init(void)
{
	return arm_cspmu_impl_register(&arm_syspmu_param);
}

static void __exit arm_syspmu_exit(void)
{
	arm_cspmu_impl_unregister(&arm_syspmu_param);
}

module_init(arm_syspmu_init);
module_exit(arm_syspmu_exit);

MODULE_DESCRIPTION("Arm System PMU Driver");
MODULE_LICENSE("GPL");
