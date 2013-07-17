/*
 * Error Location Module
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/pm_runtime.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/platform_data/elm.h>

#define	DRIVER_NAME			"omap-elm"
#define ELM_SYSCONFIG			0x010
#define ELM_IRQSTATUS			0x018
#define ELM_IRQENABLE			0x01c
#define ELM_LOCATION_CONFIG		0x020
#define ELM_PAGE_CTRL			0x080
#define ELM_SYNDROME_FRAGMENT_0		0x400
#define ELM_SYNDROME_FRAGMENT_1		0x404
#define ELM_SYNDROME_FRAGMENT_2		0x408
#define ELM_SYNDROME_FRAGMENT_3		0x40c
#define ELM_SYNDROME_FRAGMENT_4		0x410
#define ELM_SYNDROME_FRAGMENT_5		0x414
#define ELM_SYNDROME_FRAGMENT_6		0x418
#define ELM_LOCATION_STATUS		0x800
#define ELM_ERROR_LOCATION_0		0x880

/* ELM Interrupt Status Register */
#define INTR_STATUS_PAGE_VALID		BIT(8)

/* ELM Interrupt Enable Register */
#define INTR_EN_PAGE_MASK		BIT(8)

/* ELM Location Configuration Register */
#define ECC_BCH_LEVEL_MASK		0x3

/* ELM syndrome */
#define ELM_SYNDROME_VALID		BIT(16)

/* ELM_LOCATION_STATUS Register */
#define ECC_CORRECTABLE_MASK		BIT(8)
#define ECC_NB_ERRORS_MASK		0x1f

/* ELM_ERROR_LOCATION_0-15 Registers */
#define ECC_ERROR_LOCATION_MASK		0x1fff

#define ELM_ECC_SIZE			0x7ff

#define SYNDROME_FRAGMENT_REG_SIZE	0x40
#define ERROR_LOCATION_SIZE		0x100

struct elm_registers {
	u32 elm_irqenable;
	u32 elm_sysconfig;
	u32 elm_location_config;
	u32 elm_page_ctrl;
	u32 elm_syndrome_fragment_6[ERROR_VECTOR_MAX];
	u32 elm_syndrome_fragment_5[ERROR_VECTOR_MAX];
	u32 elm_syndrome_fragment_4[ERROR_VECTOR_MAX];
	u32 elm_syndrome_fragment_3[ERROR_VECTOR_MAX];
	u32 elm_syndrome_fragment_2[ERROR_VECTOR_MAX];
	u32 elm_syndrome_fragment_1[ERROR_VECTOR_MAX];
	u32 elm_syndrome_fragment_0[ERROR_VECTOR_MAX];
};

struct elm_info {
	struct device *dev;
	void __iomem *elm_base;
	struct completion elm_completion;
	struct list_head list;
	struct mtd_info *mtd;
	enum bch_ecc bch_type;
	struct elm_registers elm_regs;
	int eccsteps;
};

static LIST_HEAD(elm_devices);

static void elm_write_reg(struct elm_info *info, int offset, u32 val)
{
	writel(val, info->elm_base + offset);
}

static u32 elm_read_reg(struct elm_info *info, int offset)
{
	return readl(info->elm_base + offset);
}

/**
 * elm_config - Configure ELM module
 * @dev:	ELM device
 * @bch_type:	Type of BCH ecc
 */
int elm_config(struct device *dev, struct mtd_info *mtd,
		enum bch_ecc bch_type)
{
	u32 reg_val;
	struct elm_info	 *info;
	struct nand_chip *chip;
	if (!dev) {
		pr_err("%s: ELM device not found\n", DRIVER_NAME);
		return -ENODEV;
	}
	info = dev_get_drvdata(dev);
	if (!info) {
		pr_err("%s: ELM device data not found\n", DRIVER_NAME);
		return -ENODEV;
	}
	if (!mtd) {
		pr_err("%s: MTD device not found\n", DRIVER_NAME);
		return -ENODEV;
	}
	chip = mtd->priv;
	/* ELM supports error correction in chunks of 512bytes of data only
	 * where each 512bytes of data has its own ECC syndrome */
	if (chip->ecc.size != 512) {
		pr_err("%s: invalid ecc_size configuration", DRIVER_NAME);
		return -EINVAL;
	}
	if (mtd->writesize > 4096) {
		pr_err("%s: page-size > 4096 is not supported", DRIVER_NAME);
		return -EINVAL;
	}
	/* ELM eccsteps required to decode complete NAND page */
	info->mtd	= mtd;
	info->bch_type	= bch_type;
	info->eccsteps = mtd->writesize / chip->ecc.size;
	reg_val = (bch_type & ECC_BCH_LEVEL_MASK) | (ELM_ECC_SIZE << 16);
	elm_write_reg(info, ELM_LOCATION_CONFIG, reg_val);

	return 0;
}
EXPORT_SYMBOL(elm_config);

/**
 * elm_configure_page_mode - Enable/Disable page mode
 * @info:	elm info
 * @index:	index number of syndrome fragment vector
 * @enable:	enable/disable flag for page mode
 *
 * Enable page mode for syndrome fragment index
 */
static void elm_configure_page_mode(struct elm_info *info, int index,
		bool enable)
{
	u32 reg_val;

	reg_val = elm_read_reg(info, ELM_PAGE_CTRL);
	if (enable)
		reg_val |= BIT(index);	/* enable page mode */
	else
		reg_val &= ~BIT(index);	/* disable page mode */

	elm_write_reg(info, ELM_PAGE_CTRL, reg_val);
}

/**
 * elm_load_syndrome - Load ELM syndrome reg
 * @info:	elm info
 * @err_vec:	elm error vectors
 * @ecc:	buffer with calculated ecc
 *
 * Load syndrome fragment registers with calculated ecc in reverse order.
 */
static void elm_load_syndrome(struct elm_info *info,
		struct elm_errorvec *err_vec, u8 *ecc_calc)
{
	struct nand_chip *chip	= info->mtd->priv;
	unsigned int eccbytes	= chip->ecc.bytes;
	u8 *ecc = ecc_calc;
	int i, offset;
	u32 val;

	for (i = 0; i < info->eccsteps; i++) {
		/* Check error reported */
		if (err_vec[i].error_reported) {
			elm_configure_page_mode(info, i, true);
			offset = SYNDROME_FRAGMENT_REG_SIZE * i;
			ecc = ecc_calc + (i * eccbytes);
			switch (info->bch_type) {
			case BCH4_ECC:
				val =	((*(ecc + 6) >>  4) & 0x0F) |
					*(ecc +  5) <<  4 | *(ecc +  4) << 12 |
					*(ecc +  3) << 20 | *(ecc +  2) << 28;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_0 +
						 offset), cpu_to_le32(val));
				val =	((*(ecc + 2) >>  4) & 0x0F) |
					*(ecc +  1) <<  4 | *(ecc +  0) << 12;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_1 +
						 offset), cpu_to_le32(val));
				break;
			case BCH8_ECC:
				val =	*(ecc + 12) << 0  | *(ecc + 11) <<  8 |
					*(ecc + 10) << 16 | *(ecc +  9) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_0 +
						 offset), cpu_to_le32(val));
				val =	*(ecc +  8) <<  0 | *(ecc +  7) <<  8 |
					*(ecc +  6) << 16 | *(ecc +  5) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_1 +
						 offset), cpu_to_le32(val));
				val =	*(ecc +  4) <<  0 | *(ecc +  3) <<  8 |
					*(ecc +  2) << 16 | *(ecc +  1) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_2 +
						 offset), cpu_to_le32(val));
				val =	*(ecc +  0) <<  0 & 0x000000FF;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_3 +
						 offset), cpu_to_le32(val));
				break;
			case BCH16_ECC:
				val =	*(ecc + 25) << 0  | *(ecc + 24) <<  8 |
					*(ecc + 23) << 16 | *(ecc + 22) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_0 +
						 offset), cpu_to_le32(val));
				val =	*(ecc + 21) <<  0 | *(ecc + 20) <<  8 |
					*(ecc + 19) << 16 | *(ecc + 18) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_1 +
						 offset), cpu_to_le32(val));
				val =	*(ecc + 17) <<  0 | *(ecc + 16) <<  8 |
					*(ecc + 15) << 16 | *(ecc + 14) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_2 +
						 offset), cpu_to_le32(val));
				val =	*(ecc + 13) <<  0 | *(ecc + 12) <<  8 |
					*(ecc + 11) << 16 | *(ecc + 10) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_3 +
						 offset), cpu_to_le32(val));
				val =	*(ecc +  9) <<  0 | *(ecc +  8) <<  8 |
					*(ecc +  7) << 16 | *(ecc +  6) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_4 +
						 offset), cpu_to_le32(val));
				val =	*(ecc +  5) <<  0 | *(ecc +  4) <<  8 |
					*(ecc +  3) << 16 | *(ecc +  2) << 24;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_5 +
						 offset), cpu_to_le32(val));
				val =	*(ecc +  1) <<  0 | *(ecc +  0) <<  8;
				elm_write_reg(info, (ELM_SYNDROME_FRAGMENT_6 +
						 offset), cpu_to_le32(val));
				break;
			}
		}
	}
}

/**
 * elm_start_processing - start elm syndrome processing
 * @info:	elm info
 * @err_vec:	elm error vectors
 *
 * Set syndrome valid bit for syndrome fragment registers for which
 * elm syndrome fragment registers are loaded. This enables elm module
 * to start processing syndrome vectors.
 */
static void elm_start_processing(struct elm_info *info,
		struct elm_errorvec *err_vec)
{
	int i, offset;
	u32 reg_val;

	/*
	 * Set syndrome vector valid, so that ELM module
	 * will process it for vectors error is reported
	 */
	for (i = 0; i < info->eccsteps; i++) {
		if (err_vec[i].error_reported) {
			offset = ELM_SYNDROME_FRAGMENT_6 +
				SYNDROME_FRAGMENT_REG_SIZE * i;
			reg_val = elm_read_reg(info, offset);
			reg_val |= ELM_SYNDROME_VALID;
			elm_write_reg(info, offset, reg_val);
		}
	}
}

/**
 * elm_error_correction - locate correctable error position
 * @info:	elm info
 * @err_vec:	elm error vectors
 *
 * On completion of processing by elm module, error location status
 * register updated with correctable/uncorrectable error information.
 * In case of correctable errors, number of errors located from
 * elm location status register & read the positions from
 * elm error location register.
 */
static void elm_error_correction(struct elm_info *info,
		struct elm_errorvec *err_vec)
{
	int i, j, errors = 0;
	int offset;
	u32 reg_val;

	for (i = 0; i < info->eccsteps; i++) {

		/* Check error reported */
		if (err_vec[i].error_reported) {
			offset = ELM_LOCATION_STATUS + ERROR_LOCATION_SIZE * i;
			reg_val = elm_read_reg(info, offset);

			/* Check correctable error or not */
			if (reg_val & ECC_CORRECTABLE_MASK) {
				offset = ELM_ERROR_LOCATION_0 +
					ERROR_LOCATION_SIZE * i;
				/* Read count of correctable errors */
				err_vec[i].error_count = reg_val &
					ECC_NB_ERRORS_MASK;

				/* Update the error locations in error vector */
				for (j = 0; j < err_vec[i].error_count; j++) {
					reg_val = elm_read_reg(info, offset);
					err_vec[i].error_loc[j] = reg_val &
						ECC_ERROR_LOCATION_MASK;

					/* Update error location register */
					offset += 4;
				}

				errors += err_vec[i].error_count;
			} else {
				err_vec[i].error_uncorrectable = true;
			}

			/* Clearing interrupts for processed error vectors */
			elm_write_reg(info, ELM_IRQSTATUS, BIT(i));

			/* Disable page mode */
			elm_configure_page_mode(info, i, false);
		}
	}
}

/**
 * elm_decode_bch_error_page - Locate error position
 * @dev:	device pointer
 * @ecc_calc:	calculated ECC bytes from GPMC
 * @err_vec:	elm error vectors
 *
 * Called with one or more error reported vectors & vectors with
 * error reported is updated in err_vec[].error_reported
 */
void elm_decode_bch_error_page(struct device *dev, u8 *ecc_calc,
		struct elm_errorvec *err_vec)
{
	struct elm_info *info = dev_get_drvdata(dev);
	u32 reg_val;

	/* Enable page mode interrupt */
	reg_val = elm_read_reg(info, ELM_IRQSTATUS);
	elm_write_reg(info, ELM_IRQSTATUS, reg_val & INTR_STATUS_PAGE_VALID);
	elm_write_reg(info, ELM_IRQENABLE, INTR_EN_PAGE_MASK);

	/* Load valid ecc byte to syndrome fragment register */
	elm_load_syndrome(info, err_vec, ecc_calc);

	/* Enable syndrome processing for which syndrome fragment is updated */
	elm_start_processing(info, err_vec);

	/* Wait for ELM module to finish locating error correction */
	wait_for_completion(&info->elm_completion);

	/* Disable page mode interrupt */
	reg_val = elm_read_reg(info, ELM_IRQENABLE);
	elm_write_reg(info, ELM_IRQENABLE, reg_val & ~INTR_EN_PAGE_MASK);
	elm_error_correction(info, err_vec);
}
EXPORT_SYMBOL(elm_decode_bch_error_page);

static irqreturn_t elm_isr(int this_irq, void *dev_id)
{
	u32 reg_val;
	struct elm_info *info = dev_id;

	reg_val = elm_read_reg(info, ELM_IRQSTATUS);

	/* All error vectors processed */
	if (reg_val & INTR_STATUS_PAGE_VALID) {
		elm_write_reg(info, ELM_IRQSTATUS,
				reg_val & INTR_STATUS_PAGE_VALID);
		complete(&info->elm_completion);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int elm_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct resource *res, *irq;
	struct elm_info *info;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info) {
		dev_err(&pdev->dev, "failed to allocate memory\n");
		return -ENOMEM;
	}

	info->dev = &pdev->dev;

	irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!irq) {
		dev_err(&pdev->dev, "no irq resource defined\n");
		return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	info->elm_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(info->elm_base))
		return PTR_ERR(info->elm_base);

	ret = devm_request_irq(&pdev->dev, irq->start, elm_isr, 0,
			pdev->name, info);
	if (ret) {
		dev_err(&pdev->dev, "failure requesting irq %i\n", irq->start);
		return ret;
	}

	pm_runtime_enable(&pdev->dev);
	if (pm_runtime_get_sync(&pdev->dev)) {
		ret = -EINVAL;
		pm_runtime_disable(&pdev->dev);
		dev_err(&pdev->dev, "can't enable clock\n");
		return ret;
	}

	init_completion(&info->elm_completion);
	INIT_LIST_HEAD(&info->list);
	list_add(&info->list, &elm_devices);
	platform_set_drvdata(pdev, info);
	return ret;
}

static int elm_remove(struct platform_device *pdev)
{
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

/**
 * elm_context_save
 * saves ELM configurations to preserve them across Hardware powered-down
 */
static int elm_context_save(struct elm_info *info)
{
	struct elm_registers *regs = &info->elm_regs;
	enum bch_ecc bch_type = info->bch_type;
	u32 offset = 0, i;

	regs->elm_irqenable       = elm_read_reg(info, ELM_IRQENABLE);
	regs->elm_sysconfig       = elm_read_reg(info, ELM_SYSCONFIG);
	regs->elm_location_config = elm_read_reg(info, ELM_LOCATION_CONFIG);
	regs->elm_page_ctrl       = elm_read_reg(info, ELM_PAGE_CTRL);
	for (i = 0; i < ERROR_VECTOR_MAX; i++) {
		offset = i * SYNDROME_FRAGMENT_REG_SIZE;
		switch (bch_type) {
		case BCH8_ECC:
			regs->elm_syndrome_fragment_3[i] = elm_read_reg(info,
					ELM_SYNDROME_FRAGMENT_3 + offset);
			regs->elm_syndrome_fragment_2[i] = elm_read_reg(info,
					ELM_SYNDROME_FRAGMENT_2 + offset);
		case BCH4_ECC:
			regs->elm_syndrome_fragment_1[i] = elm_read_reg(info,
					ELM_SYNDROME_FRAGMENT_1 + offset);
			regs->elm_syndrome_fragment_0[i] = elm_read_reg(info,
					ELM_SYNDROME_FRAGMENT_0 + offset);
		default:
			return -EINVAL;
		}
		/* ELM SYNDROME_VALID bit in SYNDROME_FRAGMENT_6[] needs
		 * to be saved for all BCH schemes*/
		regs->elm_syndrome_fragment_6[i] = elm_read_reg(info,
					ELM_SYNDROME_FRAGMENT_6 + offset);
	}
	return 0;
}

/**
 * elm_context_restore
 * writes configurations saved duing power-down back into ELM registers
 */
static int elm_context_restore(struct elm_info *info)
{
	struct elm_registers *regs = &info->elm_regs;
	enum bch_ecc bch_type = info->bch_type;
	u32 offset = 0, i;

	elm_write_reg(info, ELM_IRQENABLE,	 regs->elm_irqenable);
	elm_write_reg(info, ELM_SYSCONFIG,	 regs->elm_sysconfig);
	elm_write_reg(info, ELM_LOCATION_CONFIG, regs->elm_location_config);
	elm_write_reg(info, ELM_PAGE_CTRL,	 regs->elm_page_ctrl);
	for (i = 0; i < ERROR_VECTOR_MAX; i++) {
		offset = i * SYNDROME_FRAGMENT_REG_SIZE;
		switch (bch_type) {
		case BCH8_ECC:
			elm_write_reg(info, ELM_SYNDROME_FRAGMENT_3 + offset,
					regs->elm_syndrome_fragment_3[i]);
			elm_write_reg(info, ELM_SYNDROME_FRAGMENT_2 + offset,
					regs->elm_syndrome_fragment_2[i]);
		case BCH4_ECC:
			elm_write_reg(info, ELM_SYNDROME_FRAGMENT_1 + offset,
					regs->elm_syndrome_fragment_1[i]);
			elm_write_reg(info, ELM_SYNDROME_FRAGMENT_0 + offset,
					regs->elm_syndrome_fragment_0[i]);
		default:
			return -EINVAL;
		}
		/* ELM_SYNDROME_VALID bit to be set in last to trigger FSM */
		elm_write_reg(info, ELM_SYNDROME_FRAGMENT_6 + offset,
					regs->elm_syndrome_fragment_6[i] &
							 ELM_SYNDROME_VALID);
	}
	return 0;
}

static int elm_suspend(struct device *dev)
{
	struct elm_info *info = dev_get_drvdata(dev);
	elm_context_save(info);
	pm_runtime_put_sync(dev);
	return 0;
}

static int elm_resume(struct device *dev)
{
	struct elm_info *info = dev_get_drvdata(dev);
	pm_runtime_get_sync(dev);
	elm_context_restore(info);
	return 0;
}

static SIMPLE_DEV_PM_OPS(elm_pm_ops, elm_suspend, elm_resume);

#ifdef CONFIG_OF
static const struct of_device_id elm_of_match[] = {
	{ .compatible = "ti,am3352-elm" },
	{},
};
MODULE_DEVICE_TABLE(of, elm_of_match);
#endif

static struct platform_driver elm_driver = {
	.driver	= {
		.name	= "elm",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(elm_of_match),
		.pm	= &elm_pm_ops,
	},
	.probe	= elm_probe,
	.remove	= elm_remove,
};

module_platform_driver(elm_driver);

MODULE_DESCRIPTION("ELM driver for BCH error correction");
MODULE_AUTHOR("Texas Instruments");
MODULE_ALIAS("platform: elm");
MODULE_LICENSE("GPL v2");
