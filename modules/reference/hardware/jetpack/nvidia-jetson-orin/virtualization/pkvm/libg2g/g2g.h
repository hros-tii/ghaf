/* SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libg2g: guest-to-guest channels between protected VMs, for one application
 * per VM.
 *
 * A VM's identity is the one pvmfw registers for it: the first 128 bits of
 * SHA-256("pkvm-g2g-id:" || authority hash || VM name), where the authority
 * hash (SHA-512 of the AVB key that signed the VM) is read from this VM's own
 * DICE certificate in /dev/open-dice0. Peers are named, and only the VMs in
 * the peers file (the whitelist) are ever talked to.
 *
 * A channel is one shared region the client offers to the server: a header
 * page and two byte rings, client->server and server->client, carrying
 * length-prefixed records. The pKVM mailbox only sets channels up and tears
 * them down. Both sides poll the rings.
 *
 * The peer can write the shared region at any time. Every record is copied
 * out before it is used, and every index the peer writes is bounds-checked;
 * a violation closes the channel. Callers must treat received data as
 * untrusted input too.
 *
 * The pKVM mailbox is per VM, so only one process per VM may use libg2g, from
 * one thread.
 *
 * Closing waits for the server to unmap before the client takes its pages
 * back. A client that revokes without waiting (one that crashes, or a hostile
 * one) makes the server's next access to the region raise SIGBUS, so a server
 * should run under systemd with Restart= until it handles that signal itself.
 *
 * Errors are returned as negative errno values.
 */
#ifndef G2G_H
#define G2G_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "pkvm_g2g.h"

#define G2G_PEERS_DEFAULT	"/run/g2g/peers"
#define G2G_MAX_RECORD		(1u << 20)	/* largest record a channel carries */

struct g2g;
struct g2g_chan;

/* Opens /dev/pkvm-g2g, reads the authority hash and loads the peers file. */
int g2g_open(struct g2g **g, const char *peers_path);
void g2g_close(struct g2g *g);

/* This VM's identity. */
const struct pkvm_g2g_ident *g2g_self(const struct g2g *g);

/* The authority hash from DICE, 64 bytes. */
const uint8_t *g2g_auth_hash(const struct g2g *g);

/* The identity of a listed peer, with its live incarnation (0 if not running). */
int g2g_peer(struct g2g *g, const char *name, struct pkvm_g2g_ident *id);

/*
 * Client: offers the listed peer `name` a channel of `nr_pages` pages (2..512)
 * and waits for it to be accepted. timeout_ms < 0 waits forever.
 */
int g2g_chan_connect(struct g2g *g, const char *name, unsigned int nr_pages,
		     struct g2g_chan **c, int timeout_ms);

/* Server: waits for a channel from any listed peer and accepts it. */
int g2g_chan_listen(struct g2g *g, struct g2g_chan **c, int timeout_ms);

/* The peer's name, from the peers file. */
const char *g2g_chan_peer(const struct g2g_chan *c);

/* Bytes one record may carry on this channel. */
size_t g2g_chan_max_record(const struct g2g_chan *c);

/* Queues one record, waiting while the ring is full. */
int g2g_chan_send(struct g2g_chan *c, const void *buf, size_t len, int timeout_ms);

/*
 * Copies the next record into buf and returns its length. A record longer
 * than cap is an error (-EMSGSIZE) and closes the channel.
 */
ssize_t g2g_chan_recv(struct g2g_chan *c, void *buf, size_t cap, int timeout_ms);

/* Tells the peer, unmaps and gives the shared pages back. */
void g2g_chan_close(struct g2g_chan *c);

#endif /* G2G_H */
