/*
<<<<<<< HEAD
 * Copyright (c) 2014 NVIDIA Corporation. All rights reserved.
=======
 * Copyright (C) 2014 NVIDIA Corporation
>>>>>>> e45edb177253... memory: tegra: Add EMC (external memory controller) driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SOC_TEGRA_EMC_H__
#define __SOC_TEGRA_EMC_H__

struct tegra_emc;

#ifdef CONFIG_TEGRA124_EMC
int tegra_emc_prepare_timing_change(struct tegra_emc *emc,
				    unsigned long rate);
void tegra_emc_complete_timing_change(struct tegra_emc *emc,
				      unsigned long rate);
#else
static inline int tegra_emc_prepare_timing_change(struct tegra_emc *emc,
						  unsigned long rate)
{
	return 0;
}

static inline void tegra_emc_complete_timing_change(struct tegra_emc *emc,
						    unsigned long rate)
{
}
#endif

#endif /* __SOC_TEGRA_EMC_H__ */
