#ifndef _FS_NFSD_FILECACHE_H
#define _FS_NFSD_FILECACHE_H

#include <linux/jhash.h>
#include <linux/sunrpc/xdr.h>

#include "nfsfh.h"
#include "export.h"

/* hash table for nfs4_file */
#define NFSD_FILE_HASH_BITS                   8
#define NFSD_FILE_HASH_SIZE                  (1 << NFSD_FILE_HASH_BITS)

static inline unsigned int
nfsd_fh_hashval(struct knfsd_fh *fh)
{
	return jhash2(fh->fh_base.fh_pad, XDR_QUADLEN(fh->fh_size), 0);
}

static inline unsigned int
file_hashval(struct knfsd_fh *fh)
{
	return nfsd_fh_hashval(fh) & (NFSD_FILE_HASH_SIZE - 1);
}

struct nfsd_file {
	struct hlist_node	nf_node;
	struct list_head	nf_dispose;
	struct rcu_head		nf_rcu;
	struct file		*nf_file;
	unsigned long		nf_time;
#define NFSD_FILE_HASHED	(0)
#define NFSD_FILE_PENDING	(1)
	unsigned long		nf_flags;
	struct knfsd_fh		nf_handle;
	unsigned int		nf_hashval;
	atomic_t		nf_ref;
	unsigned char		nf_may;
};

int nfsd_file_cache_init(void);
void nfsd_file_cache_purge(void);
void nfsd_file_cache_shutdown(void);
void nfsd_file_put(struct nfsd_file *nf);
__be32 nfsd_file_acquire(struct svc_rqst *rqstp, struct svc_fh *fhp,
		  unsigned int may_flags, struct nfsd_file **nfp);
#endif /* _FS_NFSD_FILECACHE_H */
