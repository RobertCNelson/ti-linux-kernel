/*
 * IRQ/DMA CROSSBAR DRIVER
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 *	Sricharan R <r.sricharan@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */
#include <linux/crossbar.h>
#include <linux/regmap.h>

static LIST_HEAD(cb_devlist);

static struct regmap_config cb_regmap_config = {
	.reg_bits = 32,
};

static unsigned cb_entry_read(struct cb_line *tmp, const void *cbs)
{
	unsigned index = 0;

	tmp->cb_name = cbs;
	index = strlen(tmp->cb_name) + 1;

	tmp->dev_name = cbs + index;
	index += strlen(tmp->dev_name) + 1;

	tmp->int_no = be32_to_cpup(cbs + index);
	index += sizeof(tmp->int_no);

	tmp->cb_no = be32_to_cpup(cbs + index);
	index += sizeof(tmp->cb_no);

	tmp->offset = be32_to_cpup(cbs + index);
	index += sizeof(tmp->offset);

	return index;
}

int crossbar_unmap(struct device_node *cbdev_node, unsigned index)
{
	const void *cbs;
	unsigned size = 0, i = 0;
	struct cb_line tmp;
	struct cb_device *cbdev;
	struct cb_entry *cbentry, *p;

	cbs = of_get_property(cbdev_node, "crossbar-lines", &size);
	if (!cbs)
		return -ENOENT;

	size = 0;

	while (i++ < index)
		size += cb_entry_read(&tmp, cbs + size);

	cb_entry_read(&tmp, cbs + size);

	list_for_each_entry(cbdev, &cb_devlist, node) {
		if (strcmp(cbdev->name, tmp.cb_name))
			continue;

		mutex_lock(&cbdev->cb_lock);
		list_for_each_entry_safe(cbentry, p, &cbdev->cb_entries,
					 cb_list) {
			if ((cbentry->line.cb_no == tmp.cb_no) &&
			    (cbentry->line.int_no == tmp.int_no)) {
				list_del(&cbentry->cb_list);
				mutex_unlock(&cbdev->cb_lock);
				dev_warn(cbdev->dev,
					 "unmapped int_no %x mapped to cb %x\n",
					 tmp.int_no, tmp.cb_no);
				return 0;
			}
		}
		mutex_unlock(&cbdev->cb_lock);
		break;
	}

	dev_warn(cbdev->dev, "%s cb entry %d not found\n",
		 __func__, tmp.cb_no);
	return -ENOENT;
}
EXPORT_SYMBOL(crossbar_unmap);

const int cb_map(struct cb_line cbl)
{
	struct cb_device *cbdev;
	struct cb_entry *cbentry, *tmp;
	unsigned val;

	/* Get corresponding device pointer */
	list_for_each_entry(cbdev, &cb_devlist, node) {
		if (strcmp(cbdev->name, cbl.cb_name))
			continue;

		mutex_lock(&cbdev->cb_lock);

		/* Check for invalid and duplicate mapping */
		list_for_each_entry_safe(cbentry, tmp, &cbdev->cb_entries,
					 cb_list) {
			if ((cbentry->line.cb_no == cbl.cb_no) &&
			    (cbentry->line.int_no != cbl.int_no)) {
				dev_warn(cbdev->dev,
					 "%s irq already mapped to irq no %d",
					 cbentry->line.dev_name,
					 cbentry->line.int_no);
				mutex_unlock(&cbdev->cb_lock);
				return -EINVAL;
			}
			if ((cbentry->line.cb_no == cbl.cb_no) &&
			    (cbentry->line.int_no == cbl.int_no)) {
				mutex_unlock(&cbdev->cb_lock);
				return 0;
			}
			if ((cbentry->line.int_no == cbl.int_no) &&
			    (cbentry->line.cb_no != cbl.cb_no)) {
				dev_warn(cbdev->dev,
					 "%s irq replaced by %s irq\n",
					 cbentry->line.dev_name,
					 cbl.dev_name);
				list_del(&(cbentry->cb_list));
				break;
			}
		}

		cbentry = devm_kzalloc(cbdev->dev, sizeof(struct cb_entry),
								GFP_KERNEL);
		cbentry->line = cbl;
		list_add_tail(&(cbentry->cb_list), &cbdev->cb_entries);

		regmap_read(cbdev->cb_regmap, cbl.offset, &val);

		/* Print the replaced entry and map the new one */
		dev_warn(cbdev->dev,
			 "replacing irq %d mapped to cb input %d with cb input %d\n",
			 cbl.int_no, val, cbl.cb_no);

		regmap_write(cbdev->cb_regmap, cbl.offset, cbl.cb_no);
		mutex_unlock(&cbdev->cb_lock);
		return 0;
	}

	dev_warn(cbdev->dev, "crossbar device %s not found", cbl.cb_name);
	return -ENODEV;
}

int crossbar_map(struct device_node *cbdev_node)
{
	const void *cbs;
	unsigned size = 0, index = 0;
	int err;

	cbs = of_get_property(cbdev_node, "crossbar-lines", &size);
	if (!cbs)
		return -ENOENT;

	while (index < size) {
		struct cb_line tmp;

		index += cb_entry_read(&tmp, cbs + index);

		err = cb_map(tmp);
		if (IS_ERR_VALUE(err))
			return err;
	}

	return 0;
}
EXPORT_SYMBOL(crossbar_map);

static int crossbar_probe(struct platform_device *pdev)
{
	struct cb_device *cbdev;
	unsigned width;
	struct device_node *cbdev_node = pdev->dev.of_node;
	int err;
	struct resource *res;

	cbdev = devm_kzalloc(&pdev->dev, sizeof(struct cb_device), GFP_KERNEL);
	if (!cbdev)
		return -ENOMEM;

	/* Get the device resources */
	of_property_read_string(cbdev_node, "crossbar-name", &(cbdev->name));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL)
		return -ENOENT;

	cbdev->base = devm_ioremap_resource(&pdev->dev, res);
	if (!cbdev->base)
		return -ENOMEM;

	cbdev->dev = &pdev->dev;

	of_property_read_u32(cbdev_node, "reg-width", &width);

	cb_regmap_config.val_bits = width;
	cb_regmap_config.reg_stride = width >> 3;

	cbdev->cb_regmap = devm_regmap_init_mmio(cbdev->dev, cbdev->base,
						 &cb_regmap_config);

	if (IS_ERR(cbdev->cb_regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		err = PTR_ERR(cbdev->cb_regmap);
		return err;
	}

	platform_set_drvdata(pdev, cbdev);
	list_add_tail(&cbdev->node, &cb_devlist);

	/* INIT LIST HEAD */
	INIT_LIST_HEAD(&cbdev->cb_entries);

	mutex_init(&cbdev->cb_lock);

	/* map the cross bar entries passed as default from DT */
	err = crossbar_map(cbdev_node);

	return err;
}

#ifdef CONFIG_OF
static const struct of_device_id crossbar_match[] = {
	{.compatible = "crossbar", },
	{},
};
#endif

static struct platform_driver crossbar_driver = {
	.probe		= crossbar_probe,
	.driver		= {
		.name		= "crossbar",
		.owner		= THIS_MODULE,
		.of_match_table = crossbar_match,
	},
};

static int __init crossbar_init(void)
{
	return platform_driver_register(&crossbar_driver);
}
postcore_initcall(crossbar_init);
