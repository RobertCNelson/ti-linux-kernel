/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM bl_hib

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_S2D_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_S2D_H

#include <trace/hooks/vendor_hooks.h>

struct file;

DECLARE_HOOK(android_vh_check_hibernation_swap,
	TP_PROTO(struct file *resume_block_file, bool *hib_swap),
	TP_ARGS(resume_block_file, hib_swap));

DECLARE_HOOK(android_vh_save_cpu_resume,
	TP_PROTO(u64 *addr, u64 phys_addr),
	TP_ARGS(addr, phys_addr));

DECLARE_HOOK(android_vh_save_hib_resume_bdev,
	TP_PROTO(struct file *hib_resume_bdev_file),
	TP_ARGS(hib_resume_bdev_file));

DECLARE_HOOK(android_vh_encrypt_page,
	TP_PROTO(void *buf),
	TP_ARGS(buf));

DECLARE_HOOK(android_vh_init_aes_encrypt,
	TP_PROTO(void *unused),
	TP_ARGS(unused));

DECLARE_HOOK(android_vh_skip_swap_map_write,
	TP_PROTO(bool *skip),
	TP_ARGS(skip));

DECLARE_HOOK(android_vh_post_image_save,
	TP_PROTO(unsigned short root_swap),
	TP_ARGS(root_swap));

DECLARE_HOOK(android_vh_hibernated_do_mem_alloc,
        TP_PROTO(unsigned long nr_pages, unsigned int swsusp_header_flags,
                 int *ret),
        TP_ARGS(nr_pages, swsusp_header_flags, ret));

DECLARE_HOOK(android_vh_hibernate_save_cmp_len,
        TP_PROTO(size_t cmp_len),
        TP_ARGS(cmp_len));

#endif /* _TRACE_HOOK_S2D_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
