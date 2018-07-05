// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * OMAP hardware spinlock test driver
 *
 * Copyright (C) 2014-2018 Texas Instruments Incorporated - http://www.ti.com
 *	Suman Anna <s-anna@ti.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <linux/hwspinlock.h>

/* load-time options */
static int count = 2;
module_param(count, int, 0444);

struct hwspinlock_data {
	const char *compatible;
	const unsigned int max_locks;
};

static int hwspin_lock_test(struct hwspinlock *hwlock)
{
	int i;
	int ret;

	pr_err("\nTesting lock %d\n", hwspin_lock_get_id(hwlock));
	for (i = 0; i < count; i++) {
		ret = hwspin_trylock(hwlock);
		if (ret) {
			pr_err("%s: Initial lock failed\n", __func__);
			return -EFAULT;
		}
		pr_err("trylock #1 status[%d] = %d\n", i, ret);

		/* Verify lock actually works - re-acquiring it should fail */
		ret = hwspin_trylock(hwlock);
		pr_err("trylock #2 status[%d] = %d\n", i, ret);
		if (!ret) {
			/* Keep locks balanced even in failure cases */
			hwspin_unlock(hwlock);
			hwspin_unlock(hwlock);
			pr_err("%s: Recursive lock succeeded unexpectedly\n",
			       __func__);
			return -EFAULT;
		}

		/* Verify unlock by re-acquiring the lock after releasing it */
		hwspin_unlock(hwlock);
		ret = hwspin_trylock(hwlock);
		pr_err("trylock after unlock status[%d] = %d\n", i, ret);
		if (ret) {
			pr_err("%s: Unlock failed\n", __func__);
			return -EINVAL;
		}

		hwspin_unlock(hwlock);
	}

	return 0;
}

static int hwspin_lock_test_all_locks(unsigned int max_locks)
{
	int i;
	int ret = 0, ret1 = 0;
	struct hwspinlock *hwlock = NULL;

	pr_err("Testing %d locks\n", max_locks);
	for (i = 0; i < max_locks; i++) {
		hwlock = hwspin_lock_request_specific(i);
		if (!hwlock) {
			pr_err("request lock %d failed\n", i);
			ret = -EIO;
			continue;
		}

		ret1 = hwspin_lock_test(hwlock);
		if (ret1) {
			pr_err("hwspinlock tests failed on lock %d\n", i);
			ret = ret1;
			goto free_lock;
		}

free_lock:
		ret1 = hwspin_lock_free(hwlock);
		if (ret1) {
			pr_err("hwspin_lock_free failed on lock %d\n", i);
			ret = ret1;
		}
	}

	return ret;
}

static const struct of_device_id omap_hwspinlock_test_of_match[] = {
	{ .compatible = "ti,omap-hwspinlock-test",   },
	{ .compatible = "ti,omap4-hwspinlock-test",  },
	{ .compatible = "ti,omap5-hwspinlock-test",  },
	{ .compatible = "ti,dra7-hwspinlock-test",   },
	{ .compatible = "ti,am33xx-hwspinlock-test", },
	{ .compatible = "ti,am43xx-hwspinlock-test", },
	{ .compatible = "ti,am654-hwspinlock-test",  },
	{ /* end */ },
};

static int hwspin_lock_test_all_phandle_locks(unsigned int max_locks)
{
	struct device_node *np = NULL;
	struct hwspinlock *hwlock = NULL;
	int ret = 0, ret1 = 0;
	unsigned int i;
	int num_locks;
	int hwlock_id;

	np = of_find_matching_node_and_match(NULL,
					     omap_hwspinlock_test_of_match,
					     NULL);
	if (!np) {
		pr_err("\nNo test node provided\n");
		return 0;
	}

	num_locks = of_count_phandle_with_args(np, "hwlocks", "#hwlock-cells");
	pr_err("Number of phandles = %d max_locks = %d\n",
	       num_locks, max_locks);

	for (i = 0; i < num_locks; i++) {
		hwlock_id = of_hwspin_lock_get_id(np, i);
		if (hwlock_id < 0) {
			pr_err("unable to get hwlock_id : %d\n", hwlock_id);
			ret = -EINVAL;
			continue;
		};

		hwlock = hwspin_lock_request_specific(hwlock_id);
		if (!hwlock) {
			pr_err("unable to get hwlock\n");
			ret = -EINVAL;
			continue;
		}

		ret1 = hwspin_lock_test(hwlock);
		if (ret1) {
			pr_err("hwspinlock test failed on DT lock %d, ret = %d\n",
			       hwspin_lock_get_id(hwlock), ret1);
			ret = ret1;
		}

		ret1 = hwspin_lock_free(hwlock);
		if (ret1) {
			pr_err("hwspin_lock_free failed on lock %d\n",
			       hwspin_lock_get_id(hwlock));
			ret = ret1;
		}
	}

	return ret;
}

static
unsigned int omap_hwspinlock_get_locks(const struct hwspinlock_data *data)
{
	unsigned int locks = 0;

	while (data->compatible) {
		if (of_machine_is_compatible(data->compatible)) {
			locks = data->max_locks;
			break;
		}
		data++;
	}

	return locks;
}

static const struct of_device_id omap_hwspinlock_of_match[] = {
	{ .compatible = "ti,omap4-hwspinlock", },
	{ .compatible = "ti,am654-hwspinlock", },
	{ /* end */ },
};

static const struct hwspinlock_data soc_data[] = {
	{ "ti,omap4",	32, },
	{ "ti,omap5",	32, },
	{ "ti,dra7",	256, },
	{ "ti,am33xx",	128, },
	{ "ti,am43",	128, },
	{ "ti,am654",	256, },
	{ "ti,dra822",	256, },
	{ /* sentinel */ },
};

static int omap_hwspinlock_test_init(void)
{
	struct device_node *np;
	unsigned int max_locks;
	int ret;

	pr_err("\n** HwSpinLock Unit Test Module initiated **\n");

	max_locks = omap_hwspinlock_get_locks(soc_data);
	if (!max_locks) {
		pr_err("\nNot a compatible platform\n");
		return -ENODEV;
	}

	np = of_find_matching_node_and_match(NULL, omap_hwspinlock_of_match,
					     NULL);
	if (!np || !of_device_is_available(np)) {
		pr_err("\nNo HwSpinlock node provided/enabled\n");
		return -ENODEV;
	}

	pr_err("\n***** Begin - Test All Locks ****\n");
	ret = hwspin_lock_test_all_locks(max_locks);
	if (ret)
		pr_err("hwspin_lock_test_all_locks failed, ret = %d\n", ret);
	pr_err("\n***** End - Test All Locks ****\n");

	pr_err("\n***** Begin - Test All pHandle Locks ****\n");
	ret = hwspin_lock_test_all_phandle_locks(max_locks);
	if (ret)
		pr_err("hwspin_lock_test_all_locks failed, ret = %d\n", ret);
	pr_err("\n***** End - Test All pHandle Locks ****\n");

	return 0;
}

static void omap_hwspinlock_test_exit(void)
{
	pr_err("\n** HwSpinLock Unit Test Module finished **\n");
}

module_init(omap_hwspinlock_test_init);
module_exit(omap_hwspinlock_test_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Hardware spinlock Test driver for TI SoCs");
MODULE_AUTHOR("Suman Anna <s-anna@ti.com>");
