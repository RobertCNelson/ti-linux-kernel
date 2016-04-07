#ifndef _LINUX_PAGETEAM_H
#define _LINUX_PAGETEAM_H

/*
 * Declarations and definitions for PageTeam pages and page->team_usage:
 * as implemented for "huge tmpfs" in mm/shmem.c and mm/huge_memory.c, when
 * CONFIG_TRANSPARENT_HUGEPAGE=y, and tmpfs is mounted with the huge=1 option.
 */

#include <linux/huge_mm.h>
#include <linux/mm_types.h>
#include <linux/mmdebug.h>
#include <asm/page.h>

static inline struct page *team_head(struct page *page)
{
	struct page *head = page - (page->index & (HPAGE_PMD_NR-1));
	/*
	 * Locating head by page->index is a faster calculation than by
	 * pfn_to_page(page_to_pfn), and we only use this function after
	 * page->index has been set (never on tail holes): but check that.
	 *
	 * Although this is only used on a PageTeam(page), the team might be
	 * disbanded racily, so it's not safe to VM_BUG_ON(!PageTeam(page));
	 * but page->index remains stable across disband and truncation.
	 */
	VM_BUG_ON_PAGE(head != pfn_to_page(round_down(page_to_pfn(page),
						      HPAGE_PMD_NR)), page);
	return head;
}

#endif /* _LINUX_PAGETEAM_H */
