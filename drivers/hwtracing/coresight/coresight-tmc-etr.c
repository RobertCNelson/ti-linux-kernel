/*
 * Copyright(C) 2016 Linaro Limited. All rights reserved.
 * Author: Mathieu Poirier <mathieu.poirier@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/coresight.h>
#include <linux/dma-mapping.h>

#include "coresight-priv.h"
#include "coresight-tmc.h"

void tmc_etr_enable_hw(struct tmc_drvdata *drvdata)
{
	u32 axictl;

	/* Zero out the memory to help with debug */
	memset(drvdata->vaddr, 0, drvdata->size);

	CS_UNLOCK(drvdata->base);

	/* Wait for TMCSReady bit to be set */
	tmc_wait_for_tmcready(drvdata);

	writel_relaxed(drvdata->size / 4, drvdata->base + TMC_RSZ);
	writel_relaxed(TMC_MODE_CIRCULAR_BUFFER, drvdata->base + TMC_MODE);

	axictl = readl_relaxed(drvdata->base + TMC_AXICTL);
	axictl |= TMC_AXICTL_WR_BURST_LEN;
	writel_relaxed(axictl, drvdata->base + TMC_AXICTL);
	axictl &= ~TMC_AXICTL_SCT_GAT_MODE;
	writel_relaxed(axictl, drvdata->base + TMC_AXICTL);
	axictl = (axictl &
		  ~(TMC_AXICTL_PROT_CTL_B0 | TMC_AXICTL_PROT_CTL_B1)) |
		  TMC_AXICTL_PROT_CTL_B1;
	writel_relaxed(axictl, drvdata->base + TMC_AXICTL);

	writel_relaxed(drvdata->paddr, drvdata->base + TMC_DBALO);
	writel_relaxed(0x0, drvdata->base + TMC_DBAHI);
	writel_relaxed(TMC_FFCR_EN_FMT | TMC_FFCR_EN_TI |
		       TMC_FFCR_FON_FLIN | TMC_FFCR_FON_TRIG_EVT |
		       TMC_FFCR_TRIGON_TRIGIN,
		       drvdata->base + TMC_FFCR);
	writel_relaxed(drvdata->trigger_cntr, drvdata->base + TMC_TRG);
	tmc_enable_hw(drvdata);

	CS_LOCK(drvdata->base);
}

static void tmc_etr_dump_hw(struct tmc_drvdata *drvdata)
{
	u32 rwp, val;

	rwp = readl_relaxed(drvdata->base + TMC_RWP);
	val = readl_relaxed(drvdata->base + TMC_STS);

	/* How much memory do we still have */
	if (val & BIT(0))
		drvdata->buf = drvdata->vaddr + rwp - drvdata->paddr;
	else
		drvdata->buf = drvdata->vaddr;
}

static void tmc_etr_disable_hw(struct tmc_drvdata *drvdata)
{
	CS_UNLOCK(drvdata->base);

	tmc_flush_and_stop(drvdata);
	tmc_disable_hw(drvdata);

	CS_LOCK(drvdata->base);
}

static int tmc_enable_etr_sink_sysfs(struct coresight_device *csdev, u32 mode)
{
	u32 val;
	bool allocated = false;
	unsigned long flags;
	void __iomem *vaddr;
	dma_addr_t paddr;
	struct tmc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	/*
	 * Contiguous  memory can't be allocated while a spinlock is held.
	 * As such allocate memory here and free it if a buffer has already
	 * been allocated (from a previous session).
	 */
	vaddr = dma_alloc_coherent(drvdata->dev, drvdata->size,
				   &paddr, GFP_KERNEL);
	if (!vaddr)
		return -ENOMEM;

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (drvdata->reading) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		dma_free_coherent(drvdata->dev, drvdata->size, vaddr, paddr);
		return -EBUSY;
	}

	val = local_cmpxchg(&drvdata->mode, CS_MODE_DISABLED, mode);
	/* No need to continue if already operated from Perf */
	if (val == CS_MODE_PERF) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		dma_free_coherent(drvdata->dev, drvdata->size, vaddr, paddr);
		return -EBUSY;
	}

	/*
	 * In sysFS mode we can have multiple writers per sink.  Since this
	 * sink is already enabled no memory is needed and the HW need not be
	 * touched.
	 */
	if (val == CS_MODE_SYSFS)
		goto out;

	/*
	 * If drvdata::buf == NULL, use the memory allocated above.
	 * Otherwise a buffer still exists from a previous session, so
	 * simply use that.
	 */
	if (!drvdata->buf) {
		allocated = true;
		drvdata->vaddr = vaddr;
		drvdata->paddr = paddr;
		drvdata->buf = drvdata->vaddr;
	}

	memset(drvdata->vaddr, 0, drvdata->size);

	tmc_etr_enable_hw(drvdata);
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

out:
	/* Free memory outside the spinlock if need be */
	if (!allocated)
		dma_free_coherent(drvdata->dev, drvdata->size, vaddr, paddr);

	dev_info(drvdata->dev, "TMC-ETR enabled\n");
	return 0;
}

static int tmc_enable_etr_sink_perf(struct coresight_device *csdev, u32 mode)
{
	u32 val;
	unsigned long flags;
	struct tmc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (drvdata->reading) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EBUSY;
	}

	val = local_cmpxchg(&drvdata->mode, CS_MODE_DISABLED, mode);
	/*
	 * In Perf mode there can be only one writer per sink.  There
	 * is also no need to continue if the ETR is already operated
	 * from sysFS.
	 */
	if (val != CS_MODE_DISABLED) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EBUSY;
	}

	tmc_etr_enable_hw(drvdata);
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	return 0;
}

static int tmc_enable_etr_sink(struct coresight_device *csdev, u32 mode)
{
	switch (mode) {
	case CS_MODE_SYSFS:
		return tmc_enable_etr_sink_sysfs(csdev, mode);
	case CS_MODE_PERF:
		return tmc_enable_etr_sink_perf(csdev, mode);
	}

	/* We shouldn't be here */
	return -EINVAL;
}

static void tmc_disable_etr_sink(struct coresight_device *csdev)
{
	u32 mode;
	unsigned long flags;
	struct tmc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock_irqsave(&drvdata->spinlock, flags);
	if (drvdata->reading) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return;
	}

	mode = local_xchg(&drvdata->mode, CS_MODE_DISABLED);
	/* Nothing to do, the ETR was already disabled */
	if (mode == CS_MODE_DISABLED)
		goto out;

	/* The engine has to be stopped in both sysFS and Perf mode */
	tmc_etr_disable_hw(drvdata);

	/*
	 * If we operated from sysFS, dump the trace data for retrieval
	 * via /dev/.  From Perf trace data is handled via the Perf ring
	 * buffer.
	 */
	if (mode == CS_MODE_SYSFS)
		tmc_etr_dump_hw(drvdata);

out:
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	dev_info(drvdata->dev, "TMC-ETR disabled\n");
}

static const struct coresight_ops_sink tmc_etr_sink_ops = {
	.enable		= tmc_enable_etr_sink,
	.disable	= tmc_disable_etr_sink,
};

const struct coresight_ops tmc_etr_cs_ops = {
	.sink_ops	= &tmc_etr_sink_ops,
};

int tmc_read_prepare_etr(struct tmc_drvdata *drvdata)
{
	unsigned long flags;

	spin_lock_irqsave(&drvdata->spinlock, flags);

	/* Don't interfere if operated from Perf */
	if (local_read(&drvdata->mode) == CS_MODE_PERF) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EBUSY;
	}

	/* The ETR isn't enabled, so there is no need to disable it */
	if (local_read(&drvdata->mode) == CS_MODE_DISABLED) {
		/*
		 * The ETR is disabled already.  If drvdata::buf is NULL
		 * trace data has been harvested.
		 */
		if (!drvdata->buf) {
			spin_unlock_irqrestore(&drvdata->spinlock, flags);
			return -EINVAL;
		}

		goto out;
	}

	if (drvdata->config_type != TMC_CONFIG_TYPE_ETR) {
		spin_unlock_irqrestore(&drvdata->spinlock, flags);
		return -EINVAL;
	}

	tmc_etr_disable_hw(drvdata);
	tmc_etr_dump_hw(drvdata);

out:
	drvdata->reading = true;
	spin_unlock_irqrestore(&drvdata->spinlock, flags);
	return 0;
}

int tmc_read_unprepare_etr(struct tmc_drvdata *drvdata)
{
	int ret = 0;
	unsigned long flags;
	void __iomem *vaddr = NULL;
	dma_addr_t paddr;

	spin_lock_irqsave(&drvdata->spinlock, flags);

	if (drvdata->config_type != TMC_CONFIG_TYPE_ETR) {
		ret = -EINVAL;
		goto err;
	}

	/* The TMC isn't enabled, so there is no need to enable it */
	if (local_read(&drvdata->mode) == CS_MODE_DISABLED) {
		/*
		 * The ETR is not tracing and trace data was just read. As
		 * such prepare to free the trace buffer.
		 */
		vaddr = drvdata->vaddr;
		paddr = drvdata->paddr;

		/*
		 * drvdata::buf is switched on in tmc_read_prepare_etr() and
		 * tmc_enable_etr_sink so it is important to set it back to
		 * NULL.
		 */
		drvdata->buf = NULL;
		goto out;
	}

	/*
	 * The trace run will continue with the same allocated trace buffer.
	 * As such zero-out the buffer so that we don't end up with stale
	 * data.
	 */
	memset(drvdata->buf, 0, drvdata->size);
	tmc_etr_enable_hw(drvdata);

out:
	drvdata->reading = false;

err:
	spin_unlock_irqrestore(&drvdata->spinlock, flags);

	/* Free allocated memory out side of the spinlock */
	if (vaddr)
		dma_free_coherent(drvdata->dev, drvdata->size, vaddr, paddr);

	return ret;
}
