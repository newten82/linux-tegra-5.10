// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Quick & dirty crypto testing module.
 *
 * This will only exist until we have a better testing mechanism
 * (e.g. a char device).
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * Copyright (c) 2002 Jean-Francois Dive <jef@linuxbe.org>
 * Copyright (c) 2007 Nokia Siemens Networks
 * Copyright (c) 2016-2021, NVIDIA Corporation. All Rights Reserved.
 *
 * Updated RFC4106 AES-GCM testing.
 *    Authors: Aidan O'Mahony (aidan.o.mahony@intel.com)
 *             Adrian Hoban <adrian.hoban@intel.com>
 *             Gabriele Paoloni <gabriele.paoloni@intel.com>
 *             Tadeusz Struk (tadeusz.struk@intel.com)
 *             Copyright (c) 2010, Intel Corporation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/aead.h>
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <crypto/akcipher.h>
#include <linux/err.h>
#include <linux/fips.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/timex.h>
#include <linux/interrupt.h>

#include "tcrypt.h"

/*
 * Need slab memory for testing (size in number of pages).
 */
#define TVMEMSIZE	4

/*
 * Used by test_cipher_speed()
 */
#define DECRYPT		0
#define ENCRYPT		1
#define SIGN		2
#define VERIFY		3

#define MAX_DIGEST_SIZE		64
#define MAX_PAGE_ORDER		10
#define MAX_PAGE_ALLOC		BIT(MAX_PAGE_ORDER)

/*
 * return a string with the driver name
 */
#define get_driver_name(tfm_type, tfm) crypto_tfm_alg_driver_name(tfm_type ## _tfm(tfm))

/*
 * Used by test_cipher_speed()
 */
static unsigned int sec;
static unsigned long dsize;
static unsigned int bsize;
static unsigned int bcnt;
static unsigned int enc_target;
static unsigned int dec_target;
static char *alg = NULL;
static u32 type;
static u32 mask;
static int mode;
static u32 num_mb = 8;
static unsigned int klen;
static char *tvmem[TVMEMSIZE];

static const char *check[] = {
	"des", "md5", "des3_ede", "rot13", "sha1", "sha224", "sha256", "sm3",
	"blowfish", "twofish", "serpent", "sha384", "sha512", "md4", "aes",
	"cast6", "arc4", "michael_mic", "deflate", "crc32c", "tea", "xtea",
	"khazad", "wp512", "wp384", "wp256", "tnepres", "xeta",  "fcrypt",
	"camellia", "seed", "salsa20", "rmd128", "rmd160", "rmd256", "rmd320",
	"lzo", "lzo-rle", "cts", "sha3-224", "sha3-256", "sha3-384",
	"sha3-512", "streebog256", "streebog512",
	NULL
};

static u32 block_sizes[] = { 16, 64, 256, 512, 1024, 1472, 8192, 0 };
static u32 aead_sizes[] = { 16, 64, 256, 512, 1024, 2048, 4096, 8192, 0 };

static atomic_t tcrypt_aes_buf[MAX_PAGE_ALLOC];

#define XBUFSIZE 8
#define MAX_IVLEN 32

static int testmgr_alloc_buf(char *buf[XBUFSIZE])
{
	int i;

	for (i = 0; i < XBUFSIZE; i++) {
		buf[i] = (void *)__get_free_page(GFP_KERNEL);
		if (!buf[i])
			goto err_free_buf;
	}

	return 0;

err_free_buf:
	while (i-- > 0)
		free_page((unsigned long)buf[i]);

	return -ENOMEM;
}

static void testmgr_free_buf(char *buf[XBUFSIZE])
{
	int i;

	for (i = 0; i < XBUFSIZE; i++)
		free_page((unsigned long)buf[i]);
}

static void sg_init_aead(struct scatterlist *sg, char *xbuf[XBUFSIZE],
			 unsigned int buflen, const void *assoc,
			 unsigned int aad_size)
{
	int np = (buflen + PAGE_SIZE - 1)/PAGE_SIZE;
	int k, rem;

	if (np > XBUFSIZE) {
		rem = PAGE_SIZE;
		np = XBUFSIZE;
	} else {
		rem = buflen % PAGE_SIZE;
	}

	sg_init_table(sg, np + 1);

	sg_set_buf(&sg[0], assoc, aad_size);

	if (rem)
		np--;
	for (k = 0; k < np; k++)
		sg_set_buf(&sg[k + 1], xbuf[k], PAGE_SIZE);

	if (rem)
		sg_set_buf(&sg[k + 1], xbuf[k], rem);
}

static inline int do_one_aead_op(struct aead_request *req, int ret)
{
	struct crypto_wait *wait = req->base.data;

	return crypto_wait_req(ret, wait);
}

struct test_mb_aead_data {
	struct scatterlist sg[XBUFSIZE];
	struct scatterlist sgout[XBUFSIZE];
	struct aead_request *req;
	struct crypto_wait wait;
	char *xbuf[XBUFSIZE];
	char *xoutbuf[XBUFSIZE];
	char *axbuf[XBUFSIZE];
};

static int do_mult_aead_op(struct test_mb_aead_data *data, int enc,
				u32 num_mb, int *rc)
{
	int i, err = 0;

	/* Fire up a bunch of concurrent requests */
	for (i = 0; i < num_mb; i++) {
		if (enc == ENCRYPT)
			rc[i] = crypto_aead_encrypt(data[i].req);
		else
			rc[i] = crypto_aead_decrypt(data[i].req);
	}

	/* Wait for all requests to finish */
	for (i = 0; i < num_mb; i++) {
		rc[i] = crypto_wait_req(rc[i], &data[i].wait);

		if (rc[i]) {
			pr_info("concurrent request %d error %d\n", i, rc[i]);
			err = rc[i];
		}
	}

	return err;
}

static int test_mb_aead_jiffies(struct test_mb_aead_data *data, int enc,
				int blen, int secs, u32 num_mb)
{
	unsigned long start, end;
	int bcount;
	int ret = 0;
	int *rc;

	rc = kcalloc(num_mb, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
	     time_before(jiffies, end); bcount++) {
		ret = do_mult_aead_op(data, enc, num_mb, rc);
		if (ret)
			goto out;
	}

	pr_cont("%d operations in %d seconds (%llu bytes)\n",
		bcount * num_mb, secs, (u64)bcount * blen * num_mb);

out:
	kfree(rc);
	return ret;
}

static int test_mb_aead_cycles(struct test_mb_aead_data *data, int enc,
			       int blen, u32 num_mb)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;
	int *rc;

	rc = kcalloc(num_mb, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_mult_aead_op(data, enc, num_mb, rc);
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		ret = do_mult_aead_op(data, enc, num_mb, rc);
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

	pr_cont("1 operation in %lu cycles (%d bytes)\n",
		(cycles + 4) / (8 * num_mb), blen);

out:
	kfree(rc);
	return ret;
}

static void test_mb_aead_speed(const char *algo, int enc, int secs,
			       struct aead_speed_template *template,
			       unsigned int tcount, u8 authsize,
			       unsigned int aad_size, u8 *keysize, u32 num_mb)
{
	struct test_mb_aead_data *data;
	struct crypto_aead *tfm;
	unsigned int i, j, iv_len;
	const char *key;
	const char *e;
	void *assoc;
	u32 *b_size;
	char *iv;
	int ret;


	if (aad_size >= PAGE_SIZE) {
		pr_err("associate data length (%u) too big\n", aad_size);
		return;
	}

	iv = kzalloc(MAX_IVLEN, GFP_KERNEL);
	if (!iv)
		return;

	if (enc == ENCRYPT)
		e = "encryption";
	else
		e = "decryption";

	data = kcalloc(num_mb, sizeof(*data), GFP_KERNEL);
	if (!data)
		goto out_free_iv;

	tfm = crypto_alloc_aead(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n",
			algo, PTR_ERR(tfm));
		goto out_free_data;
	}

	ret = crypto_aead_setauthsize(tfm, authsize);

	for (i = 0; i < num_mb; ++i)
		if (testmgr_alloc_buf(data[i].xbuf)) {
			while (i--)
				testmgr_free_buf(data[i].xbuf);
			goto out_free_tfm;
		}

	for (i = 0; i < num_mb; ++i)
		if (testmgr_alloc_buf(data[i].axbuf)) {
			while (i--)
				testmgr_free_buf(data[i].axbuf);
			goto out_free_xbuf;
		}

	for (i = 0; i < num_mb; ++i)
		if (testmgr_alloc_buf(data[i].xoutbuf)) {
			while (i--)
				testmgr_free_buf(data[i].xoutbuf);
			goto out_free_axbuf;
		}

	for (i = 0; i < num_mb; ++i) {
		data[i].req = aead_request_alloc(tfm, GFP_KERNEL);
		if (!data[i].req) {
			pr_err("alg: skcipher: Failed to allocate request for %s\n",
			       algo);
			while (i--)
				aead_request_free(data[i].req);
			goto out_free_xoutbuf;
		}
	}

	for (i = 0; i < num_mb; ++i) {
		crypto_init_wait(&data[i].wait);
		aead_request_set_callback(data[i].req,
					  CRYPTO_TFM_REQ_MAY_BACKLOG,
					  crypto_req_done, &data[i].wait);
	}

	pr_info("\ntesting speed of multibuffer %s (%s) %s\n", algo,
		get_driver_name(crypto_aead, tfm), e);

	i = 0;
	do {
		b_size = aead_sizes;
		do {
			if (*b_size + authsize > XBUFSIZE * PAGE_SIZE) {
				pr_err("template (%u) too big for buffer (%lu)\n",
				       authsize + *b_size,
				       XBUFSIZE * PAGE_SIZE);
				goto out;
			}

			pr_info("test %u (%d bit key, %d byte blocks): ", i,
				*keysize * 8, *b_size);

			/* Set up tfm global state, i.e. the key */

			memset(tvmem[0], 0xff, PAGE_SIZE);
			key = tvmem[0];
			for (j = 0; j < tcount; j++) {
				if (template[j].klen == *keysize) {
					key = template[j].key;
					break;
				}
			}

			crypto_aead_clear_flags(tfm, ~0);

			ret = crypto_aead_setkey(tfm, key, *keysize);
			if (ret) {
				pr_err("setkey() failed flags=%x\n",
				       crypto_aead_get_flags(tfm));
				goto out;
			}

			iv_len = crypto_aead_ivsize(tfm);
			if (iv_len)
				memset(iv, 0xff, iv_len);

			/* Now setup per request stuff, i.e. buffers */

			for (j = 0; j < num_mb; ++j) {
				struct test_mb_aead_data *cur = &data[j];

				assoc = cur->axbuf[0];
				memset(assoc, 0xff, aad_size);

				sg_init_aead(cur->sg, cur->xbuf,
					     *b_size + (enc ? 0 : authsize),
					     assoc, aad_size);

				sg_init_aead(cur->sgout, cur->xoutbuf,
					     *b_size + (enc ? authsize : 0),
					     assoc, aad_size);

				aead_request_set_ad(cur->req, aad_size);

				if (!enc) {

					aead_request_set_crypt(cur->req,
							       cur->sgout,
							       cur->sg,
							       *b_size, iv);
					ret = crypto_aead_encrypt(cur->req);
					ret = do_one_aead_op(cur->req, ret);

					if (ret) {
						pr_err("calculating auth failed (%d)\n",
						       ret);
						break;
					}
				}

				aead_request_set_crypt(cur->req, cur->sg,
						       cur->sgout, *b_size +
						       (enc ? 0 : authsize),
						       iv);

			}

			if (secs) {
				ret = test_mb_aead_jiffies(data, enc, *b_size,
							   secs, num_mb);
				cond_resched();
			} else {
				ret = test_mb_aead_cycles(data, enc, *b_size,
							  num_mb);
			}

			if (ret) {
				pr_err("%s() failed return code=%d\n", e, ret);
				break;
			}
			b_size++;
			i++;
		} while (*b_size);
		keysize++;
	} while (*keysize);

out:
	for (i = 0; i < num_mb; ++i)
		aead_request_free(data[i].req);
out_free_xoutbuf:
	for (i = 0; i < num_mb; ++i)
		testmgr_free_buf(data[i].xoutbuf);
out_free_axbuf:
	for (i = 0; i < num_mb; ++i)
		testmgr_free_buf(data[i].axbuf);
out_free_xbuf:
	for (i = 0; i < num_mb; ++i)
		testmgr_free_buf(data[i].xbuf);
out_free_tfm:
	crypto_free_aead(tfm);
out_free_data:
	kfree(data);
out_free_iv:
	kfree(iv);
}

static int test_aead_jiffies(struct aead_request *req, int enc,
				int blen, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		if (enc)
			ret = do_one_aead_op(req, crypto_aead_encrypt(req));
		else
			ret = do_one_aead_op(req, crypto_aead_decrypt(req));

		if (ret)
			return ret;
	}

	pr_cont("%d operations in %d seconds (%llu bytes)\n",
	        bcount, secs, (u64)bcount * blen);
	return 0;
}

static int test_aead_cycles(struct aead_request *req, int enc, int blen)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		if (enc)
			ret = do_one_aead_op(req, crypto_aead_encrypt(req));
		else
			ret = do_one_aead_op(req, crypto_aead_decrypt(req));

		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (enc)
			ret = do_one_aead_op(req, crypto_aead_encrypt(req));
		else
			ret = do_one_aead_op(req, crypto_aead_decrypt(req));
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

out:
	if (ret == 0)
		pr_info("1 operation in %lu cycles (%d bytes)\n",
			(cycles + 4) / 8, blen);

	return ret;
}

static void test_aead_speed(const char *algo, int enc, unsigned int secs,
			struct aead_speed_template *template,
			unsigned int tcount, u8 authsize,
			unsigned int aad_size, u8 *keysize)
{
	unsigned int i, j;
	struct crypto_aead *tfm;
	int ret = -ENOMEM;
	const char *key;
	struct aead_request *req;
	struct scatterlist *sg;
	struct scatterlist *sgout;
	const char *e;
	void *assoc;
	char *iv;
	char *xbuf[XBUFSIZE];
	char *xoutbuf[XBUFSIZE];
	char *axbuf[XBUFSIZE];
	unsigned int *b_size;
	unsigned int iv_len;
	struct crypto_wait wait;

	iv = kzalloc(MAX_IVLEN, GFP_KERNEL);
	if (!iv)
		return;

	if (aad_size >= PAGE_SIZE) {
		pr_err("associate data length (%u) too big\n", aad_size);
		goto out_noxbuf;
	}

	if (enc == ENCRYPT)
		e = "encryption";
	else
		e = "decryption";

	if (testmgr_alloc_buf(xbuf))
		goto out_noxbuf;
	if (testmgr_alloc_buf(axbuf))
		goto out_noaxbuf;
	if (testmgr_alloc_buf(xoutbuf))
		goto out_nooutbuf;

	sg = kmalloc(sizeof(*sg) * 9 * 2, GFP_KERNEL);
	if (!sg)
		goto out_nosg;
	sgout = &sg[9];

	tfm = crypto_alloc_aead(algo, 0, 0);

	if (IS_ERR(tfm)) {
		pr_err("alg: aead: Failed to load transform for %s: %ld\n",
			algo, PTR_ERR(tfm));
		goto out_notfm;
	}

	crypto_init_wait(&wait);
	pr_info("\ntesting speed of %s (%s) %s\n", algo,
			get_driver_name(crypto_aead, tfm), e);

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("alg: aead: Failed to allocate request for %s\n",
			algo);
		goto out_noreq;
	}

	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  crypto_req_done, &wait);

	i = 0;
	do {
		b_size = aead_sizes;
		do {
			assoc = axbuf[0];
			memset(assoc, 0xff, aad_size);

			if ((*keysize + *b_size) > TVMEMSIZE * PAGE_SIZE) {
				pr_err("template(%u) too big for tvmem (%lu)\n",
					*keysize + *b_size,
					TVMEMSIZE * PAGE_SIZE);
				goto out;
			}

			key = tvmem[0];
			for (j = 0; j < tcount; j++) {
				if (template[j].klen == *keysize) {
					key = template[j].key;
					break;
				}
			}
			ret = crypto_aead_setkey(tfm, key, *keysize);
			ret = crypto_aead_setauthsize(tfm, authsize);

			iv_len = crypto_aead_ivsize(tfm);
			if (iv_len)
				memset(iv, 0xff, iv_len);

			crypto_aead_clear_flags(tfm, ~0);
			pr_info("test %u (%d bit key,%d byte blocks):",
					i, *keysize * 8, *b_size);


			memset(tvmem[0], 0xff, PAGE_SIZE);

			if (ret) {
				pr_err("setkey() failed flags=%x\n",
						crypto_aead_get_flags(tfm));
				goto out;
			}

			sg_init_aead(sg, xbuf, *b_size + (enc ? 0 : authsize),
				     assoc, aad_size);

			sg_init_aead(sgout, xoutbuf,
				     *b_size + (enc ? authsize : 0), assoc,
				     aad_size);

			aead_request_set_ad(req, aad_size);

			if (!enc) {

				/*
				 * For decryption we need a proper auth so
				 * we do the encryption path once with buffers
				 * reversed (input <-> output) to calculate it
				 */
				aead_request_set_crypt(req, sgout, sg,
						       *b_size, iv);
				ret = do_one_aead_op(req,
						     crypto_aead_encrypt(req));

				if (ret) {
					pr_err("calculating auth failed (%d)\n",
					       ret);
					break;
				}
			}

			aead_request_set_crypt(req, sg, sgout,
					       *b_size + (enc ? 0 : authsize),
					       iv);

			if (secs) {
				ret = test_aead_jiffies(req, enc, *b_size,
							secs);
				cond_resched();
			} else {
				ret = test_aead_cycles(req, enc, *b_size);
			}

			if (ret) {
				pr_err("%s() failed return code=%d\n", e, ret);
				break;
			}
			b_size++;
			i++;
		} while (*b_size);
		keysize++;
	} while (*keysize);

out:
	aead_request_free(req);
out_noreq:
	crypto_free_aead(tfm);
out_notfm:
	kfree(sg);
out_nosg:
	testmgr_free_buf(xoutbuf);
out_nooutbuf:
	testmgr_free_buf(axbuf);
out_noaxbuf:
	testmgr_free_buf(xbuf);
out_noxbuf:
	kfree(iv);
}

static void test_hash_sg_init(struct scatterlist *sg, unsigned long dsize)
{
	int i;

	if (dsize) {
		sg_init_table(sg, 1);
		sg_set_buf(sg, tvmem[0], dsize);
		memset(tvmem[0], 0xff, dsize);
	} else {
		sg_init_table(sg, TVMEMSIZE);
		for (i = 0; i < TVMEMSIZE; i++) {
			sg_set_buf(sg + i, tvmem[i], PAGE_SIZE);
			memset(tvmem[i], 0xff, PAGE_SIZE);
		}
	}
}

static inline int do_one_ahash_op(struct ahash_request *req, int ret)
{
	struct crypto_wait *wait = req->base.data;

	return crypto_wait_req(ret, wait);
}

struct test_mb_ahash_data {
	struct scatterlist sg[XBUFSIZE];
	char result[64];
	struct ahash_request *req;
	struct crypto_wait wait;
	char *xbuf[XBUFSIZE];
};

static inline int do_mult_ahash_op(struct test_mb_ahash_data *data, u32 num_mb,
				   int *rc)
{
	int i, err = 0;

	/* Fire up a bunch of concurrent requests */
	for (i = 0; i < num_mb; i++)
		rc[i] = crypto_ahash_digest(data[i].req);

	/* Wait for all requests to finish */
	for (i = 0; i < num_mb; i++) {
		rc[i] = crypto_wait_req(rc[i], &data[i].wait);

		if (rc[i]) {
			pr_info("concurrent request %d error %d\n", i, rc[i]);
			err = rc[i];
		}
	}

	return err;
}

static int test_mb_ahash_jiffies(struct test_mb_ahash_data *data, int blen,
				 int secs, u32 num_mb)
{
	unsigned long start, end;
	int bcount;
	int ret = 0;
	int *rc;

	rc = kcalloc(num_mb, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
	     time_before(jiffies, end); bcount++) {
		ret = do_mult_ahash_op(data, num_mb, rc);
		if (ret)
			goto out;
	}

	pr_cont("%d operations in %d seconds (%llu bytes)\n",
		bcount * num_mb, secs, (u64)bcount * blen * num_mb);

out:
	kfree(rc);
	return ret;
}

static int test_mb_ahash_cycles(struct test_mb_ahash_data *data, int blen,
				u32 num_mb)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;
	int *rc;

	rc = kcalloc(num_mb, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_mult_ahash_op(data, num_mb, rc);
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		ret = do_mult_ahash_op(data, num_mb, rc);
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

	pr_cont("1 operation in %lu cycles (%d bytes)\n",
		(cycles + 4) / (8 * num_mb), blen);

out:
	kfree(rc);
	return ret;
}

static void test_mb_ahash_speed(const char *algo, unsigned int secs,
				struct hash_speed *speed, u32 num_mb)
{
	struct test_mb_ahash_data *data;
	struct crypto_ahash *tfm;
	unsigned int i, j, k;
	int ret;

	data = kcalloc(num_mb, sizeof(*data), GFP_KERNEL);
	if (!data)
		return;

	tfm = crypto_alloc_ahash(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n",
			algo, PTR_ERR(tfm));
		goto free_data;
	}

	for (i = 0; i < num_mb; ++i) {
		if (testmgr_alloc_buf(data[i].xbuf))
			goto out;

		crypto_init_wait(&data[i].wait);

		data[i].req = ahash_request_alloc(tfm, GFP_KERNEL);
		if (!data[i].req) {
			pr_err("alg: hash: Failed to allocate request for %s\n",
			       algo);
			goto out;
		}

		ahash_request_set_callback(data[i].req, 0, crypto_req_done,
					   &data[i].wait);

		sg_init_table(data[i].sg, XBUFSIZE);
		for (j = 0; j < XBUFSIZE; j++) {
			sg_set_buf(data[i].sg + j, data[i].xbuf[j], PAGE_SIZE);
			memset(data[i].xbuf[j], 0xff, PAGE_SIZE);
		}
	}

	pr_info("\ntesting speed of multibuffer %s (%s)\n", algo,
		get_driver_name(crypto_ahash, tfm));

	for (i = 0; speed[i].blen != 0; i++) {
		/* For some reason this only tests digests. */
		if (speed[i].blen != speed[i].plen)
			continue;

		if (speed[i].blen > XBUFSIZE * PAGE_SIZE) {
			pr_err("template (%u) too big for tvmem (%lu)\n",
			       speed[i].blen, XBUFSIZE * PAGE_SIZE);
			goto out;
		}

		if (klen)
			crypto_ahash_setkey(tfm, tvmem[0], klen);

		for (k = 0; k < num_mb; k++)
			ahash_request_set_crypt(data[k].req, data[k].sg,
						data[k].result, speed[i].blen);

		pr_info("test%3u "
			"(%5u byte blocks,%5u bytes per update,%4u updates): ",
			i, speed[i].blen, speed[i].plen,
			speed[i].blen / speed[i].plen);

		if (secs) {
			ret = test_mb_ahash_jiffies(data, speed[i].blen, secs,
						    num_mb);
			cond_resched();
		} else {
			ret = test_mb_ahash_cycles(data, speed[i].blen, num_mb);
		}


		if (ret) {
			pr_err("At least one hashing failed ret=%d\n", ret);
			break;
		}
	}
out:
	for (k = 0; k < num_mb; ++k)
		ahash_request_free(data[k].req);

	for (k = 0; k < num_mb; ++k)
		testmgr_free_buf(data[k].xbuf);

	crypto_free_ahash(tfm);

free_data:
	kfree(data);
}

static int test_ahash_jiffies_digest(struct ahash_request *req, int blen,
					char *out, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			return ret;
	}

	pr_info("%6u opers/sec, %9lu bytes/sec\n",
		bcount / secs, ((long)bcount * blen) / secs);

	return 0;
}

static int test_ahash_jiffies(struct ahash_request *req, int blen,
				int plen, char *out, int secs)
{
	unsigned long start, end;
	int bcount, pcount;
	int ret;

	if (plen == blen)
		return test_ahash_jiffies_digest(req, blen, out, secs);

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		ret = do_one_ahash_op(req, crypto_ahash_init(req));
		if (ret)
			return ret;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = do_one_ahash_op(req, crypto_ahash_update(req));
			if (ret)
				return ret;
		}
		/* we assume there is enough space in 'out' for the result */
		ret = do_one_ahash_op(req, crypto_ahash_final(req));
		if (ret)
			return ret;
	}

	pr_cont("%6u opers/sec, %9lu bytes/sec\n",
		bcount / secs, ((long)bcount * blen) / secs);

	return 0;
}

static int test_ahash_perf(struct ahash_request *req, unsigned long dsize)
{
	int ret, i;
	unsigned long before_t, after_t;
	unsigned long tot_time = 0;
	unsigned long long bps = 0;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			return ret;
	}
	/* The real thing. */
	for (i = 0; i < 10; i++) {
		before_t = ktime_get_real_fast_ns();

		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			return ret;

		after_t = ktime_get_real_fast_ns();
		tot_time += (after_t - before_t);
	}

	tot_time = tot_time / 10;
	bps = (unsigned long long)(dsize * 1000000000) / (tot_time);

	pr_info("\nPerformance: %llu MegaBytes/sec\n", (bps / (1024 * 1024)));

	return 0;
}

static int test_ahash_cycles_digest(struct ahash_request *req, int blen,
				char *out)
{
	unsigned long cycles = 0;
	int ret, i;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();

		ret = do_one_ahash_op(req, crypto_ahash_digest(req));
		if (ret)
			goto out;

		end = get_cycles();

		cycles += end - start;
	}

out:
	if (ret)
		return ret;

	pr_cont("%6lu cycles/operation, %4lu cycles/byte\n",
		cycles / 8, cycles / (8 * blen));

	return 0;
}

static int test_ahash_cycles(struct ahash_request *req, int blen,
			int plen, char *out)
{
	unsigned long cycles = 0;
	int i, pcount, ret;

	if (plen == blen)
		return test_ahash_cycles_digest(req, blen, out);

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_one_ahash_op(req, crypto_ahash_init(req));
		if (ret)
			goto out;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = do_one_ahash_op(req, crypto_ahash_update(req));
			if (ret)
				goto out;
		}
		ret = do_one_ahash_op(req, crypto_ahash_final(req));
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();

		ret = do_one_ahash_op(req, crypto_ahash_init(req));
		if (ret)
			goto out;
		for (pcount = 0; pcount < blen; pcount += plen) {
			ret = do_one_ahash_op(req, crypto_ahash_update(req));
			if (ret)
				goto out;
		}
		ret = do_one_ahash_op(req, crypto_ahash_final(req));
		if (ret)
			goto out;

		end = get_cycles();

		cycles += end - start;
	}

out:
	if (ret)
		return ret;

	pr_cont("%6lu cycles/operation, %4lu cycles/byte\n",
		cycles / 8, cycles / (8 * blen));

	return 0;
}

static void test_ahash_speed_common(const char *algo, unsigned int secs,
		unsigned int dsize, struct hash_speed *speed, unsigned mask)
{
	struct scatterlist sg[TVMEMSIZE];
	struct crypto_wait wait;
	struct ahash_request *req;
	struct crypto_ahash *tfm;
	unsigned char key[32];
	unsigned int keylen = 32;
	char *output;
	int i, ret;

	tfm = crypto_alloc_ahash(algo, 0, mask);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n",
			algo, PTR_ERR(tfm));
		return;
	}

	pr_info("\ntesting speed of async %s (%s)\n", algo,
			get_driver_name(crypto_ahash, tfm));

	if (crypto_ahash_digestsize(tfm) > MAX_DIGEST_SIZE) {
		pr_err("digestsize(%u) > %d\n", crypto_ahash_digestsize(tfm),
			MAX_DIGEST_SIZE);
		goto out;
	}

	test_hash_sg_init(sg, dsize);

	if (strncmp(algo, "cmac(aes)", 9) == 0) {
		pr_info("set key for cmac(aes)\n");
		crypto_ahash_clear_flags(tfm, ~0);
		memset(key, 0x0a, keylen);
		ret = crypto_ahash_setkey(tfm, key, keylen);
		if (ret) {
			pr_err("setkey failed for %s:ret=%d\n",
				algo, ret);
			goto out;
		}
	}

	req = ahash_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("ahash request allocation failure\n");
		goto out;
	}

	crypto_init_wait(&wait);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
					crypto_req_done, &wait);

	output = kmalloc(MAX_DIGEST_SIZE, GFP_KERNEL);
	if (!output)
		goto out_nomem;

	for (i = 0; (!dsize && speed[i].blen != 0); i++) {
		if (speed[i].blen > TVMEMSIZE * PAGE_SIZE) {
			pr_err("template (%u) too big for tvmem (%lu)\n",
				speed[i].blen, TVMEMSIZE * PAGE_SIZE);
			break;
		}

		if (klen)
			crypto_ahash_setkey(tfm, tvmem[0], klen);

		pr_info("test%3u "
			"(%5u byte blocks,%5u bytes per update,%4u updates): ",
			i, speed[i].blen, speed[i].plen,
			speed[i].blen / speed[i].plen);

		ahash_request_set_crypt(req, sg, output, speed[i].plen);

		if (secs) {
			ret = test_ahash_jiffies(req, speed[i].blen,
						speed[i].plen, output, secs);
			cond_resched();
		} else {
			ret = test_ahash_cycles(req, speed[i].blen,
						speed[i].plen, output);
		}

		if (ret) {
			pr_err("hashing failed ret=%d\n", ret);
			break;
		}
	}

	if (dsize) {
		ahash_request_set_crypt(req, &sg[0], output, dsize);
		ret = test_ahash_perf(req, dsize);
		if (ret)
			pr_err("hashing failed ret=%d\n", ret);
	}

	kfree(output);

out_nomem:
	ahash_request_free(req);

out:
	crypto_free_ahash(tfm);
}

static void test_ahash_speed(const char *algo, unsigned int secs,
				unsigned int dsize, struct hash_speed *speed)
{
	return test_ahash_speed_common(algo, secs, dsize, speed, 0);
}

static void test_hash_speed(const char *algo, unsigned int secs,
			    struct hash_speed *speed)
{
	return test_ahash_speed_common(algo, secs, 0, speed, CRYPTO_ALG_ASYNC);
}

struct test_mb_skcipher_data {
	struct scatterlist sg[XBUFSIZE];
	struct skcipher_request *req;
	struct crypto_wait wait;
	char *xbuf[XBUFSIZE];
};

static int do_mult_acipher_op(struct test_mb_skcipher_data *data, int enc,
				u32 num_mb, int *rc)
{
	int i, err = 0;

	/* Fire up a bunch of concurrent requests */
	for (i = 0; i < num_mb; i++) {
		if (enc == ENCRYPT)
			rc[i] = crypto_skcipher_encrypt(data[i].req);
		else
			rc[i] = crypto_skcipher_decrypt(data[i].req);
	}

	/* Wait for all requests to finish */
	for (i = 0; i < num_mb; i++) {
		rc[i] = crypto_wait_req(rc[i], &data[i].wait);

		if (rc[i]) {
			pr_info("concurrent request %d error %d\n", i, rc[i]);
			err = rc[i];
		}
	}

	return err;
}

static int test_mb_acipher_jiffies(struct test_mb_skcipher_data *data, int enc,
				int blen, int secs, u32 num_mb)
{
	unsigned long start, end;
	int bcount;
	int ret = 0;
	int *rc;

	rc = kcalloc(num_mb, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
	     time_before(jiffies, end); bcount++) {
		ret = do_mult_acipher_op(data, enc, num_mb, rc);
		if (ret)
			goto out;
	}

	pr_cont("%d operations in %d seconds (%llu bytes)\n",
		bcount * num_mb, secs, (u64)bcount * blen * num_mb);

out:
	kfree(rc);
	return ret;
}

static int test_mb_acipher_cycles(struct test_mb_skcipher_data *data, int enc,
			       int blen, u32 num_mb)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;
	int *rc;

	rc = kcalloc(num_mb, sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return -ENOMEM;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		ret = do_mult_acipher_op(data, enc, num_mb, rc);
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		ret = do_mult_acipher_op(data, enc, num_mb, rc);
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

	pr_cont("1 operation in %lu cycles (%d bytes)\n",
		(cycles + 4) / (8 * num_mb), blen);

out:
	kfree(rc);
	return ret;
}

static void test_mb_skcipher_speed(const char *algo, int enc, int secs,
				   struct cipher_speed_template *template,
				   unsigned int tcount, u8 *keysize, u32 num_mb)
{
	struct test_mb_skcipher_data *data;
	struct crypto_skcipher *tfm;
	unsigned int i, j, iv_len;
	const char *key;
	const char *e;
	u32 *b_size;
	char iv[128];
	int ret;

	if (enc == ENCRYPT)
		e = "encryption";
	else
		e = "decryption";

	data = kcalloc(num_mb, sizeof(*data), GFP_KERNEL);
	if (!data)
		return;

	tfm = crypto_alloc_skcipher(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n",
			algo, PTR_ERR(tfm));
		goto out_free_data;
	}

	for (i = 0; i < num_mb; ++i)
		if (testmgr_alloc_buf(data[i].xbuf)) {
			while (i--)
				testmgr_free_buf(data[i].xbuf);
			goto out_free_tfm;
		}

	for (i = 0; i < num_mb; ++i) {
		data[i].req = skcipher_request_alloc(tfm, GFP_KERNEL);
		if (!data[i].req) {
			pr_err("alg: skcipher: Failed to allocate request for %s\n",
			       algo);
			while (i--)
				skcipher_request_free(data[i].req);
			goto out_free_xbuf;
		}
	}

	for (i = 0; i < num_mb; ++i) {
		skcipher_request_set_callback(data[i].req,
					      CRYPTO_TFM_REQ_MAY_BACKLOG,
					      crypto_req_done, &data[i].wait);
		crypto_init_wait(&data[i].wait);
	}

	pr_info("\ntesting speed of multibuffer %s (%s) %s\n", algo,
		get_driver_name(crypto_skcipher, tfm), e);

	i = 0;
	do {
		b_size = block_sizes;
		do {
			if (*b_size > XBUFSIZE * PAGE_SIZE) {
				pr_err("template (%u) too big for buffer (%lu)\n",
				       *b_size, XBUFSIZE * PAGE_SIZE);
				goto out;
			}

			pr_info("test %u (%d bit key, %d byte blocks): ", i,
				*keysize * 8, *b_size);

			/* Set up tfm global state, i.e. the key */

			memset(tvmem[0], 0xff, PAGE_SIZE);
			key = tvmem[0];
			for (j = 0; j < tcount; j++) {
				if (template[j].klen == *keysize) {
					key = template[j].key;
					break;
				}
			}

			crypto_skcipher_clear_flags(tfm, ~0);

			ret = crypto_skcipher_setkey(tfm, key, *keysize);
			if (ret) {
				pr_err("setkey() failed flags=%x\n",
				       crypto_skcipher_get_flags(tfm));
				goto out;
			}

			iv_len = crypto_skcipher_ivsize(tfm);
			if (iv_len)
				memset(&iv, 0xff, iv_len);

			/* Now setup per request stuff, i.e. buffers */

			for (j = 0; j < num_mb; ++j) {
				struct test_mb_skcipher_data *cur = &data[j];
				unsigned int k = *b_size;
				unsigned int pages = DIV_ROUND_UP(k, PAGE_SIZE);
				unsigned int p = 0;

				sg_init_table(cur->sg, pages);

				while (k > PAGE_SIZE) {
					sg_set_buf(cur->sg + p, cur->xbuf[p],
						   PAGE_SIZE);
					memset(cur->xbuf[p], 0xff, PAGE_SIZE);
					p++;
					k -= PAGE_SIZE;
				}

				sg_set_buf(cur->sg + p, cur->xbuf[p], k);
				memset(cur->xbuf[p], 0xff, k);

				skcipher_request_set_crypt(cur->req, cur->sg,
							   cur->sg, *b_size,
							   iv);
			}

			if (secs) {
				ret = test_mb_acipher_jiffies(data, enc,
							      *b_size, secs,
							      num_mb);
				cond_resched();
			} else {
				ret = test_mb_acipher_cycles(data, enc,
							     *b_size, num_mb);
			}

			if (ret) {
				pr_err("%s() failed flags=%x\n", e,
				       crypto_skcipher_get_flags(tfm));
				break;
			}
			b_size++;
			i++;
		} while (*b_size);
		keysize++;
	} while (*keysize);

out:
	for (i = 0; i < num_mb; ++i)
		skcipher_request_free(data[i].req);
out_free_xbuf:
	for (i = 0; i < num_mb; ++i)
		testmgr_free_buf(data[i].xbuf);
out_free_tfm:
	crypto_free_skcipher(tfm);
out_free_data:
	kfree(data);
}

static inline int do_one_acipher_op(struct skcipher_request *req, int ret)
{
	struct crypto_wait *wait = req->base.data;

	return crypto_wait_req(ret, wait);
}

static int test_acipher_jiffies(struct skcipher_request *req, int enc,
				int blen, int secs)
{
	unsigned long start, end;
	int bcount;
	int ret;

	for (start = jiffies, end = start + secs * HZ, bcount = 0;
		time_before(jiffies, end); bcount++) {
		if (enc)
			ret = do_one_acipher_op(req,
						crypto_skcipher_encrypt(req));
		else
			ret = do_one_acipher_op(req,
						crypto_skcipher_decrypt(req));

		if (ret)
			return ret;
	}

	pr_cont("%d operations in %d seconds (%llu bytes)\n",
		bcount, secs, (u64)bcount * blen);
	return 0;
}

static int test_acipher_cycles(struct skcipher_request *req, int enc,
			       int blen)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		if (enc)
			ret = do_one_acipher_op(req,
						crypto_skcipher_encrypt(req));
		else
			ret = do_one_acipher_op(req,
						crypto_skcipher_decrypt(req));

		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (enc)
			ret = do_one_acipher_op(req,
						crypto_skcipher_encrypt(req));
		else
			ret = do_one_acipher_op(req,
						crypto_skcipher_decrypt(req));
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

out:
	if (ret == 0)
		pr_cont("1 operation in %lu cycles (%d bytes)\n",
			(cycles + 4) / 8, blen);

	return ret;
}

#define CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_AMOUNT (32*512)
#define CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_SIZE (1024/2)
#define CUSTOMIZED_ACIPHER_SPEED_TEST_TOTAL_BYTES \
		(CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_AMOUNT * \
		CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_SIZE)
#define CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE 16
#define CUSTOMIZED_ACIPHER_SPEED_TEST_MAX_OUTSTANDING_BLOCKS 1024
#define CUSTOMIZED_ACIPHER_SPEED_TEST_NO_RUNS 5
#define CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_ENCRYPT_SPEED 280
#define CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_DECRYPT_SPEED 300

#define MAX_AESBUF_TIMEOUT_FACTOR	200
#define WAIT_UDELAY			300

static atomic_t atomic_counter;

struct customized_tcrypt_result {
	u8 iv[CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE];
	u8 *block;
	int index;
	struct completion completion;
	struct completion restart;
	struct skcipher_request *req;
	struct scatterlist sg;
	int err;
};

static void customized_tcrypt_complete(struct crypto_async_request *req,
					int err)
{
	struct customized_tcrypt_result *res = req->data;

	if (err == -EINPROGRESS) {
		complete(&res->restart);
		return;
	}

	res->err = err;
	atomic_add(1, &atomic_counter);
	skcipher_request_free(res->req);
	atomic_set(&tcrypt_aes_buf[res->index], 1);
}

static unsigned int customized_blocks[] = {
		1024 / 2,
		1024,
		1024 * 2,
		1024 * 4,
		1024 * 8,
		1024 * 16,
		1024 * 32,
		1024 * 64
};

static unsigned int acipher_speed(const char *algo, int enc,
				unsigned int bsize, unsigned int bcnt)
{
	unsigned int ret, k, perf = 0;
	const char *e;
	struct crypto_skcipher *tfm;
	u8 keysize = CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE;
	u32 blocksize = customized_blocks[bsize];
	char key[64] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa,
		0xb, 0xc, 0xd, 0xe, 0xf, 0xf, 0xe, 0xd, 0xc, 0xb, 0xa, 0x9, 0x8,
		0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0,
			 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa,
		0xb, 0xc, 0xd, 0xe, 0xf, 0xf, 0xe, 0xd, 0xc, 0xb, 0xa, 0x9, 0x8,
		0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0 };
	unsigned long before_a, after_a, diff_in_ms;
	unsigned long blocks_to_test =
		CUSTOMIZED_ACIPHER_SPEED_TEST_BLOCK_AMOUNT * bcnt;
	unsigned long bytes_tested = blocks_to_test * blocksize;
	unsigned long bytes_per_ms = 0;
	u32 val = 0;
	u32 npages_per_block = ((blocksize / PAGE_SIZE) + 1);
	unsigned long pages;
	u32 nalloc = MAX_PAGE_ALLOC / npages_per_block;
	int index = 0;
	unsigned long aes_buf_addr[nalloc];

	if (!strcmp(algo, "xts(aes)"))
		keysize = keysize * 2;

	pages = __get_free_pages(GFP_KERNEL, MAX_PAGE_ORDER);
	if (!pages) {
		pr_err("aes pages allocation failed for %s\n", algo);
		return -ENOMEM;
	}

	for (k = 0; k < nalloc; k++) {
		aes_buf_addr[k] = pages + (k * (npages_per_block) * PAGE_SIZE);
		atomic_set(&tcrypt_aes_buf[k], 1);
	}

	for (k = nalloc; k < MAX_PAGE_ALLOC; k++)
		atomic_set(&tcrypt_aes_buf[k], 0);

	if (enc == ENCRYPT) {
		e = "encryption";
		pr_info("Testing Encryption\n");
	} else {
		e = "decryption";
		pr_info("Testing Decryption\n");
	}

	tfm = crypto_alloc_skcipher(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n", algo,
				PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}

	pr_info("testing speed of async %s (%s) %s\n", algo,
			get_driver_name(crypto_skcipher, tfm), e);
	pr_info("testing  (%d bit key, %d byte blocks)\n",
			keysize * 8, blocksize);

	crypto_skcipher_clear_flags(tfm, ~0);

	ret = crypto_skcipher_setkey(tfm, key, keysize);
	if (ret) {
		pr_err("setkey() failed flags=%x\n",
				crypto_skcipher_get_flags(tfm));
		goto out;
	}

	atomic_set(&atomic_counter, 0);
	before_a = ktime_get_real_fast_ns();

	for (k = 0; k < blocks_to_test; k++) {
		struct skcipher_request *req;
		int i = index + 1, j;
		struct customized_tcrypt_result *tresult;
		struct scatterlist *sg;
		u8 *block, *iv;
		u8 *alloc_addr = NULL;

		for (j = 0; j < (MAX_AESBUF_TIMEOUT_FACTOR * nalloc); j++, i++) {
				i = i % nalloc;
				if (atomic_read(&tcrypt_aes_buf[i])) {
					alloc_addr = (u8 *)aes_buf_addr[i];
					index = i;
					atomic_set(&tcrypt_aes_buf[i], 0);
					break;
				}
				if (j % nalloc == 0)
					udelay(WAIT_UDELAY);
		}

		if (!alloc_addr) {
			pr_err("alloc_addr for aes buffer not available\n");
			return -ENOMEM;
		}

		tresult = (struct customized_tcrypt_result *)
						(alloc_addr + blocksize);
		if (!tresult) {
			pr_err("out of memory?\n");
			goto out;
		}
		tresult->block = alloc_addr;
		tresult->index = index;

		init_completion(&tresult->completion);
		init_completion(&tresult->restart);

		req = skcipher_request_alloc(tfm, GFP_KERNEL);
		if (!req) {
			pr_err(
			"tcrypt: skcipher:Failed to allocate request for %s\n",
				algo);
			goto out;
		}

		skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			customized_tcrypt_complete, tresult);
		tresult->req = req;

		sg = &tresult->sg;
		block = tresult->block;
		iv = tresult->iv;
		sg_init_table(sg, 1);
		sg_set_buf(sg, block, blocksize);

		memset(iv, k % CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE,
			CUSTOMIZED_ACIPHER_SPEED_TEST_KEY_SIZE);

		skcipher_request_set_crypt(req, sg, sg, blocksize, iv);
		if (enc)
			ret = crypto_skcipher_encrypt(req);
		else
			ret = crypto_skcipher_decrypt(req);

		switch (ret) {
		/* async */
		case -EBUSY:
			wait_for_completion_interruptible(&tresult->restart);
			reinit_completion(&tresult->restart);
			break;
		case -EINPROGRESS:
			break;
			/* sync */
		case 0:
			customized_tcrypt_complete(&req->base, 0);
			break;
			/* error */
		default:
			pr_err("error detected\n");
			return ret;
		}
	}

	while (val < blocks_to_test)
		val = atomic_read(&atomic_counter);

	after_a = ktime_get_real_fast_ns();

	free_pages(pages, MAX_PAGE_ORDER);

	diff_in_ms = (after_a - before_a) / 1000000;

	pr_info("difference: %ld(ms)\n", diff_in_ms);
	pr_info("bytes tested: %ldMB %ldKB %ldB\n",
		bytes_tested / 1024 / 1024, (bytes_tested / 1024) % 1024,
		bytes_tested % 1024);

	bytes_per_ms = bytes_tested / diff_in_ms;
	perf = (bytes_per_ms * 1000) / (1024 * 1024);
	pr_info("Test speed: %ld.%03ld(MB/s)\n",
		(bytes_per_ms * 1000) / (1024 * 1024),
		((bytes_per_ms * 1000) / 1024) % 1024);
out:
	crypto_free_skcipher(tfm);

	return perf;
}

static int customized_test_acipher_speed(const char *algo, unsigned int bsize,
		unsigned int bcnt, unsigned int enc_target, unsigned int dec_target)
{
	int i, no_runs, target_enc_speed, target_dec_speed;
	int max_enc_speed = 0, max_dec_speed = 0, speed;

	no_runs = CUSTOMIZED_ACIPHER_SPEED_TEST_NO_RUNS;

	if (enc_target)
		target_enc_speed = enc_target;
	else
		target_enc_speed = CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_ENCRYPT_SPEED;

	if (dec_target)
		target_dec_speed = dec_target;
	else
		target_dec_speed = CUSTOMIZED_ACIPHER_SPEED_TEST_TARGET_DECRYPT_SPEED;

	for (i = 0; i < no_runs; i++) {
		speed = acipher_speed(algo, ENCRYPT, bsize, bcnt);
		if (speed < 0)
			return 1;
		if (max_enc_speed < speed)
			max_enc_speed = speed;
		speed = acipher_speed(algo, DECRYPT, bsize, bcnt);
		if (speed < 0)
			return 1;
		if (max_dec_speed < speed)
			max_dec_speed = speed;
	}

	pr_info("Target Encrypt speed: %d(MB/s) Decrypt speed: %d(MB/s)\n",
		target_enc_speed, target_dec_speed);
	pr_info("Test Encrypt speed: %d(MB/s) Decrypt speed: %d(MB/s)\n",
		max_enc_speed, max_dec_speed);

	if (max_enc_speed >= target_enc_speed &&
		       max_dec_speed >= target_dec_speed)
		return 0;
	else {
		pr_err("AES Encrypt/Decrypt target performance is not met\n");
		return 1;
	}
}

static void test_skcipher_speed(const char *algo, int enc, unsigned int secs,
				struct cipher_speed_template *template,
				unsigned int tcount, u8 *keysize, bool async)
{
	unsigned int ret, i, j, k, iv_len;
	struct crypto_wait wait;
	const char *key;
	char iv[128];
	struct skcipher_request *req;
	struct crypto_skcipher *tfm;
	const char *e;
	u32 *b_size;

	if (enc == ENCRYPT)
		e = "encryption";
	else
		e = "decryption";

	crypto_init_wait(&wait);

	tfm = crypto_alloc_skcipher(algo, 0, async ? 0 : CRYPTO_ALG_ASYNC);

	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n", algo,
			PTR_ERR(tfm));
		return;
	}

	pr_info("\ntesting speed of %s %s (%s) %s\n", async ? "async" : "sync",
		algo, get_driver_name(crypto_skcipher, tfm), e);

	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("tcrypt: skcipher: Failed to allocate request for %s\n",
			algo);
		goto out;
	}

	skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      crypto_req_done, &wait);

	i = 0;
	do {
		b_size = block_sizes;

		do {
			struct scatterlist sg[TVMEMSIZE];

			if ((*keysize + *b_size) > TVMEMSIZE * PAGE_SIZE) {
				pr_err(
				"template (%u) too big for tvmem (%lu)\n",
					*keysize + *b_size,
					TVMEMSIZE * PAGE_SIZE);
				goto out_free_req;
			}

			pr_info("test %u (%d bit key, %d byte blocks): ", i,
				*keysize * 8, *b_size);

			memset(tvmem[0], 0xff, PAGE_SIZE);

			/* set key, plain text and IV */
			key = tvmem[0];
			for (j = 0; j < tcount; j++) {
				if (template[j].klen == *keysize) {
					key = template[j].key;
					break;
				}
			}

			crypto_skcipher_clear_flags(tfm, ~0);

			ret = crypto_skcipher_setkey(tfm, key, *keysize);
			if (ret) {
				pr_err("setkey() failed flags=%x\n",
					crypto_skcipher_get_flags(tfm));
				goto out_free_req;
			}

			k = *keysize + *b_size;
			sg_init_table(sg, DIV_ROUND_UP(k, PAGE_SIZE));

			if (k > PAGE_SIZE) {
				sg_set_buf(sg, tvmem[0] + *keysize,
					PAGE_SIZE - *keysize);
				k -= PAGE_SIZE;
				j = 1;
				while (k > PAGE_SIZE) {
					sg_set_buf(sg + j, tvmem[j], PAGE_SIZE);
					memset(tvmem[j], 0xff, PAGE_SIZE);
					j++;
					k -= PAGE_SIZE;
				}
				sg_set_buf(sg + j, tvmem[j], k);
				memset(tvmem[j], 0xff, k);
			} else {
				sg_set_buf(sg, tvmem[0] + *keysize, *b_size);
			}

			iv_len = crypto_skcipher_ivsize(tfm);
			if (iv_len)
				memset(&iv, 0xff, iv_len);

			skcipher_request_set_crypt(req, sg, sg, *b_size, iv);

			if (secs) {
				ret = test_acipher_jiffies(req, enc,
							*b_size, secs);
				cond_resched();
			} else {
				ret = test_acipher_cycles(req, enc,
							*b_size);
			}

			if (ret) {
				pr_err("%s() failed flags=%x\n", e,
				       crypto_skcipher_get_flags(tfm));
				break;
			}
			b_size++;
			i++;
		} while (*b_size);
		keysize++;
	} while (*keysize);

out_free_req:
	skcipher_request_free(req);
out:
	crypto_free_skcipher(tfm);
}

static void test_acipher_speed(const char *algo, int enc, unsigned int secs,
			       struct cipher_speed_template *template,
			       unsigned int tcount, u8 *keysize)
{
	return test_skcipher_speed(algo, enc, secs, template, tcount, keysize,
				   true);
}

static void test_cipher_speed(const char *algo, int enc, unsigned int secs,
			      struct cipher_speed_template *template,
			      unsigned int tcount, u8 *keysize)
{
	return test_skcipher_speed(algo, enc, secs, template, tcount, keysize,
				   false);
}

static int test_akcipher_jiffies(struct akcipher_request *r, int op, int secs)
{
	unsigned long start, end;
	int count, ret;

	for (start = jiffies, end = start + secs * HZ, count = 0;
	     time_before(jiffies, end); count++) {

		switch (op) {
		case SIGN:
			ret = crypto_wait_req(crypto_akcipher_sign(r), r->base.data);
			break;
		case VERIFY:
			ret = crypto_wait_req(crypto_akcipher_verify(r), r->base.data);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret)
			return ret;
	}

	pr_info("%d operations in %d seconds\n", count, secs);
	return 0;
}

static int test_akcipher_cycles(struct akcipher_request *r, int op)
{
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		switch (op) {
		case SIGN:
			ret = crypto_wait_req(crypto_akcipher_sign(r), r->base.data);
			break;
		case VERIFY:
			ret = crypto_wait_req(crypto_akcipher_verify(r), r->base.data);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		switch (op) {
		case SIGN:
			ret = crypto_wait_req(crypto_akcipher_sign(r), r->base.data);
			break;
		case VERIFY:
			ret = crypto_wait_req(crypto_akcipher_verify(r), r->base.data);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}
out:
	if (ret == 0)
		pr_info("1 operation in %lu cycles\n", (cycles + 4) / 8);

	return ret;
}

static void test_akcipher_speed(const char *algo, int op, unsigned int secs,
				struct akcipher_speed_template *template,
				unsigned int tcount, u8 *keysize)
{
	unsigned int ret, i, j;
	struct crypto_wait wait;
	const char *key;
	struct akcipher_request *req;
	struct crypto_akcipher *tfm;
	unsigned int m_size = 0;
	unsigned int nbytes = 0;
	const char *o;

	if (op == SIGN)
		o = "sign";
	else if (op == VERIFY)
		o = "verify";
	else
		return;

	tfm = crypto_alloc_akcipher(algo, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("failed to load transform for %s: %ld\n", algo,
		       PTR_ERR(tfm));
		return;
	}

	req = akcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		pr_err("tcrypt: akcipher: Failed to allocate request for %s\n",
		       algo);
		goto out;
	}

	crypto_init_wait(&wait);
	akcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      crypto_req_done, &wait);

	i = 0;
	do {
		struct scatterlist sg[TVMEMSIZE];

		memset(tvmem[0], 0xff, PAGE_SIZE);

		/* set key */
		key = tvmem[0];
		for (j = 0; j < tcount; j++) {
			if (template[j].key_len == *keysize) {
				key = template[j].key;
				break;
			}
		}

		ret = crypto_akcipher_set_pub_key(tfm, key, *keysize);
		if (ret) {
			pr_err("set_pub_key() failed\n");
			goto out_free_req;
		}

		ret = crypto_akcipher_set_priv_key(tfm, key, *keysize);
		if (ret) {
			pr_err("set_priv_key() failed\n");
			goto out_free_req;
		}

		/* set up src/dst buffs */
		sg_init_table(sg, TVMEMSIZE);
		if (op == SIGN) {
			m_size = template[j].m_size;
			nbytes = template[j].c_size / 3;

			memcpy(tvmem[0], template[j].m, m_size);

			sg_set_buf(&sg[0], tvmem[0], m_size);
			akcipher_request_set_crypt(req, sg, sg,
						   m_size, PAGE_SIZE);
		} else if (op == VERIFY) {
			m_size = template[j].m_size;
			nbytes = template[j].c_size / 3;

			memcpy(tvmem[0], template[j].m, m_size);
			memcpy(tvmem[1], (u8 *)(template[j].c) + nbytes,
			       nbytes);
			memcpy(tvmem[2], (u8 *)(template[j].c) + 2 * nbytes,
			       nbytes);

			sg_set_buf(&sg[0], tvmem[0], m_size);
			sg_set_buf(&sg[1], tvmem[1], nbytes);
			sg_set_buf(&sg[2], tvmem[2], nbytes);

			akcipher_request_set_crypt(req, sg, sg,
						   m_size + 2 * nbytes,
						   PAGE_SIZE);
		} else {
			pr_err("invalid op\n");
			ret = -EINVAL;
			goto out_free_req;
		}


		pr_info("\ntesting speed of %s (%s) %s with keysize %d\n",
			algo, get_driver_name(crypto_akcipher, tfm), o,
			nbytes * 8);

		if (secs)
			ret = test_akcipher_jiffies(req, op, secs);
		else
			ret = test_akcipher_cycles(req, op);

		if (ret) {
			pr_err("%s() failed\n", o);
			break;
		}

		i++;
		keysize++;

	} while (*keysize);

out_free_req:
	akcipher_request_free(req);
out:
	crypto_free_akcipher(tfm);
}

static void test_available(void)
{
	const char **name = check;

	while (*name) {
		pr_info("alg %s %s", *name,
			crypto_has_alg(*name, 0, 0) ?
			"found\n" : "not found\n");
		name++;
	}
}

static inline int tcrypt_test(const char *alg)
{
	int ret;

	pr_debug("testing %s\n", alg);

	ret = alg_test(alg, alg, 0, 0);
	/* non-fips algs return -EINVAL in fips mode */
	if (fips_enabled && ret == -EINVAL)
		ret = 0;
	return ret;
}

static int do_test(const char *alg, u32 type, u32 mask, int m, u32 num_mb)
{
	int i;
	int ret = 0;

	switch (m) {
	case 0:
		if (alg) {
			if (!crypto_has_alg(alg, type,
					    mask ?: CRYPTO_ALG_TYPE_MASK))
				ret = -ENOENT;
			break;
		}

		for (i = 1; i < 200; i++)
			ret += do_test(NULL, 0, 0, i, num_mb);
		break;

	case 1:
		ret += tcrypt_test("md5");
		break;

	case 2:
		ret += tcrypt_test("sha1");
		break;

	case 3:
		ret += tcrypt_test("ecb(des)");
		ret += tcrypt_test("cbc(des)");
		ret += tcrypt_test("ctr(des)");
		break;

	case 4:
		ret += tcrypt_test("ecb(des3_ede)");
		ret += tcrypt_test("cbc(des3_ede)");
		ret += tcrypt_test("ctr(des3_ede)");
		break;

	case 5:
		ret += tcrypt_test("md4");
		break;

	case 6:
		ret += tcrypt_test("sha256");
		break;

	case 7:
		ret += tcrypt_test("ecb(blowfish)");
		ret += tcrypt_test("cbc(blowfish)");
		ret += tcrypt_test("ctr(blowfish)");
		break;

	case 8:
		ret += tcrypt_test("ecb(twofish)");
		ret += tcrypt_test("cbc(twofish)");
		ret += tcrypt_test("ctr(twofish)");
		ret += tcrypt_test("lrw(twofish)");
		ret += tcrypt_test("xts(twofish)");
		break;

	case 9:
		ret += tcrypt_test("ecb(serpent)");
		ret += tcrypt_test("cbc(serpent)");
		ret += tcrypt_test("ctr(serpent)");
		ret += tcrypt_test("lrw(serpent)");
		ret += tcrypt_test("xts(serpent)");
		break;

	case 10:
		ret += tcrypt_test("ecb(aes)");
		ret += tcrypt_test("cbc(aes)");
		ret += tcrypt_test("ctr(aes)");
		break;

	case 11:
		ret += tcrypt_test("sha384");
		break;

	case 12:
		ret += tcrypt_test("sha512");
		break;

	case 13:
		ret += tcrypt_test("deflate");
		break;

	case 14:
		ret += tcrypt_test("ecb(cast5)");
		ret += tcrypt_test("cbc(cast5)");
		ret += tcrypt_test("ctr(cast5)");
		break;

	case 15:
		ret += tcrypt_test("ecb(cast6)");
		ret += tcrypt_test("cbc(cast6)");
		ret += tcrypt_test("ctr(cast6)");
		ret += tcrypt_test("lrw(cast6)");
		ret += tcrypt_test("xts(cast6)");
		break;

	case 16:
		ret += tcrypt_test("ecb(arc4)");
		break;

	case 17:
		ret += tcrypt_test("michael_mic");
		break;

	case 18:
		ret += tcrypt_test("crc32c");
		break;

	case 19:
		ret += tcrypt_test("ecb(tea)");
		break;

	case 20:
		ret += tcrypt_test("ecb(xtea)");
		break;

	case 21:
		ret += tcrypt_test("ecb(khazad)");
		break;

	case 22:
		ret += tcrypt_test("wp512");
		break;

	case 23:
		ret += tcrypt_test("wp384");
		break;

	case 24:
		ret += tcrypt_test("wp256");
		break;

	case 25:
		ret += tcrypt_test("ecb(tnepres)");
		break;

	case 26:
		ret += tcrypt_test("ecb(anubis)");
		ret += tcrypt_test("cbc(anubis)");
		break;

	case 27:
		ret += tcrypt_test("tgr192");
		break;

	case 28:
		ret += tcrypt_test("tgr160");
		break;

	case 29:
		ret += tcrypt_test("tgr128");
		break;

	case 30:
		ret += tcrypt_test("ecb(xeta)");
		break;

	case 31:
		ret += tcrypt_test("pcbc(fcrypt)");
		break;

	case 32:
		ret += tcrypt_test("ecb(camellia)");
		ret += tcrypt_test("cbc(camellia)");
		ret += tcrypt_test("ctr(camellia)");
		ret += tcrypt_test("lrw(camellia)");
		ret += tcrypt_test("xts(camellia)");
		break;

	case 33:
		ret += tcrypt_test("sha224");
		break;

	case 34:
		ret += tcrypt_test("salsa20");
		break;

	case 35:
		ret += tcrypt_test("gcm(aes)");
		break;

	case 36:
		ret += tcrypt_test("lzo");
		break;

	case 37:
		ret += tcrypt_test("ccm(aes)");
		break;

	case 38:
		ret += tcrypt_test("cts(cbc(aes))");
		break;

	case 39:
		ret += tcrypt_test("rmd128");
		break;

	case 40:
		ret += tcrypt_test("rmd160");
		break;

	case 41:
		ret += tcrypt_test("rmd256");
		break;

	case 42:
		ret += tcrypt_test("rmd320");
		break;

	case 43:
		ret += tcrypt_test("ecb(seed)");
		break;

	case 45:
		ret += tcrypt_test("rfc4309(ccm(aes))");
		break;

	case 46:
		ret += tcrypt_test("ghash");
		break;

	case 47:
		ret += tcrypt_test("crct10dif");
		break;

	case 48:
		ret += tcrypt_test("sha3-224");
		break;

	case 49:
		ret += tcrypt_test("sha3-256");
		break;

	case 50:
		ret += tcrypt_test("sha3-384");
		break;

	case 51:
		ret += tcrypt_test("sha3-512");
		break;

	case 52:
		ret += tcrypt_test("sm3");
		break;

	case 53:
		ret += tcrypt_test("streebog256");
		break;

	case 54:
		ret += tcrypt_test("streebog512");
		break;

	case 55:
		ret += tcrypt_test("ecdh");
		break;

	case 56:
		ret += tcrypt_test("xts(aes)");
		break;

	case 57:
		ret += tcrypt_test("dh");
		break;

	case 58:
		ret += tcrypt_test("eddsa");
		break;

	case 59:
		ret += tcrypt_test("shake128");
		break;

	case 60:
		ret += tcrypt_test("shake256");
		break;

	case 100:
		ret += tcrypt_test("hmac(md5)");
		break;

	case 101:
		ret += tcrypt_test("hmac(sha1)");
		break;

	case 102:
		ret += tcrypt_test("hmac(sha256)");
		break;

	case 103:
		ret += tcrypt_test("hmac(sha384)");
		break;

	case 104:
		ret += tcrypt_test("hmac(sha512)");
		break;

	case 105:
		ret += tcrypt_test("hmac(sha224)");
		break;

	case 106:
		ret += tcrypt_test("xcbc(aes)");
		break;

	case 107:
		ret += tcrypt_test("hmac(rmd128)");
		break;

	case 108:
		ret += tcrypt_test("hmac(rmd160)");
		break;

	case 109:
		ret += tcrypt_test("vmac64(aes)");
		break;

	case 111:
		ret += tcrypt_test("hmac(sha3-224)");
		break;

	case 112:
		ret += tcrypt_test("hmac(sha3-256)");
		break;

	case 113:
		ret += tcrypt_test("hmac(sha3-384)");
		break;

	case 114:
		ret += tcrypt_test("hmac(sha3-512)");
		break;

	case 115:
		ret += tcrypt_test("hmac(streebog256)");
		break;

	case 116:
		ret += tcrypt_test("hmac(streebog512)");
		break;

	case 149:
		ret += tcrypt_test("rng_drbg");
		break;

	case 150:
		ret += tcrypt_test("ansi_cprng");
		break;

	case 151:
		ret += tcrypt_test("rfc4106(gcm(aes))");
		break;

	case 152:
		ret += tcrypt_test("rfc4543(gcm(aes))");
		break;

	case 153:
		ret += tcrypt_test("cmac(aes)");
		break;

	case 154:
		ret += tcrypt_test("cmac(des3_ede)");
		break;

	case 155:
		ret += tcrypt_test("authenc(hmac(sha1),cbc(aes))");
		break;

	case 156:
		ret += tcrypt_test("authenc(hmac(md5),ecb(cipher_null))");
		break;

	case 157:
		ret += tcrypt_test("authenc(hmac(sha1),ecb(cipher_null))");
		break;
	case 181:
		ret += tcrypt_test("authenc(hmac(sha1),cbc(des))");
		break;
	case 182:
		ret += tcrypt_test("authenc(hmac(sha1),cbc(des3_ede))");
		break;
	case 183:
		ret += tcrypt_test("authenc(hmac(sha224),cbc(des))");
		break;
	case 184:
		ret += tcrypt_test("authenc(hmac(sha224),cbc(des3_ede))");
		break;
	case 185:
		ret += tcrypt_test("authenc(hmac(sha256),cbc(des))");
		break;
	case 186:
		ret += tcrypt_test("authenc(hmac(sha256),cbc(des3_ede))");
		break;
	case 187:
		ret += tcrypt_test("authenc(hmac(sha384),cbc(des))");
		break;
	case 188:
		ret += tcrypt_test("authenc(hmac(sha384),cbc(des3_ede))");
		break;
	case 189:
		ret += tcrypt_test("authenc(hmac(sha512),cbc(des))");
		break;
	case 190:
		ret += tcrypt_test("authenc(hmac(sha512),cbc(des3_ede))");
		break;
	case 191:
		ret += tcrypt_test("ecb(sm4)");
		ret += tcrypt_test("cbc(sm4)");
		ret += tcrypt_test("ctr(sm4)");
		break;
	case 200:
		test_cipher_speed("ecb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ecb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("lrw(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("lrw(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("xts(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_cipher_speed("xts(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_cipher_speed("cts(cbc(aes))", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cts(cbc(aes))", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cfb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cfb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		break;

	case 201:
		test_cipher_speed("ecb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("ecb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("cbc(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("cbc(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("ctr(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_cipher_speed("ctr(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		break;

	case 202:
		test_cipher_speed("ecb(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ecb(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("lrw(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("lrw(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("xts(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_cipher_speed("xts(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		break;

	case 203:
		test_cipher_speed("ecb(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("ecb(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("cbc(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("cbc(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("ctr(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_cipher_speed("ctr(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		break;

	case 204:
		test_cipher_speed("ecb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_cipher_speed("ecb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_cipher_speed("cbc(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_cipher_speed("cbc(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 205:
		test_cipher_speed("ecb(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ecb(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("cbc(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("ctr(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_cipher_speed("lrw(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("lrw(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_cipher_speed("xts(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_cipher_speed("xts(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		break;

	case 206:
		test_cipher_speed("salsa20", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		break;

	case 207:
		test_cipher_speed("ecb(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ecb(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("lrw(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("lrw(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("xts(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_cipher_speed("xts(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 208:
		test_cipher_speed("ecb(arc4)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 209:
		test_cipher_speed("ecb(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("ecb(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("cbc(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("cbc(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("ctr(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_cipher_speed("ctr(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		break;

	case 210:
		test_cipher_speed("ecb(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ecb(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("cbc(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("ctr(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_cipher_speed("lrw(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("lrw(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_cipher_speed("xts(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_cipher_speed("xts(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 211:
		test_aead_speed("rfc4106(gcm(aes))", ENCRYPT, sec,
				NULL, 0, 16, 16, aead_speed_template_20);
		test_aead_speed("gcm(aes)", ENCRYPT, sec,
				NULL, 0, 16, 8, speed_template_16_24_32);
		test_aead_speed("rfc4106(gcm(aes))", DECRYPT, sec,
				NULL, 0, 16, 16, aead_speed_template_20);
		test_aead_speed("gcm(aes)", DECRYPT, sec,
				NULL, 0, 16, 8, speed_template_16_24_32);
		break;

	case 212:
		test_aead_speed("rfc4309(ccm(aes))", ENCRYPT, sec,
				NULL, 0, 16, 16, aead_speed_template_19);
		test_aead_speed("rfc4309(ccm(aes))", DECRYPT, sec,
				NULL, 0, 16, 16, aead_speed_template_19);
		break;

	case 213:
		test_aead_speed("rfc7539esp(chacha20,poly1305)", ENCRYPT, sec,
				NULL, 0, 16, 8, aead_speed_template_36);
		test_aead_speed("rfc7539esp(chacha20,poly1305)", DECRYPT, sec,
				NULL, 0, 16, 8, aead_speed_template_36);
		break;

	case 214:
		test_cipher_speed("chacha20", ENCRYPT, sec, NULL, 0,
				  speed_template_32);
		break;

	case 215:
		test_mb_aead_speed("rfc4106(gcm(aes))", ENCRYPT, sec, NULL,
				   0, 16, 16, aead_speed_template_20, num_mb);
		test_mb_aead_speed("gcm(aes)", ENCRYPT, sec, NULL, 0, 16, 8,
				   speed_template_16_24_32, num_mb);
		test_mb_aead_speed("rfc4106(gcm(aes))", DECRYPT, sec, NULL,
				   0, 16, 16, aead_speed_template_20, num_mb);
		test_mb_aead_speed("gcm(aes)", DECRYPT, sec, NULL, 0, 16, 8,
				   speed_template_16_24_32, num_mb);
		break;

	case 216:
		test_mb_aead_speed("rfc4309(ccm(aes))", ENCRYPT, sec, NULL, 0,
				   16, 16, aead_speed_template_19, num_mb);
		test_mb_aead_speed("rfc4309(ccm(aes))", DECRYPT, sec, NULL, 0,
				   16, 16, aead_speed_template_19, num_mb);
		break;

	case 217:
		test_mb_aead_speed("rfc7539esp(chacha20,poly1305)", ENCRYPT,
				   sec, NULL, 0, 16, 8, aead_speed_template_36,
				   num_mb);
		test_mb_aead_speed("rfc7539esp(chacha20,poly1305)", DECRYPT,
				   sec, NULL, 0, 16, 8, aead_speed_template_36,
				   num_mb);
		break;

	case 218:
		test_cipher_speed("ecb(sm4)", ENCRYPT, sec, NULL, 0,
				speed_template_16);
		test_cipher_speed("ecb(sm4)", DECRYPT, sec, NULL, 0,
				speed_template_16);
		test_cipher_speed("cbc(sm4)", ENCRYPT, sec, NULL, 0,
				speed_template_16);
		test_cipher_speed("cbc(sm4)", DECRYPT, sec, NULL, 0,
				speed_template_16);
		test_cipher_speed("ctr(sm4)", ENCRYPT, sec, NULL, 0,
				speed_template_16);
		test_cipher_speed("ctr(sm4)", DECRYPT, sec, NULL, 0,
				speed_template_16);
		break;

	case 219:
		test_cipher_speed("adiantum(xchacha12,aes)", ENCRYPT, sec, NULL,
				  0, speed_template_32);
		test_cipher_speed("adiantum(xchacha12,aes)", DECRYPT, sec, NULL,
				  0, speed_template_32);
		test_cipher_speed("adiantum(xchacha20,aes)", ENCRYPT, sec, NULL,
				  0, speed_template_32);
		test_cipher_speed("adiantum(xchacha20,aes)", DECRYPT, sec, NULL,
				  0, speed_template_32);
		break;

	case 220:
		test_acipher_speed("essiv(cbc(aes),sha256)",
				  ENCRYPT, sec, NULL, 0,
				  speed_template_16_24_32);
		test_acipher_speed("essiv(cbc(aes),sha256)",
				  DECRYPT, sec, NULL, 0,
				  speed_template_16_24_32);
		break;

	case 221:
		test_aead_speed("aegis128", ENCRYPT, sec,
				NULL, 0, 16, 8, speed_template_16);
		test_aead_speed("aegis128", DECRYPT, sec,
				NULL, 0, 16, 8, speed_template_16);
		break;

	case 300:
		if (alg) {
			test_hash_speed(alg, sec, generic_hash_speed_template);
			break;
		}
		fallthrough;
	case 301:
		test_hash_speed("md4", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 302:
		test_hash_speed("md5", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 303:
		test_hash_speed("sha1", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 304:
		test_hash_speed("sha256", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 305:
		test_hash_speed("sha384", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 306:
		test_hash_speed("sha512", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 307:
		test_hash_speed("wp256", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 308:
		test_hash_speed("wp384", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 309:
		test_hash_speed("wp512", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 310:
		test_hash_speed("tgr128", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 311:
		test_hash_speed("tgr160", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 312:
		test_hash_speed("tgr192", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 313:
		test_hash_speed("sha224", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 314:
		test_hash_speed("rmd128", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 315:
		test_hash_speed("rmd160", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 316:
		test_hash_speed("rmd256", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 317:
		test_hash_speed("rmd320", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 318:
		klen = 16;
		test_hash_speed("ghash", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 319:
		test_hash_speed("crc32c", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 320:
		test_hash_speed("crct10dif", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 321:
		test_hash_speed("poly1305", sec, poly1305_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 322:
		test_hash_speed("sha3-224", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 323:
		test_hash_speed("sha3-256", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 324:
		test_hash_speed("sha3-384", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 325:
		test_hash_speed("sha3-512", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 326:
		test_hash_speed("sm3", sec, generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 327:
		test_hash_speed("streebog256", sec,
				generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 328:
		test_hash_speed("streebog512", sec,
				generic_hash_speed_template);
		if (mode > 300 && mode < 400) break;
		fallthrough;
	case 399:
		break;

	case 400:
		if (alg) {
			test_ahash_speed(alg, sec, dsize,
				generic_hash_speed_template);
			break;
		}
		fallthrough;
	case 401:
		test_ahash_speed("md4", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 402:
		test_ahash_speed("md5", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 403:
		test_ahash_speed("sha1", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 404:
		test_ahash_speed("sha256", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 405:
		test_ahash_speed("sha384", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 406:
		test_ahash_speed("sha512", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 407:
		test_ahash_speed("wp256", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 408:
		test_ahash_speed("wp384", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 409:
		test_ahash_speed("wp512", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 410:
		test_ahash_speed("tgr128", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 411:
		test_ahash_speed("tgr160", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 412:
		test_ahash_speed("tgr192", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 413:
		test_ahash_speed("sha224", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 414:
		test_ahash_speed("rmd128", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 415:
		test_ahash_speed("rmd160", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 416:
		test_ahash_speed("rmd256", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 417:
		test_ahash_speed("rmd320", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 418:
		test_ahash_speed("sha3-224", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 419:
		test_ahash_speed("sha3-256", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 420:
		test_ahash_speed("sha3-384", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 421:
		test_ahash_speed("sha3-512", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 422:
		test_mb_ahash_speed("sha1", sec, generic_hash_speed_template,
				    num_mb);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 423:
		test_mb_ahash_speed("sha256", sec, generic_hash_speed_template,
				    num_mb);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 424:
		test_mb_ahash_speed("sha512", sec, generic_hash_speed_template,
				    num_mb);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 425:
		test_mb_ahash_speed("sm3", sec, generic_hash_speed_template,
				    num_mb);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 426:
		test_mb_ahash_speed("streebog256", sec,
				    generic_hash_speed_template, num_mb);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 427:
		test_mb_ahash_speed("streebog512", sec,
				    generic_hash_speed_template, num_mb);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 428:
		test_ahash_speed("cmac(aes)", sec, dsize,
			generic_hash_speed_template);
		if (mode > 400 && mode < 500) break;
		fallthrough;
	case 499:
		break;

	case 500:
		test_acipher_speed("ecb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ecb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("lrw(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("lrw(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("xts(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("xts(aes)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("cts(cbc(aes))", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cts(cbc(aes))", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ctr(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ctr(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cfb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cfb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ofb(aes)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ofb(aes)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("rfc3686(ctr(aes))", ENCRYPT, sec, NULL, 0,
				speed_template_20_28_36);
		test_acipher_speed("rfc3686(ctr(aes))", DECRYPT, sec, NULL, 0,
				speed_template_20_28_36);
		break;

	case 501:
		test_acipher_speed("ecb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("ecb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cbc(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cbc(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cfb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("cfb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("ofb(des3_ede)", ENCRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		test_acipher_speed("ofb(des3_ede)", DECRYPT, sec,
				des3_speed_template, DES3_SPEED_VECTORS,
				speed_template_24);
		break;

	case 502:
		test_acipher_speed("ecb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("ecb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cbc(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cbc(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cfb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("cfb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("ofb(des)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		test_acipher_speed("ofb(des)", DECRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 503:
		test_acipher_speed("ecb(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ecb(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("lrw(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("lrw(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("xts(serpent)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("xts(serpent)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 504:
		test_acipher_speed("ecb(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ecb(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("cbc(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ctr(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("ctr(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_16_24_32);
		test_acipher_speed("lrw(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("lrw(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_40_48);
		test_acipher_speed("xts(twofish)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		test_acipher_speed("xts(twofish)", DECRYPT, sec, NULL, 0,
				speed_template_32_48_64);
		break;

	case 505:
		test_acipher_speed("ecb(arc4)", ENCRYPT, sec, NULL, 0,
				speed_template_8);
		break;

	case 506:
		test_acipher_speed("ecb(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("ecb(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("cbc(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("cbc(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("ctr(cast5)", ENCRYPT, sec, NULL, 0,
				speed_template_8_16);
		test_acipher_speed("ctr(cast5)", DECRYPT, sec, NULL, 0,
				speed_template_8_16);
		break;

	case 507:
		test_acipher_speed("ecb(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ecb(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("lrw(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("lrw(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("xts(cast6)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("xts(cast6)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 508:
		test_acipher_speed("ecb(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ecb(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("cbc(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("ctr(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_16_32);
		test_acipher_speed("lrw(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("lrw(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_48);
		test_acipher_speed("xts(camellia)", ENCRYPT, sec, NULL, 0,
				speed_template_32_64);
		test_acipher_speed("xts(camellia)", DECRYPT, sec, NULL, 0,
				speed_template_32_64);
		break;

	case 509:
		test_acipher_speed("ecb(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("ecb(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("cbc(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("cbc(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("ctr(blowfish)", ENCRYPT, sec, NULL, 0,
				speed_template_8_32);
		test_acipher_speed("ctr(blowfish)", DECRYPT, sec, NULL, 0,
				speed_template_8_32);
		break;

	case 555:
		if (customized_test_acipher_speed("cbc(aes)", bsize, bcnt,
			enc_target, dec_target))
			return -EIO;
		break;

	case 556:
		if (customized_test_acipher_speed("xts(aes)", bsize, bcnt,
			enc_target, dec_target))
			return -EIO;
		break;

	case 557:
		if (customized_test_acipher_speed("ctr(aes)", bsize, bcnt,
			enc_target, dec_target))
			return -EIO;
		break;

	case 558:
		if (customized_test_acipher_speed("ecb(aes)", bsize, bcnt,
			enc_target, dec_target))
			return -EIO;
		break;

	case 559:
		if (customized_test_acipher_speed("gcm(aes)", bsize, bcnt,
			enc_target, dec_target))
			return -EIO;
		break;

	case 560:
		ret += tcrypt_test("ecdsa");
		break;

	case 561:
#ifndef CONFIG_CRYPTO_FIPS
		test_akcipher_speed("ecdsa", SIGN, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P192);
		test_akcipher_speed("ecdsa", VERIFY, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P192);
#endif
		test_akcipher_speed("ecdsa", SIGN, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P256);
		test_akcipher_speed("ecdsa", VERIFY, sec,
				    ecdsa_speed_template, ECDSA_SPEED_VECTORS,
				    akc_speed_template_P256);
		break;

	case 600:
		test_mb_skcipher_speed("ecb(aes)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ecb(aes)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("cbc(aes)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("cbc(aes)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("lrw(aes)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_40_48, num_mb);
		test_mb_skcipher_speed("lrw(aes)", DECRYPT, sec, NULL, 0,
				       speed_template_32_40_48, num_mb);
		test_mb_skcipher_speed("xts(aes)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		test_mb_skcipher_speed("xts(aes)", DECRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		test_mb_skcipher_speed("cts(cbc(aes))", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("cts(cbc(aes))", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ctr(aes)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ctr(aes)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("cfb(aes)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("cfb(aes)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ofb(aes)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ofb(aes)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("rfc3686(ctr(aes))", ENCRYPT, sec, NULL,
				       0, speed_template_20_28_36, num_mb);
		test_mb_skcipher_speed("rfc3686(ctr(aes))", DECRYPT, sec, NULL,
				       0, speed_template_20_28_36, num_mb);
		break;

	case 601:
		test_mb_skcipher_speed("ecb(des3_ede)", ENCRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		test_mb_skcipher_speed("ecb(des3_ede)", DECRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		test_mb_skcipher_speed("cbc(des3_ede)", ENCRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		test_mb_skcipher_speed("cbc(des3_ede)", DECRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		test_mb_skcipher_speed("cfb(des3_ede)", ENCRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		test_mb_skcipher_speed("cfb(des3_ede)", DECRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		test_mb_skcipher_speed("ofb(des3_ede)", ENCRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		test_mb_skcipher_speed("ofb(des3_ede)", DECRYPT, sec,
				       des3_speed_template, DES3_SPEED_VECTORS,
				       speed_template_24, num_mb);
		break;

	case 602:
		test_mb_skcipher_speed("ecb(des)", ENCRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		test_mb_skcipher_speed("ecb(des)", DECRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		test_mb_skcipher_speed("cbc(des)", ENCRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		test_mb_skcipher_speed("cbc(des)", DECRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		test_mb_skcipher_speed("cfb(des)", ENCRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		test_mb_skcipher_speed("cfb(des)", DECRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		test_mb_skcipher_speed("ofb(des)", ENCRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		test_mb_skcipher_speed("ofb(des)", DECRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		break;

	case 603:
		test_mb_skcipher_speed("ecb(serpent)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ecb(serpent)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("cbc(serpent)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("cbc(serpent)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ctr(serpent)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ctr(serpent)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("lrw(serpent)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_48, num_mb);
		test_mb_skcipher_speed("lrw(serpent)", DECRYPT, sec, NULL, 0,
				       speed_template_32_48, num_mb);
		test_mb_skcipher_speed("xts(serpent)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		test_mb_skcipher_speed("xts(serpent)", DECRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		break;

	case 604:
		test_mb_skcipher_speed("ecb(twofish)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ecb(twofish)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("cbc(twofish)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("cbc(twofish)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ctr(twofish)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("ctr(twofish)", DECRYPT, sec, NULL, 0,
				       speed_template_16_24_32, num_mb);
		test_mb_skcipher_speed("lrw(twofish)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_40_48, num_mb);
		test_mb_skcipher_speed("lrw(twofish)", DECRYPT, sec, NULL, 0,
				       speed_template_32_40_48, num_mb);
		test_mb_skcipher_speed("xts(twofish)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_48_64, num_mb);
		test_mb_skcipher_speed("xts(twofish)", DECRYPT, sec, NULL, 0,
				       speed_template_32_48_64, num_mb);
		break;

	case 605:
		test_mb_skcipher_speed("ecb(arc4)", ENCRYPT, sec, NULL, 0,
				       speed_template_8, num_mb);
		break;

	case 606:
		test_mb_skcipher_speed("ecb(cast5)", ENCRYPT, sec, NULL, 0,
				       speed_template_8_16, num_mb);
		test_mb_skcipher_speed("ecb(cast5)", DECRYPT, sec, NULL, 0,
				       speed_template_8_16, num_mb);
		test_mb_skcipher_speed("cbc(cast5)", ENCRYPT, sec, NULL, 0,
				       speed_template_8_16, num_mb);
		test_mb_skcipher_speed("cbc(cast5)", DECRYPT, sec, NULL, 0,
				       speed_template_8_16, num_mb);
		test_mb_skcipher_speed("ctr(cast5)", ENCRYPT, sec, NULL, 0,
				       speed_template_8_16, num_mb);
		test_mb_skcipher_speed("ctr(cast5)", DECRYPT, sec, NULL, 0,
				       speed_template_8_16, num_mb);
		break;

	case 607:
		test_mb_skcipher_speed("ecb(cast6)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ecb(cast6)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("cbc(cast6)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("cbc(cast6)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ctr(cast6)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ctr(cast6)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("lrw(cast6)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_48, num_mb);
		test_mb_skcipher_speed("lrw(cast6)", DECRYPT, sec, NULL, 0,
				       speed_template_32_48, num_mb);
		test_mb_skcipher_speed("xts(cast6)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		test_mb_skcipher_speed("xts(cast6)", DECRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		break;

	case 608:
		test_mb_skcipher_speed("ecb(camellia)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ecb(camellia)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("cbc(camellia)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("cbc(camellia)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ctr(camellia)", ENCRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("ctr(camellia)", DECRYPT, sec, NULL, 0,
				       speed_template_16_32, num_mb);
		test_mb_skcipher_speed("lrw(camellia)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_48, num_mb);
		test_mb_skcipher_speed("lrw(camellia)", DECRYPT, sec, NULL, 0,
				       speed_template_32_48, num_mb);
		test_mb_skcipher_speed("xts(camellia)", ENCRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		test_mb_skcipher_speed("xts(camellia)", DECRYPT, sec, NULL, 0,
				       speed_template_32_64, num_mb);
		break;

	case 609:
		test_mb_skcipher_speed("ecb(blowfish)", ENCRYPT, sec, NULL, 0,
				       speed_template_8_32, num_mb);
		test_mb_skcipher_speed("ecb(blowfish)", DECRYPT, sec, NULL, 0,
				       speed_template_8_32, num_mb);
		test_mb_skcipher_speed("cbc(blowfish)", ENCRYPT, sec, NULL, 0,
				       speed_template_8_32, num_mb);
		test_mb_skcipher_speed("cbc(blowfish)", DECRYPT, sec, NULL, 0,
				       speed_template_8_32, num_mb);
		test_mb_skcipher_speed("ctr(blowfish)", ENCRYPT, sec, NULL, 0,
				       speed_template_8_32, num_mb);
		test_mb_skcipher_speed("ctr(blowfish)", DECRYPT, sec, NULL, 0,
				       speed_template_8_32, num_mb);
		break;

	case 1000:
		test_available();
		break;
	}

	return ret;
}

static int __init tcrypt_mod_init(void)
{
	int err = -ENOMEM;
	int i;

	if (dsize) {
		tvmem[0] = kmalloc(dsize, GFP_KERNEL);
		if (!tvmem[0])
			goto err_free_tv;
	} else {
		for (i = 0; i < TVMEMSIZE; i++) {
			tvmem[i] = (void *)__get_free_page(GFP_KERNEL);
			if (!tvmem[i])
				goto err_free_tv;
		}
	}

	err = do_test(alg, type, mask, mode, num_mb);

	if (err) {
		pr_info("tcrypt: one or more tests failed!\n");
		goto err_free_tv;
	} else {
		pr_debug("all tests passed\n");
	}

err_free_tv:
	if (dsize &&  tvmem[0]) {
		kfree(tvmem[0]);
	} else {
		for (i = 0; i < TVMEMSIZE && tvmem[i]; i++)
			free_page((unsigned long)tvmem[i]);
	}
	return err;
}

/*
 * If an init function is provided, an exit function must also be provided
 * to allow module unload.
 */
static void __exit tcrypt_mod_fini(void) { }

subsys_initcall(tcrypt_mod_init);
module_exit(tcrypt_mod_fini);

module_param(alg, charp, 0);
module_param(type, uint, 0);
module_param(mask, uint, 0);
module_param(mode, int, 0);
module_param(sec, uint, 0);
module_param(dsize, ulong, 0);
module_param(bsize, uint, 0);
module_param(bcnt, uint, 0);
module_param(enc_target, uint, 0);
module_param(dec_target, uint, 0);
/* When this parameter (sec) is not supplied,
 * it calculates in CPU cycles instead
*/
MODULE_PARM_DESC(sec, "Length in seconds of speed tests");
module_param(num_mb, uint, 0000);
MODULE_PARM_DESC(num_mb, "Number of concurrent requests to be used in mb speed tests (defaults to 8)");
module_param(klen, uint, 0);
MODULE_PARM_DESC(klen, "Key length (defaults to 0)");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Quick & dirty crypto testing module");
MODULE_AUTHOR("James Morris <jmorris@intercode.com.au>");
