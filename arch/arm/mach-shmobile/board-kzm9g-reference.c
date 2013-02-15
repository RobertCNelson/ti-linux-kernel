/*
 * KZM-A9-GT board support - Reference Device Tree Implementation
 *
 * Copyright (C) 2012	Horms Solutions Ltd.
 *
 * Based on board-kzm9g.c
 * Copyright (C) 2012	Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/input.h>
#include <linux/of_platform.h>
#include <mach/sh73a0.h>
#include <mach/common.h>
#include <asm/hardware/cache-l2x0.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>

static void __init kzm_init(void)
{
	sh73a0_add_standard_devices_dt();
	sh73a0_pinmux_init();

	/* enable SCIFA4 */
	gpio_request(GPIO_FN_SCIFA4_TXD, NULL);
	gpio_request(GPIO_FN_SCIFA4_RXD, NULL);
	gpio_request(GPIO_FN_SCIFA4_RTS_, NULL);
	gpio_request(GPIO_FN_SCIFA4_CTS_, NULL);

	/* enable MMCIF */
	gpio_request(GPIO_FN_MMCCLK0,		NULL);
	gpio_request(GPIO_FN_MMCCMD0_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_0_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_1_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_2_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_3_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_4_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_5_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_6_PU,	NULL);
	gpio_request(GPIO_FN_MMCD0_7_PU,	NULL);

	/* enable SD */
	gpio_request(GPIO_FN_SDHIWP0,		NULL);
	gpio_request(GPIO_FN_SDHICD0,		NULL);
	gpio_request(GPIO_FN_SDHICMD0,		NULL);
	gpio_request(GPIO_FN_SDHICLK0,		NULL);
	gpio_request(GPIO_FN_SDHID0_3,		NULL);
	gpio_request(GPIO_FN_SDHID0_2,		NULL);
	gpio_request(GPIO_FN_SDHID0_1,		NULL);
	gpio_request(GPIO_FN_SDHID0_0,		NULL);
	gpio_request(GPIO_FN_SDHI0_VCCQ_MC0_ON,	NULL);
	gpio_request_one(GPIO_PORT15, GPIOF_OUT_INIT_HIGH, NULL); /* power */

	/* enable Micro SD */
	gpio_request(GPIO_FN_SDHID2_0,		NULL);
	gpio_request(GPIO_FN_SDHID2_1,		NULL);
	gpio_request(GPIO_FN_SDHID2_2,		NULL);
	gpio_request(GPIO_FN_SDHID2_3,		NULL);
	gpio_request(GPIO_FN_SDHICMD2,		NULL);
	gpio_request(GPIO_FN_SDHICLK2,		NULL);
	gpio_request_one(GPIO_PORT14, GPIOF_OUT_INIT_HIGH, NULL); /* power */

	/* I2C 3 */
	gpio_request(GPIO_FN_PORT27_I2C_SCL3, NULL);
	gpio_request(GPIO_FN_PORT28_I2C_SDA3, NULL);

#ifdef CONFIG_CACHE_L2X0
	/* Early BRESP enable, Shared attribute override enable, 64K*8way */
	l2x0_init(IOMEM(0xf0100000), 0x40460000, 0x82000fff);
#endif
}

static const char *kzm9g_boards_compat_dt[] __initdata = {
	"renesas,kzm9g-reference",
	NULL,
};

DT_MACHINE_START(KZM9G_DT, "kzm9g-reference")
	.smp		= smp_ops(sh73a0_smp_ops),
	.map_io		= sh73a0_map_io,
	.init_early	= sh73a0_init_delay,
	.nr_irqs	= NR_IRQS_LEGACY,
	.init_irq	= irqchip_init,
	.init_machine	= kzm_init,
	.init_time	= shmobile_timer_init,
	.dt_compat	= kzm9g_boards_compat_dt,
MACHINE_END
