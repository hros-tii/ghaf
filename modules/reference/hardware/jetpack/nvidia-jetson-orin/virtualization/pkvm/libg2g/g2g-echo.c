// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// g2g-echo: tests libg2g channels between two VMs.
//
//   g2g-echo id                                   this VM's identity and authority hash
//   g2g-echo server                               echo every record back, forever
//   g2g-echo client <peer> [size] [count] [pages] send count records of size bytes,
//                                                 check the echo, report the rate
//
// Options: -p <peers-file> (default /run/g2g/peers).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "g2g.h"

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int run_server(struct g2g *g)
{
	uint8_t *buf = malloc(G2G_MAX_RECORD);

	if (!buf)
		return 1;
	for (;;) {
		struct g2g_chan *c;
		unsigned long n = 0;
		ssize_t len;
		int ret = g2g_chan_listen(g, &c, -1);

		if (ret) {
			fprintf(stderr, "listen: %s\n", strerror(-ret));
			return 1;
		}
		printf("channel from %s, %zu-byte records\n", g2g_chan_peer(c), g2g_chan_max_record(c));
		while ((len = g2g_chan_recv(c, buf, G2G_MAX_RECORD, -1)) >= 0) {
			ret = g2g_chan_send(c, buf, len, 5000);
			if (ret) {
				len = ret;
				break;
			}
			n++;
		}
		printf("channel from %s closed after %lu records: %s\n", g2g_chan_peer(c), n,
		       strerror((int)-len));
		g2g_chan_close(c);
	}
}

static int run_client(struct g2g *g, const char *peer, size_t size, unsigned long count,
		      unsigned int pages)
{
	struct g2g_chan *c;
	uint8_t *out, *in;
	double t0, t;
	int ret = g2g_chan_connect(g, peer, pages, &c, 10000);

	if (ret) {
		fprintf(stderr, "connect to %s: %s\n", peer, strerror(-ret));
		return 1;
	}
	if (size > g2g_chan_max_record(c)) {
		fprintf(stderr, "size %zu > %zu, the most a %u-page channel carries\n", size,
			g2g_chan_max_record(c), pages);
		g2g_chan_close(c);
		return 1;
	}
	out = malloc(size ? size : 1);
	in = malloc(size ? size : 1);
	t0 = now_s();
	for (unsigned long i = 0; i < count; i++) {
		ssize_t len;

		for (size_t k = 0; k < size; k++)
			out[k] = (uint8_t)(i * 31 + k);
		ret = g2g_chan_send(c, out, size, 5000);
		if (ret) {
			fprintf(stderr, "send %lu: %s\n", i, strerror(-ret));
			break;
		}
		len = g2g_chan_recv(c, in, size, 5000);
		if (len < 0) {
			ret = (int)len;
			fprintf(stderr, "recv %lu: %s\n", i, strerror(-ret));
			break;
		}
		if ((size_t)len != size || memcmp(in, out, size)) {
			fprintf(stderr, "echo %lu differs\n", i);
			ret = -EBADMSG;
			break;
		}
	}
	t = now_s() - t0;
	if (!ret)
		printf("%lu round trips of %zu bytes with %s: %.1f us each, %.1f MB/s each way\n",
		       count, size, peer, t / count * 1e6, count * size / t / 1e6);
	g2g_chan_close(c);
	return ret ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *peers = NULL;
	struct g2g *g;
	int opt, ret;

	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((opt = getopt(argc, argv, "p:")) != -1) {
		if (opt != 'p')
			goto usage;
		peers = optarg;
	}
	if (optind >= argc)
		goto usage;
	ret = g2g_open(&g, peers);
	if (ret) {
		fprintf(stderr, "g2g_open: %s\n", strerror(-ret));
		return 1;
	}
	if (!strcmp(argv[optind], "id")) {
		const struct pkvm_g2g_ident *me = g2g_self(g);

		printf("this VM: %#llx:%#llx, incarnation %llu\nauthority: ",
		       (unsigned long long)me->id_hi, (unsigned long long)me->id_lo,
		       (unsigned long long)me->incarnation);
		for (int i = 0; i < 64; i++)
			printf("%02x", g2g_auth_hash(g)[i]);
		printf("\n");
		ret = 0;
	} else if (!strcmp(argv[optind], "server")) {
		ret = run_server(g);
	} else if (!strcmp(argv[optind], "client") && optind + 1 < argc) {
		const char *peer = argv[optind + 1];
		size_t size = optind + 2 < argc ? strtoul(argv[optind + 2], NULL, 0) : 4096;
		unsigned long count = optind + 3 < argc ? strtoul(argv[optind + 3], NULL, 0) : 1000;
		unsigned int pages = optind + 4 < argc ? strtoul(argv[optind + 4], NULL, 0) : 64;

		ret = run_client(g, peer, size, count, pages);
	} else {
		g2g_close(g);
		goto usage;
	}
	g2g_close(g);
	return ret;
usage:
	fprintf(stderr, "usage: %s [-p peers] id | server | client <peer> [size] [count] [pages]\n",
		argv[0]);
	return 2;
}
