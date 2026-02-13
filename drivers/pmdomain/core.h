/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header for PM domain core
 *
 * Copyright (C) 2011 Rafael J. Wysocki <rjw@sisk.pl>, Renesas Electronics Corp.
 */

#ifndef __PM_DOMAIN_CORE_H__
#define __PM_DOMAIN_CORE_H__

#include <linux/pm_domain.h>

int genpd_for_each_child(struct generic_pm_domain *genpd,
			 int (*fn)(struct device *dev, void *data),
			 void *data);

#endif /* __PM_DOMAIN_CORE_H__ */
