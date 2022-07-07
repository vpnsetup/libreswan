/* mechanisms for preshared keys (public, private, and preshared secrets)
 * definitions: lib/libswan/secrets.c
 *
 * Copyright (C) 1998-2002,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2016 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#ifndef _SECRETS_H
#define _SECRETS_H

#include <pk11pub.h>

#include "lswcdefs.h"
#include "x509.h"
#include "id.h"
#include "err.h"
#include "realtime.h"
#include "ckaid.h"
#include "diag.h"
#include "keyid.h"
#include "refcnt.h"
#include "crypt_mac.h"
#include "ike_alg.h"		/* for HASH_ALGORITHM_IDENTIFIER */

struct logger;
struct state;	/* forward declaration */
struct secret;	/* opaque definition, private to secrets.c */
struct pubkey;		/* forward */
union pubkey_content;	/* forward */
struct pubkey_type;	/* forward */
struct hash_desc;
struct cert;

struct RSA_public_key {
	/* public: */
	chunk_t n;	/* modulus: p * q */
	chunk_t e;	/* exponent: relatively prime to (p-1) * (q-1) [probably small] */
};

struct ECDSA_public_key {
	SECKEYPublicKey *seckey_public;
	chunk_t pub; /* includes EC_POINT_FORM_COMPRESSED */
};

union pubkey_content {
	struct RSA_public_key rsa;
	struct ECDSA_public_key ecdsa;
};

/*
 * private key types
 */
enum private_key_kind {
	/* start at one so accidental 0 will not match */
	PKK_PSK = 1,
	PKK_RSA,
	PKK_XAUTH,
	PKK_PPK,
	PKK_ECDSA, /* should not be needed */
	PKK_NULL,
	PKK_INVALID,
};

struct private_key_stuff {
	enum private_key_kind kind;
	/*
	 * This replaced "int lsw_secretlineno()", which assumes only
	 * one file (no includes) and isn't applicable to NSS.  For
	 * NSS it's the entry number.
	 */
	int line;
	union {
		chunk_t preshared_secret;
		union pubkey_content pubkey;
		/* struct smartcard *smartcard; */
	} u;

	chunk_t ppk;
	chunk_t ppk_id;
	/* for PKI */
	const struct pubkey_type *pubkey_type;
	SECKEYPrivateKey *private_key;
	size_t size;
	keyid_t keyid;

	/*
	 * NSS's(?) idea of a unique ID for a public private key pair.
	 * For RSA it is something like the SHA1 of the modulus.  It
	 * replaces KEYID.
	 *
	 * This is the value returned by
	 * PK11_GetLowLevelKeyIDForCert() or
	 * PK11_GetLowLevelKeyIDForPrivateKey() (see
	 * form_ckaid_nss()), or computed by brute force from the
	 * modulus (see form_ckaid_rsa()).
	 */
	ckaid_t ckaid;
};

diag_t private_key_stuff_to_der(struct private_key_stuff *pks, chunk_t *der);

extern struct private_key_stuff *lsw_get_pks(struct secret *s);
extern struct id_list *lsw_get_idlist(const struct secret *s);

/*
 * return 1 to continue to next,
 * return 0 to return current secret
 * return -1 to return NULL
 */
typedef int (*secret_eval)(struct secret *secret,
			   struct private_key_stuff *pks,
			   void *uservoid);

extern struct secret *lsw_foreach_secret(struct secret *secrets,
					 secret_eval func, void *uservoid);

struct hash_signature {
	size_t len;
	/*
	 * XXX: See https://tools.ietf.org/html/rfc4754#section-7 for
	 * where 1056 is coming from.
	 * It is the largest of the signature lengths amongst
	 * ECDSA 256, 384, and 521.
	 */
	uint8_t ptr[PMAX(RSA_MAX_OCTETS, BYTES_FOR_BITS(1056))];
};

struct pubkey_type {
	const char *name;
	enum pubkey_alg alg;
	enum private_key_kind private_key_kind;
	void (*free_pubkey_content)(union pubkey_content *pkc);
	/* to/from the blob in DNSKEY's Public Key field */
	err_t (*dnssec_pubkey_to_pubkey_content)(chunk_t dnskey_pubkey,
						 union pubkey_content *pkc,
						 keyid_t *keyid, ckaid_t *ckaid, size_t *size);
	err_t (*pubkey_content_to_dnssec_pubkey)(const union pubkey_content *pkc,
						 chunk_t *dnssec_pubkey);
	/* nss */
	void (*extract_pubkey_content)(union pubkey_content *pkc,
				       keyid_t *keyid, ckaid_t *ckaid, size_t *size,
				       SECKEYPublicKey *pubkey_nss, SECItem *ckaid_nss);
	void (*extract_private_key_pubkey_content)(struct private_key_stuff *pks,
						   keyid_t *keyid, ckaid_t *ckaid, size_t *size,
						   SECKEYPublicKey *pubk, SECItem *cert_ckaid);
	void (*free_secret_content)(struct private_key_stuff *pks);
	err_t (*secret_sane)(struct private_key_stuff *pks);
	const struct pubkey_signer *digital_signature_signer[DIGITAL_SIGNATURE_BLOB_ROOF];
};

struct pubkey_signer {
	const char *name;
	enum digital_signature_blob digital_signature_blob;
	const struct pubkey_type *type;
	struct hash_signature (*sign_hash)(const struct private_key_stuff *pks,
					   const uint8_t *hash_octets, size_t hash_len,
					   const struct hash_desc *hash_algo,
					   struct logger *logger);
	/*
	 * Danger! This function returns three results
	 *
	 * true;FATAL_DIAG=NULL: pubkey verified
	 * false;FATAL_DIAG=NULL: pubkey did not verify
	 * false;FATAL_DIAG!=NULL: operation should be aborted
	 */
	bool (*authenticate_signature)(const struct crypt_mac *hash,
				       shunk_t signature,
				       struct pubkey *kr,
				       const struct hash_desc *hash_algo,
				       diag_t *fatal_diag,
				       struct logger *logger);
};

extern const struct pubkey_type pubkey_type_rsa;
extern const struct pubkey_type pubkey_type_ecdsa;

extern const struct pubkey_signer pubkey_signer_raw_rsa;
extern const struct pubkey_signer pubkey_signer_pkcs1_1_5_rsa;
extern const struct pubkey_signer pubkey_signer_rsassa_pss;
extern const struct pubkey_signer pubkey_signer_ecdsa;

const struct pubkey_type *pubkey_alg_type(enum pubkey_alg alg);

/* public key machinery */
struct pubkey {
	refcnt_t refcnt;	/* reference counted! */
	struct id id;
	keyid_t keyid;	/* see ipsec_keyblobtoid(3) */
	ckaid_t ckaid;
	size_t size;
	enum dns_auth_level dns_auth_level;
	realtime_t installed_time;
	realtime_t until_time;
	uint32_t dns_ttl; /* from wire. until_time is derived using this */
	asn1_t issuer;
	const struct pubkey_type *type;
	union pubkey_content u;
	/* for overalloc of issuer */
	uint8_t end[0];
};

/*
 * XXX: While these fields seem to really belong in 'struct pubkey',
 * moving them isn't so easy - code assumes the fields are also found
 * in {RSA,ECDSA}_private_key's .pub.  Perhaps that structure have its
 * own copy.
 *
 * All pointers are references into the underlying PK structure.
 */

const ckaid_t *pubkey_ckaid(const struct pubkey *pk);
const keyid_t *pubkey_keyid(const struct pubkey *pk);
unsigned pubkey_size(const struct pubkey *pk);

const ckaid_t *secret_ckaid(const struct secret *);
const keyid_t *secret_keyid(const struct secret *);

struct pubkey_list {
	struct pubkey *key;
	struct pubkey_list *next;
};

extern struct pubkey_list *pubkeys;	/* keys from ipsec.conf */

extern struct pubkey_list *free_public_keyentry(struct pubkey_list *p);
extern void free_public_keys(struct pubkey_list **keys);
err_t unpack_dnssec_pubkey(const struct id *id, /* ASKK */
			   enum dns_auth_level dns_auth_level,
			   const struct pubkey_type *type,
			   realtime_t install_time, realtime_t until_time,
			   uint32_t ttl,
			   chunk_t dnssec_pubkey,
			   struct pubkey **pubkey,
			   struct pubkey_list **head);
void replace_public_key(struct pubkey_list **pubkey_db,
			struct pubkey **pk);
void delete_public_keys(struct pubkey_list **head,
			const struct id *id,
			const struct pubkey_type *type);
extern void form_keyid(chunk_t e, chunk_t n, keyid_t *keyid, size_t *keysize); /*XXX: make static? */

struct pubkey *pubkey_addref_where(struct pubkey *pk, where_t where);
#define pubkey_addref(PK) pubkey_addref_where(PK, HERE)
extern void pubkey_delref_where(struct pubkey **pkp, where_t where);
#define pubkey_delref(PKP) pubkey_delref_where(PKP, HERE)

extern bool same_RSA_public_key(const struct RSA_public_key *a,
				const struct RSA_public_key *b);

extern void lsw_load_preshared_secrets(struct secret **psecrets, const char *secrets_file,
				       struct logger *logger);
extern void lsw_free_preshared_secrets(struct secret **psecrets, struct logger *logger);

extern struct secret *lsw_find_secret_by_id(struct secret *secrets,
					    enum private_key_kind kind,
					    const struct id *my_id,
					    const struct id *his_id,
					    bool asym);

extern struct secret *lsw_get_ppk_by_id(struct secret *secrets, chunk_t ppk_id);

/* err_t!=NULL -> neither found nor loaded; loaded->just pulled in */
err_t find_or_load_private_key_by_cert(struct secret **secrets, const struct cert *cert,
				       const struct private_key_stuff **pks, bool *load_needed,
				       struct logger *logger);
err_t find_or_load_private_key_by_ckaid(struct secret **secrets, const ckaid_t *ckaid,
					const struct private_key_stuff **pks, bool *load_needed,
					struct logger *logger);

diag_t create_pubkey_from_cert(const struct id *id,
			       CERTCertificate *cert, struct pubkey **pk,
			       struct logger *logger) MUST_USE_RESULT;

#endif /* _SECRETS_H */
