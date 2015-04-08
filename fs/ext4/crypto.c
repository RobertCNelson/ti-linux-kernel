/*
 * linux/fs/ext4/crypto.c
 *
 * This contains encryption functions for ext4
 *
 * Written by Michael Halcrow, 2014.
 *
 * Filename encryption additions
 *	Uday Savagaonkar, 2014
 * Encryption policy handling additions
 *	Ildar Muslukhov, 2014
 *
 * This has not yet undergone a rigorous security audit.
 *
 * The usage of AES-XTS should conform to recommendations in NIST
 * Special Publication 800-38E. The usage of AES-GCM should conform to
 * the recommendations in NIST Special Publication 800-38D. Further
 * guidance for block-oriented storage is in IEEE P1619/D16. The key
 * derivation code implements an HKDF (see RFC 5869).
 */

#include <crypto/hash.h>
#include <crypto/sha.h>
#include <keys/user-type.h>
#include <keys/encrypted-type.h>
#include <linux/crypto.h>
#include <linux/ecryptfs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/list.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/scatterlist.h>
#include <linux/spinlock_types.h>

#include "ext4.h"
#include "xattr.h"

/* Encryption added and removed here! (L: */

static unsigned int num_prealloc_crypto_pages = 32;
static unsigned int num_prealloc_crypto_ctxs = 128;

module_param(num_prealloc_crypto_pages, uint, 0444);
MODULE_PARM_DESC(num_prealloc_crypto_pages,
		 "Number of crypto pages to preallocate");
module_param(num_prealloc_crypto_ctxs, uint, 0444);
MODULE_PARM_DESC(num_prealloc_crypto_ctxs,
		 "Number of crypto contexts to preallocate");

static mempool_t *ext4_bounce_page_pool;

static LIST_HEAD(ext4_free_crypto_ctxs);
static DEFINE_SPINLOCK(ext4_crypto_ctx_lock);

/**
 * ext4_release_crypto_ctx() - Releases an encryption context
 * @ctx: The encryption context to release.
 *
 * If the encryption context was allocated from the pre-allocated pool, returns
 * it to that pool. Else, frees it.
 *
 * If there's a bounce page in the context, this frees that.
 */
void ext4_release_crypto_ctx(struct ext4_crypto_ctx *ctx)
{
	unsigned long flags;

	if (ctx->bounce_page) {
		if (ctx->flags & EXT4_BOUNCE_PAGE_REQUIRES_FREE_ENCRYPT_FL)
			__free_page(ctx->bounce_page);
		else
			mempool_free(ctx->bounce_page, ext4_bounce_page_pool);
		ctx->bounce_page = NULL;
	}
	ctx->control_page = NULL;
	if (ctx->flags & EXT4_CTX_REQUIRES_FREE_ENCRYPT_FL) {
		if (ctx->tfm)
			crypto_free_tfm(ctx->tfm);
		kfree(ctx);
	} else {
		spin_lock_irqsave(&ext4_crypto_ctx_lock, flags);
		list_add(&ctx->free_list, &ext4_free_crypto_ctxs);
		spin_unlock_irqrestore(&ext4_crypto_ctx_lock, flags);
	}
}

/**
 * ext4_alloc_and_init_crypto_ctx() - Allocates and inits an encryption context
 * @mask: The allocation mask.
 *
 * Return: An allocated and initialized encryption context on success. An error
 * value or NULL otherwise.
 */
static struct ext4_crypto_ctx *ext4_alloc_and_init_crypto_ctx(gfp_t mask)
{
	struct ext4_crypto_ctx *ctx = kzalloc(sizeof(struct ext4_crypto_ctx),
					      mask);

	if (!ctx)
		return ERR_PTR(-ENOMEM);
	return ctx;
}

/**
 * ext4_get_crypto_ctx() - Gets an encryption context
 * @inode:       The inode for which we are doing the crypto
 *
 * Allocates and initializes an encryption context.
 *
 * Return: An allocated and initialized encryption context on success; error
 * value or NULL otherwise.
 */
struct ext4_crypto_ctx *ext4_get_crypto_ctx(struct inode *inode)
{
	struct ext4_crypto_ctx *ctx = NULL;
	int res = 0;
	unsigned long flags;
	struct ext4_encryption_key *key = &EXT4_I(inode)->i_encryption_key;

	/* We first try getting the ctx from a free list because in the common
	 * case the ctx will have an allocated and initialized crypto tfm, so
	 * it's probably a worthwhile optimization. For the bounce page, we
	 * first try getting it from the kernel allocator because that's just
	 * about as fast as getting it from a list and because a cache of free
	 * pages should generally be a "last resort" option for a filesystem to
	 * be able to do its job. */
	spin_lock_irqsave(&ext4_crypto_ctx_lock, flags);
	ctx = list_first_entry_or_null(&ext4_free_crypto_ctxs,
				       struct ext4_crypto_ctx, free_list);
	if (ctx)
		list_del(&ctx->free_list);
	spin_unlock_irqrestore(&ext4_crypto_ctx_lock, flags);
	if (!ctx) {
		ctx = ext4_alloc_and_init_crypto_ctx(GFP_NOFS);
		if (IS_ERR(ctx)) {
			res = PTR_ERR(ctx);
			goto out;
		}
		ctx->flags |= EXT4_CTX_REQUIRES_FREE_ENCRYPT_FL;
	} else {
		ctx->flags &= ~EXT4_CTX_REQUIRES_FREE_ENCRYPT_FL;
	}

	/* Allocate a new Crypto API context if we don't already have one or if
	 * it isn't the right mode. */
	BUG_ON(key->mode == EXT4_ENCRYPTION_MODE_INVALID);
	if (ctx->tfm && (ctx->mode != key->mode)) {
		crypto_free_tfm(ctx->tfm);
		ctx->tfm = NULL;
		ctx->mode = EXT4_ENCRYPTION_MODE_INVALID;
	}
	if (!ctx->tfm) {
		switch (key->mode) {
		case EXT4_ENCRYPTION_MODE_AES_256_XTS:
			ctx->tfm = crypto_ablkcipher_tfm(
				crypto_alloc_ablkcipher("xts(aes)", 0, 0));
			break;
		case EXT4_ENCRYPTION_MODE_AES_256_GCM:
			/* TODO(mhalcrow): AEAD w/ gcm(aes);
			 * crypto_aead_setauthsize() */
			ctx->tfm = ERR_PTR(-ENOTSUPP);
			break;
		default:
			BUG();
		}
		if (IS_ERR_OR_NULL(ctx->tfm)) {
			res = PTR_ERR(ctx->tfm);
			ctx->tfm = NULL;
			goto out;
		}
		ctx->mode = key->mode;
	}
	BUG_ON(key->size != ext4_encryption_key_size(key->mode));

	/* There shouldn't be a bounce page attached to the crypto
	 * context at this point. */
	BUG_ON(ctx->bounce_page);

out:
	if (res) {
		if (!IS_ERR_OR_NULL(ctx))
			ext4_release_crypto_ctx(ctx);
		ctx = ERR_PTR(res);
	}
	return ctx;
}

struct workqueue_struct *ext4_read_workqueue;
static DEFINE_MUTEX(crypto_init);

/**
 * ext4_exit_crypto() - Shutdown the ext4 encryption system
 */
void ext4_exit_crypto(void)
{
	struct ext4_crypto_ctx *pos, *n;

	list_for_each_entry_safe(pos, n, &ext4_free_crypto_ctxs, free_list) {
		if (pos->bounce_page) {
			if (pos->flags &
			    EXT4_BOUNCE_PAGE_REQUIRES_FREE_ENCRYPT_FL) {
				__free_page(pos->bounce_page);
			} else {
				mempool_free(pos->bounce_page,
					     ext4_bounce_page_pool);
			}
		}
		if (pos->tfm)
			crypto_free_tfm(pos->tfm);
		kfree(pos);
	}
	INIT_LIST_HEAD(&ext4_free_crypto_ctxs);
	if (ext4_bounce_page_pool)
		mempool_destroy(ext4_bounce_page_pool);
	ext4_bounce_page_pool = NULL;
	if (ext4_read_workqueue)
		destroy_workqueue(ext4_read_workqueue);
	ext4_read_workqueue = NULL;
}

/**
 * ext4_init_crypto() - Set up for ext4 encryption.
 *
 * We call this when we mount a file system which has the encryption
 * feature enabled, since it results in memory getting allocated that
 * won't be used unless we are using encryption.
 *
 * Return: Zero on success, non-zero otherwise.
 */
int ext4_init_crypto(void)
{
	int i, res = 0;

	mutex_lock(&crypto_init);
	if (ext4_read_workqueue)
		goto already_initialized;
	ext4_read_workqueue = alloc_workqueue("ext4_crypto", WQ_HIGHPRI, 0);
	if (!ext4_read_workqueue) {
		res = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < num_prealloc_crypto_ctxs; i++) {
		struct ext4_crypto_ctx *ctx;

		ctx = ext4_alloc_and_init_crypto_ctx(GFP_KERNEL);
		if (IS_ERR(ctx)) {
			res = PTR_ERR(ctx);
			goto fail;
		}
		list_add(&ctx->free_list, &ext4_free_crypto_ctxs);
	}

	ext4_bounce_page_pool =
		mempool_create_page_pool(num_prealloc_crypto_pages, 0);
	if (!ext4_bounce_page_pool)
		goto fail;
already_initialized:
	mutex_unlock(&crypto_init);
	return 0;
fail:
	ext4_exit_crypto();
	mutex_unlock(&crypto_init);
	return res;
}

/**
 * ext4_xts_tweak_for_page() - Generates an XTS tweak for a page
 * @xts_tweak: Buffer into which this writes the XTS tweak.
 * @page:      The page for which this generates a tweak.
 *
 * Generates an XTS tweak value for the given page.
 */
static void ext4_xts_tweak_for_page(u8 xts_tweak[EXT4_XTS_TWEAK_SIZE],
				    const struct page *page)
{
	/* Only do this for XTS tweak values. For other modes (CBC,
	 * GCM, etc.), you most likely will need to do something
	 * different. */
	BUILD_BUG_ON(EXT4_XTS_TWEAK_SIZE < sizeof(page->index));
	memcpy(xts_tweak, &page->index, sizeof(page->index));
	memset(&xts_tweak[sizeof(page->index)], 0,
	       EXT4_XTS_TWEAK_SIZE - sizeof(page->index));
}

void ext4_restore_control_page(struct page *data_page)
{
	struct ext4_crypto_ctx *ctx =
		(struct ext4_crypto_ctx *)page_private(data_page);

	set_page_private(data_page, (unsigned long)NULL);
	ClearPagePrivate(data_page);
	unlock_page(data_page);
	ext4_release_crypto_ctx(ctx);
}

struct ext4_crypt_result {
	struct completion completion;
	int res;
};

/**
 * ext4_crypt_complete() - The completion callback for page encryption
 * @req: The asynchronous encryption request context
 * @res: The result of the encryption operation
 */
static void ext4_crypt_complete(struct crypto_async_request *req, int res)
{
	struct ext4_crypt_result *ecr = req->data;

	if (res == -EINPROGRESS)
		return;
	ecr->res = res;
	complete(&ecr->completion);
}

/**
 * ext4_prep_pages_for_write() - Prepares pages for write
 * @ciphertext_page: Ciphertext page that will actually be written.
 * @plaintext_page:  Plaintext page that acts as a control page.
 * @ctx:             Encryption context for the pages.
 */
static void ext4_prep_pages_for_write(struct page *ciphertext_page,
				      struct page *plaintext_page,
				      struct ext4_crypto_ctx *ctx)
{
	SetPageDirty(ciphertext_page);
	SetPagePrivate(ciphertext_page);
	ctx->control_page = plaintext_page;
	set_page_private(ciphertext_page, (unsigned long)ctx);
	lock_page(ciphertext_page);
}

/**
 * ext4_xts_encrypt() - Encrypts a page using AES-256-XTS
 * @ctx:            The encryption context.
 * @plaintext_page: The page to encrypt. Must be locked.
 *
 * Allocates a ciphertext page and encrypts plaintext_page into it using the ctx
 * encryption context. Uses AES-256-XTS.
 *
 * Called on the page write path.
 *
 * Return: An allocated page with the encrypted content on success. Else, an
 * error value or NULL.
 */
static struct page *ext4_xts_encrypt(struct ext4_crypto_ctx *ctx,
				     struct page *plaintext_page)
{
	struct page *ciphertext_page = ctx->bounce_page;
	u8 xts_tweak[EXT4_XTS_TWEAK_SIZE];
	struct ablkcipher_request *req = NULL;
	struct ext4_crypt_result ecr;
	struct scatterlist dst, src;
	struct ext4_inode_info *ei = EXT4_I(plaintext_page->mapping->host);
	struct crypto_ablkcipher *atfm = __crypto_ablkcipher_cast(ctx->tfm);
	int res = 0;

	BUG_ON(!ciphertext_page);
	BUG_ON(!ctx->tfm);
	BUG_ON(ei->i_encryption_key.mode != EXT4_ENCRYPTION_MODE_AES_256_XTS);
	crypto_ablkcipher_clear_flags(atfm, ~0);
	crypto_tfm_set_flags(ctx->tfm, CRYPTO_TFM_REQ_WEAK_KEY);

	/* Since in AES-256-XTS mode we only perform one cryptographic operation
	 * on each block and there are no constraints about how many blocks a
	 * single key can encrypt, we directly use the inode master key */
	res = crypto_ablkcipher_setkey(atfm, ei->i_encryption_key.raw,
				       ei->i_encryption_key.size);
	req = ablkcipher_request_alloc(atfm, GFP_NOFS);
	if (!req) {
		printk_ratelimited(KERN_ERR
				   "%s: crypto_request_alloc() failed\n",
				   __func__);
		ciphertext_page = ERR_PTR(-ENOMEM);
		goto out;
	}
	ablkcipher_request_set_callback(
		req, CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
		ext4_crypt_complete, &ecr);
	ext4_xts_tweak_for_page(xts_tweak, plaintext_page);
	sg_init_table(&dst, 1);
	sg_set_page(&dst, ciphertext_page, PAGE_CACHE_SIZE, 0);
	sg_init_table(&src, 1);
	sg_set_page(&src, plaintext_page, PAGE_CACHE_SIZE, 0);
	ablkcipher_request_set_crypt(req, &src, &dst, PAGE_CACHE_SIZE,
				     xts_tweak);
	res = crypto_ablkcipher_encrypt(req);
	if (res == -EINPROGRESS || res == -EBUSY) {
		BUG_ON(req->base.data != &ecr);
		wait_for_completion(&ecr.completion);
		res = ecr.res;
	}
	ablkcipher_request_free(req);
	if (res) {
		printk_ratelimited(
			KERN_ERR
			"%s: crypto_ablkcipher_encrypt() returned %d\n",
			__func__, res);
		ciphertext_page = ERR_PTR(res);
		goto out;
	}
out:
	return ciphertext_page;
}

/**
 * ext4_encrypt() - Encrypts a page
 * @ctx:            The encryption context.
 * @plaintext_page: The page to encrypt. Must be locked.
 *
 * Allocates a ciphertext page and encrypts plaintext_page into it using the ctx
 * encryption context.
 *
 * Called on the page write path.
 *
 * Return: An allocated page with the encrypted content on success. Else, an
 * error value or NULL.
 */
struct page *ext4_encrypt(struct inode *inode,
			  struct page *plaintext_page)
{
	struct ext4_crypto_ctx *ctx;
	struct page *ciphertext_page = NULL;

	BUG_ON(!PageLocked(plaintext_page));

	ctx = ext4_get_crypto_ctx(inode);
	if (IS_ERR(ctx))
		return (struct page *) ctx;

	/* The encryption operation will require a bounce page. */
	ctx->bounce_page = alloc_page(GFP_NOFS);
	if (!ctx->bounce_page) {
		/* This is a potential bottleneck, but at least we'll have
		 * forward progress. */
		ctx->bounce_page = mempool_alloc(ext4_bounce_page_pool,
						 GFP_NOFS);
		if (WARN_ON_ONCE(!ctx->bounce_page)) {
			ctx->bounce_page = mempool_alloc(ext4_bounce_page_pool,
							 GFP_NOFS | __GFP_WAIT);
		}
		ctx->flags &= ~EXT4_BOUNCE_PAGE_REQUIRES_FREE_ENCRYPT_FL;
	} else {
		ctx->flags |= EXT4_BOUNCE_PAGE_REQUIRES_FREE_ENCRYPT_FL;
	}

	switch (ctx->mode) {
	case EXT4_ENCRYPTION_MODE_AES_256_XTS:
		ciphertext_page = ext4_xts_encrypt(ctx, plaintext_page);
		break;
	case EXT4_ENCRYPTION_MODE_AES_256_GCM:
		/* TODO(mhalcrow): We'll need buffers for the
		 * generated IV and/or auth tag for this mode and the
		 * ones below */
		ciphertext_page = ERR_PTR(-ENOTSUPP);
		break;
	default:
		BUG();
	}
	if (IS_ERR_OR_NULL(ciphertext_page))
		ext4_release_crypto_ctx(ctx);
	else
		ext4_prep_pages_for_write(ciphertext_page, plaintext_page, ctx);
	return ciphertext_page;
}

/**
 * ext4_xts_decrypt() - Decrypts a page using AES-256-XTS
 * @ctx:  The encryption context.
 * @page: The page to decrypt. Must be locked.
 *
 * Return: Zero on success, non-zero otherwise.
 */
static int ext4_xts_decrypt(struct ext4_crypto_ctx *ctx, struct page *page)
{
	u8 xts_tweak[EXT4_XTS_TWEAK_SIZE];
	struct ablkcipher_request *req = NULL;
	struct ext4_crypt_result ecr;
	struct scatterlist sg;
	struct ext4_inode_info *ei = EXT4_I(page->mapping->host);
	struct crypto_ablkcipher *atfm = __crypto_ablkcipher_cast(ctx->tfm);
	int res = 0;

	BUG_ON(!ctx->tfm);
	BUG_ON(ei->i_encryption_key.mode != EXT4_ENCRYPTION_MODE_AES_256_XTS);
	crypto_ablkcipher_clear_flags(atfm, ~0);
	crypto_tfm_set_flags(ctx->tfm, CRYPTO_TFM_REQ_WEAK_KEY);

	/* Since in AES-256-XTS mode we only perform one cryptographic operation
	 * on each block and there are no constraints about how many blocks a
	 * single key can encrypt, we directly use the inode master key */
	res = crypto_ablkcipher_setkey(atfm, ei->i_encryption_key.raw,
				       ei->i_encryption_key.size);
	req = ablkcipher_request_alloc(atfm, GFP_NOFS);
	if (!req) {
		res = -ENOMEM;
		goto out;
	}
	ablkcipher_request_set_callback(
		req, CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
		ext4_crypt_complete, &ecr);
	ext4_xts_tweak_for_page(xts_tweak, page);
	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, PAGE_CACHE_SIZE, 0);
	ablkcipher_request_set_crypt(req, &sg, &sg, PAGE_CACHE_SIZE, xts_tweak);
	res = crypto_ablkcipher_decrypt(req);
	if (res == -EINPROGRESS || res == -EBUSY) {
		BUG_ON(req->base.data != &ecr);
		wait_for_completion(&ecr.completion);
		res = ecr.res;
	}
	ablkcipher_request_free(req);
out:
	if (res)
		printk_ratelimited(KERN_ERR "%s: res = %d\n", __func__, res);
	return res;
}

/**
 * ext4_decrypt() - Decrypts a page in-place
 * @ctx:  The encryption context.
 * @page: The page to decrypt. Must be locked.
 *
 * Decrypts page in-place using the ctx encryption context.
 *
 * Called from the read completion callback.
 *
 * Return: Zero on success, non-zero otherwise.
 */
int ext4_decrypt(struct ext4_crypto_ctx *ctx, struct page *page)
{
	int res = 0;

	BUG_ON(!PageLocked(page));

	switch (ctx->mode) {
	case EXT4_ENCRYPTION_MODE_AES_256_XTS:
		res = ext4_xts_decrypt(ctx, page);
		break;
	case EXT4_ENCRYPTION_MODE_AES_256_GCM:
		res = -ENOTSUPP;
		break;
	default:
		BUG();
	}
	return res;
}

/*
 * Convenience function which takes care of allocating and
 * deallocating the encryption context
 */
int ext4_decrypt_one(struct inode *inode, struct page *page)
{
	int ret;

	struct ext4_crypto_ctx *ctx = ext4_get_crypto_ctx(inode);
	if (!ctx)
		return -ENOMEM;
	ret = ext4_decrypt(ctx, page);
	ext4_release_crypto_ctx(ctx);
	return ret;
}

/**
 * ext4_validate_encryption_mode() - Validates the encryption key mode
 * @mode: The key mode to validate.
 *
 * Return: The validated key mode. EXT4_ENCRYPTION_MODE_INVALID if invalid.
 */
uint32_t ext4_validate_encryption_mode(uint32_t mode)
{
	switch (mode) {
	case EXT4_ENCRYPTION_MODE_AES_256_XTS:
		return mode;
	case EXT4_ENCRYPTION_MODE_AES_256_CBC:
		return mode;
	case EXT4_ENCRYPTION_MODE_AES_256_CTS:
		return mode;
	default:
		break;
	}
	return EXT4_ENCRYPTION_MODE_INVALID;
}

/**
 * ext4_validate_encryption_key_size() - Validate the encryption key size
 * @mode: The key mode.
 * @size: The key size to validate.
 *
 * Return: The validated key size for @mode. Zero if invalid.
 */
uint32_t ext4_validate_encryption_key_size(uint32_t mode, uint32_t size)
{
	if (size == ext4_encryption_key_size(mode))
		return size;
	return 0;
}
