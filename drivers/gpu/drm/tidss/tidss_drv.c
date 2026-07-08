// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#include <linux/console.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/rcupdate.h>
#include <linux/aperture.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_module.h>
#include <drm/drm_probe_helper.h>

#include "tidss_dispc.h"
#include "tidss_drv.h"
#include "tidss_kms.h"
#include "tidss_irq.h"
#include "tidss_oldi.h"

/* Power management */

int tidss_runtime_get(struct tidss_device *tidss)
{
	int r;

	dev_dbg(tidss->dev, "%s\n", __func__);

	/* No PM in display sharing mode */
	if (tidss->shared_mode)
		return 0;

	r = pm_runtime_resume_and_get(tidss->dev);
	if (WARN_ON(r < 0))
		return r;

	if (tidss->boot_enabled_vp_mask) {
		/*
		 * If 'boot_enabled_vp_mask' is set, it means that the DSS is
		 * enabled and bootloader splash-screen is still on the screen,
		 * using bootloader's DSS HW config.
		 *
		 * This is the first time the driver is about to use the HW, and
		 * we need to do some cleanup and initial setup.
		 */
		dispc_splash_fini(tidss->dispc);
	}

	return 0;
}

void tidss_runtime_put(struct tidss_device *tidss)
{
	int r;

	dev_dbg(tidss->dev, "%s\n", __func__);

	if (tidss->shared_mode)
		return;

	r = pm_runtime_put_autosuspend(tidss->dev);
	WARN_ON(r < 0);
}

static bool tidss_any_vp_always_on(struct tidss_device *tidss)
{
	unsigned int i;

	for (i = 0; i < TIDSS_MAX_PORTS; i++)
		if (tidss->always_on_pd[i])
			return true;
	return false;
}

/* Return the TI DM device ID from the first cell of power-domains phandle */
static u32 tidss_aod_ti_dev_id(struct device *dev)
{
	struct of_phandle_args args;
	u32 id = 0;

	if (!of_parse_phandle_with_args(dev->of_node, "power-domains",
					"#power-domain-cells", 0, &args)) {
		if (args.args_count >= 1)
			id = args.args[0];
		of_node_put(args.np);
	}
	return id;
}

/*
 * Collect bridge + supplier devices for a VP on first AOD enable.
 * Deferred from probe so that all device links (PHY etc.) exist.
 */
static void tidss_aod_collect_bridge_devs(struct tidss_device *tidss,
					  u32 hw_videoport)
{
	struct device_node *np = tidss->aod_bridge_of_node[hw_videoport];
	struct platform_device *bpdev;
	struct device_link *dl;

	if (!np)
		return;

	bpdev = of_find_device_by_node(np);
	if (!bpdev)
		return;

	tidss->aod_bridge_devs[hw_videoport][tidss->aod_num_bridge_devs[hw_videoport]++] =
		&bpdev->dev;
	dev_dbg(tidss->dev, "vp%u: AOD bridge dev: %s\n",
		hw_videoport, dev_name(&bpdev->dev));

	rcu_read_lock();
	list_for_each_entry_rcu(dl, &bpdev->dev.links.suppliers, c_node) {
		if (tidss->aod_num_bridge_devs[hw_videoport] >=
		    ARRAY_SIZE(tidss->aod_bridge_devs[hw_videoport]))
			break;
		tidss->aod_bridge_devs[hw_videoport][tidss->aod_num_bridge_devs[hw_videoport]++] =
			get_device(dl->supplier);
		dev_dbg(tidss->dev, "vp%u: AOD supplier dev: %s\n",
			hw_videoport, dev_name(dl->supplier));
	}
	rcu_read_unlock();
}

/*
 * Hold or release PM references and GENPD_FLAG_ALWAYS_ON for the display
 * pipeline (TIDSS + bridges/PHYs) associated with @hw_videoport.
 */
void tidss_aod_set_genpd_always_on(struct tidss_device *tidss,
				   u32 hw_videoport, bool on)
{
	unsigned int i;
	int ret;

	if (tidss->always_on_pd[hw_videoport] == on)
		return;

	/* Collect bridge devices on first AOD enable for this VP */
	if (on && !tidss->aod_bridge_devs_collected[hw_videoport]) {
		tidss->aod_bridge_devs_collected[hw_videoport] = true;
		tidss_aod_collect_bridge_devs(tidss, hw_videoport);
	}

	/* TIDSS PM hold shared across VPs: take on first, release on last */
	if (on && !tidss_any_vp_always_on(tidss))
		pm_runtime_get_noresume(tidss->dev);

	tidss->always_on_pd[hw_videoport] = on;

	if (!on && !tidss_any_vp_always_on(tidss))
		pm_runtime_put_noidle(tidss->dev);

	ret = dev_pm_genpd_set_always_on(tidss->dev, on);
	dev_dbg(tidss->dev,
		"always-on-display: TIDSS (TI DM id=%u) rpm_hold -> %s, genpd always-on -> %s%s\n",
		tidss_aod_ti_dev_id(tidss->dev),
		on ? "ON" : "OFF",
		on ? "ON" : "OFF",
		ret ? " (no genpd)" : "");

	for (i = 0; i < tidss->aod_num_bridge_devs[hw_videoport]; i++) {
		struct device *d = tidss->aod_bridge_devs[hw_videoport][i];
		struct device *genpd_dev = d;

		if (on)
			pm_runtime_get_noresume(d);
		else
			pm_runtime_put_noidle(d);

		/* Try parent if device has no genpd (e.g. PHY child device) */
		ret = dev_pm_genpd_set_always_on(genpd_dev, on);
		if (ret == -ENODEV && genpd_dev->parent) {
			genpd_dev = genpd_dev->parent;
			ret = dev_pm_genpd_set_always_on(genpd_dev, on);
		}

		dev_dbg(tidss->dev,
			"always-on-display: vp%u %s (TI DM id=%u) rpm_hold -> %s, genpd always-on -> %s%s%s\n",
			hw_videoport, dev_name(d), tidss_aod_ti_dev_id(d),
			on ? "ON" : "OFF",
			on ? "ON" : "OFF",
			ret ? " (no genpd)" : "",
			genpd_dev != d ? " (via parent)" : "");
	}
}

static int __maybe_unused tidss_pm_runtime_suspend(struct device *dev)
{
	struct tidss_device *tidss = dev_get_drvdata(dev);

	dev_dbg(dev, "%s\n", __func__);

	return dispc_runtime_suspend(tidss->dispc);
}

static int __maybe_unused tidss_pm_runtime_resume(struct device *dev)
{
	struct tidss_device *tidss = dev_get_drvdata(dev);
	int r;

	dev_dbg(dev, "%s\n", __func__);

	r = dispc_runtime_resume(tidss->dispc);
	if (r)
		return r;

	return 0;
}

static int __maybe_unused tidss_suspend(struct device *dev)
{
	struct tidss_device *tidss = dev_get_drvdata(dev);

	dev_dbg(dev, "%s\n", __func__);

	/* Skip suspend when ALWAYS_ON_DISPLAY is active */
	if (tidss_any_vp_always_on(tidss)) {
		dev_dbg(dev, "%s: skipped (always_on_pd active)\n", __func__);
		return 0;
	}

	return drm_mode_config_helper_suspend(&tidss->ddev);
}

static int __maybe_unused tidss_resume(struct device *dev)
{
	struct tidss_device *tidss = dev_get_drvdata(dev);

	dev_dbg(dev, "%s\n", __func__);

	/* Matching skip for resume when suspend was skipped */
	if (tidss_any_vp_always_on(tidss)) {
		dev_dbg(dev, "%s: skipped (always_on_pd active)\n", __func__);
		return 0;
	}

	return drm_mode_config_helper_resume(&tidss->ddev);
}

static __maybe_unused const struct dev_pm_ops tidss_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(tidss_suspend, tidss_resume)
	SET_RUNTIME_PM_OPS(tidss_pm_runtime_suspend, tidss_pm_runtime_resume, NULL)
};

/* DRM device Information */

static void tidss_release(struct drm_device *ddev)
{
	drm_kms_helper_poll_fini(ddev);
}

DEFINE_DRM_GEM_DMA_FOPS(tidss_fops);

static const struct drm_driver tidss_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &tidss_fops,
	.release		= tidss_release,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= "tidss",
	.desc			= "TI Keystone DSS",
	.major			= 1,
	.minor			= 0,
};

static int tidss_detach_pm_domains(struct tidss_device *tidss)
{
	int i;

	if (tidss->num_domains <= 1)
		return 0;

	for (i = 0; i < tidss->num_domains; i++) {
		if (tidss->pd_link[i] && !IS_ERR(tidss->pd_link[i]))
			device_link_del(tidss->pd_link[i]);
		if (tidss->pd_dev[i] && !IS_ERR(tidss->pd_dev[i]))
			dev_pm_domain_detach(tidss->pd_dev[i], true);
		tidss->pd_dev[i] = NULL;
		tidss->pd_link[i] = NULL;
	}

	return 0;
}

static int tidss_attach_pm_domains(struct tidss_device *tidss)
{
	struct device *dev = tidss->dev;
	int i;
	int ret;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *np = pdev->dev.of_node;

	tidss->num_domains = of_count_phandle_with_args(np, "power-domains",
							"#power-domain-cells");
	if (tidss->num_domains <= 1) {
		dev_dbg(dev, "One or less power domains, no need to do attach domains\n");
		return 0;
	}

	tidss->pd_dev = devm_kmalloc_array(dev, tidss->num_domains,
					   sizeof(*tidss->pd_dev), GFP_KERNEL);
	if (!tidss->pd_dev)
		return -ENOMEM;

	tidss->pd_link = devm_kmalloc_array(dev, tidss->num_domains,
					    sizeof(*tidss->pd_link), GFP_KERNEL);
	if (!tidss->pd_link)
		return -ENOMEM;

	for (i = 0; i < tidss->num_domains; i++) {
		tidss->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR(tidss->pd_dev[i])) {
			ret = PTR_ERR(tidss->pd_dev[i]);
			goto fail;
		}

		tidss->pd_link[i] = device_link_add(dev, tidss->pd_dev[i],
						    DL_FLAG_STATELESS |
						    DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!tidss->pd_link[i]) {
			ret = -EINVAL;
			goto fail;
		}
	}

	return 0;
fail:
	tidss_detach_pm_domains(tidss);
	return ret;
}

static void check_for_simplefb_device(struct tidss_device *tidss)
{
	if (IS_ENABLED(CONFIG_FB_SIMPLE)) {
		struct device *simplefb_dev;
		struct device_node *simplefb_node;

		simplefb_node = of_find_compatible_node(NULL, NULL, "simple-framebuffer");
		if (!simplefb_node)
			return;

		simplefb_dev = bus_find_device_by_of_node(&platform_bus_type, simplefb_node);
		if (!simplefb_dev) {
			of_node_put(simplefb_node);
			return;
		}

		tidss->simplefb_enabled = true;
		dev_dbg(tidss->dev, "simple-framebuffer detected\n");
		put_device(simplefb_dev);
		of_node_put(simplefb_node);
	}
}

static int tidss_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tidss_device *tidss;
	struct drm_device *ddev;
	int ret;
	int irq;

	dev_dbg(dev, "%s\n", __func__);

	tidss = devm_drm_dev_alloc(&pdev->dev, &tidss_driver,
				   struct tidss_device, ddev);
	if (IS_ERR(tidss))
		return PTR_ERR(tidss);

	ddev = &tidss->ddev;

	tidss->dev = dev;
	tidss->feat = of_device_get_match_data(dev);

	platform_set_drvdata(pdev, tidss);

	spin_lock_init(&tidss->irq_lock);

	tidss->shared_mode = device_property_read_bool(dev, "ti,dss-shared-mode");
	if (!tidss->shared_mode) {
		/* powering up associated OLDI domains */
		ret = tidss_attach_pm_domains(tidss);
		if (ret < 0) {
			dev_err(dev, "failed to attach power domains %d\n", ret);
			goto err_detach_pm_domains;
		}
	}

	ret = dispc_init(tidss);
	if (ret) {
		dev_err(dev, "failed to initialize dispc: %d\n", ret);
		goto err_detach_pm_domains;
	}

	check_for_simplefb_device(tidss);

	ret = tidss_oldi_init(tidss);
	if (ret) {
		dev_dbg(dev, "failed to init OLDI: %d\n", ret);
		goto err_oldi_deinit;
	}

	if (!tidss->shared_mode) {
		pm_runtime_enable(dev);
		pm_runtime_set_autosuspend_delay(dev, 1000);
		pm_runtime_use_autosuspend(dev);
#ifndef CONFIG_PM
		/* If we don't have PM, we need to call resume manually */
		dispc_runtime_resume(tidss->dispc);
#endif
	}

	ret = tidss_modeset_init(tidss);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to init DRM/KMS (%d)\n", ret);
		goto err_runtime_suspend;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_runtime_suspend;
	}
	tidss->irq = irq;

	ret = tidss_irq_install(ddev, irq);
	if (ret) {
		dev_err(dev, "tidss_irq_install failed: %d\n", ret);
		goto err_runtime_suspend;
	}

	drm_kms_helper_poll_init(ddev);

	drm_mode_config_reset(ddev);

	ret = drm_dev_register(ddev, 0);
	if (ret) {
		dev_err(dev, "failed to register DRM device\n");
		goto err_irq_uninstall;
	}

	/* Remove possible early fb before setting up the fbdev */
	ret = aperture_remove_all_conflicting_devices(tidss_driver.name);
	if (ret)
		goto err_drm_dev_unreg;

	drm_client_setup(ddev, NULL);

	dev_dbg(dev, "%s done\n", __func__);

	return 0;

err_drm_dev_unreg:
	drm_dev_unregister(ddev);

err_irq_uninstall:
	tidss_irq_uninstall(ddev);

err_runtime_suspend:
	if (tidss->shared_mode)
		return ret;
#ifndef CONFIG_PM
	dispc_runtime_suspend(tidss->dispc);
#endif
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);

err_oldi_deinit:
	tidss_oldi_deinit(tidss);

err_detach_pm_domains:
	tidss_detach_pm_domains(tidss);

	return ret;
}

static void tidss_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tidss_device *tidss = platform_get_drvdata(pdev);
	struct drm_device *ddev = &tidss->ddev;

	dev_dbg(dev, "%s\n", __func__);

	drm_dev_unregister(ddev);

	drm_atomic_helper_shutdown(ddev);

	tidss_irq_uninstall(ddev);

	if (!tidss->shared_mode) {
#ifndef CONFIG_PM
		/* If we don't have PM, we need to call suspend manually */
		dispc_runtime_suspend(tidss->dispc);
#endif
		pm_runtime_dont_use_autosuspend(dev);
		pm_runtime_disable(dev);
	}

	/* Release per-VP bridge/PHY device references collected since first always-on-display vp*/
	for (int vp = 0; vp < TIDSS_MAX_PORTS; vp++)
		for (int j = 0; j < tidss->aod_num_bridge_devs[vp]; j++)
			put_device(tidss->aod_bridge_devs[vp][j]);

	tidss_oldi_deinit(tidss);

	/* devm allocated dispc goes away with the dev so mark it NULL */
	dispc_remove(tidss);

	tidss_detach_pm_domains(tidss);
	dev_dbg(dev, "%s done\n", __func__);
}

static void tidss_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id tidss_of_table[] = {
	{ .compatible = "ti,k2g-dss", .data = &dispc_k2g_feats, },
	{ .compatible = "ti,am625-dss", .data = &dispc_am625_feats, },
	{ .compatible = "ti,am62a7-dss", .data = &dispc_am62a7_feats, },
	{ .compatible = "ti,am62l-dss", .data = &dispc_am62l_feats, },
	{ .compatible = "ti,am62p-dss", .data = &dispc_am625_feats, },
	{ .compatible = "ti,am65x-dss", .data = &dispc_am65x_feats, },
	{ .compatible = "ti,j721e-dss", .data = &dispc_j721e_feats, },
	{ }
};

MODULE_DEVICE_TABLE(of, tidss_of_table);

static struct platform_driver tidss_platform_driver = {
	.probe		= tidss_probe,
	.remove		= tidss_remove,
	.shutdown	= tidss_shutdown,
	.driver		= {
		.name	= "tidss",
		.pm	= pm_ptr(&tidss_pm_ops),
		.of_match_table = tidss_of_table,
		.suppress_bind_attrs = true,
	},
};

drm_module_platform_driver(tidss_platform_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ti.com>");
MODULE_DESCRIPTION("TI Keystone DSS Driver");
MODULE_LICENSE("GPL v2");
