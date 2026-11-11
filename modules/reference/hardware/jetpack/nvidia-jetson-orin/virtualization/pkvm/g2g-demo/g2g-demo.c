// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// g2g-demo: two VMs booted through pvmfw find each other by name and share a
// page through pKVM, without the host seeing it.
//
//   g2g-demo self                               this VM's identity
//   g2g-demo id     <auth-hash> <name>          the identity <name> has
//   g2g-demo accept <auth-hash> <peer-name>     wait for <peer-name>'s offer, take it
//   g2g-demo offer  <auth-hash> <peer-name>     offer <peer-name> a page
//
// A name resolves to the identity pvmfw registers for a VM signed with the
// same key: the first 128 bits of
//   SHA-256("pkvm-g2g-id:" || auth-hash || com.android.virt.name),
// where <auth-hash> is SHA-512 of the AVB public key (128 hex digits), the
// DICE authority hash. Until DICE reaches the guest it is given here.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pkvm_g2g.h"

#define PAGE		4096
#define REPLY_AT	2048		/* the borrower writes its reply here */
#define MSG_OFFER	0x524546464f673267ULL	/* "g2gOFFER" */
#define MSG_DONE	0x454e4f4467326700ULL	/* "\0g2gDONE" */
#define TIMEOUT_S	120

/* --- SHA-256 (FIPS 180-4) ------------------------------------------------ */

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	size_t total = (len + 9 + 63) / 64 * 64;
	uint64_t bits = (uint64_t)len * 8;

	for (size_t off = 0; off < total; off += 64) {
		uint32_t w[64], a, b, c, d, e, f, g, hh;
		uint8_t blk[64];

		for (size_t i = 0; i < 64; i++) {
			size_t p = off + i;

			if (p < len)
				blk[i] = msg[p];
			else if (p == len)
				blk[i] = 0x80;
			else if (p >= total - 8)
				blk[i] = (uint8_t)(bits >> (8 * (total - 1 - p)));
			else
				blk[i] = 0;
		}
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 |
			       (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		a = h[0]; b = h[1]; c = h[2]; d = h[3];
		e = h[4]; f = h[5]; g = h[6]; hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
				      ((e & f) ^ (~e & g)) + K[i] + w[i];
			uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
				      ((a & b) ^ (a & c) ^ (b & c));

			hh = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d;
		h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	for (int i = 0; i < 8; i++) {
		out[4 * i] = h[i] >> 24;
		out[4 * i + 1] = h[i] >> 16;
		out[4 * i + 2] = h[i] >> 8;
		out[4 * i + 3] = h[i];
	}
}

/* --- names --------------------------------------------------------------- */

static void parse_auth(const char *hex, uint8_t auth[64])
{
	if (strlen(hex) != 128) {
		fprintf(stderr, "auth-hash must be 128 hex digits (SHA-512)\n");
		exit(2);
	}
	for (int i = 0; i < 64; i++) {
		unsigned int v;

		if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
			fprintf(stderr, "auth-hash: bad hex\n");
			exit(2);
		}
		auth[i] = v;
	}
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* The identity pvmfw registers for a VM named `name` and signed with `auth`. */
static struct pkvm_g2g_ident name_to_ident(const uint8_t auth[64], const char *name)
{
	static const char prefix[] = "pkvm-g2g-id:";
	size_t plen = sizeof(prefix) - 1, nlen = strlen(name);
	struct pkvm_g2g_ident id = { 0 };
	uint8_t *buf = malloc(plen + 64 + nlen), d[32];

	if (!buf)
		exit(1);
	memcpy(buf, prefix, plen);
	memcpy(buf + plen, auth, 64);
	memcpy(buf + plen + 64, name, nlen);
	sha256(buf, plen + 64 + nlen, d);
	free(buf);
	id.id_lo = le64(d);
	id.id_hi = le64(d + 8);
	return id;
}

static void print_ident(const char *what, const struct pkvm_g2g_ident *id)
{
	printf("%s %#llx:%#llx, incarnation %llu\n", what, (unsigned long long)id->id_hi,
	       (unsigned long long)id->id_lo, (unsigned long long)id->incarnation);
}

/* --- device -------------------------------------------------------------- */

static int open_dev(void)
{
	int fd = open("/dev/pkvm-g2g", O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		perror("/dev/pkvm-g2g");
		exit(1);
	}
	return fd;
}

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static void send_msg(int fd, struct pkvm_g2g_ident peer, uint64_t w0, uint64_t w1, uint64_t w2)
{
	struct pkvm_g2g_msg m = { .peer = peer, .payload = { w0, w1, w2, 0 } };
	time_t end = time(NULL) + TIMEOUT_S;

	while (ioctl(fd, PKVM_G2G_IOC_MSG_SEND, &m) < 0) {
		if (errno != EAGAIN || time(NULL) > end)
			die("MSG_SEND");
		usleep(100000);	/* the peer's mailbox is full */
	}
}

/* Wait for a message of `kind`; with `from`, ignore anyone else. */
static struct pkvm_g2g_msg recv_msg(int fd, uint64_t kind, const struct pkvm_g2g_ident *from)
{
	time_t end = time(NULL) + TIMEOUT_S;
	struct pkvm_g2g_msg m;

	for (;;) {
		m = (struct pkvm_g2g_msg){ 0 };	/* flags must be 0 */
		if (ioctl(fd, PKVM_G2G_IOC_MSG_RECV, &m) == 0) {
			if (from && (m.peer.id_lo != from->id_lo || m.peer.id_hi != from->id_hi)) {
				print_ident("ignoring a message from", &m.peer);
				continue;
			}
			if (m.payload[0] == kind)
				return m;
			continue;
		}
		if (errno != EAGAIN)
			die("MSG_RECV");
		if (time(NULL) > end) {
			fprintf(stderr, "timed out after %d s\n", TIMEOUT_S);
			exit(1);
		}
		usleep(100000);
	}
}

/* Copy out of the shared page before using it: the peer can change it any time. */
static void print_shared(const char *what, const volatile char *p)
{
	char buf[256];

	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = p[i];
	buf[sizeof(buf) - 1] = '\0';
	printf("%s: \"%s\"\n", what, buf);
}

static struct pkvm_g2g_ident lookup(int fd, const uint8_t auth[64], const char *name)
{
	struct pkvm_g2g_ident id = name_to_ident(auth, name);

	if (ioctl(fd, PKVM_G2G_IOC_LOOKUP, &id) < 0)
		die("LOOKUP");
	if (!id.incarnation) {
		fprintf(stderr, "%s is not a live VM\n", name);
		exit(1);
	}
	print_ident(name, &id);
	return id;
}

static void self(int fd, struct pkvm_g2g_ident *me)
{
	if (ioctl(fd, PKVM_G2G_IOC_SELF, me) < 0)
		die("SELF");
	print_ident("this VM:", me);
}

static int do_offer(const uint8_t auth[64], const char *peer_name)
{
	int fd = open_dev();
	struct pkvm_g2g_ident me, peer;
	struct pkvm_g2g_offer o = { 0 };
	char *p;

	self(fd, &me);
	peer = lookup(fd, auth, peer_name);

	o.peer = peer;
	o.nr_pages = 1;
	if (ioctl(fd, PKVM_G2G_IOC_OFFER, &o) < 0)
		die("OFFER");
	p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o.mmap_offset);
	if (p == MAP_FAILED)
		die("mmap");
	snprintf(p, REPLY_AT, "hello %s, from incarnation %llu", peer_name,
		 (unsigned long long)me.incarnation);
	printf("offered 1 page, handle %#llx\n", (unsigned long long)o.handle);

	send_msg(fd, peer, MSG_OFFER, o.handle, o.nr_pages);
	recv_msg(fd, MSG_DONE, &peer);
	print_shared("peer wrote back", p + REPLY_AT);

	munmap(p, PAGE);
	struct pkvm_g2g_end e = { .handle = o.handle };
	if (ioctl(fd, PKVM_G2G_IOC_REVOKE, &e) < 0)
		die("REVOKE");
	printf("revoked\n");
	close(fd);
	return 0;
}

static int do_accept(const uint8_t auth[64], const char *peer_name)
{
	struct pkvm_g2g_ident me, want = name_to_ident(auth, peer_name);
	struct pkvm_g2g_accept a = { 0 };
	struct pkvm_g2g_msg m;
	int fd = open_dev();
	char *p;

	self(fd, &me);
	printf("waiting for an offer from %s (%#llx:%#llx)\n", peer_name,
	       (unsigned long long)want.id_hi, (unsigned long long)want.id_lo);
	m = recv_msg(fd, MSG_OFFER, &want);	/* only from peer_name */
	print_ident("offer from", &m.peer);

	a.owner = m.peer;
	a.handle = m.payload[1];
	a.nr_pages = m.payload[2];
	if (ioctl(fd, PKVM_G2G_IOC_ACCEPT, &a) < 0)
		die("ACCEPT");
	p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a.mmap_offset);
	if (p == MAP_FAILED)
		die("mmap");
	print_shared("shared page says", p);
	snprintf(p + REPLY_AT, PAGE - REPLY_AT, "hello back, from incarnation %llu",
		 (unsigned long long)me.incarnation);

	send_msg(fd, m.peer, MSG_DONE, a.handle, 0);
	sleep(1);
	munmap(p, PAGE);
	struct pkvm_g2g_end e = { .handle = a.handle };
	if (ioctl(fd, PKVM_G2G_IOC_RELEASE, &e) < 0 && errno != ENOENT)
		die("RELEASE");	/* ENOENT: the owner already revoked */
	printf("released\n");
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t auth[64];

	if (argc == 2 && !strcmp(argv[1], "self")) {
		struct pkvm_g2g_ident me;

		self(open_dev(), &me);
		return 0;
	}
	if (argc != 4) {
		fprintf(stderr, "usage: %s self\n"
				"       %s id|offer|accept <auth-hash> <name>\n", argv[0], argv[0]);
		return 2;
	}
	parse_auth(argv[2], auth);
	if (!strcmp(argv[1], "id")) {
		struct pkvm_g2g_ident id = name_to_ident(auth, argv[3]);

		print_ident(argv[3], &id);
		return 0;
	}
	if (!strcmp(argv[1], "offer"))
		return do_offer(auth, argv[3]);
	if (!strcmp(argv[1], "accept"))
		return do_accept(auth, argv[3]);
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
