#ifndef _FS_NFSD_FILECACHE_H
#define _FS_NFSD_FILECACHE_H

#include <linux/fsnotify_backend.h>

struct nfsd_file_mark {
	struct fsnotify_mark	nfm_mark;
	atomic_t		nfm_ref;
};

/*
 * A representation of a file that has been opened by knfsd. These are hashed
 * in the hashtable by inode pointer value. Note that this object doesn't
 * hold a reference to the inode by itself, so the nf_inode pointer should
 * never be dereferenced, only used for comparison.
 */
struct nfsd_file {
	struct hlist_node	nf_node;
	struct list_head	nf_lru;
	struct rcu_head		nf_rcu;
	struct file		*nf_file;
#define NFSD_FILE_HASHED	(0)
#define NFSD_FILE_PENDING	(1)
#define NFSD_FILE_BREAK_READ	(2)
#define NFSD_FILE_BREAK_WRITE	(3)
#define NFSD_FILE_REFERENCED	(4)
	unsigned long		nf_flags;
	struct inode		*nf_inode;
	unsigned int		nf_hashval;
	atomic_t		nf_ref;
	unsigned char		nf_may;
	struct nfsd_file_mark	*nf_mark;
};

int nfsd_file_cache_init(void);
void nfsd_file_cache_purge(void);
void nfsd_file_cache_shutdown(void);
void nfsd_file_put(struct nfsd_file *nf);
struct nfsd_file *nfsd_file_get(struct nfsd_file *nf);
void nfsd_file_close_inode_sync(struct inode *inode);
__be32 nfsd_file_acquire(struct svc_rqst *rqstp, struct svc_fh *fhp,
		  unsigned int may_flags, struct nfsd_file **nfp);
int	nfsd_file_cache_stats_open(struct inode *, struct file *);
#endif /* _FS_NFSD_FILECACHE_H */
