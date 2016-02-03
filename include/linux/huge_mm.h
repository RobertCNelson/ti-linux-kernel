#ifndef _LINUX_HUGE_MM_H
#define _LINUX_HUGE_MM_H

extern int do_huge_pmd_anonymous_page(struct mm_struct *mm,
				      struct vm_area_struct *vma,
				      unsigned long address, pmd_t *pmd,
				      unsigned int flags);
extern int copy_huge_pmd(struct mm_struct *dst_mm, struct mm_struct *src_mm,
			 pmd_t *dst_pmd, pmd_t *src_pmd, unsigned long addr,
			 struct vm_area_struct *vma);
extern int copy_huge_pud(struct mm_struct *dst_mm, struct mm_struct *src_mm,
			 pud_t *dst_pud, pud_t *src_pud, unsigned long addr,
			 struct vm_area_struct *vma);
extern void huge_pmd_set_accessed(struct mm_struct *mm,
				  struct vm_area_struct *vma,
				  unsigned long address, pmd_t *pmd,
				  pmd_t orig_pmd, int dirty);
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
extern void huge_pud_set_accessed(struct mm_struct *mm,
				  struct vm_area_struct *vma,
				  unsigned long address, pud_t *pud,
				  pud_t orig_pud, int dirty);
#else
static inline void huge_pud_set_accessed(struct mm_struct *mm,
				  struct vm_area_struct *vma,
				  unsigned long address, pud_t *pud,
				  pud_t orig_pud, int dirty)
{
}
#endif

extern int do_huge_pmd_wp_page(struct mm_struct *mm, struct vm_area_struct *vma,
			       unsigned long address, pmd_t *pmd,
			       pmd_t orig_pmd);
extern struct page *follow_trans_huge_pmd(struct vm_area_struct *vma,
					  unsigned long addr,
					  pmd_t *pmd,
					  unsigned int flags);
extern int madvise_free_huge_pmd(struct mmu_gather *tlb,
			struct vm_area_struct *vma,
			pmd_t *pmd, unsigned long addr, unsigned long next);
extern int zap_huge_pmd(struct mmu_gather *tlb,
			struct vm_area_struct *vma,
			pmd_t *pmd, unsigned long addr);
extern int zap_huge_pud(struct mmu_gather *tlb,
			struct vm_area_struct *vma,
			pud_t *pud, unsigned long addr);
extern int mincore_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
			unsigned long addr, unsigned long end,
			unsigned char *vec);
extern bool move_huge_pmd(struct vm_area_struct *vma,
			 struct vm_area_struct *new_vma,
			 unsigned long old_addr,
			 unsigned long new_addr, unsigned long old_end,
			 pmd_t *old_pmd, pmd_t *new_pmd);
extern int change_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
			unsigned long addr, pgprot_t newprot,
			int prot_numa);
int vmf_insert_pfn_pmd(struct vm_area_struct *, unsigned long addr, pmd_t *,
			pfn_t pfn, bool write);
int vmf_insert_pfn_pud(struct vm_area_struct *, unsigned long addr, pud_t *,
			pfn_t pfn, bool write);
enum transparent_hugepage_flag {
	TRANSPARENT_HUGEPAGE_FLAG,
	TRANSPARENT_HUGEPAGE_REQ_MADV_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG,
	TRANSPARENT_HUGEPAGE_USE_ZERO_PAGE_FLAG,
#ifdef CONFIG_DEBUG_VM
	TRANSPARENT_HUGEPAGE_DEBUG_COW_FLAG,
#endif
};

#define HPAGE_PMD_ORDER (HPAGE_PMD_SHIFT-PAGE_SHIFT)
#define HPAGE_PMD_NR (1<<HPAGE_PMD_ORDER)

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define HPAGE_PMD_SHIFT PMD_SHIFT
#define HPAGE_PMD_SIZE	((1UL) << HPAGE_PMD_SHIFT)
#define HPAGE_PMD_MASK	(~(HPAGE_PMD_SIZE - 1))

#define HPAGE_PUD_SHIFT PUD_SHIFT
#define HPAGE_PUD_SIZE	((1UL) << HPAGE_PUD_SHIFT)
#define HPAGE_PUD_MASK	(~(HPAGE_PUD_SIZE - 1))

extern bool is_vma_temporary_stack(struct vm_area_struct *vma);

#define transparent_hugepage_enabled(__vma)				\
	((transparent_hugepage_flags &					\
	  (1<<TRANSPARENT_HUGEPAGE_FLAG) ||				\
	  (transparent_hugepage_flags &					\
	   (1<<TRANSPARENT_HUGEPAGE_REQ_MADV_FLAG) &&			\
	   ((__vma)->vm_flags & VM_HUGEPAGE))) &&			\
	 !((__vma)->vm_flags & VM_NOHUGEPAGE) &&			\
	 !is_vma_temporary_stack(__vma))
#define transparent_hugepage_defrag(__vma)				\
	((transparent_hugepage_flags &					\
	  (1<<TRANSPARENT_HUGEPAGE_DEFRAG_FLAG)) ||			\
	 (transparent_hugepage_flags &					\
	  (1<<TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG) &&		\
	  (__vma)->vm_flags & VM_HUGEPAGE))
#define transparent_hugepage_use_zero_page()				\
	(transparent_hugepage_flags &					\
	 (1<<TRANSPARENT_HUGEPAGE_USE_ZERO_PAGE_FLAG))
#ifdef CONFIG_DEBUG_VM
#define transparent_hugepage_debug_cow()				\
	(transparent_hugepage_flags &					\
	 (1<<TRANSPARENT_HUGEPAGE_DEBUG_COW_FLAG))
#else /* CONFIG_DEBUG_VM */
#define transparent_hugepage_debug_cow() 0
#endif /* CONFIG_DEBUG_VM */

extern unsigned long transparent_hugepage_flags;

extern void prep_transhuge_page(struct page *page);
extern void free_transhuge_page(struct page *page);

int split_huge_page_to_list(struct page *page, struct list_head *list);
static inline int split_huge_page(struct page *page)
{
	return split_huge_page_to_list(page, NULL);
}
void deferred_split_huge_page(struct page *page);

void __split_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long address);

#define split_huge_pmd(__vma, __pmd, __address)				\
	do {								\
		pmd_t *____pmd = (__pmd);				\
		if (pmd_trans_huge(*____pmd)				\
					|| pmd_devmap(*____pmd))	\
			__split_huge_pmd(__vma, __pmd, __address);	\
	}  while (0)

void __split_huge_pud(struct vm_area_struct *vma, pud_t *pud,
		unsigned long address);

#define split_huge_pud(__vma, __pud, __address)				\
	do {								\
		pud_t *____pud = (__pud);				\
		if (pud_trans_huge(*____pud)				\
					|| pud_devmap(*____pud))	\
			__split_huge_pud(__vma, __pud, __address);	\
	}  while (0)

#if HPAGE_PMD_ORDER >= MAX_ORDER
#error "hugepages can't be allocated by the buddy allocator"
#endif
extern int hugepage_madvise(struct vm_area_struct *vma,
			    unsigned long *vm_flags, int advice);
extern void vma_adjust_trans_huge(struct vm_area_struct *vma,
				    unsigned long start,
				    unsigned long end,
				    long adjust_next);
extern spinlock_t *__pmd_trans_huge_lock(pmd_t *pmd,
		struct vm_area_struct *vma);
extern spinlock_t *__pud_trans_huge_lock(pud_t *pud,
		struct vm_area_struct *vma);
/* mmap_sem must be held on entry */
static inline spinlock_t *pmd_trans_huge_lock(pmd_t *pmd,
		struct vm_area_struct *vma)
{
	VM_BUG_ON_VMA(!rwsem_is_locked(&vma->vm_mm->mmap_sem), vma);
	if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
		return __pmd_trans_huge_lock(pmd, vma);
	else
		return false;
}
static inline spinlock_t *pud_trans_huge_lock(pud_t *pud,
		struct vm_area_struct *vma)
{
	VM_BUG_ON_VMA(!rwsem_is_locked(&vma->vm_mm->mmap_sem), vma);
	if (pud_trans_huge(*pud) || pud_devmap(*pud))
		return __pud_trans_huge_lock(pud, vma);
	else
		return NULL;
}
static inline int hpage_nr_pages(struct page *page)
{
	if (unlikely(PageTransHuge(page)))
		return HPAGE_PMD_NR;
	return 1;
}

struct page *follow_devmap_pmd(struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, int flags);
struct page *follow_devmap_pud(struct vm_area_struct *vma, unsigned long addr,
		pud_t *pud, int flags);

extern int do_huge_pmd_numa_page(struct mm_struct *mm, struct vm_area_struct *vma,
				unsigned long addr, pmd_t pmd, pmd_t *pmdp);

extern struct page *huge_zero_page;

static inline bool is_huge_zero_page(struct page *page)
{
	return ACCESS_ONCE(huge_zero_page) == page;
}

static inline bool is_huge_zero_pmd(pmd_t pmd)
{
	return is_huge_zero_page(pmd_page(pmd));
}

static inline bool is_huge_zero_pud(pud_t pud)
{
	return 0;
}

struct page *get_huge_zero_page(void);

#else /* CONFIG_TRANSPARENT_HUGEPAGE */
#define HPAGE_PMD_SHIFT ({ BUILD_BUG(); 0; })
#define HPAGE_PMD_MASK ({ BUILD_BUG(); 0; })
#define HPAGE_PMD_SIZE ({ BUILD_BUG(); 0; })

#define HPAGE_PUD_SHIFT ({ BUILD_BUG(); 0; })
#define HPAGE_PUD_MASK ({ BUILD_BUG(); 0; })
#define HPAGE_PUD_SIZE ({ BUILD_BUG(); 0; })

#define hpage_nr_pages(x) 1

#define transparent_hugepage_enabled(__vma) 0

#define transparent_hugepage_flags 0UL
static inline int
split_huge_page_to_list(struct page *page, struct list_head *list)
{
	return 0;
}
static inline int split_huge_page(struct page *page)
{
	return 0;
}
static inline void deferred_split_huge_page(struct page *page) {}
#define split_huge_pmd(__vma, __pmd, __address)	\
	do { } while (0)
#define split_huge_pud(__vma, __pmd, __address)	\
	do { } while (0)
static inline int hugepage_madvise(struct vm_area_struct *vma,
				   unsigned long *vm_flags, int advice)
{
	BUG();
	return 0;
}
static inline void vma_adjust_trans_huge(struct vm_area_struct *vma,
					 unsigned long start,
					 unsigned long end,
					 long adjust_next)
{
}
static inline spinlock_t *pmd_trans_huge_lock(pmd_t *pmd,
		struct vm_area_struct *vma)
{
	return NULL;
}
static inline spinlock_t *pud_trans_huge_lock(pud_t *pud,
		struct vm_area_struct *vma)
{
	return NULL;
}

static inline int do_huge_pmd_numa_page(struct mm_struct *mm, struct vm_area_struct *vma,
					unsigned long addr, pmd_t pmd, pmd_t *pmdp)
{
	return 0;
}

static inline bool is_huge_zero_page(struct page *page)
{
	return false;
}


static inline struct page *follow_devmap_pmd(struct vm_area_struct *vma,
		unsigned long addr, pmd_t *pmd, int flags)
{
	return NULL;
}

static inline struct page *follow_devmap_pud(struct vm_area_struct *vma,
		unsigned long addr, pud_t *pud, int flags)
{
	return NULL;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#endif /* _LINUX_HUGE_MM_H */
