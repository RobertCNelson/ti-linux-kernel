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


static int userspace_start(struct sgxfreq_sgx_data *data);
static void userspace_stop(void);


static struct sgxfreq_governor userspace_gov = {
	.name =	"userspace",
	.gov_start = userspace_start,
	.gov_stop = userspace_stop,
};


static struct userspace_data {
	unsigned long freq_user; /* in Hz */
} usd;


/*********************** begin sysfs interface ***********************/

extern struct kobject *sgxfreq_kobj;


static ssize_t show_frequency_set(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "%lu\n", usd.freq_user);
}


static ssize_t store_frequency_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long freq;

	ret = sscanf(buf, "%lu", &freq);
	if (ret != 1)
		return -EINVAL;

	if (freq > sgxfreq_get_freq_max())
		freq = sgxfreq_get_freq_max();
	usd.freq_user = sgxfreq_set_freq_request(freq);
	trace_printk("USERSPACE: new freq=%luHz.\n", usd.freq_user);

	return count;
}


static DEVICE_ATTR(frequency_set, 0644,
	show_frequency_set, store_frequency_set);


static struct attribute *userspace_attributes[] = {
	&dev_attr_frequency_set.attr,
	NULL
};


static struct attribute_group userspace_attr_group = {
	.attrs = userspace_attributes,
	.name = "userspace",
};

/************************ end sysfs interface ************************/


int userspace_init(void)
{
	int ret;

	ret = sgxfreq_register_governor(&userspace_gov);
	if (ret)
		return ret;
	return 0;
}


int userspace_deinit(void)
{
	return 0;
}


static int userspace_start(struct sgxfreq_sgx_data *data)
{
	int ret;

	usd.freq_user = sgxfreq_get_freq();

	ret = sysfs_create_group(sgxfreq_kobj, &userspace_attr_group);
	if (ret)
		return ret;

	trace_printk("USERSPACE: started.\n");

	return 0;
}


static void userspace_stop(void)
{
	sysfs_remove_group(sgxfreq_kobj, &userspace_attr_group);

	trace_printk("USERSPACE: stopped.\n");
}
