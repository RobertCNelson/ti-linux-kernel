/*
 * TI Wakeup M3 Power Management Routines
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 * Dave Gerlach <d-gerlach@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __ASSEMBLER__

struct wkup_m3_wakeup_src {
	int irq_nr;
	char src[10];
};

struct am33xx_ipc_regs {
	u32 reg0;
	u32 reg1;
	u32 reg2;
	u32 reg3;
	u32 reg4;
	u32 reg5;
	u32 reg6;
	u32 reg7;
};

int wkup_m3_prepare(void);
void wkup_m3_register_txev_handler(void (*txev_handler)(void));
int wkup_m3_ping(void);
struct wkup_m3_wakeup_src wkup_m3_wake_src(void);
int wkup_m3_pm_status(void);
void wkup_m3_fw_version_clear(void);
int wkup_m3_fw_version_read(void);
void wkup_m3_pm_set_cmd(struct am33xx_ipc_regs *ipc_regs);

#endif

