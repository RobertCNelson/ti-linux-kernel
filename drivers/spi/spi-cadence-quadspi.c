// SPDX-License-Identifier: GPL-2.0-only
//
// Driver for Cadence QSPI Controller
//
// Copyright Altera Corporation (C) 2012-2014. All rights reserved.
// Copyright Intel Corporation (C) 2019-2020. All rights reserved.
// Copyright (C) 2020 Texas Instruments Incorporated - http://www.ti.com

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware/xlnx-zynqmp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/timer.h>

#define CQSPI_NAME			"cadence-qspi"
#define CQSPI_MAX_CHIPSELECT		4
#define CQSPI_AM654_NON_PHY_CLK_RATE	25000000

static_assert(CQSPI_MAX_CHIPSELECT <= SPI_DEVICE_CS_CNT_MAX);

/* Quirks */
#define CQSPI_NEEDS_WR_DELAY		BIT(0)
#define CQSPI_DISABLE_DAC_MODE		BIT(1)
#define CQSPI_SUPPORT_EXTERNAL_DMA	BIT(2)
#define CQSPI_NO_SUPPORT_WR_COMPLETION	BIT(3)
#define CQSPI_SLOW_SRAM		BIT(4)
#define CQSPI_NEEDS_APB_AHB_HAZARD_WAR	BIT(5)
#define CQSPI_RD_NO_IRQ			BIT(6)
#define CQSPI_DMA_SET_MASK		BIT(7)
#define CQSPI_SUPPORT_DEVICE_RESET	BIT(8)
#define CQSPI_DISABLE_STIG_MODE		BIT(9)
#define CQSPI_DISABLE_RUNTIME_PM	BIT(10)

/* Capabilities */
#define CQSPI_SUPPORTS_OCTAL		BIT(0)
#define CQSPI_SUPPORTS_QUAD		BIT(1)

#define CQSPI_OP_WIDTH(part) ((part).nbytes ? ilog2((part).buswidth) : 0)

enum {
	CLK_QSPI_APB = 0,
	CLK_QSPI_AHB,
	CLK_QSPI_NUM,
};

struct cqspi_st;

struct phy_setting {
	u8			rx;
	u8			tx;
	u8			read_delay;
};

struct cqspi_flash_pdata {
	struct cqspi_st	*cqspi;
	u32		clk_rate;
	u32		non_phy_clk_rate;
	u32		read_delay;
	u32		tshsl_ns;
	u32		tsd2d_ns;
	u32		tchsh_ns;
	u32		tslch_ns;
	bool		has_dqs;
	bool		use_phy;
	u8		cs;
	struct phy_setting	phy_setting;
	struct spi_mem_op	phy_read_op;
	struct spi_mem_op	phy_write_op;
};

struct cqspi_st {
	struct platform_device	*pdev;
	struct spi_controller	*host;
	struct clk		*clk;
	struct clk		*clks[CLK_QSPI_NUM];
	unsigned int		sclk;

	void __iomem		*iobase;
	void __iomem		*ahb_base;
	resource_size_t		ahb_size;
	struct completion	transfer_complete;

	struct dma_chan		*rx_chan;
	struct completion	rx_dma_complete;
	dma_addr_t		mmap_phys_base;

	int			current_cs;
	unsigned long		master_ref_clk_hz;
	bool			is_decoded_cs;
	u32			fifo_depth;
	u32			fifo_width;
	u32			num_chipselect;
	bool			rclk_en;
	u32			trigger_address;
	u32			phase_detect_selector;
	u32			wr_delay;
	bool			use_direct_mode;
	bool			use_direct_mode_wr;
	struct cqspi_flash_pdata f_pdata[CQSPI_MAX_CHIPSELECT];
	bool			use_dma_read;
	u32			pd_dev_id;
	bool			wr_completion;
	bool			slow_sram;
	bool			apb_ahb_hazard;

	bool			is_jh7110; /* Flag for StarFive JH7110 SoC */
	bool			disable_stig_mode;
	refcount_t		refcount;
	refcount_t		inflight_ops;

	const struct cqspi_driver_platdata *ddata;
};

struct cqspi_driver_platdata {
	u32 hwcaps_mask;
	u16 quirks;
	int (*indirect_read_dma)(struct cqspi_flash_pdata *f_pdata,
				 u_char *rxbuf, loff_t from_addr, size_t n_rx);
	u32 (*get_dma_status)(struct cqspi_st *cqspi);
	int (*jh7110_clk_init)(struct platform_device *pdev,
			       struct cqspi_st *cqspi);
	int (*execute_tuning)(struct spi_mem *mem, struct spi_mem_op *read_op,
			      struct spi_mem_op *write_op);
	u32 (*get_non_phy_clk_rate)(struct cqspi_st *cqspi);
};

/* Operation timeout value */
#define CQSPI_TIMEOUT_MS			500
#define CQSPI_READ_TIMEOUT_MS			10
#define CQSPI_BUSYWAIT_TIMEOUT_US		500
#define CQSPI_DLL_TIMEOUT_US			300

/* Runtime_pm autosuspend delay */
#define CQSPI_AUTOSUSPEND_TIMEOUT		2000

#define CQSPI_DUMMY_CLKS_PER_BYTE		8
#define CQSPI_DUMMY_BYTES_MAX			4
#define CQSPI_DUMMY_CLKS_MAX			31

#define CQSPI_STIG_DATA_LEN_MAX			8

/* Register map */
#define CQSPI_REG_CONFIG			0x00
#define CQSPI_REG_CONFIG_ENABLE_MASK		BIT(0)
#define CQSPI_REG_CONFIG_PHY_EN			BIT(3)
#define CQSPI_REG_CONFIG_ENB_DIR_ACC_CTRL	BIT(7)
#define CQSPI_REG_CONFIG_DECODE_MASK		BIT(9)
#define CQSPI_REG_CONFIG_CHIPSELECT_LSB		10
#define CQSPI_REG_CONFIG_DMA_MASK		BIT(15)
#define CQSPI_REG_CONFIG_BAUD_LSB		19
#define CQSPI_REG_CONFIG_DTR_PROTO		BIT(24)
#define CQSPI_REG_CONFIG_PHY_PIPELINE		BIT(25)
#define CQSPI_REG_CONFIG_DUAL_OPCODE		BIT(30)
#define CQSPI_REG_CONFIG_IDLE_LSB		31
#define CQSPI_REG_CONFIG_CHIPSELECT_MASK	0xF
#define CQSPI_REG_CONFIG_BAUD_MASK		0xF
#define CQSPI_REG_CONFIG_RESET_PIN_FLD_MASK    BIT(5)
#define CQSPI_REG_CONFIG_RESET_CFG_FLD_MASK    BIT(6)

#define CQSPI_REG_RD_INSTR			0x04
#define CQSPI_REG_RD_INSTR_OPCODE_LSB		0
#define CQSPI_REG_RD_INSTR_TYPE_INSTR_LSB	8
#define CQSPI_REG_RD_INSTR_TYPE_ADDR_LSB	12
#define CQSPI_REG_RD_INSTR_TYPE_DATA_LSB	16
#define CQSPI_REG_RD_INSTR_MODE_EN_LSB		20
#define CQSPI_REG_RD_INSTR_DUMMY_LSB		24
#define CQSPI_REG_RD_INSTR_TYPE_INSTR_MASK	0x3
#define CQSPI_REG_RD_INSTR_TYPE_ADDR_MASK	0x3
#define CQSPI_REG_RD_INSTR_TYPE_DATA_MASK	0x3
#define CQSPI_REG_RD_INSTR_DUMMY_MASK		0x1F

#define CQSPI_REG_WR_INSTR			0x08
#define CQSPI_REG_WR_INSTR_OPCODE_LSB		0
#define CQSPI_REG_WR_INSTR_TYPE_ADDR_LSB	12
#define CQSPI_REG_WR_INSTR_TYPE_DATA_LSB	16

#define CQSPI_REG_DELAY				0x0C
#define CQSPI_REG_DELAY_TSLCH_LSB		0
#define CQSPI_REG_DELAY_TCHSH_LSB		8
#define CQSPI_REG_DELAY_TSD2D_LSB		16
#define CQSPI_REG_DELAY_TSHSL_LSB		24
#define CQSPI_REG_DELAY_TSLCH_MASK		0xFF
#define CQSPI_REG_DELAY_TCHSH_MASK		0xFF
#define CQSPI_REG_DELAY_TSD2D_MASK		0xFF
#define CQSPI_REG_DELAY_TSHSL_MASK		0xFF

#define CQSPI_REG_READCAPTURE			0x10
#define CQSPI_REG_READCAPTURE_BYPASS_LSB	0
#define CQSPI_REG_READCAPTURE_DELAY_LSB		1
#define CQSPI_REG_READCAPTURE_DELAY_MASK	0xF
#define CQSPI_REG_READCAPTURE_EDGE_LSB		5
#define CQSPI_REG_READCAPTURE_DQS_LSB		8

#define CQSPI_REG_SIZE				0x14
#define CQSPI_REG_SIZE_ADDRESS_LSB		0
#define CQSPI_REG_SIZE_PAGE_LSB			4
#define CQSPI_REG_SIZE_BLOCK_LSB		16
#define CQSPI_REG_SIZE_ADDRESS_MASK		0xF
#define CQSPI_REG_SIZE_PAGE_MASK		0xFFF
#define CQSPI_REG_SIZE_BLOCK_MASK		0x3F

#define CQSPI_REG_SRAMPARTITION			0x18
#define CQSPI_REG_INDIRECTTRIGGER		0x1C

#define CQSPI_REG_DMA				0x20
#define CQSPI_REG_DMA_SINGLE_LSB		0
#define CQSPI_REG_DMA_BURST_LSB			8
#define CQSPI_REG_DMA_SINGLE_MASK		0xFF
#define CQSPI_REG_DMA_BURST_MASK		0xFF

#define CQSPI_REG_REMAP				0x24
#define CQSPI_REG_MODE_BIT			0x28

#define CQSPI_REG_SDRAMLEVEL			0x2C
#define CQSPI_REG_SDRAMLEVEL_RD_LSB		0
#define CQSPI_REG_SDRAMLEVEL_WR_LSB		16
#define CQSPI_REG_SDRAMLEVEL_RD_MASK		0xFFFF
#define CQSPI_REG_SDRAMLEVEL_WR_MASK		0xFFFF

#define CQSPI_REG_WR_COMPLETION_CTRL		0x38
#define CQSPI_REG_WR_DISABLE_AUTO_POLL		BIT(14)

#define CQSPI_REG_IRQSTATUS			0x40
#define CQSPI_REG_IRQMASK			0x44

#define CQSPI_REG_INDIRECTRD			0x60
#define CQSPI_REG_INDIRECTRD_START_MASK		BIT(0)
#define CQSPI_REG_INDIRECTRD_CANCEL_MASK	BIT(1)
#define CQSPI_REG_INDIRECTRD_DONE_MASK		BIT(5)

#define CQSPI_REG_INDIRECTRDWATERMARK		0x64
#define CQSPI_REG_INDIRECTRDSTARTADDR		0x68
#define CQSPI_REG_INDIRECTRDBYTES		0x6C

#define CQSPI_REG_CMDCTRL			0x90
#define CQSPI_REG_CMDCTRL_EXECUTE_MASK		BIT(0)
#define CQSPI_REG_CMDCTRL_INPROGRESS_MASK	BIT(1)
#define CQSPI_REG_CMDCTRL_DUMMY_LSB		7
#define CQSPI_REG_CMDCTRL_WR_BYTES_LSB		12
#define CQSPI_REG_CMDCTRL_WR_EN_LSB		15
#define CQSPI_REG_CMDCTRL_ADD_BYTES_LSB		16
#define CQSPI_REG_CMDCTRL_ADDR_EN_LSB		19
#define CQSPI_REG_CMDCTRL_RD_BYTES_LSB		20
#define CQSPI_REG_CMDCTRL_RD_EN_LSB		23
#define CQSPI_REG_CMDCTRL_OPCODE_LSB		24
#define CQSPI_REG_CMDCTRL_WR_BYTES_MASK		0x7
#define CQSPI_REG_CMDCTRL_ADD_BYTES_MASK	0x3
#define CQSPI_REG_CMDCTRL_RD_BYTES_MASK		0x7
#define CQSPI_REG_CMDCTRL_DUMMY_MASK		0x1F

#define CQSPI_REG_INDIRECTWR			0x70
#define CQSPI_REG_INDIRECTWR_START_MASK		BIT(0)
#define CQSPI_REG_INDIRECTWR_CANCEL_MASK	BIT(1)
#define CQSPI_REG_INDIRECTWR_DONE_MASK		BIT(5)

#define CQSPI_REG_INDIRECTWRWATERMARK		0x74
#define CQSPI_REG_INDIRECTWRSTARTADDR		0x78
#define CQSPI_REG_INDIRECTWRBYTES		0x7C

#define CQSPI_REG_INDTRIG_ADDRRANGE		0x80

#define CQSPI_REG_CMDADDRESS			0x94
#define CQSPI_REG_CMDREADDATALOWER		0xA0
#define CQSPI_REG_CMDREADDATAUPPER		0xA4
#define CQSPI_REG_CMDWRITEDATALOWER		0xA8
#define CQSPI_REG_CMDWRITEDATAUPPER		0xAC

#define CQSPI_REG_POLLING_STATUS		0xB0
#define CQSPI_REG_POLLING_STATUS_DUMMY_LSB	16

#define CQSPI_REG_PHY_CONFIG			0xB4
#define CQSPI_REG_PHY_CONFIG_RX_DEL_LSB		0
#define CQSPI_REG_PHY_CONFIG_RX_DEL_MASK	0x7F
#define CQSPI_REG_PHY_CONFIG_TX_DEL_LSB		16
#define CQSPI_REG_PHY_CONFIG_TX_DEL_MASK	0x7F
#define CQSPI_REG_PHY_CONFIG_DLL_RESET		BIT(30)
#define CQSPI_REG_PHY_CONFIG_RESYNC		BIT(31)

#define CQSPI_REG_PHY_DLL_MASTER		0xB8
#define CQSPI_REG_PHY_DLL_MASTER_INIT_DELAY_LSB	0
#define CQSPI_REG_PHY_DLL_MASTER_INIT_DELAY_VAL	16
#define CQSPI_REG_PHY_DLL_MASTER_DLY_ELMTS_LEN	0x7
#define CQSPI_REG_PHY_DLL_MASTER_DLY_ELMTS_LSB	20
#define CQSPI_REG_PHY_DLL_MASTER_BYPASS		BIT(23)
#define CQSPI_REG_PHY_DLL_MASTER_CYCLE		BIT(24)

#define CQSPI_REG_DLL_OBS_LOW			0xBC
#define CQSPI_REG_DLL_OBS_LOW_DLL_LOCK		BIT(0)
#define CQSPI_REG_DLL_OBS_LOW_LOOPBACK_LOCK	BIT(15)

#define CQSPI_REG_OP_EXT_LOWER			0xE0
#define CQSPI_REG_OP_EXT_READ_LSB		24
#define CQSPI_REG_OP_EXT_WRITE_LSB		16
#define CQSPI_REG_OP_EXT_STIG_LSB		0

#define CQSPI_REG_VERSAL_DMA_SRC_ADDR		0x1000

#define CQSPI_REG_VERSAL_DMA_DST_ADDR		0x1800
#define CQSPI_REG_VERSAL_DMA_DST_SIZE		0x1804

#define CQSPI_REG_VERSAL_DMA_DST_CTRL		0x180C

#define CQSPI_REG_VERSAL_DMA_DST_I_STS		0x1814
#define CQSPI_REG_VERSAL_DMA_DST_I_EN		0x1818
#define CQSPI_REG_VERSAL_DMA_DST_I_DIS		0x181C
#define CQSPI_REG_VERSAL_DMA_DST_DONE_MASK	BIT(1)

#define CQSPI_REG_VERSAL_DMA_DST_ADDR_MSB	0x1828

#define CQSPI_REG_VERSAL_DMA_DST_CTRL_VAL	0xF43FFA00
#define CQSPI_REG_VERSAL_ADDRRANGE_WIDTH_VAL	0x6

/* Interrupt status bits */
#define CQSPI_REG_IRQ_MODE_ERR			BIT(0)
#define CQSPI_REG_IRQ_UNDERFLOW			BIT(1)
#define CQSPI_REG_IRQ_IND_COMP			BIT(2)
#define CQSPI_REG_IRQ_IND_RD_REJECT		BIT(3)
#define CQSPI_REG_IRQ_WR_PROTECTED_ERR		BIT(4)
#define CQSPI_REG_IRQ_ILLEGAL_AHB_ERR		BIT(5)
#define CQSPI_REG_IRQ_WATERMARK			BIT(6)
#define CQSPI_REG_IRQ_IND_SRAM_FULL		BIT(12)

#define CQSPI_IRQ_MASK_RD		(CQSPI_REG_IRQ_WATERMARK	| \
					 CQSPI_REG_IRQ_IND_SRAM_FULL	| \
					 CQSPI_REG_IRQ_IND_COMP)

#define CQSPI_IRQ_MASK_WR		(CQSPI_REG_IRQ_IND_COMP		| \
					 CQSPI_REG_IRQ_WATERMARK	| \
					 CQSPI_REG_IRQ_UNDERFLOW)

#define CQSPI_IRQ_STATUS_MASK		0x1FFFF
#define CQSPI_DMA_UNALIGN		0x3

#define CQSPI_REG_VERSAL_DMA_VAL		0x602

#define CQSPI_PHY_INIT_RD			1
#define CQSPI_PHY_MAX_RD			4
#define CQSPI_PHY_MAX_DELAY			127
#define CQSPI_PHY_DDR_RADIUS			3
#define CQSPI_PHY_DDR_MIN_PASS_TAPS		5
#define CQSPI_PHY_DDR_DIAG_SHIFT		4
#define CQSPI_PHY_DDR_MAX_DIAG_SHIFT		48
#define CQSPI_PHY_DDR_CONSEC_FAIL		3
#define CQSPI_PHY_DDR_CONSEC_PASS		3
#define CQSPI_PHY_DDR_RD_STEP			4
#define CQSPI_PHY_RD_INVALID			U8_MAX

/*
 * PHY tuning pattern for calibrating read data capture delay. This 128-byte
 * pattern provides sufficient bit transitions across all byte lanes to
 * reliably detect timing windows at high frequencies.
 */
static const u8 phy_tuning_pattern[] __aligned(64) = {
	0xFE, 0xFF, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFE, 0xFE, 0x01,
	0x01, 0x01, 0x01, 0x00, 0x00, 0xFE, 0xFE, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0x00, 0x00, 0xFE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFE,
	0xFE, 0xFF, 0x01, 0x01, 0x01, 0x01, 0x01, 0xFE, 0x00, 0xFE, 0xFE, 0x01,
	0x01, 0x01, 0x01, 0xFE, 0x00, 0xFE, 0xFE, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFE, 0x00, 0xFE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0x00, 0xFE,
	0xFE, 0xFF, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0xFE, 0xFE, 0xFE, 0x01,
	0x01, 0x01, 0x01, 0x00, 0xFE, 0xFE, 0xFE, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFE, 0xFE,
	0xFE, 0xFF, 0x01, 0x01, 0x01, 0x01, 0x01, 0xFE, 0xFE, 0xFE, 0xFE, 0x01,
	0x01, 0x01, 0x01, 0xFE, 0xFE, 0xFE, 0xFE, 0x01,
};

static int cqspi_wait_for_bit(const struct cqspi_driver_platdata *ddata,
			      void __iomem *reg, const u32 mask, bool clr,
			      bool busywait)
{
	u64 timeout_us = CQSPI_TIMEOUT_MS * USEC_PER_MSEC;
	u32 val;

	if (busywait) {
		int ret = readl_relaxed_poll_timeout(reg, val,
						     (((clr ? ~val : val) & mask) == mask),
						     0, CQSPI_BUSYWAIT_TIMEOUT_US);

		if (ret != -ETIMEDOUT)
			return ret;

		timeout_us -= CQSPI_BUSYWAIT_TIMEOUT_US;
	}

	return readl_relaxed_poll_timeout(reg, val,
					  (((clr ? ~val : val) & mask) == mask),
					  10, timeout_us);
}

static bool cqspi_is_idle(struct cqspi_st *cqspi)
{
	u32 reg = readl(cqspi->iobase + CQSPI_REG_CONFIG);

	return reg & BIT(CQSPI_REG_CONFIG_IDLE_LSB);
}

static u32 cqspi_get_rd_sram_level(struct cqspi_st *cqspi)
{
	u32 reg = readl(cqspi->iobase + CQSPI_REG_SDRAMLEVEL);

	reg >>= CQSPI_REG_SDRAMLEVEL_RD_LSB;
	return reg & CQSPI_REG_SDRAMLEVEL_RD_MASK;
}

static u32 cqspi_get_versal_dma_status(struct cqspi_st *cqspi)
{
	u32 dma_status;

	dma_status = readl(cqspi->iobase +
					   CQSPI_REG_VERSAL_DMA_DST_I_STS);
	writel(dma_status, cqspi->iobase +
		   CQSPI_REG_VERSAL_DMA_DST_I_STS);

	return dma_status & CQSPI_REG_VERSAL_DMA_DST_DONE_MASK;
}

static irqreturn_t cqspi_irq_handler(int this_irq, void *dev)
{
	struct cqspi_st *cqspi = dev;
	const struct cqspi_driver_platdata *ddata = cqspi->ddata;
	unsigned int irq_status;

	/* Read interrupt status */
	irq_status = readl(cqspi->iobase + CQSPI_REG_IRQSTATUS);

	/* Clear interrupt */
	writel(irq_status, cqspi->iobase + CQSPI_REG_IRQSTATUS);

	if (cqspi->use_dma_read && ddata && ddata->get_dma_status) {
		if (ddata->get_dma_status(cqspi)) {
			complete(&cqspi->transfer_complete);
			return IRQ_HANDLED;
		}
	}

	else if (!cqspi->slow_sram)
		irq_status &= CQSPI_IRQ_MASK_RD | CQSPI_IRQ_MASK_WR;
	else
		irq_status &= CQSPI_REG_IRQ_WATERMARK | CQSPI_IRQ_MASK_WR;

	if (irq_status)
		complete(&cqspi->transfer_complete);

	return IRQ_HANDLED;
}

static unsigned int cqspi_calc_rdreg(const struct spi_mem_op *op)
{
	u32 rdreg = 0;

	rdreg |= CQSPI_OP_WIDTH(op->cmd) << CQSPI_REG_RD_INSTR_TYPE_INSTR_LSB;
	rdreg |= CQSPI_OP_WIDTH(op->addr) << CQSPI_REG_RD_INSTR_TYPE_ADDR_LSB;
	rdreg |= CQSPI_OP_WIDTH(op->data) << CQSPI_REG_RD_INSTR_TYPE_DATA_LSB;

	return rdreg;
}

static unsigned int cqspi_calc_dummy(const struct spi_mem_op *op)
{
	unsigned int dummy_clk;

	if (!op->dummy.nbytes)
		return 0;

	dummy_clk = op->dummy.nbytes * (8 / op->dummy.buswidth);
	if (op->cmd.dtr)
		dummy_clk /= 2;

	return dummy_clk;
}

static int cqspi_wait_idle(struct cqspi_st *cqspi)
{
	const unsigned int poll_idle_retry = 3;
	unsigned int count = 0;
	unsigned long timeout;

	timeout = jiffies + msecs_to_jiffies(CQSPI_TIMEOUT_MS);
	while (1) {
		/*
		 * Read few times in succession to ensure the controller
		 * is indeed idle, that is, the bit does not transition
		 * low again.
		 */
		if (cqspi_is_idle(cqspi))
			count++;
		else
			count = 0;

		if (count >= poll_idle_retry)
			return 0;

		if (time_after(jiffies, timeout)) {
			/* Timeout, in busy mode. */
			dev_err(&cqspi->pdev->dev,
				"QSPI is still busy after %dms timeout.\n",
				CQSPI_TIMEOUT_MS);
			return -ETIMEDOUT;
		}

		cpu_relax();
	}
}

static void cqspi_readdata_capture(struct cqspi_st *cqspi, const bool bypass,
				   const bool dqs, const unsigned int delay)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_READCAPTURE);

	if (bypass)
		reg |= BIT(CQSPI_REG_READCAPTURE_BYPASS_LSB);
	else
		reg &= ~BIT(CQSPI_REG_READCAPTURE_BYPASS_LSB);

	reg &= ~(CQSPI_REG_READCAPTURE_DELAY_MASK
		 << CQSPI_REG_READCAPTURE_DELAY_LSB);

	reg |= (delay & CQSPI_REG_READCAPTURE_DELAY_MASK)
	       << CQSPI_REG_READCAPTURE_DELAY_LSB;

	if (dqs)
		reg |= BIT(CQSPI_REG_READCAPTURE_DQS_LSB);
	else
		reg &= ~BIT(CQSPI_REG_READCAPTURE_DQS_LSB);

	writel(reg, reg_base + CQSPI_REG_READCAPTURE);
}

static void cqspi_phy_enable(struct cqspi_flash_pdata *f_pdata, bool enable)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	u32 reg;
	u8 dummy;

	if (enable) {
		cqspi_readdata_capture(cqspi, true, f_pdata->has_dqs,
				       f_pdata->phy_setting.read_delay);

		reg = readl(reg_base + CQSPI_REG_CONFIG);
		reg |= CQSPI_REG_CONFIG_PHY_EN | CQSPI_REG_CONFIG_PHY_PIPELINE;
		writel(reg, reg_base + CQSPI_REG_CONFIG);

		/* PHY mode requires one less dummy cycle */
		reg = readl(reg_base + CQSPI_REG_RD_INSTR);
		dummy = FIELD_GET(CQSPI_REG_RD_INSTR_DUMMY_MASK
					  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
				  reg);
		dummy--;
		reg &= ~(CQSPI_REG_RD_INSTR_DUMMY_MASK
			 << CQSPI_REG_RD_INSTR_DUMMY_LSB);
		reg |= FIELD_PREP(CQSPI_REG_RD_INSTR_DUMMY_MASK
					  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
				  dummy);
		writel(reg, reg_base + CQSPI_REG_RD_INSTR);
	} else {
		cqspi_readdata_capture(cqspi, !cqspi->rclk_en, false,
				       f_pdata->read_delay);

		reg = readl(reg_base + CQSPI_REG_CONFIG);
		reg &= ~(CQSPI_REG_CONFIG_PHY_EN |
			 CQSPI_REG_CONFIG_PHY_PIPELINE);
		writel(reg, reg_base + CQSPI_REG_CONFIG);

		/* Restore original dummy cycle count */
		reg = readl(reg_base + CQSPI_REG_RD_INSTR);
		dummy = FIELD_GET(CQSPI_REG_RD_INSTR_DUMMY_MASK
					  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
				  reg);
		dummy++;
		reg &= ~(CQSPI_REG_RD_INSTR_DUMMY_MASK
			 << CQSPI_REG_RD_INSTR_DUMMY_LSB);
		reg |= FIELD_PREP(CQSPI_REG_RD_INSTR_DUMMY_MASK
					  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
				  dummy);
		writel(reg, reg_base + CQSPI_REG_RD_INSTR);
	}

	cqspi_wait_idle(cqspi);
}

static int cqspi_exec_flash_cmd(struct cqspi_st *cqspi, unsigned int reg)
{
	void __iomem *reg_base = cqspi->iobase;
	int ret;

	/* Write the CMDCTRL without start execution. */
	writel(reg, reg_base + CQSPI_REG_CMDCTRL);
	/* Start execute */
	reg |= CQSPI_REG_CMDCTRL_EXECUTE_MASK;
	writel(reg, reg_base + CQSPI_REG_CMDCTRL);

	/* Polling for completion. */
	ret = cqspi_wait_for_bit(cqspi->ddata, reg_base + CQSPI_REG_CMDCTRL,
				 CQSPI_REG_CMDCTRL_INPROGRESS_MASK, 1, true);
	if (ret) {
		dev_err(&cqspi->pdev->dev,
			"Flash command execution timed out.\n");
		return ret;
	}

	/* Polling QSPI idle status. */
	return cqspi_wait_idle(cqspi);
}

static int cqspi_setup_opcode_ext(struct cqspi_flash_pdata *f_pdata,
				  const struct spi_mem_op *op,
				  unsigned int shift)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;
	u8 ext;

	if (op->cmd.nbytes != 2)
		return -EINVAL;

	/* Opcode extension is the LSB. */
	ext = op->cmd.opcode & 0xff;

	reg = readl(reg_base + CQSPI_REG_OP_EXT_LOWER);
	reg &= ~(0xff << shift);
	reg |= ext << shift;
	writel(reg, reg_base + CQSPI_REG_OP_EXT_LOWER);

	return 0;
}

static int cqspi_enable_dtr(struct cqspi_flash_pdata *f_pdata,
			    const struct spi_mem_op *op, unsigned int shift)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;
	int ret;

	reg = readl(reg_base + CQSPI_REG_CONFIG);

	/*
	 * We enable dual byte opcode here. The callers have to set up the
	 * extension opcode based on which type of operation it is.
	 */
	if (op->cmd.dtr) {
		reg |= CQSPI_REG_CONFIG_DTR_PROTO;
		reg |= CQSPI_REG_CONFIG_DUAL_OPCODE;

		/* Set up command opcode extension. */
		ret = cqspi_setup_opcode_ext(f_pdata, op, shift);
		if (ret)
			return ret;
	} else {
		unsigned int mask = CQSPI_REG_CONFIG_DTR_PROTO | CQSPI_REG_CONFIG_DUAL_OPCODE;
		/* Shortcut if DTR is already disabled. */
		if ((reg & mask) == 0)
			return 0;
		reg &= ~mask;
	}

	writel(reg, reg_base + CQSPI_REG_CONFIG);

	return cqspi_wait_idle(cqspi);
}

static int cqspi_command_read(struct cqspi_flash_pdata *f_pdata,
			      const struct spi_mem_op *op)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	u8 *rxbuf = op->data.buf.in;
	u8 opcode;
	size_t n_rx = op->data.nbytes;
	unsigned int rdreg;
	unsigned int reg;
	unsigned int dummy_clk;
	size_t read_len;
	int status;

	status = cqspi_enable_dtr(f_pdata, op, CQSPI_REG_OP_EXT_STIG_LSB);
	if (status)
		return status;

	if (!n_rx || n_rx > CQSPI_STIG_DATA_LEN_MAX || !rxbuf) {
		dev_err(&cqspi->pdev->dev,
			"Invalid input argument, len %zu rxbuf 0x%p\n",
			n_rx, rxbuf);
		return -EINVAL;
	}

	if (op->cmd.dtr)
		opcode = op->cmd.opcode >> 8;
	else
		opcode = op->cmd.opcode;

	reg = opcode << CQSPI_REG_CMDCTRL_OPCODE_LSB;

	rdreg = cqspi_calc_rdreg(op);
	writel(rdreg, reg_base + CQSPI_REG_RD_INSTR);

	dummy_clk = cqspi_calc_dummy(op);
	if (dummy_clk > CQSPI_DUMMY_CLKS_MAX)
		return -EOPNOTSUPP;

	if (dummy_clk)
		reg |= (dummy_clk & CQSPI_REG_CMDCTRL_DUMMY_MASK)
		     << CQSPI_REG_CMDCTRL_DUMMY_LSB;

	reg |= BIT(CQSPI_REG_CMDCTRL_RD_EN_LSB);

	/* 0 means 1 byte. */
	reg |= (((n_rx - 1) & CQSPI_REG_CMDCTRL_RD_BYTES_MASK)
		<< CQSPI_REG_CMDCTRL_RD_BYTES_LSB);

	/* setup ADDR BIT field */
	if (op->addr.nbytes) {
		reg |= BIT(CQSPI_REG_CMDCTRL_ADDR_EN_LSB);
		reg |= ((op->addr.nbytes - 1) &
			CQSPI_REG_CMDCTRL_ADD_BYTES_MASK)
			<< CQSPI_REG_CMDCTRL_ADD_BYTES_LSB;

		writel(op->addr.val, reg_base + CQSPI_REG_CMDADDRESS);
	}

	status = cqspi_exec_flash_cmd(cqspi, reg);
	if (status)
		return status;

	reg = readl(reg_base + CQSPI_REG_CMDREADDATALOWER);

	/* Put the read value into rx_buf */
	read_len = (n_rx > 4) ? 4 : n_rx;
	memcpy(rxbuf, &reg, read_len);
	rxbuf += read_len;

	if (n_rx > 4) {
		reg = readl(reg_base + CQSPI_REG_CMDREADDATAUPPER);

		read_len = n_rx - read_len;
		memcpy(rxbuf, &reg, read_len);
	}

	/* Reset CMD_CTRL Reg once command read completes */
	writel(0, reg_base + CQSPI_REG_CMDCTRL);

	return 0;
}

static int cqspi_command_write(struct cqspi_flash_pdata *f_pdata,
			       const struct spi_mem_op *op)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	u8 opcode;
	const u8 *txbuf = op->data.buf.out;
	size_t n_tx = op->data.nbytes;
	unsigned int reg;
	unsigned int data;
	size_t write_len;
	int ret;

	ret = cqspi_enable_dtr(f_pdata, op, CQSPI_REG_OP_EXT_STIG_LSB);
	if (ret)
		return ret;

	if (n_tx > CQSPI_STIG_DATA_LEN_MAX || (n_tx && !txbuf)) {
		dev_err(&cqspi->pdev->dev,
			"Invalid input argument, cmdlen %zu txbuf 0x%p\n",
			n_tx, txbuf);
		return -EINVAL;
	}

	reg = cqspi_calc_rdreg(op);
	writel(reg, reg_base + CQSPI_REG_RD_INSTR);

	if (op->cmd.dtr)
		opcode = op->cmd.opcode >> 8;
	else
		opcode = op->cmd.opcode;

	reg = opcode << CQSPI_REG_CMDCTRL_OPCODE_LSB;

	if (op->addr.nbytes) {
		reg |= BIT(CQSPI_REG_CMDCTRL_ADDR_EN_LSB);
		reg |= ((op->addr.nbytes - 1) &
			CQSPI_REG_CMDCTRL_ADD_BYTES_MASK)
			<< CQSPI_REG_CMDCTRL_ADD_BYTES_LSB;

		writel(op->addr.val, reg_base + CQSPI_REG_CMDADDRESS);
	}

	if (n_tx) {
		reg |= BIT(CQSPI_REG_CMDCTRL_WR_EN_LSB);
		reg |= ((n_tx - 1) & CQSPI_REG_CMDCTRL_WR_BYTES_MASK)
			<< CQSPI_REG_CMDCTRL_WR_BYTES_LSB;
		data = 0;
		write_len = (n_tx > 4) ? 4 : n_tx;
		memcpy(&data, txbuf, write_len);
		txbuf += write_len;
		writel(data, reg_base + CQSPI_REG_CMDWRITEDATALOWER);

		if (n_tx > 4) {
			data = 0;
			write_len = n_tx - 4;
			memcpy(&data, txbuf, write_len);
			writel(data, reg_base + CQSPI_REG_CMDWRITEDATAUPPER);
		}
	}

	ret = cqspi_exec_flash_cmd(cqspi, reg);

	/* Reset CMD_CTRL Reg once command write completes */
	writel(0, reg_base + CQSPI_REG_CMDCTRL);

	return ret;
}

static int cqspi_read_setup(struct cqspi_flash_pdata *f_pdata,
			    const struct spi_mem_op *op)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	unsigned int dummy_clk = 0;
	unsigned int reg;
	int ret;
	u8 opcode;

	ret = cqspi_enable_dtr(f_pdata, op, CQSPI_REG_OP_EXT_READ_LSB);
	if (ret)
		return ret;

	if (op->cmd.dtr)
		opcode = op->cmd.opcode >> 8;
	else
		opcode = op->cmd.opcode;

	reg = opcode << CQSPI_REG_RD_INSTR_OPCODE_LSB;
	reg |= cqspi_calc_rdreg(op);

	/* Setup dummy clock cycles */
	dummy_clk = cqspi_calc_dummy(op);

	if (dummy_clk > CQSPI_DUMMY_CLKS_MAX)
		return -EOPNOTSUPP;

	if (dummy_clk)
		reg |= (dummy_clk & CQSPI_REG_RD_INSTR_DUMMY_MASK)
		       << CQSPI_REG_RD_INSTR_DUMMY_LSB;

	writel(reg, reg_base + CQSPI_REG_RD_INSTR);

	/* Set address width */
	reg = readl(reg_base + CQSPI_REG_SIZE);
	reg &= ~CQSPI_REG_SIZE_ADDRESS_MASK;
	reg |= (op->addr.nbytes - 1);
	writel(reg, reg_base + CQSPI_REG_SIZE);
	readl(reg_base + CQSPI_REG_SIZE); /* Flush posted write. */
	return 0;
}

static int cqspi_indirect_read_execute(struct cqspi_flash_pdata *f_pdata,
				       u8 *rxbuf, loff_t from_addr,
				       const size_t n_rx)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	bool use_irq = !(cqspi->ddata && cqspi->ddata->quirks & CQSPI_RD_NO_IRQ);
	struct device *dev = &cqspi->pdev->dev;
	void __iomem *reg_base = cqspi->iobase;
	void __iomem *ahb_base = cqspi->ahb_base;
	unsigned int remaining = n_rx;
	unsigned int mod_bytes = n_rx % 4;
	unsigned int bytes_to_read = 0;
	u8 *rxbuf_end = rxbuf + n_rx;
	int ret = 0;

	if (!refcount_read(&cqspi->refcount))
		return -ENODEV;

	writel(from_addr, reg_base + CQSPI_REG_INDIRECTRDSTARTADDR);
	writel(remaining, reg_base + CQSPI_REG_INDIRECTRDBYTES);

	/* Clear all interrupts. */
	writel(CQSPI_IRQ_STATUS_MASK, reg_base + CQSPI_REG_IRQSTATUS);

	/*
	 * On SoCFPGA platform reading the SRAM is slow due to
	 * hardware limitation and causing read interrupt storm to CPU,
	 * so enabling only watermark interrupt to disable all read
	 * interrupts later as we want to run "bytes to read" loop with
	 * all the read interrupts disabled for max performance.
	 */

	if (use_irq && cqspi->slow_sram)
		writel(CQSPI_REG_IRQ_WATERMARK, reg_base + CQSPI_REG_IRQMASK);
	else if (use_irq)
		writel(CQSPI_IRQ_MASK_RD, reg_base + CQSPI_REG_IRQMASK);
	else
		writel(0, reg_base + CQSPI_REG_IRQMASK);

	reinit_completion(&cqspi->transfer_complete);
	writel(CQSPI_REG_INDIRECTRD_START_MASK,
	       reg_base + CQSPI_REG_INDIRECTRD);
	readl(reg_base + CQSPI_REG_INDIRECTRD); /* Flush posted write. */

	while (remaining > 0) {
		ret = 0;
		if (use_irq &&
		    !wait_for_completion_timeout(&cqspi->transfer_complete,
						 msecs_to_jiffies(CQSPI_READ_TIMEOUT_MS)))
			ret = -ETIMEDOUT;

		/*
		 * Disable all read interrupts until
		 * we are out of "bytes to read"
		 */
		if (cqspi->slow_sram)
			writel(0x0, reg_base + CQSPI_REG_IRQMASK);

		/*
		 * Prevent lost interrupt and race condition by reinitializing early.
		 * A spurious wakeup and another wait cycle can occur here,
		 * which is preferable to waiting until timeout if interrupt is lost.
		 */
		if (use_irq)
			reinit_completion(&cqspi->transfer_complete);

		bytes_to_read = cqspi_get_rd_sram_level(cqspi);

		if (ret && bytes_to_read == 0) {
			dev_err(dev, "Indirect read timeout, no bytes\n");
			goto failrd;
		}

		while (bytes_to_read != 0) {
			unsigned int word_remain = round_down(remaining, 4);

			bytes_to_read *= cqspi->fifo_width;
			bytes_to_read = bytes_to_read > remaining ?
					remaining : bytes_to_read;
			bytes_to_read = round_down(bytes_to_read, 4);
			/* Read 4 byte word chunks then single bytes */
			if (bytes_to_read) {
				ioread32_rep(ahb_base, rxbuf,
					     (bytes_to_read / 4));
			} else if (!word_remain && mod_bytes) {
				unsigned int temp = ioread32(ahb_base);

				bytes_to_read = mod_bytes;
				memcpy(rxbuf, &temp, min((unsigned int)
							 (rxbuf_end - rxbuf),
							 bytes_to_read));
			}
			rxbuf += bytes_to_read;
			remaining -= bytes_to_read;
			bytes_to_read = cqspi_get_rd_sram_level(cqspi);
		}

		if (use_irq && remaining > 0) {
			if (cqspi->slow_sram)
				writel(CQSPI_REG_IRQ_WATERMARK, reg_base + CQSPI_REG_IRQMASK);
		}
	}

	/* Check indirect done status */
	ret = cqspi_wait_for_bit(cqspi->ddata, reg_base + CQSPI_REG_INDIRECTRD,
				 CQSPI_REG_INDIRECTRD_DONE_MASK, 0, true);
	if (ret) {
		dev_err(dev, "Indirect read completion error (%i)\n", ret);
		goto failrd;
	}

	/* Disable interrupt */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Clear indirect completion status */
	writel(CQSPI_REG_INDIRECTRD_DONE_MASK, reg_base + CQSPI_REG_INDIRECTRD);

	return 0;

failrd:
	/* Disable interrupt */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Cancel the indirect read */
	writel(CQSPI_REG_INDIRECTRD_CANCEL_MASK,
	       reg_base + CQSPI_REG_INDIRECTRD);
	return ret;
}

static void cqspi_device_reset(struct cqspi_st *cqspi)
{
	u32 reg;

	reg = readl(cqspi->iobase + CQSPI_REG_CONFIG);
	reg |= CQSPI_REG_CONFIG_RESET_CFG_FLD_MASK;
	writel(reg, cqspi->iobase + CQSPI_REG_CONFIG);
	/*
	 * NOTE: Delay timing implementation is derived from
	 * spi_nor_hw_reset()
	 */
	writel(reg & ~CQSPI_REG_CONFIG_RESET_PIN_FLD_MASK, cqspi->iobase + CQSPI_REG_CONFIG);
	usleep_range(1, 5);
	writel(reg | CQSPI_REG_CONFIG_RESET_PIN_FLD_MASK, cqspi->iobase + CQSPI_REG_CONFIG);
	usleep_range(100, 150);
	writel(reg & ~CQSPI_REG_CONFIG_RESET_PIN_FLD_MASK, cqspi->iobase + CQSPI_REG_CONFIG);
	usleep_range(1000, 1200);
}

static void cqspi_controller_enable(struct cqspi_st *cqspi, bool enable)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_CONFIG);

	if (enable)
		reg |= CQSPI_REG_CONFIG_ENABLE_MASK;
	else
		reg &= ~CQSPI_REG_CONFIG_ENABLE_MASK;

	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

static int cqspi_versal_indirect_read_dma(struct cqspi_flash_pdata *f_pdata,
					  u_char *rxbuf, loff_t from_addr,
					  size_t n_rx)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	struct device *dev = &cqspi->pdev->dev;
	void __iomem *reg_base = cqspi->iobase;
	u32 reg, bytes_to_dma;
	loff_t addr = from_addr;
	void *buf = rxbuf;
	dma_addr_t dma_addr;
	u8 bytes_rem;
	int ret = 0;

	bytes_rem = n_rx % 4;
	bytes_to_dma = (n_rx - bytes_rem);

	if (!bytes_to_dma)
		goto nondmard;

	ret = zynqmp_pm_ospi_mux_select(cqspi->pd_dev_id, PM_OSPI_MUX_SEL_DMA);
	if (ret)
		return ret;

	cqspi_controller_enable(cqspi, 0);

	reg = readl(cqspi->iobase + CQSPI_REG_CONFIG);
	reg |= CQSPI_REG_CONFIG_DMA_MASK;
	writel(reg, cqspi->iobase + CQSPI_REG_CONFIG);

	cqspi_controller_enable(cqspi, 1);

	dma_addr = dma_map_single(dev, rxbuf, bytes_to_dma, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		dev_err(dev, "dma mapping failed\n");
		return -ENOMEM;
	}

	writel(from_addr, reg_base + CQSPI_REG_INDIRECTRDSTARTADDR);
	writel(bytes_to_dma, reg_base + CQSPI_REG_INDIRECTRDBYTES);
	writel(CQSPI_REG_VERSAL_ADDRRANGE_WIDTH_VAL,
	       reg_base + CQSPI_REG_INDTRIG_ADDRRANGE);

	/* Clear all interrupts. */
	writel(CQSPI_IRQ_STATUS_MASK, reg_base + CQSPI_REG_IRQSTATUS);

	/* Enable DMA done interrupt */
	writel(CQSPI_REG_VERSAL_DMA_DST_DONE_MASK,
	       reg_base + CQSPI_REG_VERSAL_DMA_DST_I_EN);

	/* Default DMA periph configuration */
	writel(CQSPI_REG_VERSAL_DMA_VAL, reg_base + CQSPI_REG_DMA);

	/* Configure DMA Dst address */
	writel(lower_32_bits(dma_addr),
	       reg_base + CQSPI_REG_VERSAL_DMA_DST_ADDR);
	writel(upper_32_bits(dma_addr),
	       reg_base + CQSPI_REG_VERSAL_DMA_DST_ADDR_MSB);

	/* Configure DMA Src address */
	writel(cqspi->trigger_address, reg_base +
	       CQSPI_REG_VERSAL_DMA_SRC_ADDR);

	/* Set DMA destination size */
	writel(bytes_to_dma, reg_base + CQSPI_REG_VERSAL_DMA_DST_SIZE);

	/* Set DMA destination control */
	writel(CQSPI_REG_VERSAL_DMA_DST_CTRL_VAL,
	       reg_base + CQSPI_REG_VERSAL_DMA_DST_CTRL);

	writel(CQSPI_REG_INDIRECTRD_START_MASK,
	       reg_base + CQSPI_REG_INDIRECTRD);

	reinit_completion(&cqspi->transfer_complete);

	if (!wait_for_completion_timeout(&cqspi->transfer_complete,
					 msecs_to_jiffies(max_t(size_t, bytes_to_dma, 500)))) {
		ret = -ETIMEDOUT;
		goto failrd;
	}

	/* Disable DMA interrupt */
	writel(0x0, cqspi->iobase + CQSPI_REG_VERSAL_DMA_DST_I_DIS);

	/* Clear indirect completion status */
	writel(CQSPI_REG_INDIRECTRD_DONE_MASK,
	       cqspi->iobase + CQSPI_REG_INDIRECTRD);
	dma_unmap_single(dev, dma_addr, bytes_to_dma, DMA_FROM_DEVICE);

	cqspi_controller_enable(cqspi, 0);

	reg = readl(cqspi->iobase + CQSPI_REG_CONFIG);
	reg &= ~CQSPI_REG_CONFIG_DMA_MASK;
	writel(reg, cqspi->iobase + CQSPI_REG_CONFIG);

	cqspi_controller_enable(cqspi, 1);

	ret = zynqmp_pm_ospi_mux_select(cqspi->pd_dev_id,
					PM_OSPI_MUX_SEL_LINEAR);
	if (ret)
		return ret;

nondmard:
	if (bytes_rem) {
		addr += bytes_to_dma;
		buf += bytes_to_dma;
		ret = cqspi_indirect_read_execute(f_pdata, buf, addr,
						  bytes_rem);
		if (ret)
			return ret;
	}

	return 0;

failrd:
	/* Disable DMA interrupt */
	writel(0x0, reg_base + CQSPI_REG_VERSAL_DMA_DST_I_DIS);

	/* Cancel the indirect read */
	writel(CQSPI_REG_INDIRECTWR_CANCEL_MASK,
	       reg_base + CQSPI_REG_INDIRECTRD);

	dma_unmap_single(dev, dma_addr, bytes_to_dma, DMA_FROM_DEVICE);

	reg = readl(cqspi->iobase + CQSPI_REG_CONFIG);
	reg &= ~CQSPI_REG_CONFIG_DMA_MASK;
	writel(reg, cqspi->iobase + CQSPI_REG_CONFIG);

	zynqmp_pm_ospi_mux_select(cqspi->pd_dev_id, PM_OSPI_MUX_SEL_LINEAR);

	return ret;
}

static int cqspi_write_setup(struct cqspi_flash_pdata *f_pdata,
			     const struct spi_mem_op *op)
{
	unsigned int reg;
	int ret;
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	u8 opcode;

	ret = cqspi_enable_dtr(f_pdata, op, CQSPI_REG_OP_EXT_WRITE_LSB);
	if (ret)
		return ret;

	if (op->cmd.dtr)
		opcode = op->cmd.opcode >> 8;
	else
		opcode = op->cmd.opcode;

	/* Set opcode. */
	reg = opcode << CQSPI_REG_WR_INSTR_OPCODE_LSB;
	reg |= CQSPI_OP_WIDTH(op->data) << CQSPI_REG_WR_INSTR_TYPE_DATA_LSB;
	reg |= CQSPI_OP_WIDTH(op->addr) << CQSPI_REG_WR_INSTR_TYPE_ADDR_LSB;
	writel(reg, reg_base + CQSPI_REG_WR_INSTR);
	reg = cqspi_calc_rdreg(op);
	writel(reg, reg_base + CQSPI_REG_RD_INSTR);

	/*
	 * SPI NAND flashes require the address of the status register to be
	 * passed in the Read SR command. Also, some SPI NOR flashes like the
	 * cypress Semper flash expect a 4-byte dummy address in the Read SR
	 * command in DTR mode.
	 *
	 * But this controller does not support address phase in the Read SR
	 * command when doing auto-HW polling. So, disable write completion
	 * polling on the controller's side. spinand and spi-nor will take
	 * care of polling the status register.
	 */
	if (cqspi->wr_completion) {
		reg = readl(reg_base + CQSPI_REG_WR_COMPLETION_CTRL);
		reg |= CQSPI_REG_WR_DISABLE_AUTO_POLL;
		writel(reg, reg_base + CQSPI_REG_WR_COMPLETION_CTRL);
		/*
		 * DAC mode require auto polling as flash needs to be polled
		 * for write completion in case of bubble in SPI transaction
		 * due to slow CPU/DMA master.
		 */
		cqspi->use_direct_mode_wr = false;
	}

	reg = readl(reg_base + CQSPI_REG_SIZE);
	reg &= ~CQSPI_REG_SIZE_ADDRESS_MASK;
	reg |= (op->addr.nbytes - 1);
	writel(reg, reg_base + CQSPI_REG_SIZE);
	readl(reg_base + CQSPI_REG_SIZE); /* Flush posted write. */
	return 0;
}

static int cqspi_indirect_write_execute(struct cqspi_flash_pdata *f_pdata,
					loff_t to_addr, const u8 *txbuf,
					const size_t n_tx)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	struct device *dev = &cqspi->pdev->dev;
	void __iomem *reg_base = cqspi->iobase;
	unsigned int remaining = n_tx;
	unsigned int write_bytes;
	bool use_phy_write;
	int ret;

	if (!refcount_read(&cqspi->refcount))
		return -ENODEV;

	writel(to_addr, reg_base + CQSPI_REG_INDIRECTWRSTARTADDR);
	writel(remaining, reg_base + CQSPI_REG_INDIRECTWRBYTES);

	/* Clear all interrupts. */
	writel(CQSPI_IRQ_STATUS_MASK, reg_base + CQSPI_REG_IRQSTATUS);

	writel(CQSPI_IRQ_MASK_WR, reg_base + CQSPI_REG_IRQMASK);

	reinit_completion(&cqspi->transfer_complete);
	writel(CQSPI_REG_INDIRECTWR_START_MASK,
	       reg_base + CQSPI_REG_INDIRECTWR);
	readl(reg_base + CQSPI_REG_INDIRECTWR); /* Flush posted write. */

	/*
	 * As per 66AK2G02 TRM SPRUHY8F section 11.15.5.3 Indirect Access
	 * Controller programming sequence, couple of cycles of
	 * QSPI_REF_CLK delay is required for the above bit to
	 * be internally synchronized by the QSPI module. Provide 5
	 * cycles of delay.
	 */
	if (cqspi->wr_delay)
		ndelay(cqspi->wr_delay);

	/*
	 * If a hazard exists between the APB and AHB interfaces, perform a
	 * dummy readback from the controller to ensure synchronization.
	 */
	if (cqspi->apb_ahb_hazard)
		readl(reg_base + CQSPI_REG_INDIRECTWR);

	/* Use PHY only for large writes where setup overhead is amortized */
	use_phy_write = n_tx >= SZ_1K && f_pdata->use_phy;

	if (use_phy_write)
		cqspi_phy_enable(f_pdata, true);

	while (remaining > 0) {
		size_t write_words, mod_bytes;

		write_bytes = remaining;
		write_words = write_bytes / 4;
		mod_bytes = write_bytes % 4;
		/* Write 4 bytes at a time then single bytes. */
		if (write_words) {
			iowrite32_rep(cqspi->ahb_base, txbuf, write_words);
			txbuf += (write_words * 4);
		}
		if (mod_bytes) {
			unsigned int temp = 0xFFFFFFFF;

			memcpy(&temp, txbuf, mod_bytes);
			iowrite32(temp, cqspi->ahb_base);
			txbuf += mod_bytes;
		}

		if (!wait_for_completion_timeout(&cqspi->transfer_complete,
						 msecs_to_jiffies(CQSPI_TIMEOUT_MS))) {
			dev_err(dev, "Indirect write timeout\n");
			ret = -ETIMEDOUT;
			goto failwr;
		}

		remaining -= write_bytes;

		if (remaining > 0)
			reinit_completion(&cqspi->transfer_complete);
	}

	/* Check indirect done status */
	ret = cqspi_wait_for_bit(cqspi->ddata, reg_base + CQSPI_REG_INDIRECTWR,
				 CQSPI_REG_INDIRECTWR_DONE_MASK, 0, false);
	if (ret) {
		dev_err(dev, "Indirect write completion error (%i)\n", ret);
		goto failwr;
	}

	if (use_phy_write)
		cqspi_phy_enable(f_pdata, false);

	/* Disable interrupt. */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Clear indirect completion status */
	writel(CQSPI_REG_INDIRECTWR_DONE_MASK, reg_base + CQSPI_REG_INDIRECTWR);

	cqspi_wait_idle(cqspi);

	return 0;

failwr:
	if (use_phy_write)
		cqspi_phy_enable(f_pdata, false);

	/* Disable interrupt. */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Cancel the indirect write */
	writel(CQSPI_REG_INDIRECTWR_CANCEL_MASK,
	       reg_base + CQSPI_REG_INDIRECTWR);
	return ret;
}

static void cqspi_chipselect(struct cqspi_flash_pdata *f_pdata)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	unsigned int chip_select = f_pdata->cs;
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	if (cqspi->is_decoded_cs) {
		reg |= CQSPI_REG_CONFIG_DECODE_MASK;
	} else {
		reg &= ~CQSPI_REG_CONFIG_DECODE_MASK;

		/* Convert CS if without decoder.
		 * CS0 to 4b'1110
		 * CS1 to 4b'1101
		 * CS2 to 4b'1011
		 * CS3 to 4b'0111
		 */
		chip_select = 0xF & ~BIT(chip_select);
	}

	reg &= ~(CQSPI_REG_CONFIG_CHIPSELECT_MASK
		 << CQSPI_REG_CONFIG_CHIPSELECT_LSB);
	reg |= (chip_select & CQSPI_REG_CONFIG_CHIPSELECT_MASK)
	    << CQSPI_REG_CONFIG_CHIPSELECT_LSB;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

static unsigned int calculate_ticks_for_ns(const unsigned int ref_clk_hz,
					   const unsigned int ns_val)
{
	unsigned int ticks;

	ticks = ref_clk_hz / 1000;	/* kHz */
	ticks = DIV_ROUND_UP(ticks * ns_val, 1000000);

	return ticks;
}

static void cqspi_delay(struct cqspi_flash_pdata *f_pdata)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *iobase = cqspi->iobase;
	const unsigned int ref_clk_hz = cqspi->master_ref_clk_hz;
	unsigned int tshsl, tchsh, tslch, tsd2d;
	unsigned int reg;
	unsigned int tsclk;

	/* calculate the number of ref ticks for one sclk tick */
	tsclk = DIV_ROUND_UP(ref_clk_hz, cqspi->sclk);

	tshsl = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tshsl_ns);
	/* this particular value must be at least one sclk */
	if (tshsl < tsclk)
		tshsl = tsclk;

	tchsh = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tchsh_ns);
	tslch = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tslch_ns);
	tsd2d = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tsd2d_ns);

	reg = (tshsl & CQSPI_REG_DELAY_TSHSL_MASK)
	       << CQSPI_REG_DELAY_TSHSL_LSB;
	reg |= (tchsh & CQSPI_REG_DELAY_TCHSH_MASK)
		<< CQSPI_REG_DELAY_TCHSH_LSB;
	reg |= (tslch & CQSPI_REG_DELAY_TSLCH_MASK)
		<< CQSPI_REG_DELAY_TSLCH_LSB;
	reg |= (tsd2d & CQSPI_REG_DELAY_TSD2D_MASK)
		<< CQSPI_REG_DELAY_TSD2D_LSB;
	writel(reg, iobase + CQSPI_REG_DELAY);
}

static void cqspi_config_baudrate_div(struct cqspi_st *cqspi)
{
	const unsigned int ref_clk_hz = cqspi->master_ref_clk_hz;
	void __iomem *reg_base = cqspi->iobase;
	u32 reg, div;

	/* Recalculate the baudrate divisor based on QSPI specification. */
	div = DIV_ROUND_UP(ref_clk_hz, 2 * cqspi->sclk) - 1;

	/* Maximum baud divisor */
	if (div > CQSPI_REG_CONFIG_BAUD_MASK) {
		div = CQSPI_REG_CONFIG_BAUD_MASK;
		dev_warn(&cqspi->pdev->dev,
			"Unable to adjust clock <= %d hz. Reduced to %d hz\n",
			cqspi->sclk, ref_clk_hz/((div+1)*2));
	}

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~(CQSPI_REG_CONFIG_BAUD_MASK << CQSPI_REG_CONFIG_BAUD_LSB);
	reg |= (div & CQSPI_REG_CONFIG_BAUD_MASK) << CQSPI_REG_CONFIG_BAUD_LSB;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

static void cqspi_set_dll(void __iomem *reg_base, u8 rx_dll, u8 tx_dll)
{
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_PHY_CONFIG);
	reg &= ~((CQSPI_REG_PHY_CONFIG_RX_DEL_MASK
		  << CQSPI_REG_PHY_CONFIG_RX_DEL_LSB) |
		 (CQSPI_REG_PHY_CONFIG_TX_DEL_MASK
		  << CQSPI_REG_PHY_CONFIG_TX_DEL_LSB));
	reg |= ((rx_dll & CQSPI_REG_PHY_CONFIG_RX_DEL_MASK)
		<< CQSPI_REG_PHY_CONFIG_RX_DEL_LSB) |
	       ((tx_dll & CQSPI_REG_PHY_CONFIG_TX_DEL_MASK)
		<< CQSPI_REG_PHY_CONFIG_TX_DEL_LSB) |
	       CQSPI_REG_PHY_CONFIG_RESYNC;
	writel(reg, reg_base + CQSPI_REG_PHY_CONFIG);
}

static int cqspi_resync_dll(struct cqspi_st *cqspi)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;
	int ret;

	ret = cqspi_wait_idle(cqspi);
	if (ret)
		return ret;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~(CQSPI_REG_CONFIG_ENABLE_MASK);
	writel(reg, reg_base + CQSPI_REG_CONFIG);

	reg = readl(reg_base + CQSPI_REG_PHY_CONFIG);
	reg &= ~(CQSPI_REG_PHY_CONFIG_DLL_RESET |
		 CQSPI_REG_PHY_CONFIG_RESYNC);
	writel(reg, reg_base + CQSPI_REG_PHY_CONFIG);

	reg = readl(reg_base + CQSPI_REG_PHY_DLL_MASTER);
	reg |= (CQSPI_REG_PHY_DLL_MASTER_INIT_DELAY_VAL
		<< CQSPI_REG_PHY_DLL_MASTER_INIT_DELAY_LSB);
	writel(reg, reg_base + CQSPI_REG_PHY_DLL_MASTER);

	reg = readl(reg_base + CQSPI_REG_PHY_CONFIG);
	reg |= CQSPI_REG_PHY_CONFIG_DLL_RESET;
	writel(reg, reg_base + CQSPI_REG_PHY_CONFIG);

	ret = readl_poll_timeout(reg_base + CQSPI_REG_DLL_OBS_LOW, reg,
				 (reg & CQSPI_REG_DLL_OBS_LOW_DLL_LOCK), 0,
				 CQSPI_DLL_TIMEOUT_US);
	if (ret)
		return ret;

	ret = readl_poll_timeout(reg_base + CQSPI_REG_DLL_OBS_LOW, reg,
				 (reg & CQSPI_REG_DLL_OBS_LOW_LOOPBACK_LOCK), 0,
				 CQSPI_DLL_TIMEOUT_US);
	if (ret)
		return ret;

	reg = readl(reg_base + CQSPI_REG_PHY_CONFIG);
	reg |= CQSPI_REG_PHY_CONFIG_RESYNC;
	writel(reg, reg_base + CQSPI_REG_PHY_CONFIG);

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg |= CQSPI_REG_CONFIG_ENABLE_MASK;
	writel(reg, reg_base + CQSPI_REG_CONFIG);

	return 0;
}

static int cqspi_phy_apply_setting(struct cqspi_flash_pdata *f_pdata,
				   struct phy_setting *phy)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	unsigned int reg;

	reg = readl(cqspi->iobase + CQSPI_REG_READCAPTURE);
	reg |= BIT(CQSPI_REG_READCAPTURE_EDGE_LSB);
	writel(reg, cqspi->iobase + CQSPI_REG_READCAPTURE);

	cqspi_set_dll(cqspi->iobase, phy->rx, phy->tx);
	f_pdata->phy_setting.read_delay = phy->read_delay;

	return cqspi_resync_dll(cqspi);
}

static void cqspi_configure(struct cqspi_flash_pdata *f_pdata,
			    unsigned long sclk)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	void __iomem *reg_base = cqspi->iobase;
	int switch_cs = (cqspi->current_cs != f_pdata->cs);
	int switch_ck = (cqspi->sclk != sclk);
	unsigned int reg;
	int ret;

	if (switch_cs || switch_ck)
		cqspi_controller_enable(cqspi, 0);

	/* Switch chip select. */
	if (switch_cs) {
		cqspi->current_cs = f_pdata->cs;
		cqspi_chipselect(f_pdata);

		if (f_pdata->has_dqs) {
			reg = readl(reg_base + CQSPI_REG_CONFIG);
			reg |= (CQSPI_REG_CONFIG_DTR_PROTO |
				CQSPI_REG_CONFIG_DUAL_OPCODE);
			writel(reg, reg_base + CQSPI_REG_CONFIG);
		} else {
			reg = readl(reg_base + CQSPI_REG_CONFIG);
			reg &= ~(CQSPI_REG_CONFIG_DTR_PROTO |
				 CQSPI_REG_CONFIG_DUAL_OPCODE);
			writel(reg, reg_base + CQSPI_REG_CONFIG);
		}

		if (f_pdata->use_phy) {
			ret = cqspi_phy_apply_setting(f_pdata,
						      &f_pdata->phy_setting);

			if (ret)
				dev_err(&cqspi->pdev->dev,
					"Applying phy_setting failed during switching chip select\n");
		}
	}

	/* Setup baudrate divisor and delays */
	if (switch_ck) {
		cqspi->sclk = sclk;
		cqspi_config_baudrate_div(cqspi);
		cqspi_delay(f_pdata);
		cqspi_readdata_capture(cqspi, !cqspi->rclk_en, false,
				       f_pdata->read_delay);
	}

	if (switch_cs || switch_ck)
		cqspi_controller_enable(cqspi, 1);
}

static ssize_t cqspi_write(struct cqspi_flash_pdata *f_pdata,
			   const struct spi_mem_op *op)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	loff_t to = op->addr.val;
	size_t len = op->data.nbytes;
	const u_char *buf = op->data.buf.out;
	int ret;

	ret = cqspi_write_setup(f_pdata, op);
	if (ret)
		return ret;

	/*
	 * Some flashes like the Cypress Semper flash expect a dummy 4-byte
	 * address (all 0s) with the read status register command in DTR mode.
	 * But this controller does not support sending dummy address bytes to
	 * the flash when it is polling the write completion register in DTR
	 * mode. So, we can not use direct mode when in DTR mode for writing
	 * data.
	 */
	if (!op->cmd.dtr && cqspi->use_direct_mode &&
	    cqspi->use_direct_mode_wr && ((to + len) <= cqspi->ahb_size)) {
		memcpy_toio(cqspi->ahb_base + to, buf, len);
		return cqspi_wait_idle(cqspi);
	}

	return cqspi_indirect_write_execute(f_pdata, to, buf, len);
}

static void cqspi_rx_dma_callback(void *param)
{
	struct cqspi_st *cqspi = param;

	complete(&cqspi->rx_dma_complete);
}

static bool cqspi_use_phy(struct cqspi_flash_pdata *f_pdata,
			  const struct spi_mem_op *op)
{
	if (!f_pdata->use_phy || op->data.nbytes < 16)
		return false;

	return op->max_freq > f_pdata->non_phy_clk_rate;
}

static int cqspi_direct_read_dma(struct cqspi_flash_pdata *f_pdata, u_char *buf,
				 loff_t from, size_t len)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	struct device *dev = &cqspi->pdev->dev;
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	dma_addr_t dma_src = (dma_addr_t)cqspi->mmap_phys_base + from;
	int ret = 0;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	dma_addr_t dma_dst;
	struct device *ddev;

	ddev = cqspi->rx_chan->device->dev;
	dma_dst = dma_map_single(ddev, buf, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(ddev, dma_dst)) {
		dev_err(dev, "dma mapping failed\n");
		return -ENOMEM;
	}
	tx = dmaengine_prep_dma_memcpy(cqspi->rx_chan, dma_dst, dma_src, len,
				       flags);
	if (!tx) {
		dev_err(dev, "device_prep_dma_memcpy error\n");
		ret = -EIO;
		goto err_unmap;
	}

	tx->callback = cqspi_rx_dma_callback;
	tx->callback_param = cqspi;
	cookie = tx->tx_submit(tx);
	reinit_completion(&cqspi->rx_dma_complete);

	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(dev, "dma_submit_error %d\n", cookie);
		ret = -EIO;
		goto err_unmap;
	}

	dma_async_issue_pending(cqspi->rx_chan);
	if (!wait_for_completion_timeout(&cqspi->rx_dma_complete,
					 msecs_to_jiffies(max_t(size_t, len, 500)))) {
		dmaengine_terminate_sync(cqspi->rx_chan);
		dev_err(dev, "DMA wait_for_completion_timeout\n");
		ret = -ETIMEDOUT;
		goto err_unmap;
	}

err_unmap:
	dma_unmap_single(ddev, dma_dst, len, DMA_FROM_DEVICE);

	return ret;
}

static void cqspi_memcpy_fromio(const struct spi_mem_op *op, void *to,
				const void __iomem *from, size_t count)
{
	if (op->data.buswidth == 8 && op->data.dtr) {
		unsigned long from_addr = (unsigned long)from;

		/* Handle unaligned start with 2-byte read */
		if (count && !IS_ALIGNED(from_addr, 4)) {
			*(u16 *)to = __raw_readw(from);
			from += 2;
			to += 2;
			count -= 2;
		}

		/* Use 4-byte reads for aligned bulk (no readq for 32-bit) */
		if (count >= 4) {
			size_t len = round_down(count, 4);

			memcpy_fromio(to, from, len);
			from += len;
			to += len;
			count -= len;
		}

		/* Handle remaining 2 bytes */
		if (count)
			*(u16 *)to = __raw_readw(from);

		return;
	}

	memcpy_fromio(to, from, count);
}

static int cqspi_direct_read_execute(struct cqspi_flash_pdata *f_pdata,
				     const struct spi_mem_op *op)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	loff_t from = op->addr.val;
	loff_t from_aligned, to_aligned;
	size_t len = op->data.nbytes;
	size_t len_aligned;
	u_char *buf = op->data.buf.in;
	int ret;

	if (!cqspi->rx_chan || !virt_addr_valid(buf) || len <= 16) {
		cqspi_memcpy_fromio(op, buf, cqspi->ahb_base + from, len);
		return 0;
	}

	if (!cqspi_use_phy(f_pdata, op))
		return cqspi_direct_read_dma(f_pdata, buf, from, len);

	/* Split into unaligned head, aligned middle, unaligned tail */
	from_aligned = ALIGN(from, 16);
	to_aligned = ALIGN_DOWN(from + len, 16);
	len_aligned = to_aligned - from_aligned;

	if (from != from_aligned) {
		ret = cqspi_direct_read_dma(f_pdata, buf, from,
					    from_aligned - from);
		if (ret)
			return ret;
		buf += from_aligned - from;
	}

	if (len_aligned) {
		cqspi_phy_enable(f_pdata, true);
		ret = cqspi_direct_read_dma(f_pdata, buf, from_aligned,
					    len_aligned);
		cqspi_phy_enable(f_pdata, false);
		if (ret)
			return ret;
		buf += len_aligned;
	}

	if (to_aligned != (from + len)) {
		ret = cqspi_direct_read_dma(f_pdata, buf, to_aligned,
					    (from + len) - to_aligned);
		if (ret)
			return ret;
		buf += (from + len) - to_aligned;
	}

	return 0;
}

static ssize_t cqspi_read(struct cqspi_flash_pdata *f_pdata,
			  const struct spi_mem_op *op)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	const struct cqspi_driver_platdata *ddata = cqspi->ddata;
	loff_t from = op->addr.val;
	size_t len = op->data.nbytes;
	u_char *buf = op->data.buf.in;
	u64 dma_align = (u64)(uintptr_t)buf;
	int ret;

	ret = cqspi_read_setup(f_pdata, op);
	if (ret)
		return ret;

	if (cqspi->use_direct_mode && ((from + len) <= cqspi->ahb_size))
		return cqspi_direct_read_execute(f_pdata, op);

	if (cqspi->use_dma_read && ddata && ddata->indirect_read_dma &&
	    virt_addr_valid(buf) && ((dma_align & CQSPI_DMA_UNALIGN) == 0))
		return ddata->indirect_read_dma(f_pdata, buf, from, len);

	return cqspi_indirect_read_execute(f_pdata, buf, from, len);
}

/*
 * Check if operation exactly matches the tuned operations.
 */
static bool cqspi_op_matches_tuned(const struct spi_mem_op *op,
				   const struct spi_mem_op *tuned_op)
{
	return op->cmd.opcode == tuned_op->cmd.opcode &&
	       op->cmd.buswidth == tuned_op->cmd.buswidth &&
	       op->cmd.dtr == tuned_op->cmd.dtr &&
	       op->addr.buswidth == tuned_op->addr.buswidth &&
	       op->addr.dtr == tuned_op->addr.dtr &&
	       op->data.buswidth == tuned_op->data.buswidth &&
	       op->data.dtr == tuned_op->data.dtr;
}

static int cqspi_mem_process(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct cqspi_st *cqspi = spi_controller_get_devdata(mem->spi->controller);
	struct cqspi_flash_pdata *f_pdata;

	f_pdata = &cqspi->f_pdata[spi_get_chipselect(mem->spi, 0)];

	/*
	 * PHY tuning allows high-frequency operation only for calibrated
	 * commands. Uncalibrated operations use safe non-PHY frequency to
	 * avoid timing violations.
	 */
	if (cqspi->ddata->execute_tuning && f_pdata->use_phy &&
	    (cqspi_op_matches_tuned(op, &f_pdata->phy_read_op) ||
	     cqspi_op_matches_tuned(op, &f_pdata->phy_write_op))) {
		cqspi_configure(f_pdata, op->max_freq);
	} else if (cqspi->ddata->execute_tuning) {
		/* Use safe frequency for untuned operations */
		cqspi_configure(f_pdata, f_pdata->non_phy_clk_rate);
	} else {
		/* No tuning support, always use requested frequency */
		cqspi_configure(f_pdata, op->max_freq);
	}

	if (op->data.dir == SPI_MEM_DATA_IN && op->data.buf.in) {
	/*
	 * Performing reads in DAC mode forces to read minimum 4 bytes
	 * which is unsupported on some flash devices during register
	 * reads, prefer STIG mode for such small reads.
	 */
		if (!op->addr.nbytes ||
		    (op->data.nbytes <= CQSPI_STIG_DATA_LEN_MAX &&
		     !cqspi->disable_stig_mode))
			return cqspi_command_read(f_pdata, op);

		return cqspi_read(f_pdata, op);
	}

	if (!op->addr.nbytes || !op->data.buf.out)
		return cqspi_command_write(f_pdata, op);

	return cqspi_write(f_pdata, op);
}

static int cqspi_exec_mem_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	int ret;
	struct cqspi_st *cqspi = spi_controller_get_devdata(mem->spi->controller);
	struct device *dev = &cqspi->pdev->dev;
	const struct cqspi_driver_platdata *ddata = of_device_get_match_data(dev);

	if (refcount_read(&cqspi->inflight_ops) == 0)
		return -ENODEV;

	if (!refcount_read(&cqspi->refcount))
		return -EBUSY;

	refcount_inc(&cqspi->inflight_ops);

	if (!refcount_read(&cqspi->refcount)) {
		if (refcount_read(&cqspi->inflight_ops))
			refcount_dec(&cqspi->inflight_ops);
		return -EBUSY;
	}

	if (!(ddata && (ddata->quirks & CQSPI_DISABLE_RUNTIME_PM))) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret) {
			dev_err(&mem->spi->dev, "resume failed with %d\n", ret);
			goto dec_inflight_refcount;
		}
	}

	ret = cqspi_mem_process(mem, op);

	if (!(ddata && (ddata->quirks & CQSPI_DISABLE_RUNTIME_PM)))
		pm_runtime_put_autosuspend(dev);

	if (ret)
		dev_err(&mem->spi->dev, "operation failed with %d\n", ret);

dec_inflight_refcount:
	if (refcount_read(&cqspi->inflight_ops) > 1)
		refcount_dec(&cqspi->inflight_ops);

	return ret;
}

static bool cqspi_supports_mem_op(struct spi_mem *mem,
				  const struct spi_mem_op *op)
{
	bool all_true, all_false;

	/*
	 * op->dummy.dtr is required for converting nbytes into ncycles.
	 * Also, don't check the dtr field of the op phase having zero nbytes.
	 */
	all_true = op->cmd.dtr &&
		   (!op->addr.nbytes || op->addr.dtr) &&
		   (!op->dummy.nbytes || op->dummy.dtr) &&
		   (!op->data.nbytes || op->data.dtr);

	all_false = !op->cmd.dtr && !op->addr.dtr && !op->dummy.dtr &&
		    !op->data.dtr;

	if (all_true) {
		/* Right now we only support 8-8-8 DTR mode. */
		if (op->cmd.nbytes && op->cmd.buswidth != 8)
			return false;
		if (op->addr.nbytes && op->addr.buswidth != 8)
			return false;
		if (op->data.nbytes && op->data.buswidth != 8)
			return false;
	} else if (!all_false) {
		/* Mixed DTR modes are not supported. */
		return false;
	}

	return spi_mem_default_supports_op(mem, op);
}

static int cqspi_write_pattern_to_cache(struct cqspi_flash_pdata *f_pdata,
					struct spi_mem *mem,
					struct spi_mem_op *write_op)
{
	struct device *dev = &f_pdata->cqspi->pdev->dev;
	int ret;

	write_op->data.nbytes = sizeof(phy_tuning_pattern);
	write_op->data.buf.out = phy_tuning_pattern;

	ret = spi_mem_exec_op(mem, write_op);
	if (ret) {
		dev_err(dev, "Failed to write PHY pattern to cache: %d\n", ret);
		return ret;
	}
	dev_dbg(dev, "PHY pattern (%zu bytes) written to cache\n",
		sizeof(phy_tuning_pattern));

	return 0;
}

static int cqspi_get_phy_pattern_offset(struct device *dev, u32 *offset)
{
	struct device_node *np, *flash_np = NULL;
	struct device_node *partition_np, *part_np;
	const char *label;
	const __be32 *reg;
	int len;

	if (!dev || !dev->of_node)
		return -EINVAL;

	for_each_child_of_node(dev->of_node, np) {
		if (of_node_name_prefix(np, "flash")) {
			flash_np = np;
			break;
		}
	}

	if (!flash_np)
		return -ENODEV;

	partition_np = of_get_child_by_name(flash_np, "partitions");
	if (!partition_np) {
		of_node_put(flash_np);
		return -ENODEV;
	}

	for_each_child_of_node(partition_np, part_np) {
		if (of_property_read_string(part_np, "label", &label) ||
		    !strstr(label, "phypattern"))
			continue;

		reg = of_get_property(part_np, "reg", &len);
		if (reg && len >= sizeof(__be32)) {
			*offset = be32_to_cpu(reg[0]);
			of_node_put(part_np);
			of_node_put(partition_np);
			of_node_put(flash_np);
			return 0;
		}
	}

	of_node_put(partition_np);
	of_node_put(flash_np);
	return -ENOENT;
}

static int cqspi_phy_check_pattern(struct cqspi_flash_pdata *f_pdata,
				   struct spi_mem *mem)
{
	struct spi_mem_op op;
	u8 *read_data;
	int ret;

	read_data = kmalloc_array(1, sizeof(phy_tuning_pattern), GFP_KERNEL);
	if (!read_data)
		return -ENOMEM;

	op = f_pdata->phy_read_op;
	op.data.buf.in = read_data;
	op.data.nbytes = sizeof(phy_tuning_pattern);

	ret = spi_mem_exec_op(mem, &op);
	if (ret)
		goto out;

	if (memcmp(read_data, phy_tuning_pattern, sizeof(phy_tuning_pattern)))
		ret = -EAGAIN;

out:
	kfree(read_data);
	return ret;
}

static void cqspi_phy_set_dll_master(struct cqspi_st *cqspi)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_PHY_DLL_MASTER);
	reg &= ~((CQSPI_REG_PHY_DLL_MASTER_DLY_ELMTS_LEN
		  << CQSPI_REG_PHY_DLL_MASTER_DLY_ELMTS_LSB) |
		 CQSPI_REG_PHY_DLL_MASTER_BYPASS |
		 CQSPI_REG_PHY_DLL_MASTER_CYCLE);
	reg |= ((cqspi->phase_detect_selector
		 << CQSPI_REG_PHY_DLL_MASTER_DLY_ELMTS_LSB) |
		CQSPI_REG_PHY_DLL_MASTER_CYCLE);

	writel(reg, reg_base + CQSPI_REG_PHY_DLL_MASTER);
}

static void cqspi_phy_pre_config(struct cqspi_st *cqspi,
				 struct cqspi_flash_pdata *f_pdata,
				 const bool bypass)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;
	u8 dummy;

	cqspi_readdata_capture(cqspi, bypass, f_pdata->has_dqs,
			       f_pdata->phy_setting.read_delay);

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~(CQSPI_REG_CONFIG_PHY_EN | CQSPI_REG_CONFIG_PHY_PIPELINE);
	reg |= CQSPI_REG_CONFIG_PHY_EN;
	writel(reg, reg_base + CQSPI_REG_CONFIG);

	reg = readl(reg_base + CQSPI_REG_RD_INSTR);
	dummy = FIELD_GET(CQSPI_REG_RD_INSTR_DUMMY_MASK
				  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
			  reg);
	dummy--;
	reg &= ~(CQSPI_REG_RD_INSTR_DUMMY_MASK << CQSPI_REG_RD_INSTR_DUMMY_LSB);
	reg |= FIELD_PREP(CQSPI_REG_RD_INSTR_DUMMY_MASK
				  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
			  dummy);
	writel(reg, reg_base + CQSPI_REG_RD_INSTR);

	cqspi_phy_set_dll_master(cqspi);
}

static void cqspi_phy_post_config(struct cqspi_st *cqspi,
				  const unsigned int delay)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;
	u8 dummy;

	reg = readl(reg_base + CQSPI_REG_READCAPTURE);
	reg &= ~(CQSPI_REG_READCAPTURE_DELAY_MASK
		 << CQSPI_REG_READCAPTURE_DELAY_LSB);

	reg |= (delay & CQSPI_REG_READCAPTURE_DELAY_MASK)
	       << CQSPI_REG_READCAPTURE_DELAY_LSB;
	writel(reg, reg_base + CQSPI_REG_READCAPTURE);

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~(CQSPI_REG_CONFIG_PHY_EN | CQSPI_REG_CONFIG_PHY_PIPELINE);
	writel(reg, reg_base + CQSPI_REG_CONFIG);

	reg = readl(reg_base + CQSPI_REG_RD_INSTR);
	dummy = FIELD_GET(CQSPI_REG_RD_INSTR_DUMMY_MASK
				  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
			  reg);
	dummy++;
	reg &= ~(CQSPI_REG_RD_INSTR_DUMMY_MASK << CQSPI_REG_RD_INSTR_DUMMY_LSB);
	reg |= FIELD_PREP(CQSPI_REG_RD_INSTR_DUMMY_MASK
				  << CQSPI_REG_RD_INSTR_DUMMY_LSB,
			  dummy);
	writel(reg, reg_base + CQSPI_REG_RD_INSTR);
}

static int cqspi_find_rx_low_sdr(struct cqspi_flash_pdata *f_pdata,
				 struct spi_mem *mem, struct phy_setting *phy)
{
	struct device *dev = &f_pdata->cqspi->pdev->dev;
	int ret;

	phy->rx = 0;
	do {
		ret = cqspi_phy_apply_setting(f_pdata, phy);
		if (!ret) {
			ret = cqspi_phy_check_pattern(f_pdata, mem);
			if (!ret)
				return 0;
		}
		phy->rx++;
	} while (phy->rx < CQSPI_PHY_MAX_DELAY - 1);

	dev_dbg(dev, "Unable to find RX low\n");
	return -ENOENT;
}

static int cqspi_find_rx_high_sdr(struct cqspi_flash_pdata *f_pdata,
				  struct spi_mem *mem, struct phy_setting *phy,
				  u8 lowerbound)
{
	struct device *dev = &f_pdata->cqspi->pdev->dev;
	int ret;

	phy->rx = CQSPI_PHY_MAX_DELAY;
	do {
		ret = cqspi_phy_apply_setting(f_pdata, phy);
		if (!ret) {
			ret = cqspi_phy_check_pattern(f_pdata, mem);
			if (!ret)
				return 0;
		}
		phy->rx--;
	} while (phy->rx > lowerbound);

	dev_dbg(dev, "Unable to find RX high\n");
	return -ENOENT;
}

static int cqspi_phy_check_consecutive_ddr(struct cqspi_flash_pdata *f_pdata,
					   struct spi_mem *mem,
					   struct phy_setting start, int target,
					   bool match_pass, bool upward)
{
	struct phy_setting probe = start;
	int step = upward ? 1 : -1;
	int count = 1;
	int ret;

	while (count < target) {
		if (upward ? (probe.tx >= CQSPI_PHY_MAX_DELAY ||
			      probe.rx >= CQSPI_PHY_MAX_DELAY) :
			     (probe.tx == 0 || probe.rx == 0))
			break;
		probe.tx += step;
		probe.rx += step;
		ret = cqspi_phy_apply_setting(f_pdata, &probe);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);
		if ((ret == 0) == match_pass)
			count++;
		else
			break;
	}

	return count;
}

static void cqspi_phy_find_next_match_ddr(struct cqspi_flash_pdata *f_pdata,
					  struct spi_mem *mem,
					  struct phy_setting *search,
					  const struct phy_setting *limit,
					  bool match_pass, bool upward)
{
	int step = upward ? 1 : -1;
	int ret;

	while (upward ? (search->tx < limit->tx && search->rx < limit->rx) :
			(search->tx > limit->tx && search->rx > limit->rx)) {
		search->tx += step;
		search->rx += step;
		ret = cqspi_phy_apply_setting(f_pdata, search);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);
		if ((ret == 0) == match_pass)
			break;
	}
}

static void cqspi_phy_find_biggest_line_ddr(struct cqspi_flash_pdata *f_pdata,
					    struct spi_mem *mem,
					    struct phy_setting *start,
					    struct phy_setting *end,
					    bool upward)
{
	struct phy_setting probe = *start;
	struct phy_setting best_start = *start;
	int step = upward ? 1 : -1;
	int cur_len = 1, best_len = 1;
	int ret;

	for (;;) {
		if (!upward && (probe.tx == 0 || probe.rx == 0))
			break;
		probe.tx += step;
		probe.rx += step;

		if (upward ? (probe.tx > end->tx || probe.rx > end->rx) :
			     (probe.tx < end->tx || probe.rx < end->rx)) {
			/* Overshot the search limit; rewind to the last tested tap */
			probe.tx -= step;
			probe.rx -= step;
			break;
		}

		ret = cqspi_phy_apply_setting(f_pdata, &probe);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);

		if (!ret) {
			cur_len++;
		} else {
			if (cur_len > best_len) {
				best_len = cur_len;
				best_start.tx = probe.tx - step * cur_len;
				best_start.rx = probe.rx - step * cur_len;
			}
			cur_len = 0;
			cqspi_phy_find_next_match_ddr(f_pdata, mem, &probe, end,
						      true, upward);
			if (upward ? (probe.tx <= end->tx &&
				      probe.rx <= end->rx) :
				     (probe.tx >= end->tx &&
				      probe.rx >= end->rx))
				cur_len = 1;
		}
	}

	if (cur_len > best_len) {
		best_len = cur_len;
		best_start.tx = probe.tx - step * (cur_len - 1);
		best_start.rx = probe.rx - step * (cur_len - 1);
	}

	start->tx = best_start.tx;
	start->rx = best_start.rx;
	end->tx = start->tx + step * (best_len - 1);
	end->rx = start->rx + step * (best_len - 1);
}

static int cqspi_phy_rd_delay_valid_ddr(struct cqspi_flash_pdata *f_pdata,
					struct spi_mem *mem,
					struct phy_setting probe,
					struct phy_setting end)
{
	int ret = -ENOENT;

	while ((int)probe.tx <= (int)end.tx - CQSPI_PHY_DDR_RD_STEP &&
	       (int)probe.rx <= (int)end.rx - CQSPI_PHY_DDR_RD_STEP && ret) {
		ret = cqspi_phy_apply_setting(f_pdata, &probe);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);
		probe.tx += CQSPI_PHY_DDR_RD_STEP;
		probe.rx += CQSPI_PHY_DDR_RD_STEP;
	}

	return ret;
}

static int cqspi_phy_find_valid_rd_delays_ddr(struct cqspi_flash_pdata *f_pdata,
					      struct spi_mem *mem,
					      struct phy_setting diag_start,
					      struct phy_setting diag_end,
					      u8 *min_rd, u8 *max_rd)
{
	struct phy_setting probe;
	u8 read_delay;

	*min_rd = CQSPI_PHY_RD_INVALID;
	*max_rd = CQSPI_PHY_RD_INVALID;

	for (read_delay = CQSPI_PHY_INIT_RD; read_delay <= CQSPI_PHY_MAX_RD;
	     read_delay++) {
		probe = diag_start;
		probe.read_delay = read_delay;
		if (!cqspi_phy_rd_delay_valid_ddr(f_pdata, mem, probe,
						  diag_end)) {
			if (*min_rd == CQSPI_PHY_RD_INVALID)
				*min_rd = read_delay;
			*max_rd = read_delay;
		}
	}

	return (*min_rd == CQSPI_PHY_RD_INVALID) ? -ENOENT : 0;
}

static int cqspi_phy_check_circle_ddr(struct cqspi_flash_pdata *f_pdata,
				      struct spi_mem *mem,
				      const struct phy_setting *center)
{
	int radius = CQSPI_PHY_DDR_RADIUS;
	int radius_sq = radius * radius;
	int rx_min = (int)center->rx - radius;
	int rx_max = (int)center->rx + radius;
	int tx_min = (int)center->tx - radius;
	int tx_max = (int)center->tx + radius;
	struct phy_setting probe;
	int rx_delta, tx_delta;
	int rx, tx, ret;

	if (rx_max > CQSPI_PHY_MAX_DELAY || rx_min < 0 ||
	    tx_max > CQSPI_PHY_MAX_DELAY || tx_min < 0)
		return -ENOENT;

	/* Bottom-left quadrant */
	for (rx = rx_min; rx <= (int)center->rx; rx++) {
		rx_delta = rx - (int)center->rx;
		for (tx = tx_min; tx <= (int)center->tx; tx++) {
			if (rx == (int)center->rx && tx == (int)center->tx)
				continue;
			tx_delta = tx - (int)center->tx;
			if (rx_delta * rx_delta + tx_delta * tx_delta >
			    radius_sq)
				continue;
			probe = (struct phy_setting){
				.rx = rx,
				.tx = tx,
				.read_delay = center->read_delay
			};
			ret = cqspi_phy_apply_setting(f_pdata, &probe);
			if (!ret)
				ret = cqspi_phy_check_pattern(f_pdata, mem);
			if (ret)
				return ret;
		}
	}

	/* Top-right quadrant */
	for (rx = (int)center->rx; rx <= rx_max; rx++) {
		rx_delta = rx - (int)center->rx;
		for (tx = (int)center->tx; tx <= tx_max; tx++) {
			if (rx == (int)center->rx && tx == (int)center->tx)
				continue;
			tx_delta = tx - (int)center->tx;
			if (rx_delta * rx_delta + tx_delta * tx_delta >
			    radius_sq)
				continue;
			probe = (struct phy_setting){
				.rx = rx,
				.tx = tx,
				.read_delay = center->read_delay
			};
			ret = cqspi_phy_apply_setting(f_pdata, &probe);
			if (!ret)
				ret = cqspi_phy_check_pattern(f_pdata, mem);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int cqspi_phy_find_tuning_point_ddr(struct cqspi_flash_pdata *f_pdata,
					   struct spi_mem *mem,
					   struct phy_setting *midpt)
{
	struct phy_setting top_left = *midpt, bot_right = *midpt;
	struct phy_setting anti_diag_mid;
	u8 prev_tx, prev_rx;
	int ret;

	/* Walk anti-diagonal top-left (tx--, rx++) until fail */
	while (top_left.tx > 0 && top_left.rx < CQSPI_PHY_MAX_DELAY) {
		prev_tx = top_left.tx;
		prev_rx = top_left.rx;
		top_left.tx--;
		top_left.rx++;
		ret = cqspi_phy_apply_setting(f_pdata, &top_left);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);
		if (ret) {
			top_left.tx = prev_tx;
			top_left.rx = prev_rx;
			break;
		}
	}

	/* Walk anti-diagonal bottom-right (tx++, rx--) until fail */
	while (bot_right.tx < CQSPI_PHY_MAX_DELAY && bot_right.rx > 0) {
		prev_tx = bot_right.tx;
		prev_rx = bot_right.rx;
		bot_right.tx++;
		bot_right.rx--;
		ret = cqspi_phy_apply_setting(f_pdata, &bot_right);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);
		if (ret) {
			bot_right.tx = prev_tx;
			bot_right.rx = prev_rx;
			break;
		}
	}

	/*
	 * Anti-diagonal span in taps. On this axis |delta_tx| == |delta_rx|
	 * for every step, so the tap count is the tx coordinate difference.
	 */
	if ((int)bot_right.tx - (int)top_left.tx < CQSPI_PHY_DDR_MIN_PASS_TAPS)
		return -ENOENT;

	midpt->tx = (top_left.tx + bot_right.tx) / 2;
	midpt->rx = (top_left.rx + bot_right.rx) / 2;
	anti_diag_mid = *midpt;

	ret = cqspi_phy_check_circle_ddr(f_pdata, mem, midpt);
	if (!ret)
		return 0;

	/*
	 * Circle check failed at the anti-diagonal midpoint. Both boundary
	 * distances from anti_diag_mid are equal (or differ by one tap for
	 * odd spans), so try toward top-left first, then toward bottom-right.
	 */
	midpt->tx = (anti_diag_mid.tx + top_left.tx) / 2;
	midpt->rx = (anti_diag_mid.rx + top_left.rx) / 2;
	ret = cqspi_phy_check_circle_ddr(f_pdata, mem, midpt);
	if (!ret)
		return 0;

	midpt->tx = (anti_diag_mid.tx + bot_right.tx) / 2;
	midpt->rx = (anti_diag_mid.rx + bot_right.rx) / 2;
	return cqspi_phy_check_circle_ddr(f_pdata, mem, midpt);
}

static int cqspi_phy_search_diagonal_ddr(struct cqspi_flash_pdata *f_pdata,
					 struct spi_mem *mem,
					 struct phy_setting *best,
					 struct phy_setting diag_start,
					 struct phy_setting diag_end)
{
	struct phy_setting gap_start = { .read_delay = CQSPI_PHY_RD_INVALID };
	struct phy_setting gap_end = { .read_delay = CQSPI_PHY_RD_INVALID };
	struct phy_setting midpoint = { .read_delay = CQSPI_PHY_RD_INVALID };
	struct phy_setting pass_start, pass_end;
	struct phy_setting low_midpt, high_midpt;
	struct phy_setting *first, *second;
	struct device *dev = &f_pdata->cqspi->pdev->dev;
	int pass_len, low_len, high_len;
	u8 min_rd, max_rd;
	int run, ret;

	ret = cqspi_phy_find_valid_rd_delays_ddr(f_pdata, mem, diag_start,
						 diag_end, &min_rd, &max_rd);
	if (ret)
		return ret;

	dev_dbg(dev, "DDR PHY diagonal: valid read delays %u..%u\n", min_rd,
		max_rd);

	/* Refine pass_start: walk forward at min_rd to first passing point */
	pass_start = diag_start;
	pass_start.read_delay = min_rd;
	for (;;) {
		ret = cqspi_phy_apply_setting(f_pdata, &pass_start);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);
		if (!ret)
			break;
		if (pass_start.tx >= CQSPI_PHY_MAX_DELAY ||
		    pass_start.rx >= CQSPI_PHY_MAX_DELAY)
			break;
		pass_start.tx++;
		pass_start.rx++;
	}

	/* Refine pass_end: walk backward at max_rd to first passing point */
	pass_end = diag_end;
	pass_end.read_delay = max_rd;
	for (;;) {
		ret = cqspi_phy_apply_setting(f_pdata, &pass_end);
		if (!ret)
			ret = cqspi_phy_check_pattern(f_pdata, mem);
		if (!ret)
			break;
		if (pass_end.tx == 0 || pass_end.rx == 0)
			break;
		pass_end.tx--;
		pass_end.rx--;
	}

	if (pass_start.read_delay == pass_end.read_delay) {
		/* Single region: find the longest run, take its midpoint */
		cqspi_phy_find_biggest_line_ddr(f_pdata, mem, &pass_start,
						&pass_end, true);

		/* tx and rx increase equally on the diagonal */
		pass_len = (int)pass_end.tx - (int)pass_start.tx;
		dev_dbg(dev, "DDR PHY diagonal: single region, %d taps\n",
			pass_len);
		if (pass_len < CQSPI_PHY_DDR_MIN_PASS_TAPS)
			return -ENOENT;

		midpoint.tx = (pass_start.tx + pass_end.tx) / 2;
		midpoint.rx = (pass_start.rx + pass_end.rx) / 2;
		midpoint.read_delay = pass_start.read_delay;

		ret = cqspi_phy_find_tuning_point_ddr(f_pdata, mem, &midpoint);
		if (!ret)
			*best = midpoint;
		return ret;
	}

	/* Dual regions: locate bottom passing region from pass_start upward */
	do {
		run = cqspi_phy_check_consecutive_ddr(f_pdata, mem, pass_start,
						      CQSPI_PHY_DDR_CONSEC_PASS,
						      true, true);
		if (run < CQSPI_PHY_DDR_CONSEC_PASS) {
			pass_start.tx += run + 1;
			pass_start.rx += run + 1;
			cqspi_phy_find_next_match_ddr(f_pdata, mem, &pass_start,
						      &pass_end, true, true);
		}
	} while (run < CQSPI_PHY_DDR_CONSEC_PASS &&
		 pass_start.tx < pass_end.tx);

	if (run < CQSPI_PHY_DDR_CONSEC_PASS) {
		pass_start.read_delay = CQSPI_PHY_RD_INVALID;
	} else {
		/*
		 * Walk past confirmed region to find gap start boundary.
		 * Clamp the jump so tx/rx never step past CQSPI_PHY_MAX_DELAY.
		 */
		int off = CQSPI_PHY_DDR_CONSEC_PASS + 1;

		if (pass_start.tx > CQSPI_PHY_MAX_DELAY - off)
			off = CQSPI_PHY_MAX_DELAY - pass_start.tx;
		if (pass_start.rx > CQSPI_PHY_MAX_DELAY - off)
			off = CQSPI_PHY_MAX_DELAY - pass_start.rx;

		gap_start.rx = pass_start.rx + off;
		gap_start.tx = pass_start.tx + off;
		gap_start.read_delay = pass_start.read_delay;

		for (;;) {
			ret = cqspi_phy_apply_setting(f_pdata, &gap_start);
			if (!ret)
				ret = cqspi_phy_check_pattern(f_pdata, mem);
			if (ret)
				break;
			if (gap_start.tx >= CQSPI_PHY_MAX_DELAY ||
			    gap_start.rx >= CQSPI_PHY_MAX_DELAY)
				break;
			gap_start.tx++;
			gap_start.rx++;
		}

		do {
			run = cqspi_phy_check_consecutive_ddr(
				f_pdata, mem, gap_start,
				CQSPI_PHY_DDR_CONSEC_FAIL, false, true);
			if (run < CQSPI_PHY_DDR_CONSEC_FAIL) {
				gap_start.tx += run + 1;
				gap_start.rx += run + 1;
				cqspi_phy_find_next_match_ddr(f_pdata, mem,
							      &gap_start,
							      &pass_end, false,
							      true);
			}
		} while (run < CQSPI_PHY_DDR_CONSEC_FAIL &&
			 gap_start.tx < pass_end.tx);

		/* Narrow pass_start..gap_start to the best passing run */
		cqspi_phy_find_biggest_line_ddr(f_pdata, mem, &pass_start,
						&gap_start, true);
	}

	/* Locate top passing region from pass_end downward */
	do {
		run = cqspi_phy_check_consecutive_ddr(f_pdata, mem, pass_end,
						      CQSPI_PHY_DDR_CONSEC_PASS,
						      true, false);
		if (run < CQSPI_PHY_DDR_CONSEC_PASS) {
			pass_end.tx -= run + 1;
			pass_end.rx -= run + 1;
			cqspi_phy_find_next_match_ddr(f_pdata, mem, &pass_end,
						      &pass_start, true, false);
		}
	} while (run < CQSPI_PHY_DDR_CONSEC_PASS &&
		 pass_end.tx >= (pass_start.tx + CQSPI_PHY_DDR_CONSEC_PASS));

	if (run < CQSPI_PHY_DDR_CONSEC_PASS) {
		pass_end.read_delay = CQSPI_PHY_RD_INVALID;
	} else {
		/*
		 * Walk past confirmed region to find gap end boundary.
		 * Clamp the jump so tx/rx never step below zero.
		 */
		int off = CQSPI_PHY_DDR_CONSEC_PASS + 1;

		if (pass_end.tx < off)
			off = pass_end.tx;
		if (pass_end.rx < off)
			off = pass_end.rx;

		gap_end.rx = pass_end.rx - off;
		gap_end.tx = pass_end.tx - off;
		gap_end.read_delay = pass_end.read_delay;

		for (;;) {
			ret = cqspi_phy_apply_setting(f_pdata, &gap_end);
			if (!ret)
				ret = cqspi_phy_check_pattern(f_pdata, mem);
			if (ret)
				break;
			if (gap_end.tx == 0 || gap_end.rx == 0)
				break;
			gap_end.tx--;
			gap_end.rx--;
		}

		do {
			run = cqspi_phy_check_consecutive_ddr(
				f_pdata, mem, gap_end,
				CQSPI_PHY_DDR_CONSEC_FAIL, false, false);
			if (run < CQSPI_PHY_DDR_CONSEC_FAIL) {
				gap_end.tx -= run + 1;
				gap_end.rx -= run + 1;
				cqspi_phy_find_next_match_ddr(f_pdata, mem,
							      &gap_end,
							      &pass_start,
							      false, false);
			}
		} while (run < CQSPI_PHY_DDR_CONSEC_FAIL &&
			 gap_end.tx >= pass_start.tx);

		/* Narrow gap_end..pass_end to the best passing run */
		cqspi_phy_find_biggest_line_ddr(f_pdata, mem, &pass_end,
						&gap_end, false);
	}

	if (pass_start.read_delay == CQSPI_PHY_RD_INVALID &&
	    pass_end.read_delay == CQSPI_PHY_RD_INVALID)
		return -ENOENT;

	low_len = 0;
	high_len = 0;

	if (pass_start.read_delay != CQSPI_PHY_RD_INVALID)
		low_len = (int)gap_start.tx - (int)pass_start.tx;

	if (pass_end.read_delay != CQSPI_PHY_RD_INVALID)
		high_len = (int)pass_end.tx - (int)gap_end.tx;

	dev_dbg(dev,
		"DDR PHY diagonal: dual region, low=%d taps, high=%d taps\n",
		low_len, high_len);

	if (low_len < CQSPI_PHY_DDR_MIN_PASS_TAPS &&
	    high_len < CQSPI_PHY_DDR_MIN_PASS_TAPS)
		return -ENOENT;

	low_midpt = (struct phy_setting){ .read_delay = CQSPI_PHY_RD_INVALID };
	high_midpt = (struct phy_setting){ .read_delay = CQSPI_PHY_RD_INVALID };

	if (high_len >= CQSPI_PHY_DDR_MIN_PASS_TAPS) {
		high_midpt.rx = (pass_end.rx + gap_end.rx) / 2;
		high_midpt.tx = (pass_end.tx + gap_end.tx) / 2;
		high_midpt.read_delay = pass_end.read_delay;
	}

	if (low_len >= CQSPI_PHY_DDR_MIN_PASS_TAPS) {
		low_midpt.rx = (pass_start.rx + gap_start.rx) / 2;
		low_midpt.tx = (pass_start.tx + gap_start.tx) / 2;
		low_midpt.read_delay = pass_start.read_delay;
	}

	/* Try the larger region first; fall back to the smaller one */
	if (high_len >= low_len) {
		first = high_midpt.read_delay != CQSPI_PHY_RD_INVALID ?
				&high_midpt :
				NULL;
		second = low_midpt.read_delay != CQSPI_PHY_RD_INVALID ?
				 &low_midpt :
				 NULL;
	} else {
		first = low_midpt.read_delay != CQSPI_PHY_RD_INVALID ?
				&low_midpt :
				NULL;
		second = high_midpt.read_delay != CQSPI_PHY_RD_INVALID ?
				 &high_midpt :
				 NULL;
	}

	if (first && !cqspi_phy_find_tuning_point_ddr(f_pdata, mem, first)) {
		*best = *first;
		return 0;
	}

	if (second && !cqspi_phy_find_tuning_point_ddr(f_pdata, mem, second)) {
		*best = *second;
		return 0;
	}

	return -ENOENT;
}

static inline void cqspi_phy_reset_setting(struct phy_setting *phy)
{
	*phy = (struct phy_setting){ .rx = 0, .tx = 127, .read_delay = 0 };
}

static int cqspi_phy_tuning_ddr(struct cqspi_flash_pdata *f_pdata,
				struct spi_mem *mem)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	struct device *dev = &cqspi->pdev->dev;
	struct phy_setting diag_start, diag_end;
	struct phy_setting tuning_pt = { 0 };
	int shift, ret;

	f_pdata->use_phy = true;

	diag_start = (struct phy_setting){ .tx = 0,
					   .rx = 0,
					   .read_delay = CQSPI_PHY_RD_INVALID };
	diag_end = (struct phy_setting){ .tx = CQSPI_PHY_MAX_DELAY,
					 .rx = CQSPI_PHY_MAX_DELAY,
					 .read_delay = CQSPI_PHY_RD_INVALID };

	dev_dbg(dev, "DDR PHY tuning: searching main diagonal\n");
	ret = cqspi_phy_search_diagonal_ddr(f_pdata, mem, &tuning_pt,
					    diag_start, diag_end);

	for (shift = CQSPI_PHY_DDR_DIAG_SHIFT;
	     ret && shift <= CQSPI_PHY_DDR_MAX_DIAG_SHIFT;
	     shift += CQSPI_PHY_DDR_DIAG_SHIFT) {
		/* Up-shifted diagonal: rx-biased */
		dev_dbg(dev, "DDR PHY tuning: up-shifted diagonal, shift=%d\n",
			shift);
		diag_start = (struct phy_setting){
			.tx = 0, .rx = shift, .read_delay = CQSPI_PHY_RD_INVALID
		};
		diag_end =
			(struct phy_setting){ .tx = CQSPI_PHY_MAX_DELAY - shift,
					      .rx = CQSPI_PHY_MAX_DELAY,
					      .read_delay =
						      CQSPI_PHY_RD_INVALID };
		ret = cqspi_phy_search_diagonal_ddr(f_pdata, mem, &tuning_pt,
						    diag_start, diag_end);
		if (ret) {
			/* Right-shifted diagonal: tx-biased */
			dev_dbg(dev,
				"DDR PHY tuning: right-shifted diagonal, shift=%d\n",
				shift);
			diag_start = (struct phy_setting){
				.tx = shift,
				.rx = 0,
				.read_delay = CQSPI_PHY_RD_INVALID
			};
			diag_end = (struct phy_setting){
				.tx = CQSPI_PHY_MAX_DELAY,
				.rx = CQSPI_PHY_MAX_DELAY - shift,
				.read_delay = CQSPI_PHY_RD_INVALID
			};
			ret = cqspi_phy_search_diagonal_ddr(
				f_pdata, mem, &tuning_pt, diag_start, diag_end);
		}
	}

	if (ret) {
		dev_err(dev,
			"DDR PHY tuning failed to find valid configuration\n");
		goto out;
	}

	dev_dbg(dev, "DDR PHY tuning point: TX=%u RX=%u RD=%u\n", tuning_pt.tx,
		tuning_pt.rx, tuning_pt.read_delay);

	ret = cqspi_phy_apply_setting(f_pdata, &tuning_pt);
	if (!ret)
		ret = cqspi_phy_check_pattern(f_pdata, mem);
	if (ret) {
		dev_err(dev, "DDR PHY: pattern check failed at tuning point\n");
	} else {
		f_pdata->phy_setting.read_delay = tuning_pt.read_delay;
		f_pdata->phy_setting.rx = tuning_pt.rx;
		f_pdata->phy_setting.tx = tuning_pt.tx;
	}

out:
	if (ret)
		f_pdata->use_phy = false;

	return ret;
}

static int cqspi_phy_tuning_sdr(struct cqspi_flash_pdata *f_pdata,
				struct spi_mem *mem)
{
	struct cqspi_st *cqspi = f_pdata->cqspi;
	struct device *dev = &cqspi->pdev->dev;
	struct phy_setting rxlow, rxhigh, first, second, final;
	u8 window1 = 0;
	u8 window2 = 0;
	int ret;

	/*
	 * SDR tuning: 1D search for optimal RX delay (TX less critical).
	 * Find two consecutive windows, choose larger, use midpoint.
	 *
	 *              rx
	 *          127 ^
	 *              |        |-----window at----------|
	 *              |        |-----read_dealy = n+1---|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        rxlow(n+1)   midpoint    rxhigh(n+1)
	 *              |
	 *              |   |---window at--------|
	 *              |   |---read_dealy = n---|
	 *              |   |xxxxxxxxxxxxxxxxxxxx|
	 *              |   |xxxxxxxxxxxxxxxxxxxx|
	 *              |   |xxxxxxxxxxxxxxxxxxxx|
	 *              |   |xxxxxxxxxxxxxxxxxxxx|
	 *              |   |xxxxxxxxxxxxxxxxxxxx|
	 *              |   rxlow(n)   midpoint  rxhigh(n)
	 *              |
	 *              -----------------------------------------> tx
	 *              0                                      127
	 *                  read_delay=n                  read_dealy=n+1
	 */

	f_pdata->use_phy = true;
	cqspi_phy_reset_setting(&rxlow);
	cqspi_phy_reset_setting(&rxhigh);
	cqspi_phy_reset_setting(&first);

	/* First window: Find rxlow by incrementing read_delay from 0 */

	/*
	 *              rx
	 *          127 ^
	 *              |   |xxxxxxxxxxxxxxxxxxxx|
	 *  search      |   |xxxxxxxxxxxxxxxxxxxx|
	 *  rxlow       |   |xxxxxxxxxxxxxxxxxxxx|
	 *  increasing  |   |xxxxxxxxxxxxxxxxxxxx|
	 *           --------->|xxxxxxxxxxxxxxxxxxxx|
	 *  read_delay  |        |xxxxxxxxxxxxxxxxxxx|
	 *  until found |        |xxxxxxxxxxxxxxxxxxx|
	 *              |        rxlow
	 *              -----------------------------------------> tx
	 *              0                            tx fixed at 127
	 */

	do {
		ret = cqspi_find_rx_low_sdr(f_pdata, mem, &rxlow);

		if (ret)
			rxlow.read_delay++;
	} while (ret && rxlow.read_delay <= CQSPI_PHY_MAX_RD);

	/* Find rxhigh: Decrement from RX=127 at same read_delay */

	/*
	 *              rx
	 *          127 ^        search rxhigh
	 *              |        (decrement from
	 *              |        127 until found)
	 *              |          |
	 *              |          |
	 *              |          v
	 *              |        |------------------------|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        rxlow                    rxhigh
	 *              -----------------------------------------> tx
	 *              0                            tx fixed at 127
	 */

	rxhigh.read_delay = rxlow.read_delay;
	ret = cqspi_find_rx_high_sdr(f_pdata, mem, &rxhigh, rxlow.rx);
	if (ret)
		goto out;

	/* Calculate first window midpoint for max margin */

	/*
	 *              rx
	 *          127 ^
	 *              |        |--------window1---------|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxx * xxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        rxlow        ^           rxhigh
	 *              ----------------------|------------------> tx
	 *              0                     |      tx fixed at 127
	 *                            window1/2
	 */

	first.read_delay = rxlow.read_delay;
	window1 = rxhigh.rx - rxlow.rx;
	first.rx = rxlow.rx + (window1 / 2);

	dev_dbg(dev, "First tuning point: RX: %d TX: %d RD: %d\n", first.rx,
		first.tx, first.read_delay);
	ret = cqspi_phy_apply_setting(f_pdata, &first);
	if (!ret)
		ret = cqspi_phy_check_pattern(f_pdata, mem);

	if (ret || first.read_delay > CQSPI_PHY_MAX_RD)
		goto out;

	/* Second window: Search at read_delay+1, may differ in size */

	/*
	 *              rx
	 *          127 ^
	 *              |        |-------|
	 *              |        |xxxxxxx|
	 *              |        |xxxxxxx|
	 *              |        |xxxxxxx|
	 *              |        |xxxxxxx|
	 *              |        |xxxxxxx|
	 *              |        rxlow   rxhigh
	 *              -----------------------------------------> tx
	 *              0
	 *              read_delay = n (smaller window)
	 *
	 *              rx
	 *          127 ^
	 *              |        |-----------------|
	 *              |        |xxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxx|
	 *              |        rxlow             rxhigh
	 *              -----------------------------------------> tx
	 *              0
	 *              read_delay = n+! (larger window - better)
	 */

	cqspi_phy_reset_setting(&rxlow);
	cqspi_phy_reset_setting(&rxhigh);
	cqspi_phy_reset_setting(&second);

	rxlow.read_delay = first.read_delay + 1;
	if (rxlow.read_delay > CQSPI_PHY_MAX_RD)
		goto compare;

	ret = cqspi_find_rx_low_sdr(f_pdata, mem, &rxlow);
	if (ret)
		goto compare;

	rxhigh.read_delay = rxlow.read_delay;
	ret = cqspi_find_rx_high_sdr(f_pdata, mem, &rxhigh, rxlow.rx);
	if (ret)
		goto compare;

	/* Calculate second window midpoint */

	/*
	 *              rx
	 *          127 ^
	 *              |        |--------window2---------|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxx * xxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        |xxxxxxxxxxxxxxxxxxxxxxxx|
	 *              |        rxlow        ^           rxhigh
	 *              ----------------------|------------------> tx
	 *              0                     |      tx fixed at 127
	 *                            window1/2
	 *              read_delay = n+1
	 */

	window2 = rxhigh.rx - rxlow.rx;
	second.rx = rxlow.rx + (window2 / 2);
	second.read_delay = rxlow.read_delay;

	dev_dbg(dev, "Second tuning point: RX: %d TX: %d RD: %d\n", second.rx,
		second.tx, second.read_delay);
	ret = cqspi_phy_apply_setting(f_pdata, &second);
	if (!ret)
		ret = cqspi_phy_check_pattern(f_pdata, mem);

	if (ret || second.read_delay > CQSPI_PHY_MAX_RD)
		window2 = 0;

	/* Window comparison: Choose larger window for better margin */

compare:
	cqspi_phy_reset_setting(&final);
	if (window2 > window1) {
		final.rx = second.rx;
		final.read_delay = second.read_delay;
	} else {
		final.rx = first.rx;
		final.read_delay = first.read_delay;
	}

	/* Apply and verify final tuning point */

	dev_dbg(dev, "Final tuning point: RX: %d TX: %d RD: %d\n", final.rx,
		final.tx, final.read_delay);
	ret = cqspi_phy_apply_setting(f_pdata, &final);
	if (!ret)
		ret = cqspi_phy_check_pattern(f_pdata, mem);

	if (ret) {
		ret = -EINVAL;
		goto out;
	}

	f_pdata->phy_setting.read_delay = final.read_delay;
	f_pdata->phy_setting.rx = final.rx;
	f_pdata->phy_setting.tx = final.tx;

out:
	if (ret)
		f_pdata->use_phy = false;

	return ret;
}

static int cqspi_am654_ospi_execute_tuning(struct spi_mem *mem,
					   struct spi_mem_op *read_op,
					   struct spi_mem_op *write_op)
{
	struct cqspi_st *cqspi =
		spi_controller_get_devdata(mem->spi->controller);
	struct cqspi_flash_pdata *f_pdata;
	struct device *dev = &cqspi->pdev->dev;
	int ret;
	u32 phy_offset;

	f_pdata = &cqspi->f_pdata[spi_get_chipselect(mem->spi, 0)];

	if (read_op->max_freq <= f_pdata->non_phy_clk_rate) {
		dev_dbg(dev,
			"Frequency %u Hz below PHY threshold %u Hz, skipping tuning\n",
			read_op->max_freq, f_pdata->non_phy_clk_rate);
		return 0;
	}

	if (cqspi->phase_detect_selector >
	    CQSPI_REG_PHY_DLL_MASTER_DLY_ELMTS_LEN) {
		dev_warn(dev,
			 "Phase Detect Selector should be in between [0, 7]. Skipping Calibration\n");
		return 0;
	}

	if (write_op) {
		ret = cqspi_write_pattern_to_cache(f_pdata, mem, write_op);
		if (ret) {
			dev_warn(dev,
				 "failed to write pattern to cache: %d, skipping PHY tuning\n",
				 ret);
			return ret;
		}

		f_pdata->phy_write_op = *write_op;
	} else {
		ret = cqspi_get_phy_pattern_offset(dev, &phy_offset);
		if (ret) {
			dev_warn(dev,
				 "PHY pattern partition not found: %d, skipping PHY tuning\n",
				 ret);
			return ret;
		}

		read_op->addr.val = phy_offset;
	}

	f_pdata->phy_read_op = *read_op;

	ret = cqspi_phy_check_pattern(f_pdata, mem);
	if (ret) {
		dev_warn(dev,
			 "PHY pattern not found: %d, skipping PHY tuning\n",
			 ret);
		return ret;
	}

	if (read_op->cmd.dtr || read_op->addr.dtr || read_op->dummy.dtr ||
	    read_op->data.dtr) {
		f_pdata->has_dqs = true;
		cqspi_phy_pre_config(cqspi, f_pdata, false);
		ret = cqspi_phy_tuning_ddr(f_pdata, mem);
	} else {
		f_pdata->has_dqs = false;
		cqspi_phy_pre_config(cqspi, f_pdata, true);
		ret = cqspi_phy_tuning_sdr(f_pdata, mem);
	}

	if (ret)
		dev_warn(dev, "PHY tuning failed: %d\n", ret);

	cqspi_phy_post_config(cqspi, f_pdata->read_delay);

	return ret;
}

static u32 cqspi_am654_ospi_get_non_phy_clk_rate(struct cqspi_st *cqspi)
{
	return CQSPI_AM654_NON_PHY_CLK_RATE;
}

static int cqspi_mem_op_execute_tuning(struct spi_mem *mem,
				       struct spi_mem_op *read_op,
				       struct spi_mem_op *write_op)
{
	struct cqspi_st *cqspi =
		spi_controller_get_devdata(mem->spi->controller);

	if (!cqspi->ddata->execute_tuning)
		return -EOPNOTSUPP;

	return cqspi->ddata->execute_tuning(mem, read_op, write_op);
}

static int cqspi_of_get_flash_pdata(struct platform_device *pdev,
				    struct cqspi_flash_pdata *f_pdata,
				    struct device_node *np)
{
	if (of_property_read_u32(np, "cdns,read-delay", &f_pdata->read_delay)) {
		dev_err(&pdev->dev, "couldn't determine read-delay\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tshsl-ns", &f_pdata->tshsl_ns)) {
		dev_err(&pdev->dev, "couldn't determine tshsl-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tsd2d-ns", &f_pdata->tsd2d_ns)) {
		dev_err(&pdev->dev, "couldn't determine tsd2d-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tchsh-ns", &f_pdata->tchsh_ns)) {
		dev_err(&pdev->dev, "couldn't determine tchsh-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tslch-ns", &f_pdata->tslch_ns)) {
		dev_err(&pdev->dev, "couldn't determine tslch-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "spi-max-frequency", &f_pdata->clk_rate)) {
		dev_err(&pdev->dev, "couldn't determine spi-max-frequency\n");
		return -ENXIO;
	}

	if (f_pdata->cqspi->ddata->get_non_phy_clk_rate)
		f_pdata->non_phy_clk_rate =
			f_pdata->cqspi->ddata->get_non_phy_clk_rate(f_pdata->cqspi);

	return 0;
}

static int cqspi_of_get_pdata(struct cqspi_st *cqspi)
{
	struct device *dev = &cqspi->pdev->dev;
	struct device_node *np = dev->of_node;
	u32 id[2];

	cqspi->is_decoded_cs = of_property_read_bool(np, "cdns,is-decoded-cs");

	if (of_property_read_u32(np, "cdns,fifo-depth", &cqspi->fifo_depth)) {
		/* Zero signals FIFO depth should be runtime detected. */
		cqspi->fifo_depth = 0;
	}

	if (of_property_read_u32(np, "cdns,fifo-width", &cqspi->fifo_width)) {
		dev_err(dev, "couldn't determine fifo-width\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,trigger-address",
				 &cqspi->trigger_address)) {
		dev_err(dev, "couldn't determine trigger-address\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,phase-detect-selector",
				 &cqspi->phase_detect_selector)) {
		dev_warn(dev, "couldn't determine phase-detect-selector\n");
		cqspi->phase_detect_selector =
			CQSPI_REG_PHY_DLL_MASTER_DLY_ELMTS_LEN + 1;
	}

	if (of_property_read_u32(np, "num-cs", &cqspi->num_chipselect))
		cqspi->num_chipselect = CQSPI_MAX_CHIPSELECT;

	cqspi->rclk_en = of_property_read_bool(np, "cdns,rclk-en");

	if (!of_property_read_u32_array(np, "power-domains", id,
					ARRAY_SIZE(id)))
		cqspi->pd_dev_id = id[1];

	return 0;
}

static void cqspi_controller_init(struct cqspi_st *cqspi)
{
	u32 reg;

	/* Configure the remap address register, no remap */
	writel(0, cqspi->iobase + CQSPI_REG_REMAP);

	/* Disable all interrupts. */
	writel(0, cqspi->iobase + CQSPI_REG_IRQMASK);

	/* Configure the SRAM split to 1:1 . */
	writel(cqspi->fifo_depth / 2, cqspi->iobase + CQSPI_REG_SRAMPARTITION);

	/* Load indirect trigger address. */
	writel(cqspi->trigger_address,
	       cqspi->iobase + CQSPI_REG_INDIRECTTRIGGER);

	/* Program read watermark -- 1/2 of the FIFO. */
	writel(cqspi->fifo_depth * cqspi->fifo_width / 2,
	       cqspi->iobase + CQSPI_REG_INDIRECTRDWATERMARK);
	/* Program write watermark -- 1/8 of the FIFO. */
	writel(cqspi->fifo_depth * cqspi->fifo_width / 8,
	       cqspi->iobase + CQSPI_REG_INDIRECTWRWATERMARK);

	/* Disable direct access controller */
	if (!cqspi->use_direct_mode) {
		reg = readl(cqspi->iobase + CQSPI_REG_CONFIG);
		reg &= ~CQSPI_REG_CONFIG_ENB_DIR_ACC_CTRL;
		writel(reg, cqspi->iobase + CQSPI_REG_CONFIG);
	}

	/* Enable DMA interface */
	if (cqspi->use_dma_read) {
		reg = readl(cqspi->iobase + CQSPI_REG_CONFIG);
		reg |= CQSPI_REG_CONFIG_DMA_MASK;
		writel(reg, cqspi->iobase + CQSPI_REG_CONFIG);
	}
}

static void cqspi_controller_detect_fifo_depth(struct cqspi_st *cqspi)
{
	struct device *dev = &cqspi->pdev->dev;
	u32 reg, fifo_depth;

	/*
	 * Bits N-1:0 are writable while bits 31:N are read as zero, with 2^N
	 * the FIFO depth.
	 */
	writel(U32_MAX, cqspi->iobase + CQSPI_REG_SRAMPARTITION);
	reg = readl(cqspi->iobase + CQSPI_REG_SRAMPARTITION);
	fifo_depth = reg + 1;

	/* FIFO depth of zero means no value from devicetree was provided. */
	if (cqspi->fifo_depth == 0) {
		cqspi->fifo_depth = fifo_depth;
		dev_dbg(dev, "using FIFO depth of %u\n", fifo_depth);
	} else if (fifo_depth != cqspi->fifo_depth) {
		dev_warn(dev, "detected FIFO depth (%u) different from config (%u)\n",
			 fifo_depth, cqspi->fifo_depth);
	}
}

static int cqspi_request_mmap_dma(struct cqspi_st *cqspi)
{
	dma_cap_mask_t mask;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	cqspi->rx_chan = dma_request_chan_by_mask(&mask);
	if (IS_ERR(cqspi->rx_chan)) {
		int ret = (-EPROBE_DEFER);

		cqspi->rx_chan = NULL;
		if (ret == -ENODEV) {
			/* DMA support is not mandatory */
			dev_info(&cqspi->pdev->dev, "No Rx DMA available\n");
			return 0;
		}

		return dev_err_probe(&cqspi->pdev->dev, ret, "No Rx DMA available\n");
	}
	init_completion(&cqspi->rx_dma_complete);

	return 0;
}

static const char *cqspi_get_name(struct spi_mem *mem)
{
	struct cqspi_st *cqspi = spi_controller_get_devdata(mem->spi->controller);
	struct device *dev = &cqspi->pdev->dev;

	return devm_kasprintf(dev, GFP_KERNEL, "%s.%d", dev_name(dev),
			      spi_get_chipselect(mem->spi, 0));
}

static const struct spi_controller_mem_ops cqspi_mem_ops = {
	.exec_op = cqspi_exec_mem_op,
	.get_name = cqspi_get_name,
	.supports_op = cqspi_supports_mem_op,
	.execute_tuning = cqspi_mem_op_execute_tuning,
};

static const struct spi_controller_mem_caps cqspi_mem_caps = {
	.dtr = true,
	.per_op_freq = true,
};

static int cqspi_setup_flash(struct cqspi_st *cqspi)
{
	struct platform_device *pdev = cqspi->pdev;
	struct device *dev = &pdev->dev;
	struct cqspi_flash_pdata *f_pdata;
	int ret, cs, max_cs = -1;

	/* Get flash device data */
	for_each_available_child_of_node_scoped(dev->of_node, np) {
		ret = of_property_read_u32(np, "reg", &cs);
		if (ret) {
			dev_err(dev, "Couldn't determine chip select.\n");
			return ret;
		}

		if (cs >= cqspi->num_chipselect) {
			dev_err(dev, "Chip select %d out of range.\n", cs);
			return -EINVAL;
		}

		max_cs = max_t(int, cs, max_cs);

		f_pdata = &cqspi->f_pdata[cs];
		f_pdata->cqspi = cqspi;
		f_pdata->cs = cs;

		ret = cqspi_of_get_flash_pdata(pdev, f_pdata, np);
		if (ret)
			return ret;
	}

	if (max_cs < 0) {
		dev_err(dev, "No flash device declared\n");
		return -ENODEV;
	}

	cqspi->num_chipselect = max_cs + 1;
	return 0;
}

static int cqspi_jh7110_clk_init(struct platform_device *pdev, struct cqspi_st *cqspi)
{
	static struct clk_bulk_data qspiclk[] = {
		{ .id = "apb" },
		{ .id = "ahb" },
	};

	int ret = 0;

	ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(qspiclk), qspiclk);
	if (ret) {
		dev_err(&pdev->dev, "%s: failed to get qspi clocks\n", __func__);
		return ret;
	}

	cqspi->clks[CLK_QSPI_APB] = qspiclk[0].clk;
	cqspi->clks[CLK_QSPI_AHB] = qspiclk[1].clk;

	ret = clk_prepare_enable(cqspi->clks[CLK_QSPI_APB]);
	if (ret) {
		dev_err(&pdev->dev, "%s: failed to enable CLK_QSPI_APB\n", __func__);
		return ret;
	}

	ret = clk_prepare_enable(cqspi->clks[CLK_QSPI_AHB]);
	if (ret) {
		dev_err(&pdev->dev, "%s: failed to enable CLK_QSPI_AHB\n", __func__);
		goto disable_apb_clk;
	}

	cqspi->is_jh7110 = true;

	return 0;

disable_apb_clk:
	clk_disable_unprepare(cqspi->clks[CLK_QSPI_APB]);

	return ret;
}

static void cqspi_jh7110_disable_clk(struct platform_device *pdev, struct cqspi_st *cqspi)
{
	clk_disable_unprepare(cqspi->clks[CLK_QSPI_AHB]);
	clk_disable_unprepare(cqspi->clks[CLK_QSPI_APB]);
}
static int cqspi_probe(struct platform_device *pdev)
{
	const struct cqspi_driver_platdata *ddata;
	struct reset_control *rstc, *rstc_ocp, *rstc_ref;
	struct device *dev = &pdev->dev;
	struct spi_controller *host;
	struct resource *res_ahb;
	struct cqspi_st *cqspi;
	int ret;
	int irq;

	host = devm_spi_alloc_host(&pdev->dev, sizeof(*cqspi));
	if (!host)
		return -ENOMEM;

	host->mode_bits = SPI_RX_QUAD | SPI_RX_DUAL;
	host->mem_ops = &cqspi_mem_ops;
	host->mem_caps = &cqspi_mem_caps;
	host->dev.of_node = pdev->dev.of_node;

	cqspi = spi_controller_get_devdata(host);

	cqspi->pdev = pdev;
	cqspi->host = host;
	cqspi->is_jh7110 = false;
	cqspi->ddata = ddata = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, cqspi);

	/* Obtain configuration from OF. */
	ret = cqspi_of_get_pdata(cqspi);
	if (ret) {
		dev_err(dev, "Cannot get mandatory OF data.\n");
		return -ENODEV;
	}

	ret = cqspi_setup_flash(cqspi);
	if (ret) {
		dev_err(dev, "failed to setup flash parameters %d\n", ret);
		return ret;
	}

	/* Obtain QSPI clock. */
	cqspi->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(cqspi->clk)) {
		dev_err(dev, "Cannot claim QSPI clock.\n");
		ret = PTR_ERR(cqspi->clk);
		return ret;
	}

	/* Obtain and remap controller address. */
	cqspi->iobase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cqspi->iobase)) {
		dev_err(dev, "Cannot remap controller address.\n");
		ret = PTR_ERR(cqspi->iobase);
		return ret;
	}

	/* Obtain and remap AHB address. */
	cqspi->ahb_base = devm_platform_get_and_ioremap_resource(pdev, 1, &res_ahb);
	if (IS_ERR(cqspi->ahb_base)) {
		dev_err(dev, "Cannot remap AHB address.\n");
		ret = PTR_ERR(cqspi->ahb_base);
		return ret;
	}
	cqspi->mmap_phys_base = (dma_addr_t)res_ahb->start;
	cqspi->ahb_size = resource_size(res_ahb);

	init_completion(&cqspi->transfer_complete);

	/* Obtain IRQ line. */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -ENXIO;

	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;


	ret = clk_prepare_enable(cqspi->clk);
	if (ret) {
		dev_err(dev, "Cannot enable QSPI clock.\n");
		goto probe_clk_failed;
	}

	/* Obtain QSPI reset control */
	rstc = devm_reset_control_get_optional_exclusive(dev, "qspi");
	if (IS_ERR(rstc)) {
		ret = PTR_ERR(rstc);
		dev_err(dev, "Cannot get QSPI reset.\n");
		goto probe_reset_failed;
	}

	rstc_ocp = devm_reset_control_get_optional_exclusive(dev, "qspi-ocp");
	if (IS_ERR(rstc_ocp)) {
		ret = PTR_ERR(rstc_ocp);
		dev_err(dev, "Cannot get QSPI OCP reset.\n");
		goto probe_reset_failed;
	}

	if (of_device_is_compatible(pdev->dev.of_node, "starfive,jh7110-qspi")) {
		rstc_ref = devm_reset_control_get_optional_exclusive(dev, "rstc_ref");
		if (IS_ERR(rstc_ref)) {
			ret = PTR_ERR(rstc_ref);
			dev_err(dev, "Cannot get QSPI REF reset.\n");
			goto probe_reset_failed;
		}
		reset_control_assert(rstc_ref);
		reset_control_deassert(rstc_ref);
	}

	reset_control_assert(rstc);
	reset_control_deassert(rstc);

	reset_control_assert(rstc_ocp);
	reset_control_deassert(rstc_ocp);

	cqspi->master_ref_clk_hz = clk_get_rate(cqspi->clk);
	host->max_speed_hz = cqspi->master_ref_clk_hz;

	/* write completion is supported by default */
	cqspi->wr_completion = true;

	if (ddata) {
		if (ddata->quirks & CQSPI_NEEDS_WR_DELAY)
			cqspi->wr_delay = 50 * DIV_ROUND_UP(NSEC_PER_SEC,
						cqspi->master_ref_clk_hz);
		if (ddata->hwcaps_mask & CQSPI_SUPPORTS_OCTAL)
			host->mode_bits |= SPI_RX_OCTAL | SPI_TX_OCTAL;
		if (ddata->hwcaps_mask & CQSPI_SUPPORTS_QUAD)
			host->mode_bits |= SPI_TX_QUAD;
		if (!(ddata->quirks & CQSPI_DISABLE_DAC_MODE)) {
			cqspi->use_direct_mode = true;
			cqspi->use_direct_mode_wr = true;
		}
		if (ddata->quirks & CQSPI_SUPPORT_EXTERNAL_DMA)
			cqspi->use_dma_read = true;
		if (ddata->quirks & CQSPI_NO_SUPPORT_WR_COMPLETION)
			cqspi->wr_completion = false;
		if (ddata->quirks & CQSPI_SLOW_SRAM)
			cqspi->slow_sram = true;
		if (ddata->quirks & CQSPI_NEEDS_APB_AHB_HAZARD_WAR)
			cqspi->apb_ahb_hazard = true;

		if (ddata->jh7110_clk_init) {
			ret = cqspi_jh7110_clk_init(pdev, cqspi);
			if (ret)
				goto probe_reset_failed;
		}
		if (ddata->quirks & CQSPI_DISABLE_STIG_MODE)
			cqspi->disable_stig_mode = true;

		if (ddata->quirks & CQSPI_DMA_SET_MASK) {
			ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
			if (ret)
				goto probe_reset_failed;
		}
	}

	refcount_set(&cqspi->refcount, 1);
	refcount_set(&cqspi->inflight_ops, 1);

	ret = devm_request_irq(dev, irq, cqspi_irq_handler, 0,
			       pdev->name, cqspi);
	if (ret) {
		dev_err(dev, "Cannot request IRQ.\n");
		goto probe_reset_failed;
	}

	cqspi_wait_idle(cqspi);
	cqspi_controller_enable(cqspi, 0);
	cqspi_controller_detect_fifo_depth(cqspi);
	cqspi_controller_init(cqspi);
	cqspi_controller_enable(cqspi, 1);
	cqspi->current_cs = -1;
	cqspi->sclk = 0;

	if (!(ddata && (ddata->quirks & CQSPI_DISABLE_RUNTIME_PM))) {
		pm_runtime_enable(dev);
		pm_runtime_set_autosuspend_delay(dev, CQSPI_AUTOSUSPEND_TIMEOUT);
		pm_runtime_use_autosuspend(dev);
		pm_runtime_get_noresume(dev);
	}

	host->num_chipselect = cqspi->num_chipselect;

	if (ddata && (ddata->quirks & CQSPI_SUPPORT_DEVICE_RESET))
		cqspi_device_reset(cqspi);

	if (cqspi->use_direct_mode) {
		ret = cqspi_request_mmap_dma(cqspi);
		if (ret == -EPROBE_DEFER)
			goto probe_setup_failed;
	}

	ret = spi_register_controller(host);
	if (ret) {
		dev_err(&pdev->dev, "failed to register SPI ctlr %d\n", ret);
		goto probe_setup_failed;
	}

	if (!(ddata && (ddata->quirks & CQSPI_DISABLE_RUNTIME_PM))) {
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
	}

	return 0;
probe_setup_failed:
	if (!(ddata && (ddata->quirks & CQSPI_DISABLE_RUNTIME_PM)))
		pm_runtime_disable(dev);
	cqspi_controller_enable(cqspi, 0);
probe_reset_failed:
	if (cqspi->is_jh7110)
		cqspi_jh7110_disable_clk(pdev, cqspi);

	if (pm_runtime_get_sync(&pdev->dev) >= 0)
		clk_disable_unprepare(cqspi->clk);
probe_clk_failed:
	return ret;
}

static void cqspi_remove(struct platform_device *pdev)
{
	const struct cqspi_driver_platdata *ddata;
	struct cqspi_st *cqspi = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int ret = 0;

	ddata = of_device_get_match_data(dev);

	refcount_set(&cqspi->refcount, 0);

	if (!refcount_dec_and_test(&cqspi->inflight_ops))
		cqspi_wait_idle(cqspi);

	spi_unregister_controller(cqspi->host);

	if (cqspi->rx_chan)
		dma_release_channel(cqspi->rx_chan);

	if (!(ddata && (ddata->quirks & CQSPI_DISABLE_RUNTIME_PM)))
		ret = pm_runtime_get_sync(&pdev->dev);

	if (ret >= 0) {
		cqspi_controller_enable(cqspi, 0);
		clk_disable(cqspi->clk);
	}

	if (cqspi->is_jh7110)
		cqspi_jh7110_disable_clk(pdev, cqspi);

	if (!(ddata && (ddata->quirks & CQSPI_DISABLE_RUNTIME_PM))) {
		pm_runtime_put_sync(&pdev->dev);
		pm_runtime_disable(&pdev->dev);
	}
}

static void __maybe_unused cqspi_restore_context(struct cqspi_st *cqspi)
{
	struct device *dev = &cqspi->pdev->dev;
	int ret;

	cqspi_phy_set_dll_master(cqspi);

	ret = cqspi_phy_apply_setting(cqspi->f_pdata,
				      &cqspi->f_pdata->phy_setting);

	if (ret)
		dev_err(dev, "Applying phy_setting failed during restore\n");
}

static int cqspi_runtime_suspend(struct device *dev)
{
	struct cqspi_st *cqspi = dev_get_drvdata(dev);

	cqspi_controller_enable(cqspi, 0);
	clk_disable_unprepare(cqspi->clk);
	return 0;
}

static int cqspi_runtime_resume(struct device *dev)
{
	struct cqspi_st *cqspi = dev_get_drvdata(dev);

	clk_prepare_enable(cqspi->clk);
	cqspi_wait_idle(cqspi);
	cqspi_controller_enable(cqspi, 0);
	cqspi_controller_init(cqspi);
	cqspi_controller_enable(cqspi, 1);

	/*
	 * Only restore context if PHY is enabled, or else skip this step
	 */
	if ((cqspi->f_pdata->use_phy) == true)
		cqspi_restore_context(cqspi);

	cqspi->current_cs = -1;
	cqspi->sclk = 0;
	return 0;
}

static int cqspi_suspend(struct device *dev)
{
	struct cqspi_st *cqspi = dev_get_drvdata(dev);
	int ret;

	ret = spi_controller_suspend(cqspi->host);
	if (ret)
		return ret;

	return pm_runtime_force_suspend(dev);
}

static int cqspi_resume(struct device *dev)
{
	struct cqspi_st *cqspi = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret) {
		dev_err(dev, "pm_runtime_force_resume failed on resume\n");
		return ret;
	}

	return spi_controller_resume(cqspi->host);
}

static const struct dev_pm_ops cqspi_dev_pm_ops = {
	RUNTIME_PM_OPS(cqspi_runtime_suspend, cqspi_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(cqspi_suspend, cqspi_resume)
};

static const struct cqspi_driver_platdata cdns_qspi = {
	.quirks = CQSPI_DISABLE_DAC_MODE,
};

static const struct cqspi_driver_platdata k2g_qspi = {
	.quirks = CQSPI_NEEDS_WR_DELAY,
};

static const struct cqspi_driver_platdata am654_ospi = {
	.hwcaps_mask = CQSPI_SUPPORTS_OCTAL | CQSPI_SUPPORTS_QUAD,
	.quirks = CQSPI_NEEDS_WR_DELAY,
	.execute_tuning = cqspi_am654_ospi_execute_tuning,
	.get_non_phy_clk_rate = cqspi_am654_ospi_get_non_phy_clk_rate,
};

static const struct cqspi_driver_platdata intel_lgm_qspi = {
	.quirks = CQSPI_DISABLE_DAC_MODE,
};

static const struct cqspi_driver_platdata socfpga_qspi = {
	.quirks = CQSPI_DISABLE_DAC_MODE
			| CQSPI_NO_SUPPORT_WR_COMPLETION
			| CQSPI_SLOW_SRAM
			| CQSPI_DISABLE_STIG_MODE
			| CQSPI_DISABLE_RUNTIME_PM,
};

static const struct cqspi_driver_platdata versal_ospi = {
	.hwcaps_mask = CQSPI_SUPPORTS_OCTAL,
	.quirks = CQSPI_DISABLE_DAC_MODE | CQSPI_SUPPORT_EXTERNAL_DMA
			| CQSPI_DMA_SET_MASK,
	.indirect_read_dma = cqspi_versal_indirect_read_dma,
	.get_dma_status = cqspi_get_versal_dma_status,
};

static const struct cqspi_driver_platdata versal2_ospi = {
	.hwcaps_mask = CQSPI_SUPPORTS_OCTAL,
	.quirks = CQSPI_DISABLE_DAC_MODE | CQSPI_SUPPORT_EXTERNAL_DMA
			| CQSPI_DMA_SET_MASK
			| CQSPI_SUPPORT_DEVICE_RESET,
	.indirect_read_dma = cqspi_versal_indirect_read_dma,
	.get_dma_status = cqspi_get_versal_dma_status,
};

static const struct cqspi_driver_platdata jh7110_qspi = {
	.quirks = CQSPI_DISABLE_DAC_MODE,
	.jh7110_clk_init = cqspi_jh7110_clk_init,
};

static const struct cqspi_driver_platdata pensando_cdns_qspi = {
	.quirks = CQSPI_NEEDS_APB_AHB_HAZARD_WAR | CQSPI_DISABLE_DAC_MODE,
};

static const struct cqspi_driver_platdata mobileye_eyeq5_ospi = {
	.hwcaps_mask = CQSPI_SUPPORTS_OCTAL,
	.quirks = CQSPI_DISABLE_DAC_MODE | CQSPI_NO_SUPPORT_WR_COMPLETION |
			CQSPI_RD_NO_IRQ,
};

static const struct of_device_id cqspi_dt_ids[] = {
	{
		.compatible = "cdns,qspi-nor",
		.data = &cdns_qspi,
	},
	{
		.compatible = "ti,k2g-qspi",
		.data = &k2g_qspi,
	},
	{
		.compatible = "ti,am654-ospi",
		.data = &am654_ospi,
	},
	{
		.compatible = "intel,lgm-qspi",
		.data = &intel_lgm_qspi,
	},
	{
		.compatible = "xlnx,versal-ospi-1.0",
		.data = &versal_ospi,
	},
	{
		.compatible = "intel,socfpga-qspi",
		.data = &socfpga_qspi,
	},
	{
		.compatible = "starfive,jh7110-qspi",
		.data = &jh7110_qspi,
	},
	{
		.compatible = "amd,pensando-elba-qspi",
		.data = &pensando_cdns_qspi,
	},
	{
		.compatible = "mobileye,eyeq5-ospi",
		.data = &mobileye_eyeq5_ospi,
	},
	{
		.compatible = "amd,versal2-ospi",
		.data = &versal2_ospi,
	},
	{ /* end of table */ }
};

MODULE_DEVICE_TABLE(of, cqspi_dt_ids);

static struct platform_driver cqspi_platform_driver = {
	.probe = cqspi_probe,
	.remove = cqspi_remove,
	.driver = {
		.name = CQSPI_NAME,
		.pm = pm_ptr(&cqspi_dev_pm_ops),
		.of_match_table = cqspi_dt_ids,
	},
};

module_platform_driver(cqspi_platform_driver);

MODULE_DESCRIPTION("Cadence QSPI Controller Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" CQSPI_NAME);
MODULE_AUTHOR("Ley Foon Tan <lftan@altera.com>");
MODULE_AUTHOR("Graham Moore <grmoore@opensource.altera.com>");
MODULE_AUTHOR("Vadivel Murugan R <vadivel.muruganx.ramuthevar@intel.com>");
MODULE_AUTHOR("Vignesh Raghavendra <vigneshr@ti.com>");
MODULE_AUTHOR("Pratyush Yadav <p.yadav@ti.com>");
