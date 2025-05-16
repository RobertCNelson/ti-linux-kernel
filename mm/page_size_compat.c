// SPDX-License-Identifier: GPL-2.0
/*
 * Page Size Emulation
 *
 * Copyright (c) 2024, Google LLC.
 * Author: Kalesh Singh <kaleshsingh@goole.com>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/page_size_compat.h>

#define MIN_PAGE_SHIFT_COMPAT (PAGE_SHIFT + 1)
#define MAX_PAGE_SHIFT_COMPAT 16 /* Max of 64KB */
#define __MMAP_RND_BITS(x)      (x - (__PAGE_SHIFT - PAGE_SHIFT))

DEFINE_STATIC_KEY_FALSE(page_shift_compat_enabled);
EXPORT_SYMBOL_GPL(page_shift_compat_enabled);

int page_shift_compat __ro_after_init = MIN_PAGE_SHIFT_COMPAT;
EXPORT_SYMBOL_GPL(page_shift_compat);

static int __init early_page_shift_compat(char *buf)
{
	int ret;

	ret = kstrtoint(buf, 10, &page_shift_compat);
	if (ret)
		return ret;

	/* Only supported on 4KB kernel */
	if (PAGE_SHIFT != 12)
		return -ENOTSUPP;

	if (page_shift_compat < MIN_PAGE_SHIFT_COMPAT ||
		page_shift_compat > MAX_PAGE_SHIFT_COMPAT)
		return -EINVAL;

	static_branch_enable(&page_shift_compat_enabled);

	return 0;
}
early_param("page_shift", early_page_shift_compat);

static int __init init_mmap_rnd_bits(void)
{
	if (!static_branch_unlikely(&page_shift_compat_enabled))
		return 0;

#ifdef CONFIG_HAVE_ARCH_MMAP_RND_BITS
	mmap_rnd_bits_min = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS_MIN);
	mmap_rnd_bits_max = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS_MAX);
	mmap_rnd_bits = __MMAP_RND_BITS(CONFIG_ARCH_MMAP_RND_BITS);
#endif

	return 0;
}
core_initcall(init_mmap_rnd_bits);

/*
 * Returns size of the portion of the VMA backed by the
 * underlying file.
 */
unsigned long ___filemap_len(struct inode *inode, unsigned long pgoff, unsigned long len,
			     unsigned long flags)
{
	unsigned long file_size;
	unsigned long filemap_len;
	pgoff_t max_pgcount;
	pgoff_t last_pgoff;

	if (flags & __MAP_NO_COMPAT)
		return len;

	file_size = (unsigned long) i_size_read(inode);

	/*
	 * Round up, so that this is a count (not an index). This simplifies
	 * the following calculations.
	 */
	max_pgcount = DIV_ROUND_UP(file_size, PAGE_SIZE);
	last_pgoff = pgoff + (len >> PAGE_SHIFT);

	if (unlikely(last_pgoff >= max_pgcount)) {
		filemap_len = (max_pgcount - pgoff)  << PAGE_SHIFT;
		/* Careful of underflows in special files */
		if (filemap_len > 0 && filemap_len < len)
			return filemap_len;
	}

	return len;
}

static inline bool is_shmem_fault(const struct vm_operations_struct *vm_ops)
{
#ifdef CONFIG_SHMEM
	return vm_ops->fault == shmem_fault;
#else
	return false;
#endif
}

static inline bool is_f2fs_filemap_fault(const struct vm_operations_struct *vm_ops)
{
#ifdef CONFIG_F2FS_FS
	return vm_ops->fault == f2fs_filemap_fault;
#else
	return false;
#endif
}

static inline bool is_filemap_fault(const struct vm_operations_struct *vm_ops)
{
	return vm_ops->fault == filemap_fault;
}

/*
 * Given a file mapping of 48KiB backed by a file of size 18KiB, the
 * faulting behaviour of the different page-size configurations is
 * explained below.

 * In a 4KiB base page size system, when a file backed mapping extends
 * past the end of the file, accessed is allowed to the entire last
 * page that at least partially corresponds to valid offsets on the
 * file. However, access beyond that page will generate a SIGBUS, since
 * the offset we are trying to fault doesn't correspond to anywhere on
 * the backing file.
 *
 * This is illustrated below. The offsets are given in units of KiB.
 *
 *                    Access OK (4KiB page paritially backed by file)
 *                                │
 *    ┌──────────────────────────┬┼─┬─────────────────────────────────────────┐
 *    │                          │▼ │                                         │
 *    │       File backed        │  │     SIGBUS (Invalid filemap_fault)      │
 *    │                          │  │                                         │
 *    └──────────────────────────┴──┴─────────────────────────────────────────┘
 *
 *    └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
 *    0     4     8     12   16    20    24    28    32    36    40    44    48
 *
 * In a x86_64 emulated 16KiB page size system, userspace beleives the page
 * size is 16KiB and therefore shoud be able to access the entire last 16KiB page
 * that is at least partially backed by the file. However, the kernel is still a
 * 4KiB kernel and will fault at each 4KiB page that makes up the "emulated"
 * 16KiB page, which will generate a SIGBUS any of the 4KiB pages making up the
 * 16KiB expect the first is being faulted.
 *
 *                    Access OK (4KiB page paritially backed by file)
 *                                │
 *    ┌──────────────────────────┬┼─┬─────────────────────────────────────────┐
 *    │                          │▼ │                                         │
 *    │       File backed        │  │     SIGBUS (Invalid filemap_fault)      │
 *    │                          │  │                                         │
 *    └──────────────────────────┴──┴─────────────────────────────────────────┘
 *
 *    └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
 *    0     4     8     12   16    20    24    28    32    36    40    44    48
 *
 * To fix this semantic in the emulated page size mode an anonymous mapping is
 * inserted into to replace the full 4KiB pages that make up the last 16KiB page
 * partially backed by the file.
 *
 *
 *                    Access OK (4KiB page paritially backed by file)
 *                                │
 *                                │        ┌─── Access OK
 *                                │        │   (16KiB page partially backed
 *                                │        │       by file)
 *                                │        │
 *    ┌──────────────────────────┬┼─┬──────┼──────────┬───────────────────────┐
 *    │                          │▼ │      ▼          │      SIGBUS           │
 *    │       File backed        │  │   Access OK     │(Invalid filemap fault)│
 *    │                          │  │  (Anon Mapping) │                       │
 *    └──────────────────────────┴──┴─────────────────┴───────────────────────┘
 *
 *    └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
 *    0     4     8     12   16    20    24    28    32    36    40    44    48
 */
void ___filemap_fixup(unsigned long addr, unsigned long prot, unsigned long file_backed_len,
		      unsigned long len)
{
	unsigned long anon_addr = addr + file_backed_len;
	unsigned long __offset = __offset_in_page(anon_addr);
	unsigned long anon_len = __offset ? __PAGE_SIZE - __offset : 0;
	struct mm_struct *mm = current->mm;
	unsigned long populate = 0;
	struct vm_area_struct *vma;
	const struct vm_operations_struct *vm_ops;

	if (!anon_len)
		return;

	BUG_ON(anon_len >= __PAGE_SIZE);

	/* The original do_mmap() failed */
	if (IS_ERR_VALUE(addr))
		return;

	vma = find_vma(mm, addr);

	/*
	 * This should never happen, VMA was inserted and we still
	 * haven't released the mmap write lock.
	 */
	BUG_ON(!vma);

	vm_ops = vma->vm_ops;
	if (!vm_ops)
		return;

	/*
	 * Insert fixup vmas for file backed and shmem backed VMAs.
	 *
	 * Faulting off the end of a file will result in SIGBUS since there is no
	 * file page for the given file offset.
	 *
	 * shmem pages live in page cache or swap cache. Looking up a page cache
	 * page with an index (pgoff) beyond the file is invalid and will result
	 * in shmem_get_folio_gfp() returning -EINVAL.
	 */
	if (!is_filemap_fault(vm_ops) && !is_f2fs_filemap_fault(vm_ops) &&
	    !is_shmem_fault(vm_ops))
		return;

	/*
	 * Override the partial emulated page of the file backed portion of the VMA
	 * with an anonymous mapping.
	 */
	anon_addr = do_mmap(NULL, anon_addr, anon_len, prot,
					MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|__MAP_NO_COMPAT,
					0, 0, &populate, NULL);
}

/*
 * Folds any anon fixup entries created by ___filemap_fixup()
 * into the previous mapping so that /proc/<pid>/[s]maps don't
 * show unaliged entries.
 */
void __fold_filemap_fixup_entry(struct vma_iterator *iter, unsigned long *end)
{
	struct vm_area_struct *next_vma;

	/* Not emulating page size? */
	if (!static_branch_unlikely(&page_shift_compat_enabled))
		return;

	/* Advance iterator */
	next_vma = vma_next(iter);

	/* If fixup VMA, adjust the end to cover its extent */
	if (next_vma && (next_vma->vm_flags & __VM_NO_COMPAT)) {
		*end = next_vma->vm_end;
		return;
	}

	/* Rewind iterator */
	vma_prev(iter);
}
