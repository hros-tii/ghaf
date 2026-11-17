/* SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * A single-producer, single-consumer byte ring in memory shared with an
 * untrusted peer, carrying records: an 8-byte header (u32 length, u32 zero)
 * and the payload, padded to 8 bytes. A payload may wrap around the end.
 *
 * head and tail are byte counts that only grow; the producer owns head, the
 * consumer tail. Each side keeps its own index privately and only publishes
 * it, so the peer cannot rewind or skip it; the index read from the peer is
 * checked before use. The ring size comes from the caller, never from shared
 * memory.
 */
#ifndef G2G_RING_H
#define G2G_RING_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

struct g2g_ring_ctl {
	_Alignas(64) uint64_t head;	/* written by the producer */
	_Alignas(64) uint64_t tail;	/* written by the consumer */
};

struct g2g_ring {
	struct g2g_ring_ctl *ctl;	/* shared */
	uint8_t *data;			/* shared, size bytes */
	uint64_t size;			/* a multiple of 8, from the caller */
	uint64_t pos;			/* this side's own index (head or tail) */
};

#define G2G_REC_HDR	8u

static inline uint64_t g2g_rec_space(uint64_t len)
{
	return G2G_REC_HDR + ((len + 7) & ~7ull);
}

static inline void g2g_ring_write(struct g2g_ring *r, uint64_t at, const void *src, uint64_t n)
{
	uint64_t off = at % r->size, first = r->size - off < n ? r->size - off : n;

	memcpy(r->data + off, src, first);
	memcpy(r->data, (const uint8_t *)src + first, n - first);
}

static inline void g2g_ring_read(const struct g2g_ring *r, uint64_t at, void *dst, uint64_t n)
{
	uint64_t off = at % r->size, first = r->size - off < n ? r->size - off : n;

	memcpy(dst, r->data + off, first);
	memcpy((uint8_t *)dst + first, r->data, n - first);
}

/* Producer: 0, -EAGAIN when full, -EMSGSIZE, or -EPROTO if the peer's tail is bad. */
static inline int g2g_ring_put(struct g2g_ring *r, const void *buf, uint32_t len)
{
	uint64_t need = g2g_rec_space(len), tail, used;
	uint32_t hdr[2] = { len, 0 };

	if (need > r->size)
		return -EMSGSIZE;
	tail = __atomic_load_n(&r->ctl->tail, __ATOMIC_ACQUIRE);
	used = r->pos - tail;
	if (used > r->size || (used & 7))
		return -EPROTO;
	if (r->size - used < need)
		return -EAGAIN;
	g2g_ring_write(r, r->pos, hdr, sizeof(hdr));
	g2g_ring_write(r, r->pos + G2G_REC_HDR, buf, len);
	r->pos += need;
	__atomic_store_n(&r->ctl->head, r->pos, __ATOMIC_RELEASE);
	return 0;
}

/*
 * Consumer: copies the next record into buf and returns its length; -EAGAIN
 * when empty, -EMSGSIZE if it is longer than cap, -EPROTO if the peer's head
 * or the record header is bad.
 */
static inline ssize_t g2g_ring_get(struct g2g_ring *r, void *buf, size_t cap)
{
	uint64_t head, avail, need;
	uint32_t hdr[2];

	head = __atomic_load_n(&r->ctl->head, __ATOMIC_ACQUIRE);
	avail = head - r->pos;
	if (avail > r->size || (avail & 7))
		return -EPROTO;
	if (!avail)
		return -EAGAIN;
	g2g_ring_read(r, r->pos, hdr, sizeof(hdr));	/* copied once, then trusted */
	need = g2g_rec_space(hdr[0]);
	if (hdr[1] || need > avail)
		return -EPROTO;
	if (hdr[0] > cap)
		return -EMSGSIZE;
	g2g_ring_read(r, r->pos + G2G_REC_HDR, buf, hdr[0]);
	r->pos += need;
	__atomic_store_n(&r->ctl->tail, r->pos, __ATOMIC_RELEASE);
	return hdr[0];
}

#endif /* G2G_RING_H */
