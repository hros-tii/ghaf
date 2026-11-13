// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// g2g-agent: keeps this VM's guest-to-guest channels to the peers it trusts.
//
//   g2g-agent <peers-file>            run the agent
//   g2g-agent --auth-hash             print this VM's DICE authority hash
//
// <peers-file> lists one VM name per line ('#' starts a comment). A name
// resolves to the identity pvmfw registers for a VM signed with the same key:
// the first 128 bits of SHA-256("pkvm-g2g-id:" || auth-hash || name). The
// auth-hash, SHA-512 of the AVB public key, is the authority hash in this VM's
// own DICE certificate, read from /dev/open-dice0.
//
// The agent greets every listed peer that is alive (HELLO, answered with
// HELLO_ACK), repeating it until acknowledged and again whenever the peer comes
// back with a new incarnation, and drops anything sent by a VM that is not
// listed. Keep the peers file and this
// binary in the signed initrd: the rootfs is not authenticated.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pkvm_g2g.h"

#define MAX_PEERS	16
#define NAME_MAX_LEN	64
#define MSG_HELLO	0x4f4c4c4548673267ULL	/* "g2gHELLO" */
#define MSG_HELLO_ACK	0x4b43416f6c6c6548ULL	/* "HelloACK" */
#define TICK_US		500000

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

/* --- names --------------------------------------------------------------- */

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

/* --- DICE handover (CBOR) ------------------------------------------------ */

/*
 * pvmfw leaves the VM a DICE handover: a CBOR map {1: CDI_Attest, 2: CDI_Seal,
 * 3: DICE chain}. The chain is an array [root COSE_Key, cert, ..., cert] whose
 * last certificate describes this VM. Each certificate is a COSE_Sign1
 * [protected, unprotected, payload, signature]; the payload is a CWT map, and
 * its authorityHash claim (-4670549) is the hash of the key that signed this
 * VM. The CDIs are secrets: they are skipped, never copied or printed.
 */
#define CWT_AUTHORITY_HASH	(-4670549LL)

struct cbor {
	const uint8_t *p, *end;
};

/* Reads an item head: its major type and argument. Rejects indefinite lengths. */
static int cbor_head(struct cbor *c, int *major, uint64_t *arg)
{
	uint8_t ib, ai;
	int n;

	if (c->p >= c->end)
		return -1;
	ib = *c->p++;
	*major = ib >> 5;
	ai = ib & 0x1f;
	if (ai < 24) {
		*arg = ai;
		return 0;
	}
	if (ai > 27)
		return -1;
	n = 1 << (ai - 24);
	if (c->end - c->p < n)
		return -1;
	*arg = 0;
	while (n--)
		*arg = *arg << 8 | *c->p++;
	return 0;
}

static int cbor_skip(struct cbor *c, int depth)
{
	uint64_t arg, i;
	int major;

	if (depth > 16 || cbor_head(c, &major, &arg))
		return -1;
	switch (major) {
	case 0: case 1: case 7:		/* integers, simple values, floats */
		return 0;
	case 2: case 3:			/* byte and text strings */
		if ((uint64_t)(c->end - c->p) < arg)
			return -1;
		c->p += arg;
		return 0;
	case 4:				/* array */
		for (i = 0; i < arg; i++)
			if (cbor_skip(c, depth + 1))
				return -1;
		return 0;
	case 5:				/* map */
		for (i = 0; i < 2 * arg; i++)
			if (cbor_skip(c, depth + 1))
				return -1;
		return 0;
	case 6:				/* tag */
		return cbor_skip(c, depth + 1);
	}
	return -1;
}

static int cbor_bstr(struct cbor *c, const uint8_t **data, uint64_t *len)
{
	int major;

	if (cbor_head(c, &major, len) || major != 2 || (uint64_t)(c->end - c->p) < *len)
		return -1;
	*data = c->p;
	c->p += *len;
	return 0;
}

static int cbor_int(struct cbor *c, long long *v)
{
	uint64_t arg;
	int major;

	if (cbor_head(c, &major, &arg) || (major != 0 && major != 1) || arg > INT64_MAX)
		return -1;
	*v = major == 0 ? (long long)arg : -1 - (long long)arg;
	return 0;
}

/* Finds the authority hash in the last certificate of the handover's DICE chain. */
static int dice_authority_hash(const uint8_t *buf, size_t size, uint8_t out[64])
{
	struct cbor c = { buf, buf + size }, cert, cwt;
	const uint8_t *chain = NULL, *payload, *v;
	uint64_t n, i, len, plen, vlen;
	long long key;
	int major;

	if (cbor_head(&c, &major, &n) || major != 5)
		return -1;
	for (i = 0; i < n; i++) {
		if (cbor_int(&c, &key))
			return -1;
		if (key == 3)
			chain = c.p;
		if (cbor_skip(&c, 0))
			return -1;
	}
	if (!chain)
		return -1;

	c.p = chain;
	if (cbor_head(&c, &major, &len) || major != 4 || len < 2)
		return -1;
	for (i = 0; i + 1 < len; i++)	/* the root key and every cert but the last */
		if (cbor_skip(&c, 0))
			return -1;

	cert = c;
	if (cbor_head(&cert, &major, &len))
		return -1;
	if (major == 6 && cbor_head(&cert, &major, &len))	/* optional COSE_Sign1 tag */
		return -1;
	if (major != 4 || len != 4)
		return -1;
	if (cbor_skip(&cert, 0) || cbor_skip(&cert, 0) || cbor_bstr(&cert, &payload, &plen))
		return -1;

	cwt = (struct cbor){ payload, payload + plen };
	if (cbor_head(&cwt, &major, &n) || major != 5)
		return -1;
	for (i = 0; i < n; i++) {
		if (cbor_int(&cwt, &key))
			return -1;
		if (key == CWT_AUTHORITY_HASH) {
			if (cbor_bstr(&cwt, &v, &vlen) || vlen != 64)
				return -1;
			memcpy(out, v, 64);
			return 0;
		}
		if (cbor_skip(&cwt, 0))
			return -1;
	}
	return -1;
}

static int read_auth_hash(const char *dev, uint8_t out[64])
{
	unsigned long size;
	void *map;
	int fd, ret;

	fd = open(dev, O_RDONLY | O_CLOEXEC);	/* never write: a write wipes it */
	if (fd < 0) {
		perror(dev);
		return -1;
	}
	if (read(fd, &size, sizeof(size)) != sizeof(size) || !size) {
		fprintf(stderr, "%s: cannot read its size\n", dev);
		close(fd);
		return -1;
	}
	map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	ret = dice_authority_hash(map, size, out);
	munmap(map, size);
	if (ret)
		fprintf(stderr, "%s: no authority hash in the DICE chain\n", dev);
	return ret;
}

/* --- agent --------------------------------------------------------------- */

struct peer {
	char name[NAME_MAX_LEN];
	struct pkvm_g2g_ident id;	/* id_lo/id_hi from the name; incarnation unused */
	uint64_t live;			/* incarnation last seen alive, 0 if none */
	int acked;			/* that incarnation acknowledged our HELLO */
	unsigned long next_hello;	/* tick of the next HELLO while unacknowledged */
	struct pkvm_g2g_ident ack_to;	/* HELLO to acknowledge; incarnation 0 if none */
};

static void hex(const uint8_t *p, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", p[i]);
}

static int load_peers(const char *path, const uint8_t auth[64], struct peer *peers)
{
	char line[256];
	int n = 0;
	FILE *f = fopen(path, "re");

	if (!f) {
		perror(path);
		exit(1);
	}
	while (fgets(line, sizeof(line), f)) {
		char *s = line, *e;

		if ((e = strchr(s, '#')))
			*e = '\0';
		while (*s == ' ' || *s == '\t')
			s++;
		for (e = s + strlen(s); e > s && (e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'); e--)
			;
		*e = '\0';
		if (!*s)
			continue;
		if (n == MAX_PEERS || strlen(s) >= NAME_MAX_LEN) {
			fprintf(stderr, "%s: too many peers or name too long: %s\n", path, s);
			exit(1);
		}
		strcpy(peers[n].name, s);
		peers[n].id = name_to_ident(auth, s);
		n++;
	}
	fclose(f);
	return n;
}

static struct peer *find_peer(struct peer *peers, int n, const struct pkvm_g2g_ident *id)
{
	for (int i = 0; i < n; i++)
		if (peers[i].id.id_lo == id->id_lo && peers[i].id.id_hi == id->id_hi)
			return &peers[i];
	return NULL;
}

static int send_msg(int fd, struct pkvm_g2g_ident to, uint64_t kind, uint64_t arg)
{
	struct pkvm_g2g_msg m = { .peer = to, .payload = { kind, arg, 0, 0 } };

	return ioctl(fd, PKVM_G2G_IOC_MSG_SEND, &m);
}

/* Sends a pending HELLO_ACK; a full mailbox (EAGAIN) leaves it for the next tick. */
static void send_ack(int fd, struct peer *p)
{
	if (!p->ack_to.incarnation)
		return;
	if (send_msg(fd, p->ack_to, MSG_HELLO_ACK, 0) == 0)
		p->ack_to.incarnation = 0;
	else if (errno != EAGAIN) {
		fprintf(stderr, "HELLO_ACK to %s: %s\n", p->name, strerror(errno));
		p->ack_to.incarnation = 0;
	}
}

/*
 * Greets each listed peer that is alive, and repeats the HELLO every
 * HELLO_RETRY ticks until that incarnation acknowledges it: a message can be
 * refused while the peer's mailbox is full, or sent before its agent runs.
 */
#define HELLO_RETRY	10

static void greet(int fd, const struct pkvm_g2g_ident *me, struct peer *peers, int n,
		  unsigned long tick)
{
	for (int i = 0; i < n; i++) {
		struct peer *p = &peers[i];
		struct pkvm_g2g_ident id = p->id;

		if (id.id_lo == me->id_lo && id.id_hi == me->id_hi)
			continue;		/* this VM is in its own list */
		send_ack(fd, p);
		id.incarnation = 0;
		if (ioctl(fd, PKVM_G2G_IOC_LOOKUP, &id) < 0 || !id.incarnation) {
			p->live = 0;		/* not running */
			continue;
		}
		if (id.incarnation != p->live) {
			p->live = id.incarnation;
			p->acked = 0;
			p->next_hello = tick;
			printf("%s (incarnation %llu): alive\n", p->name,
			       (unsigned long long)p->live);
		}
		if (p->acked || tick < p->next_hello)
			continue;
		if (send_msg(fd, id, MSG_HELLO, me->incarnation) < 0 && errno != EAGAIN)
			fprintf(stderr, "HELLO to %s: %s\n", p->name, strerror(errno));
		p->next_hello = tick + HELLO_RETRY;
	}
}

static void handle_messages(int fd, struct peer *peers, int n)
{
	struct pkvm_g2g_msg m;

	for (;;) {
		struct peer *p;

		m = (struct pkvm_g2g_msg){ 0 };	/* flags must be 0 */
		if (ioctl(fd, PKVM_G2G_IOC_MSG_RECV, &m) < 0) {
			if (errno != EAGAIN)
				fprintf(stderr, "MSG_RECV: %s\n", strerror(errno));
			return;
		}
		p = find_peer(peers, n, &m.peer);
		if (!p) {
			printf("dropped a message from %#llx:%#llx (not a listed peer)\n",
			       (unsigned long long)m.peer.id_hi, (unsigned long long)m.peer.id_lo);
			continue;
		}
		switch (m.payload[0]) {
		case MSG_HELLO:
			printf("%s (incarnation %llu): HELLO received\n", p->name,
			       (unsigned long long)m.peer.incarnation);
			p->ack_to = m.peer;
			send_ack(fd, p);
			break;
		case MSG_HELLO_ACK:
			if (m.peer.incarnation == p->live && !p->acked) {
				p->acked = 1;
				printf("%s (incarnation %llu): HELLO acknowledged\n", p->name,
				       (unsigned long long)m.peer.incarnation);
			}
			break;
		default:
			printf("%s: ignored message kind %#llx\n", p->name,
			       (unsigned long long)m.payload[0]);
		}
	}
}

int main(int argc, char **argv)
{
	struct peer peers[MAX_PEERS] = { 0 };
	struct pkvm_g2g_ident me;
	unsigned long tick = 0;
	uint8_t auth[64];
	char auth_hex[129];
	int fd, n;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 2) {
		fprintf(stderr, "usage: %s <peers-file> | --auth-hash\n", argv[0]);
		return 2;
	}
	if (read_auth_hash("/dev/open-dice0", auth))
		return 1;
	hex(auth, sizeof(auth), auth_hex);
	if (!strcmp(argv[1], "--auth-hash")) {
		printf("%s\n", auth_hex);
		return 0;
	}

	n = load_peers(argv[1], auth, peers);
	fd = open("/dev/pkvm-g2g", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("/dev/pkvm-g2g");
		return 1;
	}
	if (ioctl(fd, PKVM_G2G_IOC_SELF, &me) < 0) {
		perror("SELF");
		return 1;
	}
	if (!me.id_lo && !me.id_hi) {
		fprintf(stderr, "this VM has no g2g identity (not booted through pvmfw?)\n");
		return 1;
	}
	printf("this VM: %#llx:%#llx, incarnation %llu; authority %.16s...; %d peer(s)\n",
	       (unsigned long long)me.id_hi, (unsigned long long)me.id_lo,
	       (unsigned long long)me.incarnation, auth_hex, n);
	for (int i = 0; i < n; i++)
		printf("  peer %s = %#llx:%#llx\n", peers[i].name,
		       (unsigned long long)peers[i].id.id_hi, (unsigned long long)peers[i].id.id_lo);

	for (;; tick++) {
		handle_messages(fd, peers, n);
		greet(fd, &me, peers, n, tick);
		usleep(TICK_US);
	}
}
