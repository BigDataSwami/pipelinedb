/* Copyright (c) 2013-2015 PipelineDB */

#include "postgres.h"

#include <openssl/aes.h>
#include <sys/time.h>
#include <stdio.h>
#include "postmaster/license.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

const unsigned char pipeline_key[] = {0x54, 0x48, 0x45, 0x50, 0x49, 0x50, 0x45, 0x4c, 0x49, 0x4e, 0x45, 0x4b, 0x45, 0x59, 0x30, 0x30};

typedef struct LicenseKey
{
	int start_time;
	int end_time;
} LicenseKey;

/*
 * Decode the license key into shmem do we only need to do it once
 */
static void
decode_license_key(LicenseKey *key, bool fail)
{
	char in[1024];
	char out[1024];
	char tmp[11];
	char *iv;
	char *payload;
	int ctlen;
	int payloadlen;
	int colon;
	AES_KEY wctx;
	int level = fail ? ERROR : WARNING;

	memset(out, 0, 1024);

	ctlen = hex_decode(license_key, strlen(license_key), in);

	payloadlen = ctlen - 16;
	iv = pnstrdup(in, 16);
	payload = pnstrdup(in + 16, payloadlen);

	AES_set_decrypt_key(pipeline_key, 128, &wctx);
	AES_cbc_encrypt((unsigned char *) payload, (unsigned char *) out, payloadlen, &wctx, (unsigned char *) iv, AES_DECRYPT);

	/* time start */
	colon = strchr(out, ':') - out;
	if (colon != 10)
	{
		elog(level, "invalid license key found in pipelinedb.conf");
		return;
	}

	strncpy(tmp, out, colon);
	tmp[colon] = '\0';

	key->start_time = atoi(tmp);

	/* time end */
	strncpy(tmp, out + colon + 1, 10);
	tmp[10] = '\0';

	key->end_time = atoi(tmp);
}

/*
 * CheckLicense
 *
 * Check if the license key is still valid based on the current time.
 *
 * XXX(derekjn) Ideally this should call out to an external license server
 */
void
CheckLicense(bool fail) {
	struct timeval tv;
	int current_time;
	int level = fail ? ERROR : WARNING;
	LicenseKey *key;
	bool found;

	if (!license_key)
	{
		elog(level, "license_key not set in pipelinedb.conf");
		return;
	}

	gettimeofday(&tv, NULL);

	current_time = tv.tv_sec;

	key = ShmemInitStruct("LicenseKey", sizeof(LicenseKey), &found);
	if (!found)
		decode_license_key(key, fail);

	if (current_time > key->end_time)
	{
		elog(level, "license key is expired");
		return;
	}
}
