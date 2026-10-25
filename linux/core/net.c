/*
 * Rufus for Linux: downloads of signed boot files
 * Copyright © 2012-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
 *
 * Boot files that are not embedded (other Syslinux or GRUB versions) are
 * fetched from the Rufus server, where each file comes with an RSA-2048
 * SHA-256 signature, which we validate against the same public key as
 * Windows Rufus.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

#include "internal.h"

#define RSA_SIGNATURE_SIZE  256
#define MAX_DOWNLOAD_SIZE   (64 * MB)

/* Public key modulus (big endian, with a leading 0x00) - same key as Windows Rufus */
static const uint8_t rsa_pubkey_modulus[] = {
	0x00, 0xb6, 0x40, 0x7d, 0xd1, 0x98, 0x7b, 0x81, 0x9e, 0xbe, 0x23, 0x0f, 0x32, 0x5d, 0x55,
	0x60, 0xc6, 0xbf, 0xb4, 0x41, 0xbb, 0x43, 0x1b, 0xf1, 0xe1, 0xe6, 0xf9, 0x2b, 0xd6, 0xdd,
	0x11, 0x50, 0xe8, 0xb9, 0x3f, 0x19, 0x97, 0x5e, 0xa7, 0x8b, 0x4a, 0x30, 0xc6, 0x76, 0x58,
	0x72, 0x1c, 0xac, 0xff, 0xa1, 0xf8, 0x96, 0x6c, 0x51, 0x5d, 0x13, 0x11, 0xe3, 0x5b, 0x11,
	0x82, 0xf5, 0x9a, 0x69, 0xe4, 0x28, 0x97, 0x0f, 0xca, 0x1f, 0x02, 0xea, 0x1f, 0x7d, 0xdc,
	0xf9, 0xfc, 0x79, 0x2f, 0x61, 0xff, 0x8e, 0x45, 0x60, 0x65, 0xba, 0x37, 0x9b, 0xde, 0x49,
	0x05, 0x6a, 0xa8, 0xfd, 0x70, 0xd0, 0x0c, 0x79, 0xb6, 0xd7, 0x81, 0xaa, 0x54, 0xc3, 0xc6,
	0x4a, 0x87, 0xa0, 0x45, 0xee, 0xca, 0xd5, 0xd5, 0xc5, 0xc2, 0xac, 0x86, 0x42, 0xb3, 0x58,
	0x27, 0xd2, 0x43, 0xb9, 0x37, 0xf2, 0xe6, 0x75, 0x66, 0x17, 0x53, 0xd0, 0x38, 0xd0, 0xc6,
	0x57, 0xc2, 0x55, 0x36, 0xa2, 0x43, 0x87, 0xea, 0x24, 0xf0, 0x96, 0xec, 0x34, 0xdd, 0x79,
	0x4d, 0x80, 0x54, 0x9d, 0x84, 0x81, 0xa7, 0xcf, 0x0c, 0xa5, 0x7c, 0xd6, 0x63, 0xfa, 0x7a,
	0x66, 0x30, 0xa9, 0x50, 0xee, 0xf0, 0xe5, 0xf8, 0xa2, 0x2d, 0xac, 0xfc, 0x24, 0x21, 0xfe,
	0xef, 0xe8, 0xd3, 0x6f, 0x0e, 0x27, 0xb0, 0x64, 0x22, 0x95, 0x3e, 0x6d, 0xa6, 0x66, 0x97,
	0xc6, 0x98, 0xc2, 0x47, 0xb3, 0x98, 0x69, 0x4d, 0xb1, 0xb5, 0xd3, 0x6f, 0x43, 0xf5, 0xd7,
	0xa5, 0x13, 0x5e, 0x8c, 0x28, 0x4f, 0x62, 0x4e, 0x01, 0x48, 0x0a, 0x63, 0x89, 0xe7, 0xca,
	0x34, 0xaa, 0x7d, 0x2f, 0xbb, 0x70, 0xe0, 0x31, 0xbb, 0x39, 0x49, 0xa3, 0xd2, 0xc9, 0x2e,
	0xa6, 0x30, 0x54, 0x9a, 0x5c, 0x4d, 0x58, 0x17, 0xd9, 0xfc, 0x3a, 0x43, 0xe6, 0x8e, 0x2a,
	0x18, 0xe9
};

bool rf_verify_signature(const uint8_t* buf, size_t len, const uint8_t* sig, size_t sig_len)
{
	OSSL_PARAM_BLD* bld = NULL;
	OSSL_PARAM* params = NULL;
	EVP_PKEY_CTX* pctx = NULL;
	EVP_PKEY* pkey = NULL;
	EVP_MD_CTX* mdctx = NULL;
	BIGNUM *n = NULL, *e = NULL;
	bool r = false;

	if (sig_len != RSA_SIGNATURE_SIZE)
		return false;
	n = BN_bin2bn(rsa_pubkey_modulus, sizeof(rsa_pubkey_modulus), NULL);
	e = BN_new();
	if (n == NULL || e == NULL || !BN_set_word(e, 65537))
		goto out;
	bld = OSSL_PARAM_BLD_new();
	if (bld == NULL || !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) ||
		!OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e))
		goto out;
	params = OSSL_PARAM_BLD_to_param(bld);
	pctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	if (params == NULL || pctx == NULL || EVP_PKEY_fromdata_init(pctx) <= 0 ||
		EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
		goto out;
	mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL || EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pkey) <= 0)
		goto out;
	r = (EVP_DigestVerify(mdctx, sig, sig_len, buf, len) == 1);
out:
	EVP_MD_CTX_free(mdctx);
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(pctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(bld);
	BN_free(n);
	BN_free(e);
	return r;
}

const char* rf_cache_dir(void)
{
	static char dir[1024] = "";
	const char* xdg = getenv("XDG_CACHE_HOME");
	const char* home = getenv("HOME");

	if (dir[0] != 0)
		return dir;
	if (xdg != NULL && xdg[0] == '/') {
		snprintf(dir, sizeof(dir), "%s/rufus", xdg);
	} else {
		if (home == NULL || home[0] == 0) {
			struct passwd* pw = getpwuid(getuid());
			home = (pw != NULL) ? pw->pw_dir : "/tmp";
		}
		snprintf(dir, sizeof(dir), "%s/.cache/rufus", home);
	}
	rf_mkdir_p(dir, 0755);
	return dir;
}

typedef struct {
	uint8_t* data;
	size_t len;
	bool too_large;
} membuf_t;

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata)
{
	membuf_t* mb = userdata;
	size_t n = size * nmemb;
	uint8_t* p;

	if (mb->len + n > MAX_DOWNLOAD_SIZE) {
		mb->too_large = true;
		return 0;
	}
	p = realloc(mb->data, mb->len + n + 1);
	if (p == NULL)
		return 0;
	mb->data = p;
	memcpy(&mb->data[mb->len], ptr, n);
	mb->len += n;
	mb->data[mb->len] = 0;
	return n;
}

static int xferinfo_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	(void)ultotal;
	(void)ulnow;
	if (dltotal > 0)
		rf_progress(OP_DOWNLOAD, (double)dlnow / (double)dltotal, "Downloading %s", (const char*)clientp);
	return rf_cancelled() ? 1 : 0;
}

static long download(const char* url, membuf_t* mb, const char* name)
{
	CURL* curl = curl_easy_init();
	long http_code = 0;
	CURLcode res;

	if (curl == NULL)
		return 0;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Rufus/" RUFUS_LINUX_VERSION " (Linux)");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, mb);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)name);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
	res = curl_easy_perform(curl);
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	else
		uprintf("Download of '%s' failed: %s", url, curl_easy_strerror(res));
	curl_easy_cleanup(curl);
	return http_code;
}

bool rf_download_signed(const char* rel_path, char* out_path, size_t out_size)
{
	char url[1024], url_sig[1040], dest[2048];
	membuf_t file = { 0 }, sig = { 0 };
	struct stat st;
	bool r = false;
	char* p;

	snprintf(dest, sizeof(dest), "%s/%s", rf_cache_dir(), rel_path);
	if (stat(dest, &st) == 0 && st.st_size > 0) {
		uprintf("Using cached '%s'", dest);
		snprintf(out_path, out_size, "%s", dest);
		return true;
	}
	snprintf(url, sizeof(url), "%s/%s", FILES_URL, rel_path);
	snprintf(url_sig, sizeof(url_sig), "%s.sig", url);
	uprintf("Downloading %s", url);
	if (download(url, &file, rel_path) != 200 || file.len == 0) {
		uprintf("Could not download '%s'", url);
		goto out;
	}
	if (download(url_sig, &sig, rel_path) != 200 || !rf_verify_signature(file.data, file.len, sig.data, sig.len)) {
		uprintf("FATAL: Download signature is invalid ✗");
		goto out;
	}
	uprintf("Download signature is valid ✓");
	p = strrchr(dest, '/');
	*p = 0;
	rf_mkdir_p(dest, 0755);
	*p = '/';
	if (!rf_write_file(dest, file.data, file.len))
		goto out;
	snprintf(out_path, out_size, "%s", dest);
	r = true;
out:
	free(file.data);
	free(sig.data);
	return r;
}

void rf_fetch_boot_files(const rf_image_t* img, rf_options_t* opts)
{
	char path[2048], rel[512];

	/* Syslinux 6 boot selection needs the matching ldlinux.c32, which we don't embed */
	if (opts->boot_type == BT_SYSLINUX_V6 && opts->ldlinux_c32[0] == 0) {
		snprintf(rel, sizeof(rel), "syslinux-%s/ldlinux.c32", embedded_sl_version_str[1]);
		if (rf_download_signed(rel, path, sizeof(path)))
			snprintf(opts->ldlinux_c32, sizeof(opts->ldlinux_c32), "%s", path);
		return;
	}
	if (opts->boot_type != BT_IMAGE || opts->image_mode != IM_ISO || img == NULL || !img->is_iso)
		return;
	if (opts->target_type != TT_BIOS)
		return;

	/* Syslinux v5+ versions are incompatible with one another: we need the exact ldlinux.sys/.bss */
	if (HAS_SYSLINUX(img) && SL_MAJOR(img->sl_version) >= 5 &&
		(img->sl_version != embedded_sl_version[1] || img->sl_version_ext[0] != 0) && opts->ldlinux_sys[0] == 0) {
		bool ok = true;
		for (int i = 0; i < 2 && ok; i++) {
			const char* ext = (i == 0) ? "sys" : "bss";
			snprintf(rel, sizeof(rel), "syslinux-%s%s/ldlinux.%s", img->sl_version_str, img->sl_version_ext, ext);
			ok = rf_download_signed(rel, path, sizeof(path));
			if (!ok && img->sl_version_ext[0] != 0) {
				/* Fall back to the version without the extra suffix */
				snprintf(rel, sizeof(rel), "syslinux-%s/ldlinux.%s", img->sl_version_str, ext);
				ok = rf_download_signed(rel, path, sizeof(path));
			}
			if (ok)
				snprintf((i == 0) ? opts->ldlinux_sys : opts->ldlinux_bss, sizeof(opts->ldlinux_sys), "%s", path);
		}
		if (!ok) {
			opts->ldlinux_sys[0] = opts->ldlinux_bss[0] = 0;
			uprintf("Could not download Syslinux %s%s files - will use embedded %s", img->sl_version_str,
				img->sl_version_ext, embedded_sl_version_str[1]);
		}
	}

	/* GRUB 2: core.img must match the GRUB version of the ISO's modules */
	if ((img->has_grub2 & 0x7f) && !HAS_SYSLINUX(img) && img->grub2_version[0] != 0 &&
		strcmp(img->grub2_version, embedded_grub2_version) != 0 && opts->grub2_core_img[0] == 0) {
		char version[96];
		snprintf(version, sizeof(version), "%s", img->grub2_version);
		/* Try progressively shorter version strings, as Windows Rufus does */
		for (int i = (int)strlen(version); i > 0; i--) {
			if (i != (int)strlen(version) && version[i] != '-' && version[i] != '.' && version[i] != '~')
				continue;
			version[i] = 0;
			snprintf(rel, sizeof(rel), "grub-%s/core.img", version);
			if (rf_download_signed(rel, path, sizeof(path))) {
				snprintf(opts->grub2_core_img, sizeof(opts->grub2_core_img), "%s", path);
				break;
			}
		}
		if (opts->grub2_core_img[0] == 0)
			uprintf("Could not download a GRUB %s core.img - will use embedded %s", img->grub2_version,
				embedded_grub2_version);
	}
}
