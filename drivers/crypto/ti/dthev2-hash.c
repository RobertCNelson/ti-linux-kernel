// SPDX-License-Identifier: GPL-2.0-only
/*
 * K3 DTHE V2 crypto accelerator driver
 *
 * Copyright (C) Texas Instruments 2025 - https://www.ti.com
 * Author: T Pratham <t-pratham@ti.com>
 */

#include <crypto/algapi.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <crypto/md5.h>
#include <crypto/sha2.h>

#include "dthev2-common.h"

#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/scatterlist.h>

/* Registers */

#define DTHE_P_HASH_BASE		0x5000
#define DTHE_P_HASH512_IDIGEST_A	0x0240
#define DTHE_P_HASH512_DIGEST_COUNT	0x0280
#define DTHE_P_HASH512_MODE		0x0284
#define DTHE_P_HASH512_LENGTH		0x0288
#define DTHE_P_HASH512_DATA_IN_START	0x0080
#define DTHE_P_HASH512_DATA_IN_END	0x00FC

#define DTHE_P_HASH_SYSCONFIG		0x0110
#define DTHE_P_HASH_IRQSTATUS		0x0118
#define DTHE_P_HASH_IRQENABLE		0x011C

/* Register write values and macros */
#define DTHE_HASH_SYSCONFIG_INT_EN		BIT(2)
#define DTHE_HASH_SYSCONFIG_DMA_EN		BIT(3)
#define DTHE_HASH_IRQENABLE_EN_ALL		GENMASK(3, 0)
#define DTHE_HASH_IRQSTATUS_OP_READY		BIT(0)
#define DTHE_HASH_IRQSTATUS_IP_READY		BIT(1)
#define DTHE_HASH_IRQSTATUS_PH_READY		BIT(2)
#define DTHE_HASH_IRQSTATUS_CTX_READY		BIT(3)

#define DTHE_HASH_MODE_USE_ALG_CONST		BIT(3)
#define DTHE_HASH_MODE_CLOSE_HASH		BIT(4)

enum dthe_hash_op {
	DTHE_HASH_OP_UPDATE = 0,
	DTHE_HASH_OP_FINUP,
};

static void dthe_hash_write_zero_message(enum dthe_hash_alg_sel mode, void *dst)
{
	switch (mode) {
	case DTHE_HASH_SHA512:
		memcpy(dst, sha512_zero_message_hash, SHA512_DIGEST_SIZE);
		break;
	case DTHE_HASH_SHA384:
		memcpy(dst, sha384_zero_message_hash, SHA384_DIGEST_SIZE);
		break;
	case DTHE_HASH_SHA256:
		memcpy(dst, sha256_zero_message_hash, SHA256_DIGEST_SIZE);
		break;
	case DTHE_HASH_SHA224:
		memcpy(dst, sha224_zero_message_hash, SHA224_DIGEST_SIZE);
		break;
	case DTHE_HASH_MD5:
		memcpy(dst, md5_zero_message_hash, MD5_DIGEST_SIZE);
		break;
	default:
		break;
	}
}

static enum dthe_hash_alg_sel dthe_hash_get_hash_mode(struct crypto_ahash *tfm)
{
	unsigned int ds = crypto_ahash_digestsize(tfm);
	enum dthe_hash_alg_sel hash_mode;

	/*
	 * Currently, all hash algorithms supported by DTHEv2 have unique digest sizes.
	 * So we can do this. Otherwise, we would have to get the algorithm from the
	 * alg_name and do a strcmp.
	 */
	switch (ds) {
	case SHA512_DIGEST_SIZE:
		hash_mode = DTHE_HASH_SHA512;
		break;
	case SHA384_DIGEST_SIZE:
		hash_mode = DTHE_HASH_SHA384;
		break;
	case SHA256_DIGEST_SIZE:
		hash_mode = DTHE_HASH_SHA256;
		break;
	case SHA224_DIGEST_SIZE:
		hash_mode = DTHE_HASH_SHA224;
		break;
	case MD5_DIGEST_SIZE:
		hash_mode = DTHE_HASH_MD5;
		break;
	default:
		hash_mode = DTHE_HASH_ERR;
		break;
	}

	return hash_mode;
}

static unsigned int dthe_hash_get_phash_size(struct dthe_tfm_ctx *ctx)
{
	unsigned int phash_size = 0;

	switch (ctx->hash_mode) {
	case DTHE_HASH_SHA512:
	case DTHE_HASH_SHA384:
		phash_size = SHA512_DIGEST_SIZE;
		break;
	case DTHE_HASH_SHA256:
	case DTHE_HASH_SHA224:
		phash_size = SHA256_DIGEST_SIZE;
		break;
	case DTHE_HASH_MD5:
		phash_size = MD5_DIGEST_SIZE;
		break;
	default:
		break;
	}

	return phash_size;
}

static int dthe_hash_init_tfm(struct crypto_ahash *tfm)
{
	struct dthe_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	struct dthe_data *dev_data = dthe_get_dev(ctx);

	if (!dev_data)
		return -ENODEV;

	ctx->dev_data = dev_data;

	ctx->hash_mode = dthe_hash_get_hash_mode(tfm);
	if (ctx->hash_mode == DTHE_HASH_ERR)
		return -EINVAL;

	ctx->phash_size = dthe_hash_get_phash_size(ctx);

	return 0;
}

static int dthe_hash_config_dma_chan(struct dma_chan *chan, struct crypto_ahash *tfm)
{
	struct dma_slave_config cfg;
	int bs = crypto_ahash_blocksize(tfm);

	memzero_explicit(&cfg, sizeof(cfg));

	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_maxburst = bs / 4;

	return dmaengine_slave_config(chan, &cfg);
}

static void dthe_hash_dma_in_callback(void *data)
{
	struct ahash_request *req = (struct ahash_request *)data;
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);

	complete(&rctx->hash_compl);
}

static int dthe_hash_dma_start(struct ahash_request *req, struct scatterlist *src,
			       int src_nents, size_t len)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct dthe_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);
	struct dthe_data *dev_data = dthe_get_dev(ctx);
	struct device *tx_dev;
	struct dma_async_tx_descriptor *desc_out;
	int mapped_nents;
	enum dma_data_direction src_dir = DMA_TO_DEVICE;
	u32 hash_mode;
	int ds = crypto_ahash_digestsize(tfm);
	int ret = 0;
	u32 *dst;
	u32 dst_len;
	void __iomem *sha_base_reg = dev_data->regs + DTHE_P_HASH_BASE;

	u32 hash_sysconfig_val = DTHE_HASH_SYSCONFIG_INT_EN | DTHE_HASH_SYSCONFIG_DMA_EN;
	u32 hash_irqenable_val = DTHE_HASH_IRQENABLE_EN_ALL;

	writel_relaxed(hash_sysconfig_val, sha_base_reg + DTHE_P_HASH_SYSCONFIG);
	writel_relaxed(hash_irqenable_val, sha_base_reg + DTHE_P_HASH_IRQENABLE);

	/* Config SHA DMA channel as per SHA mode */
	ret = dthe_hash_config_dma_chan(dev_data->dma_sha_tx, tfm);
	if (ret) {
		dev_err(dev_data->dev, "Can't configure sha_tx dmaengine slave: %d\n", ret);
		goto hash_err;
	}

	tx_dev = dmaengine_get_dma_device(dev_data->dma_sha_tx);
	if (!tx_dev) {
		ret = -ENODEV;
		goto hash_err;
	}

	mapped_nents = dma_map_sg(tx_dev, src, src_nents, src_dir);
	if (mapped_nents == 0) {
		ret = -EINVAL;
		goto hash_err;
	}

	desc_out = dmaengine_prep_slave_sg(dev_data->dma_sha_tx, src, mapped_nents,
					   DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc_out) {
		dev_err(dev_data->dev, "OUT prep_slave_sg() failed\n");
		ret = -EINVAL;
		goto hash_prep_err;
	}

	desc_out->callback = dthe_hash_dma_in_callback;
	desc_out->callback_param = req;

	init_completion(&rctx->hash_compl);

	hash_mode = ctx->hash_mode;

	if (rctx->flags == DTHE_HASH_OP_FINUP)
		hash_mode |= DTHE_HASH_MODE_CLOSE_HASH;

	if (rctx->phash_available) {
		for (int i = 0; i < ctx->phash_size / sizeof(u32); ++i)
			writel_relaxed(rctx->phash[i],
				       sha_base_reg +
				       DTHE_P_HASH512_IDIGEST_A +
				       (DTHE_REG_SIZE * i));

		writel_relaxed(rctx->digestcnt[0],
			       sha_base_reg + DTHE_P_HASH512_DIGEST_COUNT);
	} else {
		hash_mode |= DTHE_HASH_MODE_USE_ALG_CONST;
	}

	writel_relaxed(hash_mode, sha_base_reg + DTHE_P_HASH512_MODE);
	writel_relaxed(len, sha_base_reg + DTHE_P_HASH512_LENGTH);

	dmaengine_submit(desc_out);

	dma_async_issue_pending(dev_data->dma_sha_tx);

	ret = wait_for_completion_timeout(&rctx->hash_compl,
					  msecs_to_jiffies(DTHE_DMA_TIMEOUT_MS));
	if (!ret) {
		dmaengine_terminate_sync(dev_data->dma_sha_tx);
		ret = -ETIMEDOUT;
	} else {
		ret = 0;
	}

	if (rctx->flags == DTHE_HASH_OP_UPDATE) {
		/* If coming from update, we need to read the phash and store it for future */
		dst = rctx->phash;
		dst_len = ctx->phash_size / sizeof(u32);
	} else {
		/* If coming from finup or final, we need to read the final digest */
		dst = (u32 *)req->result;
		dst_len = ds / sizeof(u32);
	}

	for (int i = 0; i < dst_len; ++i)
		dst[i] = readl_relaxed(sha_base_reg +
				       DTHE_P_HASH512_IDIGEST_A +
				       (DTHE_REG_SIZE * i));

	rctx->digestcnt[0] = readl_relaxed(sha_base_reg + DTHE_P_HASH512_DIGEST_COUNT);
	rctx->phash_available = 1;

hash_prep_err:
	dma_unmap_sg(tx_dev, src, src_nents, src_dir);
hash_err:
	return ret;
}

static int dthe_hash_run(struct crypto_engine *engine, void *areq)
{
	struct ahash_request *req = container_of(areq, struct ahash_request, base);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);

	struct scatterlist *src, *sg;
	int src_nents = 0;
	unsigned int bs = crypto_ahash_blocksize(tfm);
	unsigned int tot_len = req->nbytes;
	unsigned int len_to_process;
	unsigned int len_to_buffer;
	unsigned int pad_len = 0;
	u8 *pad_buf = rctx->padding;
	int ret = 0;

	if (rctx->flags == DTHE_HASH_OP_UPDATE) {
		len_to_process = tot_len - (tot_len % bs);
		len_to_buffer = tot_len % bs;

		if (len_to_process == 0) {
			ret = len_to_buffer;
			goto hash_buf_all;
		}
	} else {
		len_to_process = tot_len;
		len_to_buffer = 0;
	}

	src_nents = sg_nents_for_len(req->src, len_to_process);

	/*
	 * Certain DMA restrictions forced us to send data in multiples of BLOCK_SIZE
	 * bytes. So, add a padding 0s at the end of src scatterlist if data is not a
	 * multiple of block_size bytes (Can only happen in final or finup). The extra
	 * data is ignored by the DTHE hardware.
	 */
	if (len_to_process % bs) {
		pad_len = bs - (len_to_process % bs);
		src_nents++;
	}

	src = kcalloc(src_nents, sizeof(*src), GFP_KERNEL);
	if (!src) {
		ret = -ENOMEM;
		goto hash_buf_all;
	}

	sg_init_table(src, src_nents);
	sg = dthe_copy_sg(src, req->src, len_to_process);
	if (pad_len > 0) {
		memset(pad_buf, 0, pad_len);
		sg_set_buf(sg, pad_buf, pad_len);
	}

	ret = dthe_hash_dma_start(req, src, src_nents, len_to_process);
	if (!ret)
		ret = len_to_buffer;

	kfree(src);

hash_buf_all:
	local_bh_disable();
	crypto_finalize_hash_request(engine, req, ret);
	local_bh_enable();
	return 0;
}

static int dthe_hash_init(struct ahash_request *req)
{
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);

	rctx->phash_available = 0;
	rctx->digestcnt[0] = 0;
	rctx->digestcnt[1] = 0;

	return 0;
}

static int dthe_hash_update(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct dthe_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);
	struct dthe_data *dev_data = dthe_get_dev(ctx);
	struct crypto_engine *engine = dev_data->hash_engine;

	if (req->nbytes == 0)
		return 0;

	rctx->flags = DTHE_HASH_OP_UPDATE;

	return crypto_transfer_hash_request_to_engine(engine, req);
}

static int dthe_hash_final(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct dthe_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);
	struct dthe_data *dev_data = dthe_get_dev(ctx);
	struct crypto_engine *engine = dev_data->hash_engine;

	/**
	 * We are always buffering data in update, except when nbytes = 0.
	 * So, either we get the buffered data here (nbytes > 0) or
	 * it is the case that we got zero message to begin with
	 */
	if (req->nbytes > 0) {
		rctx->flags = DTHE_HASH_OP_FINUP;

		return crypto_transfer_hash_request_to_engine(engine, req);
	}

	dthe_hash_write_zero_message(ctx->hash_mode, req->result);

	return 0;
}

static int dthe_hash_finup(struct ahash_request *req)
{
	/* With AHASH_ALG_BLOCK_ONLY, final becomes same as finup. */
	return dthe_hash_final(req);
}

static int dthe_hash_digest(struct ahash_request *req)
{
	dthe_hash_init(req);
	return dthe_hash_finup(req);
}

static int dthe_hash_export(struct ahash_request *req, void *out)
{
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct dthe_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	union {
		u8 *u8;
		u64 *u64;
	} p = { .u8 = out };

	memcpy(out, rctx->phash, ctx->phash_size);
	p.u8 += ctx->phash_size;
	put_unaligned(rctx->digestcnt[0], p.u64++);
	if (ctx->phash_size >= SHA512_DIGEST_SIZE)
		put_unaligned(rctx->digestcnt[1], p.u64++);

	return 0;
}

static int dthe_hash_import(struct ahash_request *req, const void *in)
{
	struct dthe_hash_req_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct dthe_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	union {
		const u8 *u8;
		const u64 *u64;
	} p = { .u8 = in };

	memcpy(rctx->phash, in, ctx->phash_size);
	p.u8 += ctx->phash_size;
	rctx->digestcnt[0] = get_unaligned(p.u64++);
	if (ctx->phash_size >= SHA512_DIGEST_SIZE)
		rctx->digestcnt[1] = get_unaligned(p.u64++);
	rctx->phash_available = ((rctx->digestcnt[0]) ? 1 : 0);

	return 0;
}

static struct ahash_engine_alg hash_algs[] = {
	{
		.base.init_tfm	= dthe_hash_init_tfm,
		.base.init	= dthe_hash_init,
		.base.update	= dthe_hash_update,
		.base.final	= dthe_hash_final,
		.base.finup	= dthe_hash_finup,
		.base.digest	= dthe_hash_digest,
		.base.export	= dthe_hash_export,
		.base.import	= dthe_hash_import,
		.base.halg	= {
			.digestsize = SHA512_DIGEST_SIZE,
			.statesize = sizeof(struct dthe_hash_req_ctx),
			.base = {
				.cra_name	 = "sha512",
				.cra_driver_name = "sha512-dthev2",
				.cra_priority	 = 299,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
						   CRYPTO_ALG_ASYNC |
						   CRYPTO_ALG_OPTIONAL_KEY |
						   CRYPTO_ALG_KERN_DRIVER_ONLY |
						   CRYPTO_ALG_ALLOCATES_MEMORY |
						   CRYPTO_AHASH_ALG_BLOCK_ONLY |
						   CRYPTO_AHASH_ALG_FINAL_NONZERO |
						   CRYPTO_AHASH_ALG_FINUP_MAX |
						   CRYPTO_AHASH_ALG_NO_EXPORT_CORE,
				.cra_blocksize	 = SHA512_BLOCK_SIZE,
				.cra_ctxsize	 = sizeof(struct dthe_tfm_ctx),
				.cra_reqsize	 = sizeof(struct dthe_hash_req_ctx),
				.cra_module	 = THIS_MODULE,
			}
		},
		.op.do_one_request = dthe_hash_run,
	},
	{
		.base.init_tfm	= dthe_hash_init_tfm,
		.base.init	= dthe_hash_init,
		.base.update	= dthe_hash_update,
		.base.final	= dthe_hash_final,
		.base.finup	= dthe_hash_finup,
		.base.digest	= dthe_hash_digest,
		.base.export	= dthe_hash_export,
		.base.import	= dthe_hash_import,
		.base.halg	= {
			.digestsize = SHA384_DIGEST_SIZE,
			.statesize = sizeof(struct dthe_hash_req_ctx),
			.base = {
				.cra_name	 = "sha384",
				.cra_driver_name = "sha384-dthev2",
				.cra_priority	 = 299,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
						   CRYPTO_ALG_ASYNC |
						   CRYPTO_ALG_OPTIONAL_KEY |
						   CRYPTO_ALG_KERN_DRIVER_ONLY |
						   CRYPTO_ALG_ALLOCATES_MEMORY |
						   CRYPTO_AHASH_ALG_BLOCK_ONLY |
						   CRYPTO_AHASH_ALG_FINAL_NONZERO |
						   CRYPTO_AHASH_ALG_FINUP_MAX |
						   CRYPTO_AHASH_ALG_NO_EXPORT_CORE,
				.cra_blocksize	 = SHA384_BLOCK_SIZE,
				.cra_ctxsize	 = sizeof(struct dthe_tfm_ctx),
				.cra_reqsize	 = sizeof(struct dthe_hash_req_ctx),
				.cra_module	 = THIS_MODULE,
			}
		},
		.op.do_one_request = dthe_hash_run,
	},
	{
		.base.init_tfm	= dthe_hash_init_tfm,
		.base.init	= dthe_hash_init,
		.base.update	= dthe_hash_update,
		.base.final	= dthe_hash_final,
		.base.finup	= dthe_hash_finup,
		.base.digest	= dthe_hash_digest,
		.base.export	= dthe_hash_export,
		.base.import	= dthe_hash_import,
		.base.halg	= {
			.digestsize = SHA256_DIGEST_SIZE,
			.statesize = sizeof(struct dthe_hash_req_ctx),
			.base = {
				.cra_name	 = "sha256",
				.cra_driver_name = "sha256-dthev2",
				.cra_priority	 = 299,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
						   CRYPTO_ALG_ASYNC |
						   CRYPTO_ALG_OPTIONAL_KEY |
						   CRYPTO_ALG_KERN_DRIVER_ONLY |
						   CRYPTO_ALG_ALLOCATES_MEMORY |
						   CRYPTO_AHASH_ALG_BLOCK_ONLY |
						   CRYPTO_AHASH_ALG_FINAL_NONZERO |
						   CRYPTO_AHASH_ALG_FINUP_MAX |
						   CRYPTO_AHASH_ALG_NO_EXPORT_CORE,
				.cra_blocksize	 = SHA256_BLOCK_SIZE,
				.cra_ctxsize	 = sizeof(struct dthe_tfm_ctx),
				.cra_reqsize	 = sizeof(struct dthe_hash_req_ctx),
				.cra_module	 = THIS_MODULE,
			}
		},
		.op.do_one_request = dthe_hash_run,
	},
	{
		.base.init_tfm	= dthe_hash_init_tfm,
		.base.init	= dthe_hash_init,
		.base.update	= dthe_hash_update,
		.base.final	= dthe_hash_final,
		.base.finup	= dthe_hash_finup,
		.base.digest	= dthe_hash_digest,
		.base.export	= dthe_hash_export,
		.base.import	= dthe_hash_import,
		.base.halg	= {
			.digestsize = SHA224_DIGEST_SIZE,
			.statesize = sizeof(struct dthe_hash_req_ctx),
			.base = {
				.cra_name	 = "sha224",
				.cra_driver_name = "sha224-dthev2",
				.cra_priority	 = 299,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
						   CRYPTO_ALG_ASYNC |
						   CRYPTO_ALG_OPTIONAL_KEY |
						   CRYPTO_ALG_KERN_DRIVER_ONLY |
						   CRYPTO_ALG_ALLOCATES_MEMORY |
						   CRYPTO_AHASH_ALG_BLOCK_ONLY |
						   CRYPTO_AHASH_ALG_FINAL_NONZERO |
						   CRYPTO_AHASH_ALG_FINUP_MAX |
						   CRYPTO_AHASH_ALG_NO_EXPORT_CORE,
				.cra_blocksize	 = SHA224_BLOCK_SIZE,
				.cra_ctxsize	 = sizeof(struct dthe_tfm_ctx),
				.cra_reqsize	 = sizeof(struct dthe_hash_req_ctx),
				.cra_module	 = THIS_MODULE,
			}
		},
		.op.do_one_request = dthe_hash_run,
	},
	{
		.base.init_tfm	= dthe_hash_init_tfm,
		.base.init	= dthe_hash_init,
		.base.update	= dthe_hash_update,
		.base.final	= dthe_hash_final,
		.base.finup	= dthe_hash_finup,
		.base.digest	= dthe_hash_digest,
		.base.export	= dthe_hash_export,
		.base.import	= dthe_hash_import,
		.base.halg	= {
			.digestsize = MD5_DIGEST_SIZE,
			.statesize = sizeof(struct dthe_hash_req_ctx),
			.base = {
				.cra_name	 = "md5",
				.cra_driver_name = "md5-dthev2",
				.cra_priority	 = 299,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
						   CRYPTO_ALG_ASYNC |
						   CRYPTO_ALG_OPTIONAL_KEY |
						   CRYPTO_ALG_KERN_DRIVER_ONLY |
						   CRYPTO_ALG_ALLOCATES_MEMORY |
						   CRYPTO_AHASH_ALG_BLOCK_ONLY |
						   CRYPTO_AHASH_ALG_FINAL_NONZERO |
						   CRYPTO_AHASH_ALG_FINUP_MAX |
						   CRYPTO_AHASH_ALG_NO_EXPORT_CORE,
				.cra_blocksize	 = MD5_BLOCK_SIZE,
				.cra_ctxsize	 = sizeof(struct dthe_tfm_ctx),
				.cra_reqsize	 = sizeof(struct dthe_hash_req_ctx),
				.cra_module	 = THIS_MODULE,
			}
		},
		.op.do_one_request = dthe_hash_run,
	},
};

int dthe_register_hash_algs(void)
{
	return crypto_engine_register_ahashes(hash_algs, ARRAY_SIZE(hash_algs));
}

void dthe_unregister_hash_algs(void)
{
	crypto_engine_unregister_ahashes(hash_algs, ARRAY_SIZE(hash_algs));
}
