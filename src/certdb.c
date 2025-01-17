// SPDX-License-Identifier: GPLv2
/*
 * certdb.c - helpers to manage the EFI security databases
 * Copyright Peter Jones <pjones@redhat.com>
 * Copyright Red Hat, Inc.
 */
#include "fix_coverity.h"

#include <fcntl.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <nss.h>
#include <prerror.h>
#include <cert.h>
#include <pkcs7t.h>
#include <pk11pub.h>

#include "pesigcheck.h"

static int
add_db_file(pesigcheck_context *ctx, db_specifier which, const char *dbfile,
	    db_f_type type)
{
	dblist *db = calloc(1, sizeof (dblist));
	int errno_guard;

	if (!db)
		return -1;

	db->type = type;
	db->fd = open(dbfile, O_RDONLY);
	set_errno_guard_with_override(&errno_guard);
	if (db->fd < 0) {
		free(db);
		return -1;
	}

	char *path = strdup(dbfile);
	if (!path) {
		override_errno_guard(&errno_guard, errno);
		free(db);
		return -1;
	}

	db->path = basename(path);
	db->path = strdup(db->path);
	free(path);
	if (!db->path) {
		override_errno_guard(&errno_guard, errno);
		close(db->fd);
		free(db);
		return -1;
	}

	struct stat sb;
	int rc = fstat(db->fd, &sb);
	if (rc < 0) {
		override_errno_guard(&errno_guard, errno);
		close(db->fd);
		free(db->path);
		free(db);
		return -1;
	}
	db->size = sb.st_size;

	db->map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, db->fd, 0);
	if (db->map == MAP_FAILED) {
		db->map = NULL;
		size_t sz = 0;
		rc = read_file(db->fd, (char **)&db->map, &sz);
		if (rc < 0) {
			override_errno_guard(&errno_guard, errno);
			close(db->fd);
			free(db->path);
			free(db);
			return -1;
		}
	}

	EFI_SIGNATURE_LIST *certlist;
	EFI_SIGNATURE_DATA *cert;
	efi_guid_t efi_x509 = efi_guid_x509_cert;

	switch (type) {
	case DB_FILE:
		db->data = db->map;
		db->datalen = db->size;
		break;
	case DB_EFIVAR:
		/* skip the first 4 bytes (EFI attributes) */
		db->data = db->map + 4;
		db->datalen = db->size - 4;
		break;
	case DB_CERT:
		db->datalen = db->size + sizeof(EFI_SIGNATURE_LIST) +
			      sizeof(efi_guid_t);
		db->data = calloc(1, db->datalen);
		if (!db->data) {
			override_errno_guard(&errno_guard, errno);
			return -1;
		}

		certlist = (EFI_SIGNATURE_LIST *)db->data;
		memcpy((void *)&certlist->SignatureType, &efi_x509, sizeof(efi_guid_t));
		certlist->SignatureListSize = db->datalen;
		certlist->SignatureHeaderSize = 0;
		certlist->SignatureSize = db->size + sizeof(efi_guid_t);

		cert = (EFI_SIGNATURE_DATA *)(db->data + sizeof(EFI_SIGNATURE_LIST));
		memcpy((void *)cert->SignatureData, db->map, db->size);
		break;
	default:
		break;
	}

	dblist **tmp = which == DB ? &ctx->db : &ctx->dbx;

	db->next = *tmp;
	*tmp = db;

	override_errno_guard(&errno_guard, 0);
	return 0;
}

int
add_cert_db(pesigcheck_context *ctx, const char *filename)
{
	return add_db_file(ctx, DB, filename, DB_FILE);
}

int
add_cert_dbx(pesigcheck_context *ctx, const char *filename)
{
	return add_db_file(ctx, DBX, filename, DB_FILE);
}

int
add_cert_file(pesigcheck_context *ctx, const char *filename)
{
	return add_db_file(ctx, DB, filename, DB_CERT);
}

#define DB_PATH "/sys/firmware/efi/efivars/db-d719b2cb-3d3a-4596-a3bc-dad00e67656f"
#define MOK_PATH "/sys/firmware/efi/efivars/MokListRT-605dab50-e046-4300-abb6-3dd810dd8b23"
#define DBX_PATH "/sys/firmware/efi/efivars/dbx-d719b2cb-3d3a-4596-a3bc-dad00e67656f"
#define MOKX_PATH "/sys/firmware/efi/efivars/MokListXRT-605dab50-e046-4300-abb6-3dd810dd8b23"

void
init_cert_db(pesigcheck_context *ctx, int use_system_dbs)
{
	int rc = 0;

	if (!use_system_dbs)
		return;

	rc = add_db_file(ctx, DB, DB_PATH, DB_EFIVAR);
	if (rc < 0 && errno != ENOENT) {
		fprintf(stderr, "pesigcheck: Could not add key database "
			"\"%s\": %m\n", DB_PATH);
		exit(1);
	}

	rc = add_db_file(ctx, DB, MOK_PATH, DB_EFIVAR);
	if (rc < 0 && errno != ENOENT) {
		fprintf(stderr, "pesigcheck: Could not add key database "
			"\"%s\": %m\n", MOK_PATH);
		exit(1);
	}

	if (ctx->db == NULL) {
		fprintf(stderr, "pesigcheck: warning: "
			"No key database available\n");
	}

	rc = add_db_file(ctx, DBX, DBX_PATH, DB_EFIVAR);
	if (rc < 0 && errno != ENOENT) {
		fprintf(stderr, "pesigcheck: Could not add revocation "
			"database \"%s\": %m\n", DBX_PATH);
		exit(1);
	}

	rc = add_db_file(ctx, DBX, MOKX_PATH, DB_EFIVAR);
	if (rc < 0 && errno != ENOENT) {
		fprintf(stderr, "pesigcheck: Could not add key database "
			"\"%s\": %m\n", MOKX_PATH);
		exit(1);
	}

	if (ctx->dbx == NULL) {
		fprintf(stderr, "pesigcheck: warning: "
			"No key recovation database available\n");
	}
}

typedef db_status (*checkfn)(pesigcheck_context *ctx, SECItem *sig,
			     efi_guid_t *sigtype, SECItem *pkcs7sig);

static db_status
check_db(db_specifier which, pesigcheck_context *ctx, checkfn check,
	 void *data, ssize_t datalen, SECItem *match)
{
	SECItem pkcs7sig, sig;
	dblist *dbl = which == DB ? ctx->db : ctx->dbx;
	db_status found = NOT_FOUND;

	pkcs7sig.data = data;
	pkcs7sig.len = datalen;
	pkcs7sig.type = siBuffer;

	sig.type = siBuffer;

	while (dbl) {
		printf("Searching %s %s\n", which == DB ? "db" : "dbx",
		       dbl->path);
		EFI_SIGNATURE_LIST *certlist;
		EFI_SIGNATURE_DATA *cert;
		size_t dbsize = dbl->datalen;
		unsigned long certcount;

		certlist = dbl->data;
		while (dbsize > 0 && dbsize >= certlist->SignatureListSize) {
			certcount = (certlist->SignatureListSize -
				     certlist->SignatureHeaderSize)
				    / certlist->SignatureSize;
			cert = (EFI_SIGNATURE_DATA *)((uint8_t *)certlist +
				sizeof(EFI_SIGNATURE_LIST) +
				certlist->SignatureHeaderSize);

			for (unsigned int i = 0; i < certcount; i++) {
				sig.data = cert->SignatureData;
				sig.len = certlist->SignatureSize
					  - sizeof(efi_guid_t);
				found = check(ctx, &sig,
					      &certlist->SignatureType,
					      &pkcs7sig);
				if (found == FOUND) {
					if (match)
						memcpy(match, &sig,
						       sizeof(sig));
					return FOUND;
				}
				cert = (EFI_SIGNATURE_DATA *)((uint8_t *)cert +
				        certlist->SignatureSize);
			}

			dbsize -= certlist->SignatureListSize;
			certlist = (EFI_SIGNATURE_LIST *)((uint8_t *)certlist +
			            certlist->SignatureListSize);
		}
		dbl = dbl->next;
	}
	return NOT_FOUND;
}

static db_status
check_hash(pesigcheck_context *ctx, SECItem *sig, efi_guid_t *sigtype,
	   SECItem *pkcs7sig UNUSED)
{
	efi_guid_t efi_sha256 = efi_guid_sha256;
	efi_guid_t efi_sha1 = efi_guid_sha1;
	void *digest;

	if (memcmp(sigtype, &efi_sha256, sizeof(efi_guid_t)) == 0) {
		digest = ctx->cms_ctx->digests[0].pe_digest->data;
		if (memcmp (digest, sig->data, 32) == 0)
			return FOUND;
	} else if (memcmp(sigtype, &efi_sha1, sizeof(efi_guid_t)) == 0) {
		digest = ctx->cms_ctx->digests[1].pe_digest->data;
		if (memcmp (digest, sig->data, 20) == 0)
			return FOUND;
	}

	return NOT_FOUND;
}

db_status
check_db_hash(db_specifier which, pesigcheck_context *ctx)
{
	return check_db(which, ctx, check_hash, NULL, 0, NULL);
}

static void
find_cert_times(SEC_PKCS7ContentInfo *cinfo,
		PRTime *notBefore, PRTime *notAfter)
{
	CERTCertDBHandle *defaultdb, *certdb;
	SEC_PKCS7SignedData *sdp;
	CERTCertificate **certs = NULL;
	SECItem **rawcerts;
	int i, certcount;
	SECStatus rv;

	if (cinfo->contentTypeTag->offset != SEC_OID_PKCS7_SIGNED_DATA) {
err:
		*notBefore = 0;
		*notAfter = 0x7fffffffffffffff;
		return;
	}

	sdp = cinfo->content.signedData;
	rawcerts = sdp->rawCerts;

	defaultdb = CERT_GetDefaultCertDB();

	certdb = defaultdb;
	if (certdb == NULL)
		goto err;

	certcount = 0;
	if (rawcerts != NULL) {
		for (; rawcerts[certcount] != NULL; certcount++)
			;
	}
	rv = CERT_ImportCerts(certdb, certUsageObjectSigner, certcount,
			      rawcerts, &certs, PR_FALSE, PR_FALSE, NULL);
	if (rv != SECSuccess)
		goto err;

	for (i = 0; i < certcount; i++) {
		PRTime nb = 0, na = 0x7fffffffffff;
		CERT_GetCertTimes(certs[i], &nb, &na);
		if (*notBefore < nb)
			*notBefore = nb;
		if (*notAfter > na)
			*notAfter = na;
	}

	CERT_DestroyCertArray(certs, certcount);
}

static db_status
check_cert(pesigcheck_context *ctx, SECItem *sig, efi_guid_t *sigtype,
	   SECItem *pkcs7sig)
{
	SEC_PKCS7ContentInfo *cinfo = NULL;
	CERTCertificate *cert = NULL;
	CERTCertTrust trust;
	SECItem *content, *digest = NULL;
	PK11Context *pk11ctx = NULL;
	SECOidData *oid;
	PRBool result;
	SECStatus rv;
	db_status status = NOT_FOUND;
	PRTime atTime = PR_Now();
	SECItem *eTime;
	PRTime earlyNow = 0, lateNow = 0x7fffffffffffffff;
	PRTime notBefore, notAfter;

	efi_guid_t efi_x509 = efi_guid_x509_cert;

	if (memcmp(sigtype, &efi_x509, sizeof(efi_guid_t)) != 0)
		return NOT_FOUND;

	cinfo = SEC_PKCS7DecodeItem(pkcs7sig, NULL, NULL, NULL, NULL, NULL,
				    NULL, NULL);
	if (!cinfo)
		goto out;

	notBefore = earlyNow;
	notAfter = lateNow;
	find_cert_times(cinfo, &notBefore, &notAfter);
	if (earlyNow < notBefore)
		earlyNow = notBefore;
	if (lateNow > notAfter)
		lateNow = notAfter;

	// atTime = determine_reasonable_time(cert);
	eTime = SEC_PKCS7GetSigningTime(cinfo);
	if (eTime != NULL) {
		if (DER_DecodeTimeChoice (&atTime, eTime) == SECSuccess) {
			if (earlyNow < atTime)
				earlyNow = atTime;
			if (lateNow > atTime)
				lateNow = atTime;
		}
	}

	if (lateNow < earlyNow)
		printf("Signature has impossible time constraint: %lld <= %lld\n",
		       earlyNow / 1000000LL, lateNow / 1000000LL);
	atTime = earlyNow / 2 + lateNow / 2;

	cinfo = SEC_PKCS7DecodeItem(pkcs7sig, NULL, NULL, NULL, NULL, NULL,
				    NULL, NULL);
	if (!cinfo)
		goto out;

	/* Generate the digest of contentInfo */
	/* XXX support only sha256 for now */
	digest = SECITEM_AllocItem(NULL, NULL, 32);
	if (digest == NULL)
		goto out;

	content = cinfo->content.signedData->contentInfo.content.data;
	oid = SECOID_FindOIDByTag(SEC_OID_SHA256);
	if (oid == NULL)
		goto out;
	pk11ctx = PK11_CreateDigestContext(oid->offset);
	if (ctx == NULL)
		goto out;
	if (PK11_DigestBegin(pk11ctx) != SECSuccess)
		goto out;
	/*   Skip the SEQUENCE tag */
	if (PK11_DigestOp(pk11ctx, content->data + 2, content->len - 2) != SECSuccess)
		goto out;
	if (PK11_DigestFinal(pk11ctx, digest->data, &digest->len, 32) != SECSuccess)
		goto out;

	/* Import the trusted certificate */
	cert = CERT_NewTempCertificate(CERT_GetDefaultCertDB(), sig, "Temp CA",
				       PR_FALSE, PR_TRUE);
	if (!cert) {
		fprintf(stderr, "Unable to create cert: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		goto out;
	}

	rv = CERT_DecodeTrustString(&trust, ",,CP");
	if (rv != SECSuccess) {
		fprintf(stderr, "Unable to decode trust string: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		goto out;
	}

	rv = CERT_ChangeCertTrust(CERT_GetDefaultCertDB(), cert, &trust);
	if (rv != SECSuccess) {
		fprintf(stderr, "Failed to change cert trust: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		goto out;
	}

	/* Verify the signature */
	result = SEC_PKCS7VerifyDetachedSignatureAtTime(cinfo,
						certUsageObjectSigner,
						digest, HASH_AlgSHA256,
						PR_FALSE, atTime);
	if (!result) {
		fprintf(stderr, "%s\n",	PORT_ErrorToString(PORT_GetError()));
		goto out;
	}

	status = FOUND;
out:
	if (cinfo)
		SEC_PKCS7DestroyContentInfo(cinfo);
	if (cert)
		CERT_DestroyCertificate(cert);
	if (pk11ctx)
		PK11_DestroyContext(pk11ctx, PR_TRUE);
	if (digest)
		SECITEM_FreeItem(digest, PR_FALSE);

	return status;
}

db_status
check_db_cert(db_specifier which, pesigcheck_context *ctx,
	      void *data, ssize_t datalen, SECItem *match)
{
	return check_db(which, ctx, check_cert, data, datalen, match);
}
