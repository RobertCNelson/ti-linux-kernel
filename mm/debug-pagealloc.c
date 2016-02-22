#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/page_ext.h>
#include <linux/poison.h>
#include <linux/ratelimit.h>

void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	kernel_poison_pages(page, numpages, enable);
}
