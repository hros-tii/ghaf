/* SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g2g-cuda: the CUDA Driver API, forwarded over a libg2g channel.
 *
 * Every call is one request record and one response record. A request is a
 * struct req and an optional payload (a module image, a kernel name, host
 * data, kernel parameters); a response is a struct rsp and an optional
 * payload (a device name, device data, kernel parameter layout). Handles are
 * opaque 64-bit ids the server maps back to its own objects; device pointers
 * are the server's real device addresses, checked against its allocations
 * before every copy and free.
 *
 * The transport is a libg2g channel, or, for tests, a pair of file
 * descriptors (G2G_CUDA_FD=<rfd>,<wfd>) carrying length-prefixed records.
 */
#ifndef G2G_CUDA_PROTO_H
#define G2G_CUDA_PROTO_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "g2g.h"

#define G2G_CUDA_PAGES		512	/* the largest channel: ~1 MiB records */
#define G2G_CUDA_SERVER		"gui-vm"
#define G2G_CUDA_MAX_PARAMS	64
#define G2G_CUDA_NAME_MAX	256

enum op {
	OP_INIT = 1,		/* a0 flags */
	OP_DRIVER_VERSION,	/* -> r0 */
	OP_DEVICE_COUNT,	/* -> r0 */
	OP_DEVICE_GET,		/* a0 ordinal -> r0 device */
	OP_DEVICE_NAME,		/* a0 device -> payload: name */
	OP_DEVICE_ATTR,		/* a0 attribute, a1 device -> r0 */
	OP_DEVICE_TOTALMEM,	/* a0 device -> r0 */
	OP_PRIMARY_RETAIN,	/* a0 device -> r0 context */
	OP_PRIMARY_RELEASE,	/* a0 device */
	OP_CTX_SET_CURRENT,	/* a0 context (0: none) */
	OP_CTX_SYNC,
	OP_MODULE_LOAD,		/* payload: image -> r0 module */
	OP_MODULE_UNLOAD,	/* a0 module */
	OP_FUNCTION_GET,	/* a0 module, payload: name -> r0 function, r1 params,
				   payload: struct param_info[r1] */
	OP_MEM_ALLOC,		/* a0 bytes -> r0 device pointer */
	OP_MEM_FREE,		/* a0 device pointer */
	OP_MEMCPY_HTOD,		/* a0 device pointer, payload: data */
	OP_MEMCPY_DTOH,		/* a0 device pointer, a1 bytes -> payload: data */
	OP_LAUNCH,		/* a0 function, a1..a3 grid, a4..a6 block, a7 shared bytes,
				   payload: the kernel parameter buffer */
};

struct req {
	uint32_t op, seq;
	uint64_t a[8];
};

struct rsp {
	uint32_t op, seq;
	int32_t status;		/* a CUresult */
	uint32_t pad;
	uint64_t r[2];
};

struct param_info {
	uint32_t offset, size;
};

/* --- transport ----------------------------------------------------------- */

struct xport {
	struct g2g *g;
	struct g2g_chan *c;
	int rfd, wfd;		/* tests only */
	size_t max;		/* largest record */
};

static int fd_full(int fd, void *buf, size_t n, int write_it)
{
	while (n) {
		ssize_t k = write_it ? write(fd, buf, n) : read(fd, buf, n);

		if (k <= 0)
			return k < 0 ? -errno : -EPIPE;
		buf = (char *)buf + k;
		n -= k;
	}
	return 0;
}

/* Test transport if G2G_CUDA_FD is set; 1 if it is, 0 if not, <0 on error. */
static int xport_from_env(struct xport *x)
{
	const char *e = getenv("G2G_CUDA_FD");

	if (!e)
		return 0;
	if (sscanf(e, "%d,%d", &x->rfd, &x->wfd) != 2)
		return -EINVAL;
	x->g = NULL;
	x->c = NULL;
	x->max = 1 << 20;
	return 1;
}

static int xport_send(struct xport *x, const void *buf, size_t len)
{
	uint32_t n = len;
	int ret;

	if (x->c)
		return g2g_chan_send(x->c, buf, len, 30000);
	ret = fd_full(x->wfd, &n, sizeof(n), 1);
	return ret ? ret : fd_full(x->wfd, (void *)buf, len, 1);
}

static ssize_t xport_recv(struct xport *x, void *buf, size_t cap, int timeout_ms)
{
	uint32_t n;
	int ret;

	if (x->c)
		return g2g_chan_recv(x->c, buf, cap, timeout_ms);
	ret = fd_full(x->rfd, &n, sizeof(n), 0);
	if (ret)
		return ret;
	if (n > cap)
		return -EMSGSIZE;
	ret = fd_full(x->rfd, buf, n, 0);
	return ret ? ret : (ssize_t)n;
}

#endif /* G2G_CUDA_PROTO_H */
