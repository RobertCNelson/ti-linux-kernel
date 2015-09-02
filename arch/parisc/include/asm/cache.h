/*
 * include/asm-parisc/cache.h
 */

#ifndef __ARCH_PARISC_CACHE_H
#define __ARCH_PARISC_CACHE_H


/*
 * Most PA 2.0 processors have 64-byte cachelines, but PA8800 and PA8900
 * processors have a cache line length of 128 bytes.
 * PA 1.1 processors have 32-byte cachelines.
 * There is a potential livelock danger when running a machine with this value
 * set too small, but it's more probable you'll just ruin performance.
 */
#if defined(CONFIG_PA8X00)
#define L1_CACHE_BYTES 128
#define L1_CACHE_SHIFT 7
#elif defined(CONFIG_PA20)
#define L1_CACHE_BYTES 64
#define L1_CACHE_SHIFT 6
#else /* PA7XXX */
#define L1_CACHE_BYTES 32
#define L1_CACHE_SHIFT 5
#endif

#ifndef __ASSEMBLY__

#define SMP_CACHE_BYTES L1_CACHE_BYTES

#define ARCH_DMA_MINALIGN	L1_CACHE_BYTES

#define __read_mostly __attribute__((__section__(".data..read_mostly")))

void parisc_cache_init(void);	/* initializes cache-flushing */
void disable_sr_hashing_asm(int); /* low level support for above */
void disable_sr_hashing(void);   /* turns off space register hashing */
void free_sid(unsigned long);
unsigned long alloc_sid(void);

struct seq_file;
extern void show_cache_info(struct seq_file *m);

extern int split_tlb;
extern int dcache_stride;
extern int icache_stride;
extern struct pdc_cache_info cache_info;
void parisc_setup_cache_timing(void);

#define pdtlb(addr)         asm volatile("pdtlb 0(%%sr1,%0)" : : "r" (addr));
#define pitlb(addr)         asm volatile("pitlb 0(%%sr1,%0)" : : "r" (addr));
#define pdtlb_kernel(addr)  asm volatile("pdtlb 0(%0)" : : "r" (addr));

#endif /* ! __ASSEMBLY__ */

/* Classes of processor wrt: disabling space register hashing */

#define SRHASH_PCXST    0   /* pcxs, pcxt, pcxt_ */
#define SRHASH_PCXL     1   /* pcxl */
#define SRHASH_PA20     2   /* pcxu, pcxu_, pcxw, pcxw_ */

#endif
