// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// libg2g: see g2g.h. Single-threaded: call it from one thread.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "g2g.h"
#include "g2g_ring.h"

#define PAGE		4096u
#define MAX_PEERS	16
#define NAME_MAX_LEN	64
#define MAX_PENDING	8
#define LIVENESS_MS	200	/* how often a wait checks the peer is still alive */
#define CLOSE_ACK_MS	1000	/* how long an owner waits for the borrower to let go */

/* Mailbox messages: payload[0] is the kind, then two arguments. */
#define G2G_MSG(t)	(0x6732676300000000ULL | (t))	/* "g2gc" */
#define MSG_OPEN	G2G_MSG(1)	/* client -> server: handle, nr_pages */
#define MSG_READY	G2G_MSG(2)	/* server -> client: handle */
#define MSG_CLOSE	G2G_MSG(3)	/* either way: handle */
#define MSG_CLOSE_ACK	G2G_MSG(4)	/* server -> client: handle */

#define CHAN_MAGIC	0x314e414843673267ULL	/* "g2gCHAN1" */
#define CHAN_VERSION	1

/* Page 0 of a channel's shared region; the two rings follow it. */
struct chan_hdr {
	uint64_t magic;
	uint64_t version;
	uint64_t nr_pages;
	uint64_t ring_size;
	struct g2g_ring_ctl c2s;	/* client -> server */
	struct g2g_ring_ctl s2c;	/* server -> client */
};
_Static_assert(sizeof(struct chan_hdr) <= PAGE, "channel header must fit a page");

struct peer {
	char name[NAME_MAX_LEN];
	struct pkvm_g2g_ident id;	/* incarnation unused */
};

struct pending_open {
	int peer;
	struct pkvm_g2g_ident from;
	uint64_t handle, nr_pages;
};

struct g2g_chan {
	struct g2g *g;
	struct g2g_chan *next;
	int peer;
	struct pkvm_g2g_ident remote;	/* with the incarnation the channel is bound to */
	int client;			/* this side offered the pages */
	uint64_t handle, nr_pages;
	void *base;			/* the mapping; NULL once unmapped */
	struct g2g_ring tx, rx;
	int ready, acked, dead;		/* dead: closed, failed or peer gone */
	struct timespec checked;	/* last liveness check */
};

struct g2g {
	int fd;
	struct pkvm_g2g_ident me;
	uint8_t auth[64];
	struct peer peers[MAX_PEERS];
	int npeers;
	struct pending_open pending[MAX_PENDING];
	int npending;
	struct g2g_chan *chans;
};

/* --- names --------------------------------------------------------------- */

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* The identity pvmfw registers for a VM named `name` and signed with `auth`. */
static int name_to_ident(const uint8_t auth[64], const char *name, struct pkvm_g2g_ident *id)
{
	static const char prefix[] = "pkvm-g2g-id:";
	size_t plen = sizeof(prefix) - 1, nlen = strlen(name);
	uint8_t *buf = malloc(plen + 64 + nlen), d[SHA256_DIGEST_LENGTH];

	if (!buf)
		return -ENOMEM;
	memcpy(buf, prefix, plen);
	memcpy(buf + plen, auth, 64);
	memcpy(buf + plen + 64, name, nlen);
	SHA256(buf, plen + 64 + nlen, d);
	free(buf);
	*id = (struct pkvm_g2g_ident){ .id_lo = le64(d), .id_hi = le64(d + 8) };
	return 0;
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

/* --- setup --------------------------------------------------------------- */

static int load_peers(struct g2g *g, const char *path)
{
	char line[256];
	FILE *f = fopen(path, "re");
	int ret = 0;

	if (!f)
		return -errno;
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
		if (g->npeers == MAX_PEERS || strlen(s) >= NAME_MAX_LEN) {
			ret = -E2BIG;
			break;
		}
		strcpy(g->peers[g->npeers].name, s);
		ret = name_to_ident(g->auth, s, &g->peers[g->npeers].id);
		if (ret)
			break;
		g->npeers++;
	}
	fclose(f);
	return ret;
}

static void guard_sigbus(void);

int g2g_open(struct g2g **gp, const char *peers_path)
{
	struct g2g *g = calloc(1, sizeof(*g));
	int ret;

	if (!g)
		return -ENOMEM;
	g->fd = -1;
	guard_sigbus();
	if (read_auth_hash("/dev/open-dice0", g->auth)) {
		ret = -ENODEV;
		goto fail;
	}
	ret = load_peers(g, peers_path ? peers_path : G2G_PEERS_DEFAULT);
	if (ret)
		goto fail;
	g->fd = open("/dev/pkvm-g2g", O_RDWR | O_CLOEXEC);
	if (g->fd < 0) {
		ret = -errno;
		goto fail;
	}
	if (ioctl(g->fd, PKVM_G2G_IOC_SELF, &g->me) < 0) {
		ret = -errno;
		goto fail;
	}
	if (!g->me.id_lo && !g->me.id_hi) {
		ret = -EPERM;		/* not booted through pvmfw */
		goto fail;
	}
	*gp = g;
	return 0;
fail:
	if (g->fd >= 0)
		close(g->fd);
	free(g);
	return ret;
}

const struct pkvm_g2g_ident *g2g_self(const struct g2g *g)
{
	return &g->me;
}

const uint8_t *g2g_auth_hash(const struct g2g *g)
{
	return g->auth;
}

static int find_peer_name(const struct g2g *g, const char *name)
{
	for (int i = 0; i < g->npeers; i++)
		if (!strcmp(g->peers[i].name, name))
			return i;
	return -1;
}

static int find_peer_id(const struct g2g *g, const struct pkvm_g2g_ident *id)
{
	for (int i = 0; i < g->npeers; i++)
		if (g->peers[i].id.id_lo == id->id_lo && g->peers[i].id.id_hi == id->id_hi)
			return i;
	return -1;
}

/* The peer's live identity; incarnation 0 if it is not running. */
static int lookup(struct g2g *g, int peer, struct pkvm_g2g_ident *id)
{
	*id = g->peers[peer].id;
	id->incarnation = 0;
	return ioctl(g->fd, PKVM_G2G_IOC_LOOKUP, id) < 0 ? -errno : 0;
}

int g2g_peer(struct g2g *g, const char *name, struct pkvm_g2g_ident *id)
{
	int peer = find_peer_name(g, name);

	return peer < 0 ? -ENOENT : lookup(g, peer, id);
}

/* --- waiting ------------------------------------------------------------- */

static long ms_since(const struct timespec *t)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

struct waiter {
	struct timespec start;
	int timeout_ms;
	unsigned int spins, sleep_us;
};

static void wait_init(struct waiter *w, int timeout_ms)
{
	clock_gettime(CLOCK_MONOTONIC, &w->start);
	w->timeout_ms = timeout_ms;
	w->spins = 0;
	w->sleep_us = 10;
}

/* Backs off a little more each call: spin, then sleep up to 1 ms. */
static int wait_step(struct waiter *w)
{
	if (w->timeout_ms >= 0 && ms_since(&w->start) >= w->timeout_ms)
		return -ETIMEDOUT;
	if (w->spins < 1000) {
		w->spins++;
		return 0;
	}
	usleep(w->sleep_us);
	if (w->sleep_us < 1000)
		w->sleep_us *= 2;
	return 0;
}

/* --- mailbox ------------------------------------------------------------- */

static void pump(struct g2g *g);

/* Sends a control message, draining our own mailbox while the peer's is full. */
static int send_ctl(struct g2g *g, const struct pkvm_g2g_ident *to, uint64_t kind,
		    uint64_t a, uint64_t b, int timeout_ms)
{
	struct pkvm_g2g_msg m = { .peer = *to, .payload = { kind, a, b, 0 } };
	struct waiter w;

	wait_init(&w, timeout_ms);
	while (ioctl(g->fd, PKVM_G2G_IOC_MSG_SEND, &m) < 0) {
		int ret = -errno;

		if (ret != -EAGAIN)
			return ret;
		pump(g);
		ret = wait_step(&w);
		if (ret)
			return ret;
	}
	return 0;
}

static void chan_unmap(struct g2g_chan *c)
{
	struct pkvm_g2g_end e = { .handle = c->handle };

	if (!c->base)
		return;
	munmap(c->base, c->nr_pages * PAGE);
	c->base = NULL;
	if (ioctl(c->g->fd, c->client ? PKVM_G2G_IOC_REVOKE : PKVM_G2G_IOC_RELEASE, &e) < 0 &&
	    !(errno == ENOENT && !c->client))	/* the owner already ended it */
		fprintf(stderr, "libg2g: %s of handle %#llx: %s\n", c->client ? "REVOKE" : "RELEASE",
			(unsigned long long)c->handle, strerror(errno));
}

static struct g2g_chan *find_chan(struct g2g *g, int peer, uint64_t handle)
{
	for (struct g2g_chan *c = g->chans; c; c = c->next)
		if (c->peer == peer && c->handle == handle && !c->dead)
			return c;
	return NULL;
}

/* Handles every waiting mailbox message; drops anything not from a listed peer. */
static void pump(struct g2g *g)
{
	struct pkvm_g2g_msg m;

	for (;;) {
		struct g2g_chan *c;
		int peer;

		m = (struct pkvm_g2g_msg){ 0 };	/* flags must be 0 */
		if (ioctl(g->fd, PKVM_G2G_IOC_MSG_RECV, &m) < 0)
			return;
		peer = find_peer_id(g, &m.peer);
		if (peer < 0)
			continue;
		switch (m.payload[0]) {
		case MSG_OPEN:
			if (g->npending < MAX_PENDING)
				g->pending[g->npending++] = (struct pending_open){
					peer, m.peer, m.payload[1], m.payload[2] };
			break;
		case MSG_READY:
			if ((c = find_chan(g, peer, m.payload[1])) && c->client &&
			    c->remote.incarnation == m.peer.incarnation)
				c->ready = 1;
			break;
		case MSG_CLOSE:
			/* Single-threaded: nothing is reading the region right now. */
			if ((c = find_chan(g, peer, m.payload[1]))) {
				c->dead = 1;
				chan_unmap(c);
				if (!c->client)
					send_ctl(g, &c->remote, MSG_CLOSE_ACK, c->handle, 0, 100);
			}
			break;
		case MSG_CLOSE_ACK:
			for (c = g->chans; c; c = c->next)
				if (c->peer == peer && c->handle == m.payload[1] && c->client)
					c->acked = 1;
			break;
		}
	}
}

/* Between ring polls: handle the mailbox, and notice a peer that went away. */
static int chan_check(struct g2g_chan *c)
{
	struct pkvm_g2g_ident now;

	pump(c->g);
	if (!c->dead && ms_since(&c->checked) >= LIVENESS_MS) {
		clock_gettime(CLOCK_MONOTONIC, &c->checked);
		if (!lookup(c->g, c->peer, &now) && now.incarnation != c->remote.incarnation) {
			c->dead = 1;
			chan_unmap(c);
		}
	}
	return c->dead ? -EPIPE : 0;
}

/* --- channels ------------------------------------------------------------ */

static uint64_t ring_size_for(uint64_t nr_pages)
{
	return ((nr_pages - 1) * PAGE / 2) & ~7ull;
}

static void chan_setup_rings(struct g2g_chan *c)
{
	struct chan_hdr *h = c->base;
	uint8_t *rings = (uint8_t *)c->base + PAGE;
	uint64_t size = ring_size_for(c->nr_pages);
	struct g2g_ring c2s = { &h->c2s, rings, size, 0 }, s2c = { &h->s2c, rings + size, size, 0 };

	c->tx = c->client ? c2s : s2c;
	c->rx = c->client ? s2c : c2s;
}

/*
 * Ends a live channel: tells the peer, and as the owner waits (briefly) for the
 * borrower to unmap before taking the pages back, since a borrower touching a
 * revoked page gets SIGBUS.
 */
static void chan_shutdown(struct g2g_chan *c, int told_peer)
{
	if (c->dead)
		return;
	c->dead = 1;
	if (told_peer && !send_ctl(c->g, &c->remote, MSG_CLOSE, c->handle, 0, 100) && c->client) {
		struct waiter w;

		wait_init(&w, CLOSE_ACK_MS);
		while (!c->acked && !wait_step(&w))
			pump(c->g);
	}
	chan_unmap(c);
}

static struct g2g_chan *chan_new(struct g2g *g, int peer, const struct pkvm_g2g_ident *remote,
				 int client)
{
	struct g2g_chan *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	c->g = g;
	c->peer = peer;
	c->remote = *remote;
	c->client = client;
	clock_gettime(CLOCK_MONOTONIC, &c->checked);
	c->next = g->chans;
	g->chans = c;
	return c;
}

static void chan_free(struct g2g_chan *c)
{
	for (struct g2g_chan **p = &c->g->chans; *p; p = &(*p)->next)
		if (*p == c) {
			*p = c->next;
			break;
		}
	free(c);
}

int g2g_chan_connect(struct g2g *g, const char *name, unsigned int nr_pages,
		     struct g2g_chan **cp, int timeout_ms)
{
	struct pkvm_g2g_offer o = { 0 };
	struct pkvm_g2g_ident remote;
	struct g2g_chan *c;
	struct chan_hdr *h;
	struct waiter w;
	int peer = find_peer_name(g, name), ret, opened = 0;

	if (peer < 0)
		return -ENOENT;
	if (nr_pages < 2 || nr_pages > PKVM_G2G_MAX_PAGES)
		return -EINVAL;
	ret = lookup(g, peer, &remote);
	if (ret)
		return ret;
	if (!remote.incarnation)
		return -ESRCH;

	o.peer = remote;
	o.nr_pages = nr_pages;
	if (ioctl(g->fd, PKVM_G2G_IOC_OFFER, &o) < 0)
		return -errno;
	c = chan_new(g, peer, &remote, 1);
	if (!c) {
		struct pkvm_g2g_end e = { .handle = o.handle };

		ioctl(g->fd, PKVM_G2G_IOC_REVOKE, &e);
		return -ENOMEM;
	}
	c->handle = o.handle;
	c->nr_pages = nr_pages;
	c->base = mmap(NULL, nr_pages * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, g->fd, o.mmap_offset);
	if (c->base == MAP_FAILED) {
		ret = -errno;
		c->base = NULL;
		goto fail;
	}
	h = c->base;			/* the pages come zeroed */
	h->magic = CHAN_MAGIC;
	h->version = CHAN_VERSION;
	h->nr_pages = nr_pages;
	h->ring_size = ring_size_for(nr_pages);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	chan_setup_rings(c);

	ret = send_ctl(g, &remote, MSG_OPEN, c->handle, nr_pages, timeout_ms);
	if (ret)
		goto fail;
	opened = 1;
	wait_init(&w, timeout_ms);
	while (!c->ready) {
		ret = chan_check(c);
		if (!ret)
			ret = wait_step(&w);
		if (ret)
			goto fail;
	}
	*cp = c;
	return 0;
fail:
	chan_shutdown(c, opened);	/* the server may have mapped it already */
	chan_free(c);
	return ret;
}

/* Maps an offered channel and checks its header against what we expect. */
static int chan_accept(struct g2g *g, const struct pending_open *p, struct g2g_chan **cp)
{
	struct pkvm_g2g_accept a = { 0 };
	volatile struct chan_hdr *h;
	struct g2g_chan *c;
	int ret = -EPROTO;

	if (p->nr_pages < 2 || p->nr_pages > PKVM_G2G_MAX_PAGES)
		return -EPROTO;
	a.owner = p->from;
	a.handle = p->handle;
	a.nr_pages = p->nr_pages;
	if (ioctl(g->fd, PKVM_G2G_IOC_ACCEPT, &a) < 0)
		return -errno;
	c = chan_new(g, p->peer, &p->from, 0);
	if (!c) {
		struct pkvm_g2g_end e = { .handle = p->handle };

		ioctl(g->fd, PKVM_G2G_IOC_RELEASE, &e);
		return -ENOMEM;
	}
	c->handle = p->handle;
	c->nr_pages = p->nr_pages;
	c->base = mmap(NULL, c->nr_pages * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, g->fd,
		       a.mmap_offset);
	if (c->base == MAP_FAILED) {
		ret = -errno;
		c->base = NULL;
		goto fail;
	}
	h = c->base;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (h->magic != CHAN_MAGIC || h->version != CHAN_VERSION || h->nr_pages != c->nr_pages ||
	    h->ring_size != ring_size_for(c->nr_pages))
		goto fail;
	chan_setup_rings(c);		/* sizes from nr_pages, not from the header */
	ret = send_ctl(g, &c->remote, MSG_READY, c->handle, 0, 1000);
	if (ret)
		goto fail;
	*cp = c;
	return 0;
fail:
	chan_unmap(c);
	chan_free(c);
	return ret;
}

int g2g_chan_listen(struct g2g *g, struct g2g_chan **cp, int timeout_ms)
{
	struct waiter w;
	int ret;

	wait_init(&w, timeout_ms);
	for (;;) {
		pump(g);
		while (g->npending) {
			struct pending_open p = g->pending[0];

			memmove(g->pending, g->pending + 1, --g->npending * sizeof(p));
			if (!chan_accept(g, &p, cp))
				return 0;
		}
		ret = wait_step(&w);
		if (ret)
			return ret;
	}
}

const char *g2g_chan_peer(const struct g2g_chan *c)
{
	return c->g->peers[c->peer].name;
}

size_t g2g_chan_max_record(const struct g2g_chan *c)
{
	size_t max = c->tx.size - G2G_REC_HDR;

	return max < G2G_MAX_RECORD ? max : G2G_MAX_RECORD;
}

/*
 * The owner can take a channel's pages back at any moment (a client that
 * exits or crashes without closing has them revoked when its file closes),
 * and the borrower's next access then raises SIGBUS. Ring accesses run under
 * this handler, which turns that SIGBUS into -EPIPE for the channel instead of
 * killing the process. A SIGBUS anywhere else gets the previous handler.
 */
static __thread sigjmp_buf *ring_jmp;
static struct sigaction old_sigbus;

static void on_sigbus(int sig, siginfo_t *si, void *uc)
{
	if (ring_jmp)
		siglongjmp(*ring_jmp, 1);
	if (old_sigbus.sa_flags & SA_SIGINFO) {
		old_sigbus.sa_sigaction(sig, si, uc);
	} else if (old_sigbus.sa_handler != SIG_IGN && old_sigbus.sa_handler != SIG_DFL) {
		old_sigbus.sa_handler(sig);
	} else {
		signal(SIGBUS, SIG_DFL);
		raise(SIGBUS);
	}
}

static void guard_sigbus(void)
{
	static int done;
	struct sigaction sa = { .sa_sigaction = on_sigbus, .sa_flags = SA_SIGINFO };

	if (done)
		return;
	sigemptyset(&sa.sa_mask);
	if (!sigaction(SIGBUS, &sa, &old_sigbus))
		done = 1;
}

static int ring_put_guarded(struct g2g_ring *r, const void *buf, uint32_t len)
{
	sigjmp_buf jb;
	int ret;

	if (sigsetjmp(jb, 1)) {
		ring_jmp = NULL;
		return -EPIPE;
	}
	ring_jmp = &jb;
	ret = g2g_ring_put(r, buf, len);
	ring_jmp = NULL;
	return ret;
}

static ssize_t ring_get_guarded(struct g2g_ring *r, void *buf, size_t cap)
{
	sigjmp_buf jb;
	ssize_t ret;

	if (sigsetjmp(jb, 1)) {
		ring_jmp = NULL;
		return -EPIPE;
	}
	ring_jmp = &jb;
	ret = g2g_ring_get(r, buf, cap);
	ring_jmp = NULL;
	return ret;
}

/* The pages are gone: forget them without touching them again. */
static int chan_lost(struct g2g_chan *c)
{
	c->dead = 1;
	chan_unmap(c);
	return -EPIPE;
}

/* A broken ring means a misbehaving peer: close the channel. */
static int chan_fail(struct g2g_chan *c, int err)
{
	chan_shutdown(c, 1);
	return err;
}

int g2g_chan_send(struct g2g_chan *c, const void *buf, size_t len, int timeout_ms)
{
	struct waiter w;
	int ret;

	if (c->dead)
		return -EPIPE;
	if (len > g2g_chan_max_record(c))
		return -EMSGSIZE;
	wait_init(&w, timeout_ms);
	for (;;) {
		ret = ring_put_guarded(&c->tx, buf, len);
		if (ret == -EPIPE)
			return chan_lost(c);
		if (ret != -EAGAIN)
			return ret == -EPROTO ? chan_fail(c, ret) : ret;
		ret = chan_check(c);
		if (!ret)
			ret = wait_step(&w);
		if (ret)
			return ret;
	}
}

ssize_t g2g_chan_recv(struct g2g_chan *c, void *buf, size_t cap, int timeout_ms)
{
	struct waiter w;
	ssize_t ret;

	if (c->dead)
		return -EPIPE;
	wait_init(&w, timeout_ms);
	for (;;) {
		ret = ring_get_guarded(&c->rx, buf, cap);
		if (ret == -EPIPE)
			return chan_lost(c);
		if (ret != -EAGAIN)
			return ret < 0 ? chan_fail(c, ret) : ret;
		ret = chan_check(c);
		if (!ret)
			ret = wait_step(&w);
		if (ret)
			return ret;
	}
}

void g2g_chan_close(struct g2g_chan *c)
{
	chan_shutdown(c, 1);
	chan_free(c);
}

void g2g_close(struct g2g *g)
{
	while (g->chans)
		g2g_chan_close(g->chans);
	close(g->fd);
	free(g);
}
