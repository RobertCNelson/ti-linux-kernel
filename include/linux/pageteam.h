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
 * Layout of team head's page->team_usage field, as on x86_64 and arm64_4K:
 *
 *  63        32 31          22 21      12     11         10    9          0
 * +------------+--------------+----------+----------+---------+------------+
 * | pmd_mapped & instantiated |pte_mapped| reserved | mlocked | lru_weight |
 * |   42 bits       10 bits   |  10 bits |  1 bit   |  1 bit  |   10 bits  |
 * +------------+--------------+----------+----------+---------+------------+
 *
 * TEAM_LRU_WEIGHT_ONE               1  (1<<0)
 * TEAM_LRU_WEIGHT_MASK            3ff  (1<<10)-1
 * TEAM_PMD_MLOCKED                400  (1<<10)
 * TEAM_RESERVED_FLAG              800  (1<<11)
 * TEAM_PTE_COUNTER               1000  (1<<12)
 * TEAM_PTE_MASK                3ff000  (1<<22)-(1<<12)
 * TEAM_PAGE_COUNTER            400000  (1<<22)
 * TEAM_COMPLETE              80000000  (1<<31)
 * TEAM_MAPPING_COUNTER         400000  (1<<22)
 * TEAM_PMD_MAPPED            80400000  (1<<31)
 *
 * The upper bits count up to TEAM_COMPLETE as pages are instantiated,
 * and then, above TEAM_COMPLETE, they count huge mappings of the team.
 * Team tails have team_usage either 1 (lru_weight 1) or 0 (lru_weight 0).
 */
/*
 * Mask for lower bits of team_usage, giving the weight 0..HPAGE_PMD_NR of the
 * page on its LRU: normal pages have weight 1, tails held unevictable until
 * head is evicted have weight 0, and the head gathers weight 1..HPAGE_PMD_NR.
 */
#define TEAM_LRU_WEIGHT_ONE	1L
#define TEAM_LRU_WEIGHT_MASK	((1L << (HPAGE_PMD_ORDER + 1)) - 1)
/*
 * Single bit to indicate whether team is hugely mlocked (like PageMlocked).
 * Then another bit reserved for experiments with other team flags.
 */
#define TEAM_PMD_MLOCKED	(1L << (HPAGE_PMD_ORDER + 1))
#define TEAM_RESERVED_FLAG	(1L << (HPAGE_PMD_ORDER + 2))
#ifdef CONFIG_64BIT
/*
 * Count how many pages of team are individually mapped into userspace.
 */
#define TEAM_PTE_COUNTER	(1L << (HPAGE_PMD_ORDER + 3))
#define TEAM_HIGH_COUNTER	(1L << (2*HPAGE_PMD_ORDER + 4))
#define TEAM_PTE_MASK		(TEAM_HIGH_COUNTER - TEAM_PTE_COUNTER)
#define team_pte_count(usage)	(((usage) & TEAM_PTE_MASK) / TEAM_PTE_COUNTER)
#else /* 32-bit */
/*
 * Not enough bits in atomic_long_t: we prefer not to bloat struct page just to
 * avoid duplication in Mapped, when a page is mapped both hugely and unhugely.
 */
#define TEAM_HIGH_COUNTER	(1L << (HPAGE_PMD_ORDER + 3))
#define team_pte_count(usage)	1 /* allows for the extra page_add_file_rmap */
#endif /* CONFIG_64BIT */
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
 * need to be updated.  Together with the number of pages which then need
 * to be accounted (can be ignored when false returned): because some team
 * members may have been mapped unhugely by pte, so already counted as Mapped.
 */
static inline bool inc_team_pmd_mapped(struct page *head, int *nr_pages)
{
	long team_usage;

	team_usage = atomic_long_add_return(TEAM_MAPPING_COUNTER,
					    &head->team_usage);
	*nr_pages = HPAGE_PMD_NR - team_pte_count(team_usage);
	return team_usage < TEAM_PMD_MAPPED + TEAM_MAPPING_COUNTER;
}

/*
 * Returns true if this was the last mapping by pmd, whereupon mapped stats
 * need to be updated.  Together with the number of pages which then need
 * to be accounted (can be ignored when false returned): because some team
 * members may still be mapped unhugely by pte, so remain counted as Mapped.
 */
static inline bool dec_team_pmd_mapped(struct page *head, int *nr_pages)
{
	long team_usage;

	team_usage = atomic_long_sub_return(TEAM_MAPPING_COUNTER,
					    &head->team_usage);
	*nr_pages = HPAGE_PMD_NR - team_pte_count(team_usage);
	return team_usage < TEAM_PMD_MAPPED;
}

/*
 * Supplies those values which mem_cgroup_move_account()
 * needs to maintain memcg's huge tmpfs stats correctly.
 */
static inline void count_team_pmd_mapped(struct page *head, int *file_mapped,
					 bool *pmd_mapped, bool *team_complete)
{
	long team_usage;

	*file_mapped = 1;
	team_usage = atomic_long_read(&head->team_usage);
	*team_complete = team_usage >= TEAM_COMPLETE;
	*pmd_mapped = team_usage >= TEAM_PMD_MAPPED;
	if (*pmd_mapped)
		*file_mapped = HPAGE_PMD_NR - team_pte_count(team_usage);
}

/*
 * Slightly misnamed, team_page_mapcount() returns the number of times
 * any page is mapped into userspace, either by pte or covered by pmd:
 * it is a generalization of page_mapcount() to include the case of a
 * team page.  We don't complicate page_mapcount() itself in this way,
 * because almost nothing needs this number: only smaps accounting PSS.
 * If something else wants it, we might have to worry more about races.
 */
static inline int team_page_mapcount(struct page *page)
{
	struct page *head;
	long team_usage;
	int mapcount;

	mapcount = page_mapcount(page);
	if (!PageTeam(page))
		return mapcount;
	head = team_head(page);
	/* We always page_add_file_rmap to head when we page_add_team_rmap */
	if (page == head)
		return mapcount;

	team_usage = atomic_long_read(&head->team_usage) - TEAM_COMPLETE;
	/* Beware racing shmem_disband_hugehead() and add_to_swap_cache() */
	smp_rmb();
	if (PageTeam(head) && team_usage > 0)
		mapcount += team_usage / TEAM_MAPPING_COUNTER;
	return mapcount;
}

/*
 * Returns true if this pte mapping is of a non-team page, or of a team page not
 * covered by an existing huge pmd mapping: whereupon stats need to be updated.
 * Only called when mapcount goes up from 0 to 1 i.e. _mapcount from -1 to 0.
 */
static inline bool inc_team_pte_mapped(struct page *page)
{
#ifdef CONFIG_64BIT
	struct page *head;
	long team_usage;
	long old;

	if (likely(!PageTeam(page)))
		return true;
	head = team_head(page);
	team_usage = atomic_long_read(&head->team_usage);
	for (;;) {
		/* Is team now being disbanded? Stop once team_usage is reset */
		if (unlikely(!PageTeam(head) ||
			     team_usage / TEAM_PAGE_COUNTER == 0))
			return true;
		/*
		 * XXX: but despite the impressive-looking cmpxchg, gthelen
		 * points out that head might be freed and reused and assigned
		 * a matching value in ->private now: tiny chance, must revisit.
		 */
		old = atomic_long_cmpxchg(&head->team_usage,
			team_usage, team_usage + TEAM_PTE_COUNTER);
		if (likely(old == team_usage))
			break;
		team_usage = old;
	}
	return team_usage < TEAM_PMD_MAPPED;
#else /* 32-bit */
	return true;
#endif
}

/*
 * Returns true if this pte mapping is of a non-team page, or of a team page not
 * covered by a remaining huge pmd mapping: whereupon stats need to be updated.
 * Only called when mapcount goes down from 1 to 0 i.e. _mapcount from 0 to -1.
 */
static inline bool dec_team_pte_mapped(struct page *page)
{
#ifdef CONFIG_64BIT
	struct page *head;
	long team_usage;
	long old;

	if (likely(!PageTeam(page)))
		return true;
	head = team_head(page);
	team_usage = atomic_long_read(&head->team_usage);
	for (;;) {
		/* Is team now being disbanded? Stop once team_usage is reset */
		if (unlikely(!PageTeam(head) ||
			     team_usage / TEAM_PAGE_COUNTER == 0))
			return true;
		/*
		 * XXX: but despite the impressive-looking cmpxchg, gthelen
		 * points out that head might be freed and reused and assigned
		 * a matching value in ->private now: tiny chance, must revisit.
		 */
		old = atomic_long_cmpxchg(&head->team_usage,
			team_usage, team_usage - TEAM_PTE_COUNTER);
		if (likely(old == team_usage))
			break;
		team_usage = old;
	}
	return team_usage < TEAM_PMD_MAPPED;
#else /* 32-bit */
	return true;
#endif
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

static inline bool team_pmd_mlocked(struct page *head)
{
	VM_BUG_ON_PAGE(head != team_head(head), head);
	return atomic_long_read(&head->team_usage) & TEAM_PMD_MLOCKED;
}

static inline void set_team_pmd_mlocked(struct page *head)
{
	long team_usage;

	VM_BUG_ON_PAGE(head != team_head(head), head);
	team_usage = atomic_long_read(&head->team_usage);
	while (!(team_usage & TEAM_PMD_MLOCKED)) {
		team_usage = atomic_long_cmpxchg(&head->team_usage,
				team_usage, team_usage | TEAM_PMD_MLOCKED);
	}
}

static inline void clear_team_pmd_mlocked(struct page *head)
{
	long team_usage;

	VM_BUG_ON_PAGE(head != team_head(head), head);
	team_usage = atomic_long_read(&head->team_usage);
	while (team_usage & TEAM_PMD_MLOCKED) {
		team_usage = atomic_long_cmpxchg(&head->team_usage,
				team_usage, team_usage & ~TEAM_PMD_MLOCKED);
	}
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int map_team_by_pmd(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd, struct page *page);
void unmap_team_by_pmd(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd, struct page *page);
void remap_team_by_ptes(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd);
void remap_team_by_pmd(struct vm_area_struct *vma,
			unsigned long addr, pmd_t *pmd, struct page *page);
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
