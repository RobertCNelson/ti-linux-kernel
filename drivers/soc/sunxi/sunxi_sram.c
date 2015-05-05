/*
 * Allwinner SoCs SRAM Controller Driver
 *
 * Copyright (C) 2015 Maxime Ripard
 *
 * Author: Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <linux/soc/sunxi/sunxi_sram.h>

struct sunxi_sram_func {
	char	*func;
	u8	val;
};

struct sunxi_sram_desc {
	enum sunxi_sram_type	type;
	char			*name;
	u8			reg;
	u8			offset;
	u8			width;
	struct sunxi_sram_func	*func;
	bool			claimed;
	bool			enabled;
};

#define SUNXI_SRAM_MAP(_val, _func)				\
	{							\
		.func = _func,					\
		.val = _val,					\
	}

#define SUNXI_SRAM_DESC(_type, _name, _reg, _off, _width, ...)	\
	{							\
		.type = _type,					\
		.name = _name,					\
		.reg = _reg,					\
		.offset = _off,					\
		.width = _width,				\
		.func = (struct sunxi_sram_func[]){		\
			__VA_ARGS__, { } },			\
	}

struct sunxi_sram_desc sun4i_sram_desc[] = {
	SUNXI_SRAM_DESC(SUNXI_SRAM_EMAC, "A3-A4", 0x4, 0x4, 1,
			SUNXI_SRAM_MAP(0, "cpu"),
			SUNXI_SRAM_MAP(1, "emac")),
	SUNXI_SRAM_DESC(SUNXI_SRAM_USB_OTG, "D", 0x4, 0x0, 1,
			SUNXI_SRAM_MAP(0, "cpu"),
			SUNXI_SRAM_MAP(1, "usb-otg")),
	{ /* Sentinel */ },
};

static struct sunxi_sram_desc *sram_list;
static DEFINE_SPINLOCK(sram_lock);
static void __iomem *base;

static int sunxi_sram_show(struct seq_file *s, void *data)
{
	struct sunxi_sram_desc *sram;
	struct sunxi_sram_func *func;
	u32 val;

	seq_puts(s, "Allwinner sunXi SRAM\n");
	seq_puts(s, "--------------------\n");


	for (sram = sram_list; sram->name; sram++) {
		if (!sram->enabled)
			continue;

		seq_printf(s, "\n%s\n", sram->name);

		val = readl(base + sram->reg);
		val >>= sram->offset;
		val &= sram->width;

		for (func = sram->func; func->func; func++) {
			seq_printf(s, "\t\t%s%c\n", func->func,
				   func->val == val ? '*' : ' ');
		}
	}

	return 0;
}

static int sunxi_sram_open(struct inode *inode, struct file *file)
{
	return single_open(file, sunxi_sram_show, inode->i_private);
}

static const struct file_operations sunxi_sram_fops = {
	.open = sunxi_sram_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int sunxi_sram_claim(enum sunxi_sram_type type, const char *function)
{
	struct sunxi_sram_desc *sram;
	struct sunxi_sram_func *func;
	u32 val;

	if (IS_ERR(base))
		return -EPROBE_DEFER;

	for (sram = sram_list; sram->name; sram++) {
		if (sram->type != type)
			continue;

		if (!sram->enabled)
			return -ENODEV;

		spin_lock(&sram_lock);

		if (sram->claimed) {
			spin_unlock(&sram_lock);
			return -EBUSY;
		}

		sram->claimed = true;
		spin_unlock(&sram_lock);

		for (func = sram->func; func->func; func++) {
			if (strcmp(function, func->func))
				continue;

			val = readl(base + sram->reg);
			val &= ~GENMASK(sram->offset + sram->width,
					sram->offset);
			writel(val | func->val, base + sram->reg);

			return 0;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL(sunxi_sram_claim);

int sunxi_sram_release(enum sunxi_sram_type type)
{
	struct sunxi_sram_desc *sram;

	for (sram = sram_list; sram->type; sram++) {
		if (sram->type != type)
			continue;

		if (!sram->enabled)
			return -ENODEV;

		spin_lock(&sram_lock);
		sram->claimed = false;
		spin_unlock(&sram_lock);

		return 0;
	}

	return -EINVAL;
}
EXPORT_SYMBOL(sunxi_sram_release);

static const struct of_device_id sunxi_sram_dt_match[] = {
	{ .compatible = "allwinner,sun4i-a10-sram-controller",
	  .data = &sun4i_sram_desc },
	{ },
};
MODULE_DEVICE_TABLE(of, sunxi_sram_dt_match);

static int sunxi_sram_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct sunxi_sram_desc *sram;
	struct device_node *node;
	struct resource *res;
	struct dentry *d;
	const char *name;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	match = of_match_device(sunxi_sram_dt_match, &pdev->dev);
	if (!match)
		return -ENODEV;

	sram_list = (struct sunxi_sram_desc *)match->data;

	for_each_compatible_node(node, NULL, "allwinner,sun4i-a10-sram") {
		if (of_property_read_string(node, "allwinner,sram-name", &name))
			continue;

		for (sram = sram_list; sram->name; sram++)
			if (!strcmp(name, sram->name))
				break;

		if (!sram->name)
			continue;

		sram->enabled = true;
	}

	d = debugfs_create_file("sram", S_IRUGO, NULL, NULL,
				&sunxi_sram_fops);
	if (!d)
		return -ENOMEM;

	return 0;
}

static struct platform_driver sunxi_sram_driver = {
	.driver = {
		.name		= "sunxi-sram",
		.of_match_table	= sunxi_sram_dt_match,
	},
	.probe	= sunxi_sram_probe,
};
module_platform_driver(sunxi_sram_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Allwinner sunXi SRAM Controller Driver");
MODULE_LICENSE("GPL");
