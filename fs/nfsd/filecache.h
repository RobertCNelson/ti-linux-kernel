#ifndef _FS_NFSD_FILECACHE_H
#define _FS_NFSD_FILECACHE_H

#include <linux/jhash.h>
#include <linux/sunrpc/xdr.h>

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

#endif /* _FS_NFSD_FILECACHE_H */
