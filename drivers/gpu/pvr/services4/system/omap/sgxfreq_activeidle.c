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

#include <linux/sysfs.h>
#include "sgxfreq.h"

static int activeidle_start(struct sgxfreq_sgx_data *data);
static void activeidle_stop(void);
static void activeidle_sgx_active(void);
static void activeidle_sgx_idle(void);

static struct activeidle_data {
	unsigned long freq_active;
	unsigned long freq_idle;
	struct mutex mutex;
	bool sgx_active;
} aid;

static struct sgxfreq_governor activeidle_gov = {
	.name =	"activeidle",
	.gov_start = activeidle_start,
	.gov_stop = activeidle_stop,
	.sgx_active = activeidle_sgx_active,
	.sgx_idle = activeidle_sgx_idle,
};

/*********************** begin sysfs interface ***********************/

extern struct kobject *sgxfreq_kobj;

static ssize_t show_freq_active(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%lu\n", aid.freq_active);
}

static ssize_t store_freq_active(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned long freq;

	ret = sscanf(buf, "%lu", &freq);
	if (ret != 1)
		return -EINVAL;

	freq = sgxfreq_get_freq_ceil(freq);

	mutex_lock(&aid.mutex);

	aid.freq_active = freq;
	if (aid.sgx_active)
		sgxfreq_set_freq_request(aid.freq_active);

	mutex_unlock(&aid.mutex);

	return count;
}

static ssize_t show_freq_idle(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sprintf(buf, "%lu\n", aid.freq_idle);
}

static ssize_t store_freq_idle(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int ret;
	unsigned long freq;

	ret = sscanf(buf, "%lu", &freq);
	if (ret != 1)
		return -EINVAL;

	freq = sgxfreq_get_freq_floor(freq);

	mutex_lock(&aid.mutex);

	aid.freq_idle = freq;
	if (!aid.sgx_active)
		sgxfreq_set_freq_request(aid.freq_idle);

	mutex_unlock(&aid.mutex);

	return count;
}
static DEVICE_ATTR(freq_active, 0644, show_freq_active, store_freq_active);
static DEVICE_ATTR(freq_idle, 0644, show_freq_idle, store_freq_idle);

static struct attribute *activeidle_attributes[] = {
	&dev_attr_freq_active.attr,
	&dev_attr_freq_idle.attr,
	NULL
};

static struct attribute_group activeidle_attr_group = {
	.attrs = activeidle_attributes,
	.name = "activeidle",
};

/************************ end sysfs interface ************************/

int activeidle_init(void)
{
	int ret;

	mutex_init(&aid.mutex);

	ret = sgxfreq_register_governor(&activeidle_gov);
	if (ret)
		return ret;

	aid.freq_idle = sgxfreq_get_freq_min();
	aid.freq_active = sgxfreq_get_freq_max();

	return 0;
}

int activeidle_deinit(void)
{
	return 0;
}

static int activeidle_start(struct sgxfreq_sgx_data *data)
{
	int ret;

	aid.sgx_active = data->active;

	ret = sysfs_create_group(sgxfreq_kobj, &activeidle_attr_group);
	if (ret)
		return ret;

	if (aid.sgx_active)
		sgxfreq_set_freq_request(aid.freq_active);
	else
		sgxfreq_set_freq_request(aid.freq_idle);

	return 0;
}

static void activeidle_stop(void)
{
	sysfs_remove_group(sgxfreq_kobj, &activeidle_attr_group);
}

static void activeidle_sgx_active(void)
{
	mutex_lock(&aid.mutex);

	aid.sgx_active = true;
	sgxfreq_set_freq_request(aid.freq_active);

	mutex_unlock(&aid.mutex);
}

static void activeidle_sgx_idle(void)
{
	mutex_lock(&aid.mutex);

	aid.sgx_active = false;
	sgxfreq_set_freq_request(aid.freq_idle);

	mutex_unlock(&aid.mutex);
}
