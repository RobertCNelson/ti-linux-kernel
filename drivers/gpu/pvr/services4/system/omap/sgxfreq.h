/*
 * Copyright (C) 2012 Texas Instruments, Inc
 *
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifndef SGXFREQ_H
#define SGXFREQ_H

#include <linux/device.h>
#include <linux/time.h>

#define SGXFREQ_NAME_LEN 16

//#define SGXFREQ_DEBUG_FTRACE
#if defined(SGXFREQ_DEBUG_FTRACE)
#define SGXFREQ_TRACE(...) trace_printk(__VA_ARGS__)
#else
#define SGXFREQ_TRACE(...)
#endif

struct sgxfreq_sgx_data {
	bool clk_on;
	bool active;
};

struct sgxfreq_governor {
	char name[SGXFREQ_NAME_LEN];
	int (*gov_start) (struct sgxfreq_sgx_data *data);
	void (*gov_stop) (void);
	void (*sgx_clk_on) (void);
	void (*sgx_clk_off) (void);
	void (*sgx_active) (void);
	void (*sgx_idle) (void);
	void (*sgx_frame_done) (void);
	struct list_head governor_list;
};

/* sgxfreq_init must be called before any other api */
int sgxfreq_init(struct device *dev);
int sgxfreq_deinit(void);

int sgxfreq_register_governor(struct sgxfreq_governor *governor);
void sgxfreq_unregister_governor(struct sgxfreq_governor *governor);

int sgxfreq_set_governor(const char *name);

int sgxfreq_get_freq_list(unsigned long **pfreq_list);

unsigned long sgxfreq_get_freq_min(void);
unsigned long sgxfreq_get_freq_max(void);

unsigned long sgxfreq_get_freq_floor(unsigned long freq);
unsigned long sgxfreq_get_freq_ceil(unsigned long freq);

unsigned long sgxfreq_get_freq(void);
unsigned long sgxfreq_get_freq_request(void);
unsigned long sgxfreq_get_freq_limit(void);

unsigned long sgxfreq_set_freq_request(unsigned long freq_request);
unsigned long sgxfreq_set_freq_limit(unsigned long freq_limit);

unsigned long sgxfreq_get_total_active_time(void);
unsigned long sgxfreq_get_total_idle_time(void);

/* Helper functions */
static inline unsigned long __tv2msec(struct timeval tv)
{
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

static inline unsigned long __delta32(unsigned long a, unsigned long b)
{
	if (a >= b)
		return a - b;
	else
		return 1 + (0xFFFFFFFF - b) + a;
}

/* External notifications to sgxfreq */
void sgxfreq_notif_sgx_clk_on(void);
void sgxfreq_notif_sgx_clk_off(void);
void sgxfreq_notif_sgx_active(void);
void sgxfreq_notif_sgx_idle(void);
void sgxfreq_notif_sgx_frame_done(void);

#endif
