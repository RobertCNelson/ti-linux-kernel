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
 * Mask for lower bits of team_usage, giving the weight 0..HPAGE_PMD_NR of the
 * page on its LRU: normal pages have weight 1, tails held unevictable until
 * head is evicted have weight 0, and the head gathers weight 1..HPAGE_PMD_NR.
 */
#define TEAM_LRU_WEIGHT_ONE	1L
#define TEAM_LRU_WEIGHT_MASK	((1L << (HPAGE_PMD_ORDER + 1)) - 1)

#define TEAM_HIGH_COUNTER	(1L << (HPAGE_PMD_ORDER + 1))
/*
 * Count how many pages of team are instantiated, as it is built up.
 */
#define TEAM_PAGE_COUNTER	TEAM_HIGH_COUNTER
#define TEAM_COMPLETE		(TEAM_PAGE_COUNTER << HPAGE_PMD_ORDER)
/*
 * And when complete, count how many huge mappings (like page_mapcount): an
 * incomplete team cannot be hugely mapped (would expose uninitialized holes).
 */
#define TEAM_MAPPING_COUNTER	TEAM_HIGH_COUNTER
#define TEAM_PMD_MAPPED	(TEAM_COMPLETE + TEAM_MAPPING_COUNTER)

/*
 * Returns true if this team is mapped by pmd somewhere.
 */
static inline bool team_pmd_mapped(struct page *head)
{
	return atomic_long_read(&head->team_usage) >= TEAM_PMD_MAPPED;
}

/*
 * Returns true if this was the first mapping by pmd, whereupon mapped stats
 * need to be updated.
 */
static inline bool inc_team_pmd_mapped(struct page *head)
{
	return atomic_long_add_return(TEAM_MAPPING_COUNTER, &head->team_usage)
		< TEAM_PMD_MAPPED + TEAM_MAPPING_COUNTER;
}

/*
 * Returns true if this was the last mapping by pmd, whereupon mapped stats
 * need to be updated.
 */
static inline bool dec_team_pmd_mapped(struct page *head)
{
	return atomic_long_sub_return(TEAM_MAPPING_COUNTER, &head->team_usage)
		< TEAM_PMD_MAPPED;
}

static inline void inc_lru_weight(struct page *head)
{
	atomic_long_inc(&head->team_usage);
	VM_BUG_ON_PAGE((atomic_long_read(&head->team_usage) &
			TEAM_LRU_WEIGHT_MASK) > HPAGE_PMD_NR, head);
}

static inline void set_lru_weight(struct page *page)
{
	VM_BUG_ON_PAGE(atomic_long_read(&page->team_usage) != 0, page);
	atomic_long_set(&page->team_usage, 1);
}

static inline void clear_lru_weight(struct page *page)
{
	VM_BUG_ON_PAGE(atomic_long_read(&page->team_usage) != 1, page);
	atomic_long_set(&page->team_usage, 0);
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
