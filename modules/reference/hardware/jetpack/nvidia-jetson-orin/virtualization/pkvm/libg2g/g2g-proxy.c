// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// g2g-proxy: carries local TCP connections from one VM to a service in another
// VM over libg2g channels, so the two VMs need no network between them.
//
//   g2g-proxy client <peer> <port>   accept connections on 127.0.0.1:<port> and
//                                    carry each one to <peer>
//   g2g-proxy server <port>          connect each channel from a listed peer to
//                                    127.0.0.1:<port>
//
// Options: -p <peers-file> (default /run/g2g/peers).
//
// One connection, one channel. The bytes are carried as they are; the proxy
// knows nothing of the protocol above (HTTP for Ollama). Every record starts
// with a type byte: 'D' and the data, or 'E' alone for end of stream (the
// sender's side of the TCP connection was shut down). A connection is done when
// both directions have ended; then the client closes the channel. The server
// never closes first on success, because the client drops whatever it has not
// yet read when the server's close arrives.
//
// The server side only ever connects to its own 127.0.0.1:<port>: a peer cannot
// choose where its bytes go. libg2g is one process per VM and one thread, so
// connections are carried one at a time; a waiting client connection stays in
// the listen backlog.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "g2g.h"

#define PROXY_PAGES	64	/* a ~126 KiB ring each way */
#define CONNECT_MS	5000
#define SEND_MS		30000
#define DRAIN_MS	5000	/* server: how long to wait for the client to close */
#define SPIN_PASSES	2000	/* idle passes before sleeping 1 ms per pass */

#define REC_DATA	'D'
#define REC_END		'E'

struct stats {
	unsigned long long up, down;	/* bytes TCP -> channel, channel -> TCP */
};

struct bufs {
	uint8_t *tx, *rx;		/* one record each */
};

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t k = write(fd, p, n);

		if (k < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += k;
		n -= k;
	}
	return 0;
}

/*
 * Moves bytes both ways between the TCP socket and the channel until both
 * directions have ended. Returns 0, or a negative errno: -EPIPE when the
 * channel is gone, anything else for a TCP or protocol error.
 */
static int relay(struct g2g_chan *c, int fd, struct bufs *b, struct stats *st)
{
	uint8_t *txbuf = b->tx, *rxbuf = b->rx;
	size_t max = g2g_chan_max_record(c);
	int tcp_done = 0, chan_done = 0;
	unsigned int idle = 0;

	while (!tcp_done || !chan_done) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int busy = 0, timeout = idle < SPIN_PASSES ? 0 : 1, pr;
		ssize_t r;

		/* TCP -> channel. With nothing to poll, poll() just sleeps. */
		pr = poll(&p, tcp_done ? 0 : 1, timeout);
		if (pr < 0 && errno != EINTR)
			return -errno;
		if (!tcp_done && pr > 0 && p.revents) {
			ssize_t n = read(fd, txbuf + 1, max - 1);

			if (n < 0 && errno != EINTR && errno != EAGAIN)
				return -errno;
			if (n >= 0) {
				int ret;

				txbuf[0] = n ? REC_DATA : REC_END;
				ret = g2g_chan_send(c, txbuf, n + 1, SEND_MS);
				if (ret)
					return ret;
				st->up += n;
				tcp_done = !n;
				busy = 1;
			}
		}

		/* Channel -> TCP: take whatever is waiting, without blocking. */
		r = g2g_chan_recv(c, rxbuf, G2G_MAX_RECORD, 0);
		if (r >= 0) {
			int ret = 0;

			if (r < 1 || chan_done)
				return -EPROTO;
			if (rxbuf[0] == REC_DATA) {
				ret = write_all(fd, rxbuf + 1, r - 1);
				st->down += r - 1;
			} else if (rxbuf[0] == REC_END && r == 1) {
				shutdown(fd, SHUT_WR);
				chan_done = 1;
			} else {
				return -EPROTO;
			}
			if (ret)
				return ret;
			busy = 1;
		} else if (r != -ETIMEDOUT) {
			return (int)r;
		}

		idle = busy ? 0 : idle + 1;
	}
	return 0;
}

static int run_client(struct g2g *g, struct bufs *b, const char *peer, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int one = 1, lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	unsigned long nconn = 0;

	if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
	    bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 16)) {
		perror("g2g-proxy client: listen");
		return 1;
	}
	fprintf(stderr, "g2g-proxy: 127.0.0.1:%d -> %s\n", port, peer);
	for (;;) {
		struct g2g_chan *c;
		struct stats st = { 0 };
		int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC), ret;

		if (fd < 0) {
			if (errno != EINTR)
				perror("g2g-proxy client: accept");
			continue;
		}
		nconn++;
		ret = g2g_chan_connect(g, peer, PROXY_PAGES, &c, CONNECT_MS);
		if (ret) {
			fprintf(stderr, "g2g-proxy: connection %lu: no channel to %s: %s\n", nconn,
				peer, strerror(-ret));
			close(fd);
			continue;
		}
		ret = relay(c, fd, b, &st);
		fprintf(stderr, "g2g-proxy: connection %lu: %llu bytes to %s, %llu back, %s\n",
			nconn, st.up, peer, st.down, ret ? strerror(-ret) : "done");
		close(fd);
		g2g_chan_close(c);
	}
}

static int run_server(struct g2g *g, struct bufs *b, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	unsigned long nconn = 0;

	fprintf(stderr, "g2g-proxy: listed peers -> 127.0.0.1:%d\n", port);
	for (;;) {
		struct g2g_chan *c;
		struct stats st = { 0 };
		int fd, ret = g2g_chan_listen(g, &c, -1);

		if (ret) {
			fprintf(stderr, "g2g-proxy: listen: %s\n", strerror(-ret));
			sleep(1);
			continue;
		}
		nconn++;
		fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
			fprintf(stderr, "g2g-proxy: connection %lu from %s: 127.0.0.1:%d: %s\n", nconn,
				g2g_chan_peer(c), port, strerror(errno));
			if (fd >= 0)
				close(fd);
			g2g_chan_close(c);	/* nothing sent yet: the client loses nothing */
			continue;
		}
		ret = relay(c, fd, b, &st);
		fprintf(stderr, "g2g-proxy: connection %lu from %s: %llu bytes in, %llu out, %s\n",
			nconn, g2g_chan_peer(c), st.down, st.up,
			ret ? strerror(-ret) : "done");
		close(fd);
		if (!ret) {
			/* Let the client read our last records and close first. */
			ssize_t r;

			do
				r = g2g_chan_recv(c, b->rx, G2G_MAX_RECORD, DRAIN_MS);
			while (r >= 0);
		}
		g2g_chan_close(c);
	}
	return 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: g2g-proxy [-p peers] client <peer> <port>\n"
			"       g2g-proxy [-p peers] server <port>\n");
	return 2;
}

static int parse_port(const char *s)
{
	char *end;
	long p = strtol(s, &end, 10);

	return *s && !*end && p > 0 && p < 65536 ? (int)p : -1;
}

/* Runs one side; argv is "client <peer> <port>" or "server <port>". */
#ifndef G2G_PROXY_TEST
static
#endif
int proxy(const char *peers, int argc, char **argv)
{
	struct bufs b = { malloc(G2G_MAX_RECORD), malloc(G2G_MAX_RECORD) };
	struct g2g *g;
	int ret, port;

	if (argc == 3 && !strcmp(argv[0], "client"))
		port = parse_port(argv[2]);
	else if (argc == 2 && !strcmp(argv[0], "server"))
		port = parse_port(argv[1]);
	else
		return usage();
	if (port < 0)
		return usage();

	if (!b.tx || !b.rx)
		return 1;
	signal(SIGPIPE, SIG_IGN);	/* a closed socket is an EPIPE from write() */
	ret = g2g_open(&g, peers);
	if (ret) {
		fprintf(stderr, "g2g_open: %s\n", strerror(-ret));
		return 1;
	}
	return argv[0][0] == 'c' ? run_client(g, &b, argv[1], port) : run_server(g, &b, port);
}

#ifndef G2G_PROXY_TEST
int main(int argc, char **argv)
{
	const char *peers = G2G_PEERS_DEFAULT;
	int opt;

	while ((opt = getopt(argc, argv, "p:")) != -1) {
		if (opt != 'p')
			return usage();
		peers = optarg;
	}
	return proxy(peers, argc - optind, argv + optind);
}
#endif
