// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// g2g-cuda-server: runs the CUDA Driver API calls a client VM forwards over a
// libg2g channel, on this VM's GPU. One client at a time; everything the
// client created is freed when its channel ends.
//
// Every handle and device pointer the client sends is checked against what
// it created. Device pointers inside kernel parameters cannot be checked: a
// client can make its kernels touch any memory of the server's CUDA context,
// which holds only this client's allocations.
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext, *CUmodule, *CUfunction, *CUstream;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS			0
#define CUDA_ERROR_INVALID_VALUE	1
#define CUDA_ERROR_OUT_OF_MEMORY	2
#define CUDA_ERROR_INVALID_CONTEXT	201
#define CUDA_ERROR_INVALID_HANDLE	400
#define CUDA_ERROR_NOT_SUPPORTED	801

static struct {
	CUresult (*Init)(unsigned int);
	CUresult (*DriverGetVersion)(int *);
	CUresult (*DeviceGetCount)(int *);
	CUresult (*DeviceGet)(CUdevice *, int);
	CUresult (*DeviceGetName)(char *, int, CUdevice);
	CUresult (*DeviceGetAttribute)(int *, int, CUdevice);
	CUresult (*DeviceTotalMem)(size_t *, CUdevice);
	CUresult (*PrimaryCtxRetain)(CUcontext *, CUdevice);
	CUresult (*PrimaryCtxRelease)(CUdevice);
	CUresult (*CtxSetCurrent)(CUcontext);
	CUresult (*CtxSynchronize)(void);
	CUresult (*ModuleLoadData)(CUmodule *, const void *);
	CUresult (*ModuleUnload)(CUmodule);
	CUresult (*ModuleGetFunction)(CUfunction *, CUmodule, const char *);
	CUresult (*FuncGetParamInfo)(CUfunction, size_t, size_t *, size_t *);
	CUresult (*MemAlloc)(CUdeviceptr *, size_t);
	CUresult (*MemFree)(CUdeviceptr);
	CUresult (*MemcpyHtoD)(CUdeviceptr, const void *, size_t);
	CUresult (*MemcpyDtoH)(void *, CUdeviceptr, size_t);
	CUresult (*LaunchKernel)(CUfunction, unsigned int, unsigned int, unsigned int,
				 unsigned int, unsigned int, unsigned int, unsigned int,
				 CUstream, void **, void **);
} cu;

static const struct sym {
	void **fn;
	const char *names[3];	/* tried in order */
	int optional;
} syms[] = {
	{ .fn = (void **)&cu.Init, .names = { "cuInit" } },
	{ .fn = (void **)&cu.DriverGetVersion, .names = { "cuDriverGetVersion" } },
	{ .fn = (void **)&cu.DeviceGetCount, .names = { "cuDeviceGetCount" } },
	{ .fn = (void **)&cu.DeviceGet, .names = { "cuDeviceGet" } },
	{ .fn = (void **)&cu.DeviceGetName, .names = { "cuDeviceGetName" } },
	{ .fn = (void **)&cu.DeviceGetAttribute, .names = { "cuDeviceGetAttribute" } },
	{ .fn = (void **)&cu.DeviceTotalMem, .names = { "cuDeviceTotalMem_v2", "cuDeviceTotalMem" } },
	{ .fn = (void **)&cu.PrimaryCtxRetain, .names = { "cuDevicePrimaryCtxRetain" } },
	{ .fn = (void **)&cu.PrimaryCtxRelease, .names = { "cuDevicePrimaryCtxRelease_v2", "cuDevicePrimaryCtxRelease" } },
	{ .fn = (void **)&cu.CtxSetCurrent, .names = { "cuCtxSetCurrent" } },
	{ .fn = (void **)&cu.CtxSynchronize, .names = { "cuCtxSynchronize" } },
	{ .fn = (void **)&cu.ModuleLoadData, .names = { "cuModuleLoadData" } },
	{ .fn = (void **)&cu.ModuleUnload, .names = { "cuModuleUnload" } },
	{ .fn = (void **)&cu.ModuleGetFunction, .names = { "cuModuleGetFunction" } },
	{ .fn = (void **)&cu.FuncGetParamInfo, .names = { "cuFuncGetParamInfo" }, .optional = 1 },
	{ .fn = (void **)&cu.MemAlloc, .names = { "cuMemAlloc_v2", "cuMemAlloc" } },
	{ .fn = (void **)&cu.MemFree, .names = { "cuMemFree_v2", "cuMemFree" } },
	{ .fn = (void **)&cu.MemcpyHtoD, .names = { "cuMemcpyHtoD_v2", "cuMemcpyHtoD" } },
	{ .fn = (void **)&cu.MemcpyDtoH, .names = { "cuMemcpyDtoH_v2", "cuMemcpyDtoH" } },
	{ .fn = (void **)&cu.LaunchKernel, .names = { "cuLaunchKernel" } },
};

static int load_libcuda(void)
{
	const char *paths[] = { getenv("G2G_LIBCUDA"), "/run/current-system/sw/lib/libcuda.so.1",
				"libcuda.so.1" };
	void *h = NULL;

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && !h; i++)
		if (paths[i])
			h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "cannot load libcuda.so.1: %s\n", dlerror());
		return -1;
	}
	for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
		for (int k = 0; k < 3 && syms[i].names[k] && !*syms[i].fn; k++)
			*syms[i].fn = dlsym(h, syms[i].names[k]);
		if (!*syms[i].fn && !syms[i].optional) {
			fprintf(stderr, "libcuda.so.1 has no %s\n", syms[i].names[0]);
			return -1;
		}
	}
	return 0;
}

/*
 * Initialising CUDA and creating the primary context takes tens of seconds on
 * Orin, so do it once at start and keep the context retained: a client's
 * retain then only bumps its reference count.
 */
static int warm_up(void)
{
	CUcontext c;
	CUdevice d;
	CUresult st = cu.Init(0);

	if (!st)
		st = cu.DeviceGet(&d, 0);
	if (!st)
		st = cu.PrimaryCtxRetain(&c, d);
	if (st) {
		fprintf(stderr, "CUDA warm-up failed: error %d\n", st);
		return -1;
	}
	printf("CUDA ready\n");
	return 0;
}

/* --- what the client created ---------------------------------------------- */

#define MAX_CTX		16
#define MAX_MOD		256
#define MAX_FUNC	1024

static struct {
	CUcontext ctx;
	CUdevice dev;
} ctxs[MAX_CTX];

static CUmodule mods[MAX_MOD];

static struct {
	CUfunction fn;
	uint64_t mod;
	uint32_t nparams, param_bytes;
	struct param_info params[G2G_CUDA_MAX_PARAMS];
} funcs[MAX_FUNC];

static struct alloc {
	CUdeviceptr ptr;
	uint64_t size;
} *allocs;
static size_t nallocs, cap_allocs;

/* id = index + 1, 0 is never valid */
#define LOOKUP(table, max, id, field) \
	((id) >= 1 && (id) <= (max) && table[(id) - 1].field ? &table[(id) - 1] : NULL)

static struct alloc *find_alloc(uint64_t ptr, uint64_t len, int exact)
{
	for (size_t i = 0; i < nallocs; i++) {
		struct alloc *a = &allocs[i];

		if (exact ? ptr == a->ptr
			  : ptr >= a->ptr && len <= a->size && ptr - a->ptr <= a->size - len)
			return a;
	}
	return NULL;
}

static void session_cleanup(void)
{
	for (size_t i = 0; i < nallocs; i++)
		cu.MemFree(allocs[i].ptr);
	nallocs = 0;
	for (int i = 0; i < MAX_MOD; i++)
		if (mods[i]) {
			cu.ModuleUnload(mods[i]);
			mods[i] = NULL;
		}
	memset(funcs, 0, sizeof(funcs));
	cu.CtxSetCurrent(NULL);
	for (int i = 0; i < MAX_CTX; i++)
		if (ctxs[i].ctx) {
			cu.PrimaryCtxRelease(ctxs[i].dev);
			ctxs[i].ctx = NULL;
		}
}

/* --- one request ------------------------------------------------------------ */

static struct xport x;
static uint8_t *rxbuf, *txbuf;

static int reply(const struct req *q, CUresult st, uint64_t r0, uint64_t r1, const void *p, size_t plen)
{
	struct rsp *s = (struct rsp *)txbuf;

	*s = (struct rsp){ .op = q->op, .seq = q->seq, .status = st, .r = { r0, r1 } };
	if (p && plen)
		memmove(txbuf + sizeof(*s), p, plen);
	return xport_send(&x, txbuf, sizeof(*s) + plen);
}

static int handle(const struct req *q, const uint8_t *p, size_t plen)
{
	size_t room = x.max - sizeof(struct rsp);
	CUresult st;

	switch (q->op) {
	case OP_INIT:
		return reply(q, cu.Init(q->a[0]), 0, 0, NULL, 0);
	case OP_DRIVER_VERSION: {
		int v = 0;

		st = cu.DriverGetVersion(&v);
		return reply(q, st, v, 0, NULL, 0);
	}
	case OP_DEVICE_COUNT: {
		int n = 0;

		st = cu.DeviceGetCount(&n);
		return reply(q, st, n, 0, NULL, 0);
	}
	case OP_DEVICE_GET: {
		CUdevice d = 0;

		st = cu.DeviceGet(&d, (int)q->a[0]);
		return reply(q, st, d, 0, NULL, 0);
	}
	case OP_DEVICE_NAME: {
		char name[G2G_CUDA_NAME_MAX] = "";

		st = cu.DeviceGetName(name, sizeof(name), (CUdevice)q->a[0]);
		name[sizeof(name) - 1] = '\0';
		return reply(q, st, 0, 0, name, strlen(name) + 1);
	}
	case OP_DEVICE_ATTR: {
		int v = 0;

		st = cu.DeviceGetAttribute(&v, (int)q->a[0], (CUdevice)q->a[1]);
		return reply(q, st, (uint64_t)(int64_t)v, 0, NULL, 0);
	}
	case OP_DEVICE_TOTALMEM: {
		size_t b = 0;

		st = cu.DeviceTotalMem(&b, (CUdevice)q->a[0]);
		return reply(q, st, b, 0, NULL, 0);
	}
	case OP_PRIMARY_RETAIN: {
		CUcontext c = NULL;

		for (int i = 0; i < MAX_CTX; i++)
			if (!ctxs[i].ctx) {
				st = cu.PrimaryCtxRetain(&c, (CUdevice)q->a[0]);
				if (st == CUDA_SUCCESS) {
					ctxs[i].ctx = c;
					ctxs[i].dev = (CUdevice)q->a[0];
				}
				return reply(q, st, st ? 0 : i + 1, 0, NULL, 0);
			}
		return reply(q, CUDA_ERROR_OUT_OF_MEMORY, 0, 0, NULL, 0);
	}
	case OP_PRIMARY_RELEASE:
		for (int i = 0; i < MAX_CTX; i++)
			if (ctxs[i].ctx && ctxs[i].dev == (CUdevice)q->a[0]) {
				ctxs[i].ctx = NULL;
				return reply(q, cu.PrimaryCtxRelease(ctxs[i].dev), 0, 0, NULL, 0);
			}
		return reply(q, CUDA_ERROR_INVALID_CONTEXT, 0, 0, NULL, 0);
	case OP_CTX_SET_CURRENT: {
		__typeof__(&ctxs[0]) c = LOOKUP(ctxs, MAX_CTX, q->a[0], ctx);

		if (q->a[0] && !c)
			return reply(q, CUDA_ERROR_INVALID_CONTEXT, 0, 0, NULL, 0);
		return reply(q, cu.CtxSetCurrent(c ? c->ctx : NULL), 0, 0, NULL, 0);
	}
	case OP_CTX_SYNC:
		return reply(q, cu.CtxSynchronize(), 0, 0, NULL, 0);
	case OP_MODULE_LOAD: {
		CUmodule m = NULL;
		uint8_t *img;

		for (int i = 0; i < MAX_MOD; i++)
			if (!mods[i]) {
				/* a private, aligned copy with a NUL after it, for PTX */
				img = malloc(plen + 1);
				if (!img)
					return reply(q, CUDA_ERROR_OUT_OF_MEMORY, 0, 0, NULL, 0);
				memcpy(img, p, plen);
				img[plen] = '\0';
				st = cu.ModuleLoadData(&m, img);
				free(img);
				if (st == CUDA_SUCCESS)
					mods[i] = m;
				return reply(q, st, st ? 0 : i + 1, 0, NULL, 0);
			}
		return reply(q, CUDA_ERROR_OUT_OF_MEMORY, 0, 0, NULL, 0);
	}
	case OP_MODULE_UNLOAD: {
		CUmodule *m = q->a[0] >= 1 && q->a[0] <= MAX_MOD ? &mods[q->a[0] - 1] : NULL;

		if (!m || !*m)
			return reply(q, CUDA_ERROR_INVALID_HANDLE, 0, 0, NULL, 0);
		st = cu.ModuleUnload(*m);
		*m = NULL;
		for (int i = 0; i < MAX_FUNC; i++)
			if (funcs[i].fn && funcs[i].mod == q->a[0])
				funcs[i].fn = NULL;
		return reply(q, st, 0, 0, NULL, 0);
	}
	case OP_FUNCTION_GET: {
		CUmodule *m = q->a[0] >= 1 && q->a[0] <= MAX_MOD ? &mods[q->a[0] - 1] : NULL;
		char name[G2G_CUDA_NAME_MAX];
		CUfunction f = NULL;

		if (!m || !*m)
			return reply(q, CUDA_ERROR_INVALID_HANDLE, 0, 0, NULL, 0);
		if (!plen || plen > sizeof(name) || p[plen - 1] != '\0')
			return reply(q, CUDA_ERROR_INVALID_VALUE, 0, 0, NULL, 0);
		memcpy(name, p, plen);
		if (!cu.FuncGetParamInfo)
			return reply(q, CUDA_ERROR_NOT_SUPPORTED, 0, 0, NULL, 0);
		for (int i = 0; i < MAX_FUNC; i++)
			if (!funcs[i].fn) {
				uint32_t n = 0, bytes = 0;
				size_t off, size;

				st = cu.ModuleGetFunction(&f, *m, name);
				if (st)
					return reply(q, st, 0, 0, NULL, 0);
				while (n < G2G_CUDA_MAX_PARAMS &&
				       cu.FuncGetParamInfo(f, n, &off, &size) == CUDA_SUCCESS) {
					funcs[i].params[n] = (struct param_info){ off, size };
					if (off + size > bytes)
						bytes = off + size;
					n++;
				}
				funcs[i].fn = f;
				funcs[i].mod = q->a[0];
				funcs[i].nparams = n;
				funcs[i].param_bytes = bytes;
				return reply(q, CUDA_SUCCESS, i + 1, n, funcs[i].params,
					     n * sizeof(struct param_info));
			}
		return reply(q, CUDA_ERROR_OUT_OF_MEMORY, 0, 0, NULL, 0);
	}
	case OP_MEM_ALLOC: {
		CUdeviceptr d = 0;

		if (nallocs == cap_allocs) {
			size_t n = cap_allocs ? 2 * cap_allocs : 64;
			struct alloc *a = realloc(allocs, n * sizeof(*a));

			if (!a)
				return reply(q, CUDA_ERROR_OUT_OF_MEMORY, 0, 0, NULL, 0);
			allocs = a;
			cap_allocs = n;
		}
		st = cu.MemAlloc(&d, q->a[0]);
		if (st == CUDA_SUCCESS)
			allocs[nallocs++] = (struct alloc){ d, q->a[0] };
		return reply(q, st, d, 0, NULL, 0);
	}
	case OP_MEM_FREE: {
		struct alloc *a = find_alloc(q->a[0], 0, 1);

		if (!a)
			return reply(q, CUDA_ERROR_INVALID_VALUE, 0, 0, NULL, 0);
		st = cu.MemFree(a->ptr);
		*a = allocs[--nallocs];
		return reply(q, st, 0, 0, NULL, 0);
	}
	case OP_MEMCPY_HTOD:
		if (!find_alloc(q->a[0], plen, 0))
			return reply(q, CUDA_ERROR_INVALID_VALUE, 0, 0, NULL, 0);
		return reply(q, cu.MemcpyHtoD(q->a[0], p, plen), 0, 0, NULL, 0);
	case OP_MEMCPY_DTOH: {
		uint8_t *out = txbuf + sizeof(struct rsp);

		if (q->a[1] > room || !find_alloc(q->a[0], q->a[1], 0))
			return reply(q, CUDA_ERROR_INVALID_VALUE, 0, 0, NULL, 0);
		st = cu.MemcpyDtoH(out, q->a[0], q->a[1]);
		return reply(q, st, 0, 0, NULL, st ? 0 : q->a[1]);	/* data already in place */
	}
	case OP_LAUNCH: {
		__typeof__(&funcs[0]) f = LOOKUP(funcs, MAX_FUNC, q->a[0], fn);
		void *args[G2G_CUDA_MAX_PARAMS];

		if (!f)
			return reply(q, CUDA_ERROR_INVALID_HANDLE, 0, 0, NULL, 0);
		if (plen != f->param_bytes)
			return reply(q, CUDA_ERROR_INVALID_VALUE, 0, 0, NULL, 0);
		for (int i = 1; i <= 6; i++)
			if (!q->a[i] || q->a[i] > 0x7fffffff)
				return reply(q, CUDA_ERROR_INVALID_VALUE, 0, 0, NULL, 0);
		for (uint32_t i = 0; i < f->nparams; i++)
			args[i] = (void *)(p + f->params[i].offset);
		st = cu.LaunchKernel(f->fn, q->a[1], q->a[2], q->a[3], q->a[4], q->a[5], q->a[6],
				     q->a[7], NULL, f->nparams ? args : NULL, NULL);
		return reply(q, st, 0, 0, NULL, 0);
	}
	}
	return reply(q, CUDA_ERROR_NOT_SUPPORTED, 0, 0, NULL, 0);
}

static void session(const char *who)
{
	unsigned long n = 0;
	ssize_t len;

	printf("client %s connected\n", who);
	while ((len = xport_recv(&x, rxbuf, x.max, -1)) >= 0) {
		const struct req *q = (const struct req *)rxbuf;
		uint8_t *payload;

		if ((size_t)len < sizeof(*q)) {
			len = -EPROTO;
			break;
		}
		/* the request is ours now: rxbuf is private, the ring copy is done */
		payload = rxbuf + sizeof(*q);
		if (handle(q, payload, len - sizeof(*q)))
			break;
		n++;
	}
	session_cleanup();
	printf("client %s gone after %lu calls (%s)\n", who, n, strerror((int)(len < 0 ? -len : 0)));
}

int main(void)
{
	int ret, test;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (load_libcuda())
		return 1;
	if (warm_up())
		return 1;
	test = xport_from_env(&x);
	if (test < 0)
		return 1;
	if (!test) {
		ret = g2g_open(&x.g, NULL);
		if (ret) {
			fprintf(stderr, "g2g_open: %s\n", strerror(-ret));
			return 1;
		}
	}
	rxbuf = malloc(G2G_MAX_RECORD);
	txbuf = malloc(G2G_MAX_RECORD);
	if (!rxbuf || !txbuf)
		return 1;
	if (test) {
		session("on test fds");
		return 0;
	}
	for (;;) {
		ret = g2g_chan_listen(x.g, &x.c, -1);
		if (ret) {
			fprintf(stderr, "listen: %s\n", strerror(-ret));
			return 1;
		}
		x.max = g2g_chan_max_record(x.c);
		session(g2g_chan_peer(x.c));
		g2g_chan_close(x.c);
		x.c = NULL;
	}
}
