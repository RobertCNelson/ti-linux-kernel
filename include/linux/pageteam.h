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

/*
 * Returns true if this team is mapped by pmd somewhere.
 */
static inline bool team_pmd_mapped(struct page *head)
{
	return atomic_long_read(&head->team_usage) > HPAGE_PMD_NR;
}

/*
 * Returns true if this was the first mapping by pmd, whereupon mapped stats
 * need to be updated.
 */
static inline bool inc_team_pmd_mapped(struct page *head)
{
	return atomic_long_inc_return(&head->team_usage) == HPAGE_PMD_NR+1;
}

/*
 * Returns true if this was the last mapping by pmd, whereupon mapped stats
 * need to be updated.
 */
static inline bool dec_team_pmd_mapped(struct page *head)
{
	return atomic_long_dec_return(&head->team_usage) == HPAGE_PMD_NR;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int map_team_by_pmd(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd, struct page *page);
void unmap_team_by_pmd(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd, struct page *page);
void remap_team_by_ptes(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd);
#else
static inline int map_team_by_pmd(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd, struct page *page)
{
	VM_BUG_ON_PAGE(1, page);
	return 0;
}
static inline void unmap_team_by_pmd(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd, struct page *page)
{
	VM_BUG_ON_PAGE(1, page);
}
static inline void remap_team_by_ptes(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd)
{
	VM_BUG_ON(1);
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#endif /* _LINUX_PAGETEAM_H */
