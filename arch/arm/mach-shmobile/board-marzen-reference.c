/*
 * marzen board support - Reference DT implementation
 *
 * Copyright (C) 2011  Renesas Solutions Corp.
 * Copyright (C) 2011  Magnus Damm
 * Copyright (C) 2013  Simon Horman
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

#include <linux/gpio.h>
#include <mach/r8a7779.h>
#include <mach/common.h>
#include <mach/irqs.h>
#include <asm/irq.h>
#include <asm/mach/arch.h>

static void __init marzen_init(void)
{
	r8a7779_pinmux_init();

	/* SCIF2 (CN18: DEBUG0) */
	gpio_request(GPIO_FN_TX2_C, NULL);
	gpio_request(GPIO_FN_RX2_C, NULL);

	/* SCIF4 (CN19: DEBUG1) */
	gpio_request(GPIO_FN_TX4, NULL);
	gpio_request(GPIO_FN_RX4, NULL);

	/* LAN89218 */
	gpio_request(GPIO_FN_EX_CS0, NULL); /* nCS */
	gpio_request(GPIO_FN_IRQ1_B, NULL); /* IRQ + PME */

	/* SD0 (CN20) */
	gpio_request(GPIO_FN_SD0_CLK, NULL);
	gpio_request(GPIO_FN_SD0_CMD, NULL);
	gpio_request(GPIO_FN_SD0_DAT0, NULL);
	gpio_request(GPIO_FN_SD0_DAT1, NULL);
	gpio_request(GPIO_FN_SD0_DAT2, NULL);
	gpio_request(GPIO_FN_SD0_DAT3, NULL);
	gpio_request(GPIO_FN_SD0_CD, NULL);
	gpio_request(GPIO_FN_SD0_WP, NULL);

	r8a7779_add_standard_devices_dt();
}

static const char *marzen_boards_compat_dt[] __initdata = {
	"renesas,marzen-reference",
	NULL,
};

DT_MACHINE_START(MARZEN, "marzen")
	.smp		= smp_ops(r8a7779_smp_ops),
	.map_io		= r8a7779_map_io,
	.init_early	= r8a7779_init_delay,
	.nr_irqs	= NR_IRQS_LEGACY,
	.init_irq	= r8a7779_init_irq_dt,
	.init_machine	= marzen_init,
	.init_time	= shmobile_timer_init,
	.dt_compat	= marzen_boards_compat_dt,
MACHINE_END
