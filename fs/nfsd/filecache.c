/*
 * Open file cache.
 *
 * (c) 2015 - Jeff Layton <jeff.layton@primarydata.com>
 */

#include <linux/slab.h>
#include <linux/hash.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/module.h>

#include "vfs.h"
#include "nfsd.h"
#include "nfsfh.h"
#include "filecache.h"

#define NFSDDBG_FACILITY	NFSDDBG_VFS

/* Min time we should keep around a file cache entry (in jiffies) */
static unsigned int			nfsd_file_cache_expiry = HZ;
module_param(nfsd_file_cache_expiry, uint, 0644);
MODULE_PARM_DESC(nfsd_file_cache_expiry, "Expire time for open file cache (in jiffies)");

/* We only care about NFSD_MAY_READ/WRITE for this cache */
#define NFSD_FILE_MAY_MASK	(NFSD_MAY_READ|NFSD_MAY_WRITE)

struct nfsd_fcache_bucket {
	struct hlist_head	nfb_head;
	spinlock_t		nfb_lock;
};

static struct nfsd_fcache_bucket	*nfsd_file_hashtbl;

/* Count of hashed nfsd_file objects */
static atomic_t				nfsd_file_count;

/* Periodic job for cleaning nfsd_file cache */
static struct delayed_work		nfsd_file_cache_clean_work;

static void
nfsd_file_count_inc(void)
{
	if (atomic_inc_return(&nfsd_file_count) == 1)
		queue_delayed_work(nfsd_laundry_wq, &nfsd_file_cache_clean_work,
					nfsd_file_cache_expiry);
}

static void
nfsd_file_count_dec(void)
{
	if (atomic_dec_and_test(&nfsd_file_count))
		cancel_delayed_work(&nfsd_file_cache_clean_work);
}

static struct nfsd_file *
nfsd_file_alloc(struct knfsd_fh *fh, unsigned int may, unsigned int hashval)
{
	struct nfsd_file *nf;

	/* FIXME: create a new slabcache for these? */
	nf = kzalloc(sizeof(*nf), GFP_KERNEL);
	if (nf) {
		INIT_HLIST_NODE(&nf->nf_node);
		INIT_LIST_HEAD(&nf->nf_dispose);
		nf->nf_time = jiffies;
		fh_copy_shallow(&nf->nf_handle, fh);
		nf->nf_hashval = hashval;
		atomic_set(&nf->nf_ref, 1);
		nf->nf_may = NFSD_FILE_MAY_MASK & may;
	}
	return nf;
}

static void
nfsd_file_put_final(struct nfsd_file *nf)
{
	if (nf->nf_file)
		fput(nf->nf_file);
	kfree_rcu(nf, nf_rcu);
}

static void
nfsd_file_unhash(struct nfsd_file *nf)
{
	if (test_and_clear_bit(NFSD_FILE_HASHED, &nf->nf_flags)) {
		hlist_del_rcu(&nf->nf_node);
		nfsd_file_count_dec();
	}
}

static void
nfsd_file_put_locked(struct nfsd_file *nf, struct list_head *dispose)
{
	if (!atomic_dec_and_test(&nf->nf_ref)) {
		nf->nf_time = jiffies;
		return;
	}

	nfsd_file_unhash(nf);
	list_add(&nf->nf_dispose, dispose);
}

void
nfsd_file_put(struct nfsd_file *nf)
{
	if (!atomic_dec_and_lock(&nf->nf_ref,
				&nfsd_file_hashtbl[nf->nf_hashval].nfb_lock)) {
		nf->nf_time = jiffies;
		return;
	}

	nfsd_file_unhash(nf);
	spin_unlock(&nfsd_file_hashtbl[nf->nf_hashval].nfb_lock);
	nfsd_file_put_final(nf);
}

static void
nfsd_file_dispose_list(struct list_head *dispose)
{
	struct nfsd_file *nf;

	while(!list_empty(dispose)) {
		nf = list_first_entry(dispose, struct nfsd_file, nf_dispose);
		list_del(&nf->nf_dispose);
		nfsd_file_put_final(nf);
	}
}

void
nfsd_file_cache_purge(void)
{
	unsigned int		i;
	struct nfsd_file	*nf;
	LIST_HEAD(dispose);

	if (!atomic_read(&nfsd_file_count))
		return;

	for (i = 0; i < NFSD_FILE_HASH_SIZE; i++) {
		spin_lock(&nfsd_file_hashtbl[i].nfb_lock);
		while(!hlist_empty(&nfsd_file_hashtbl[i].nfb_head)) {
			nf = hlist_entry(nfsd_file_hashtbl[i].nfb_head.first,
					 struct nfsd_file, nf_node);
			nfsd_file_unhash(nf);
			/* put the hash reference */
			nfsd_file_put_locked(nf, &dispose);
		}
		spin_unlock(&nfsd_file_hashtbl[i].nfb_lock);
		nfsd_file_dispose_list(&dispose);
	}
}

static void
nfsd_file_cache_prune(void)
{
	unsigned int		i;
	struct nfsd_file	*nf;
	struct hlist_node	*tmp;
	LIST_HEAD(dispose);

	for (i = 0; i < NFSD_FILE_HASH_SIZE; i++) {
		if (hlist_empty(&nfsd_file_hashtbl[i].nfb_head))
			continue;

		spin_lock(&nfsd_file_hashtbl[i].nfb_lock);
		hlist_for_each_entry_safe(nf, tmp,
				&nfsd_file_hashtbl[i].nfb_head, nf_node) {

			/* does someone else have a reference? */
			if (atomic_read(&nf->nf_ref) > 1)
				continue;

			/* Was this file touched recently? */
			if (time_before(nf->nf_time + nfsd_file_cache_expiry,
					jiffies))
				continue;

			/* Ok, it's expired...unhash it */
			nfsd_file_unhash(nf);

			/* ...and put the hash reference */
			nfsd_file_put_locked(nf, &dispose);
		}
		spin_unlock(&nfsd_file_hashtbl[i].nfb_lock);
		nfsd_file_dispose_list(&dispose);
	}
}

static void
nfsd_file_cache_cleaner(struct work_struct *work)
{
	if (!atomic_read(&nfsd_file_count))
		return;

	nfsd_file_cache_prune();

	if (atomic_read(&nfsd_file_count))
		queue_delayed_work(nfsd_laundry_wq, &nfsd_file_cache_clean_work,
					nfsd_file_cache_expiry);
}

int
nfsd_file_cache_init(void)
{
	unsigned int i;

	if (nfsd_file_hashtbl)
		return 0;

	nfsd_file_hashtbl = kcalloc(NFSD_FILE_HASH_SIZE,
				sizeof(*nfsd_file_hashtbl), GFP_KERNEL);
	if (!nfsd_file_hashtbl)
		goto out_nomem;

	for (i = 0; i < NFSD_FILE_HASH_SIZE; i++) {
		INIT_HLIST_HEAD(&nfsd_file_hashtbl[i].nfb_head);
		spin_lock_init(&nfsd_file_hashtbl[i].nfb_lock);
	}

	INIT_DELAYED_WORK(&nfsd_file_cache_clean_work, nfsd_file_cache_cleaner);
	return 0;
out_nomem:
	printk(KERN_ERR "nfsd: failed to init nfsd file cache\n");
	return -ENOMEM;
}

void
nfsd_file_cache_shutdown(void)
{
	LIST_HEAD(dispose);

	cancel_delayed_work_sync(&nfsd_file_cache_clean_work);
	nfsd_file_cache_purge();
	kfree(nfsd_file_hashtbl);
	nfsd_file_hashtbl = NULL;
}

/*
 * Search nfsd_file_hashtbl[] for file. We hash on the filehandle and also on
 * the NFSD_MAY_READ/WRITE flags. If the file is open for r/w, then it's usable
 * for either.
 */
static struct nfsd_file *
nfsd_file_find_locked(struct knfsd_fh *fh, unsigned int may_flags,
			unsigned int hashval)
{
	struct nfsd_file *nf;
	unsigned char need = may_flags & NFSD_FILE_MAY_MASK;

	hlist_for_each_entry_rcu(nf, &nfsd_file_hashtbl[hashval].nfb_head,
				 nf_node) {
		if ((need & nf->nf_may) != need)
			continue;
		if (fh_match(&nf->nf_handle, fh)) {
			if (atomic_inc_not_zero(&nf->nf_ref))
				return nf;
		}
	}
	return NULL;
}

__be32
nfsd_file_acquire(struct svc_rqst *rqstp, struct svc_fh *fhp,
		  unsigned int may_flags, struct nfsd_file **pnf)
{
	__be32	status = nfs_ok;
	struct nfsd_file *nf, *new = NULL;
	struct knfsd_fh *fh = &fhp->fh_handle;
	unsigned int hashval = file_hashval(fh);

	/* Mask off any extraneous bits */
	may_flags &= NFSD_FILE_MAY_MASK;
retry:
	rcu_read_lock();
	nf = nfsd_file_find_locked(fh, may_flags, hashval);
	rcu_read_unlock();
	if (nf)
		goto wait_for_construction;

	if (!new) {
		new = nfsd_file_alloc(&fhp->fh_handle, may_flags, hashval);
		if (!new)
			return nfserr_jukebox;
	}

	spin_lock(&nfsd_file_hashtbl[hashval].nfb_lock);
	nf = nfsd_file_find_locked(fh, may_flags, hashval);
	if (likely(nf == NULL)) {
		/* Take reference for the hashtable */
		atomic_inc(&new->nf_ref);
		__set_bit(NFSD_FILE_HASHED, &new->nf_flags);
		__set_bit(NFSD_FILE_PENDING, &new->nf_flags);
		hlist_add_head_rcu(&new->nf_node,
				&nfsd_file_hashtbl[hashval].nfb_head);
		spin_unlock(&nfsd_file_hashtbl[hashval].nfb_lock);
		nfsd_file_count_inc();
		nf = new;
		new = NULL;
		goto open_file;
	}
	spin_unlock(&nfsd_file_hashtbl[hashval].nfb_lock);

wait_for_construction:
	wait_on_bit(&nf->nf_flags, NFSD_FILE_PENDING, TASK_UNINTERRUPTIBLE);

	/* Did construction of this file fail? */
	if (!nf->nf_file) {
		/*
		 * We can only take over construction for this nfsd_file if the
		 * MAY flags are equal. Otherwise, we put the reference and try
		 * again.
		 */
		if (may_flags != nf->nf_may) {
			nfsd_file_put(nf);
			goto retry;
		}

		/* try to take over construction for this file */
		if (test_and_set_bit(NFSD_FILE_PENDING, &nf->nf_flags))
			goto wait_for_construction;
		goto open_file;
	}

	/*
	 * We have a file that was opened in the context of another rqst. We
	 * must check permissions. Since we're dealing with open files here,
	 * we always want to set the OWNER_OVERRIDE bit.
	 */
	status = fh_verify(rqstp, fhp, S_IFREG, may_flags);
	if (status == nfs_ok)
		status = nfsd_permission(rqstp, fhp->fh_export, fhp->fh_dentry,
						may_flags|NFSD_MAY_OWNER_OVERRIDE);
out:
	if (status == nfs_ok)
		*pnf = nf;
	else
		nfsd_file_put(nf);

	if (new)
		nfsd_file_put(new);
	return status;
open_file:
	status = nfsd_open(rqstp, fhp, S_IFREG, may_flags, &nf->nf_file);
	clear_bit(NFSD_FILE_PENDING, &nf->nf_flags);
	wake_up_bit(&nf->nf_flags, NFSD_FILE_PENDING);
	goto out;
}
