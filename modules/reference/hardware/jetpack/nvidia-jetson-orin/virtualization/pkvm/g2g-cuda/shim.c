// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// libcuda.so.1 for a VM without a GPU: the CUDA Driver API calls below are
// forwarded over a libg2g channel to g2g-cuda-server in the VM that has one
// (G2G_CUDA_SERVER, default gui-vm). Calls are serialised; the channel is
// opened by the first one.
//
// Covered: cuInit, cuDriverGetVersion, cuDeviceGet*, cuDeviceTotalMem,
// cuDevicePrimaryCtxRetain/Release, cuCtxSetCurrent, cuCtxSynchronize,
// cuModuleLoadData/Unload/GetFunction, cuFuncGetParamInfo, cuMemAlloc/Free,
// cuMemcpyHtoD/DtoH, cuLaunchKernel (kernelParams only, default stream
// semantics), cuGetErrorName/String. Anything else is not exported, so a
// program that needs more fails to load or to resolve it.
#include <pthread.h>
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
#define CUDA_ERROR_NOT_INITIALIZED	3
#define CUDA_ERROR_INVALID_IMAGE	200
#define CUDA_ERROR_INVALID_HANDLE	400
#define CUDA_ERROR_NOT_SUPPORTED	801
#define CUDA_ERROR_UNKNOWN		999

#define EXPORT __attribute__((visibility("default")))

struct rfunc {
	uint64_t id;
	uint32_t nparams, bytes;
	struct param_info params[G2G_CUDA_MAX_PARAMS];
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct xport x;
static int state;		/* 0: not tried, 1: connected, -1: failed */
static uint8_t *txbuf, *rxbuf;
static uint32_t seq;

static int connect_server(void)
{
	const char *server = getenv("G2G_CUDA_SERVER");
	int ret;

	if (state)
		return state > 0 ? 0 : -1;
	state = -1;
	txbuf = malloc(G2G_MAX_RECORD);
	rxbuf = malloc(G2G_MAX_RECORD);
	if (!txbuf || !rxbuf)
		return -1;
	ret = xport_from_env(&x);
	if (ret < 0)
		return -1;
	if (!ret) {
		ret = g2g_open(&x.g, NULL);
		if (!ret)
			ret = g2g_chan_connect(x.g, server ? server : G2G_CUDA_SERVER, G2G_CUDA_PAGES,
					       &x.c, 10000);
		if (ret) {
			fprintf(stderr, "g2g-cuda: no channel to %s: %s\n",
				server ? server : G2G_CUDA_SERVER, strerror(-ret));
			return -1;
		}
		x.max = g2g_chan_max_record(x.c);
	}
	state = 1;
	return 0;
}

/*
 * One request and its response. On return *payload points into rxbuf (valid
 * until the next call) and *plen is its length.
 */
static CUresult call(uint32_t op, const uint64_t a[8], const void *data, size_t dlen,
		     uint64_t r[2], const uint8_t **payload, size_t *plen)
{
	struct req *q;
	const struct rsp *s;
	ssize_t len;

	if (connect_server())
		return CUDA_ERROR_NOT_INITIALIZED;
	q = (struct req *)txbuf;
	s = (const struct rsp *)rxbuf;
	if (dlen > x.max - sizeof(*q))
		return CUDA_ERROR_INVALID_VALUE;
	*q = (struct req){ .op = op, .seq = ++seq };
	if (a)
		memcpy(q->a, a, sizeof(q->a));
	if (dlen)
		memcpy(txbuf + sizeof(*q), data, dlen);
	if (xport_send(&x, txbuf, sizeof(*q) + dlen))
		goto broken;
	len = xport_recv(&x, rxbuf, x.max, 60000);
	if (len < (ssize_t)sizeof(*s) || s->op != op || s->seq != seq)
		goto broken;
	if (r) {
		r[0] = s->r[0];
		r[1] = s->r[1];
	}
	if (payload) {
		*payload = rxbuf + sizeof(*s);
		*plen = len - sizeof(*s);
	}
	return s->status;
broken:
	fprintf(stderr, "g2g-cuda: lost the channel to the server\n");
	state = -1;
	return CUDA_ERROR_UNKNOWN;
}

/*
 * Close the channel properly when the program exits, so the server unmaps
 * before our pages are taken back (a plain exit revokes them under it).
 */
__attribute__((destructor)) static void shim_fini(void)
{
	pthread_mutex_lock(&lock);
	if (state > 0 && x.c) {
		g2g_chan_close(x.c);
		g2g_close(x.g);
		x.c = NULL;
		x.g = NULL;
		state = -1;
	}
	pthread_mutex_unlock(&lock);
}

#define LOCKED(expr) ({ CUresult _r; pthread_mutex_lock(&lock); _r = (expr); pthread_mutex_unlock(&lock); _r; })

static CUresult simple(uint32_t op, uint64_t a0, uint64_t a1, uint64_t *r0)
{
	uint64_t a[8] = { a0, a1 }, r[2];
	CUresult st = call(op, a, NULL, 0, r, NULL, NULL);

	if (r0 && st == CUDA_SUCCESS)
		*r0 = r[0];
	return st;
}

EXPORT CUresult cuInit(unsigned int flags)
{
	return LOCKED(simple(OP_INIT, flags, 0, NULL));
}

EXPORT CUresult cuDriverGetVersion(int *v)
{
	uint64_t r;
	CUresult st;

	if (!v)
		return CUDA_ERROR_INVALID_VALUE;
	st = LOCKED(simple(OP_DRIVER_VERSION, 0, 0, &r));
	if (!st)
		*v = (int)r;
	return st;
}

EXPORT CUresult cuDeviceGetCount(int *n)
{
	uint64_t r;
	CUresult st;

	if (!n)
		return CUDA_ERROR_INVALID_VALUE;
	st = LOCKED(simple(OP_DEVICE_COUNT, 0, 0, &r));
	if (!st)
		*n = (int)r;
	return st;
}

EXPORT CUresult cuDeviceGet(CUdevice *d, int ordinal)
{
	uint64_t r;
	CUresult st;

	if (!d)
		return CUDA_ERROR_INVALID_VALUE;
	st = LOCKED(simple(OP_DEVICE_GET, (uint64_t)ordinal, 0, &r));
	if (!st)
		*d = (CUdevice)r;
	return st;
}

static CUresult device_name(char *name, int len, CUdevice d)
{
	uint64_t a[8] = { (uint64_t)d };
	const uint8_t *p;
	size_t plen;
	CUresult st = call(OP_DEVICE_NAME, a, NULL, 0, NULL, &p, &plen);

	if (!st) {
		size_t n = plen < (size_t)len ? plen : (size_t)len;

		memcpy(name, p, n);
		name[n ? n - 1 : 0] = '\0';
	}
	return st;
}

EXPORT CUresult cuDeviceGetName(char *name, int len, CUdevice d)
{
	if (!name || len <= 0)
		return CUDA_ERROR_INVALID_VALUE;
	return LOCKED(device_name(name, len, d));
}

EXPORT CUresult cuDeviceGetAttribute(int *v, int attr, CUdevice d)
{
	uint64_t r;
	CUresult st;

	if (!v)
		return CUDA_ERROR_INVALID_VALUE;
	st = LOCKED(simple(OP_DEVICE_ATTR, (uint64_t)attr, (uint64_t)d, &r));
	if (!st)
		*v = (int)(int64_t)r;
	return st;
}

EXPORT CUresult cuDeviceTotalMem_v2(size_t *bytes, CUdevice d)
{
	uint64_t r;
	CUresult st;

	if (!bytes)
		return CUDA_ERROR_INVALID_VALUE;
	st = LOCKED(simple(OP_DEVICE_TOTALMEM, (uint64_t)d, 0, &r));
	if (!st)
		*bytes = r;
	return st;
}

EXPORT CUresult cuDeviceTotalMem(size_t *bytes, CUdevice d)
{
	return cuDeviceTotalMem_v2(bytes, d);
}

/* A context is (void *)(remote id). */
EXPORT CUresult cuDevicePrimaryCtxRetain(CUcontext *c, CUdevice d)
{
	uint64_t r;
	CUresult st;

	if (!c)
		return CUDA_ERROR_INVALID_VALUE;
	st = LOCKED(simple(OP_PRIMARY_RETAIN, (uint64_t)d, 0, &r));
	if (!st)
		*c = (CUcontext)(uintptr_t)r;
	return st;
}

EXPORT CUresult cuDevicePrimaryCtxRelease_v2(CUdevice d)
{
	return LOCKED(simple(OP_PRIMARY_RELEASE, (uint64_t)d, 0, NULL));
}

EXPORT CUresult cuDevicePrimaryCtxRelease(CUdevice d)
{
	return cuDevicePrimaryCtxRelease_v2(d);
}

EXPORT CUresult cuCtxSetCurrent(CUcontext c)
{
	return LOCKED(simple(OP_CTX_SET_CURRENT, (uintptr_t)c, 0, NULL));
}

EXPORT CUresult cuCtxSynchronize(void)
{
	return LOCKED(simple(OP_CTX_SYNC, 0, 0, NULL));
}

/* cuModuleLoadData gets no size: work it out from the image's own header. */
static size_t image_size(const uint8_t *img)
{
	uint64_t shoff, fat;
	uint16_t shentsize, shnum, hsize;
	uint32_t magic;

	if (!memcmp(img, "\177ELF", 4) && img[4] == 2) {	/* 64-bit ELF: a cubin */
		memcpy(&shoff, img + 0x28, 8);
		memcpy(&shentsize, img + 0x3a, 2);
		memcpy(&shnum, img + 0x3c, 2);
		return shoff + (uint64_t)shentsize * shnum;
	}
	memcpy(&magic, img, 4);
	if (magic == 0xba55ed50) {				/* a fatbin */
		memcpy(&hsize, img + 6, 2);
		memcpy(&fat, img + 8, 8);
		return hsize + fat;
	}
	return strlen((const char *)img) + 1;			/* PTX text */
}

static CUresult module_load(CUmodule *m, const void *img)
{
	uint64_t r[2];
	CUresult st;
	size_t n = image_size(img);

	if (connect_server())
		return CUDA_ERROR_NOT_INITIALIZED;
	if (n > x.max - sizeof(struct req))
		return CUDA_ERROR_NOT_SUPPORTED;	/* TODO: images over one record */
	st = call(OP_MODULE_LOAD, NULL, img, n, r, NULL, NULL);
	if (!st)
		*m = (CUmodule)(uintptr_t)r[0];
	return st;
}

EXPORT CUresult cuModuleLoadData(CUmodule *m, const void *img)
{
	if (!m || !img)
		return CUDA_ERROR_INVALID_VALUE;
	return LOCKED(module_load(m, img));
}

EXPORT CUresult cuModuleUnload(CUmodule m)
{
	return LOCKED(simple(OP_MODULE_UNLOAD, (uintptr_t)m, 0, NULL));
}

static CUresult function_get(CUfunction *f, CUmodule m, const char *name)
{
	uint64_t a[8] = { (uintptr_t)m }, r[2];
	const uint8_t *p;
	size_t plen, nlen = strlen(name) + 1;
	struct rfunc *rf;
	CUresult st;

	if (nlen > G2G_CUDA_NAME_MAX)
		return CUDA_ERROR_INVALID_VALUE;
	st = call(OP_FUNCTION_GET, a, name, nlen, r, &p, &plen);
	if (st)
		return st;
	if (r[1] > G2G_CUDA_MAX_PARAMS || plen != r[1] * sizeof(struct param_info))
		return CUDA_ERROR_UNKNOWN;
	rf = calloc(1, sizeof(*rf));	/* lives as long as the program: CUfunction has no free */
	if (!rf)
		return CUDA_ERROR_OUT_OF_MEMORY;
	rf->id = r[0];
	rf->nparams = r[1];
	memcpy(rf->params, p, plen);
	for (uint32_t i = 0; i < rf->nparams; i++)
		if (rf->params[i].offset + rf->params[i].size > rf->bytes)
			rf->bytes = rf->params[i].offset + rf->params[i].size;
	*f = rf;
	return CUDA_SUCCESS;
}

EXPORT CUresult cuModuleGetFunction(CUfunction *f, CUmodule m, const char *name)
{
	if (!f || !name)
		return CUDA_ERROR_INVALID_VALUE;
	return LOCKED(function_get(f, m, name));
}

EXPORT CUresult cuFuncGetParamInfo(CUfunction f, size_t i, size_t *offset, size_t *size)
{
	const struct rfunc *rf = f;

	if (!rf || i >= rf->nparams)
		return CUDA_ERROR_INVALID_VALUE;
	if (offset)
		*offset = rf->params[i].offset;
	if (size)
		*size = rf->params[i].size;
	return CUDA_SUCCESS;
}

EXPORT CUresult cuMemAlloc_v2(CUdeviceptr *d, size_t bytes)
{
	uint64_t r;
	CUresult st;

	if (!d)
		return CUDA_ERROR_INVALID_VALUE;
	st = LOCKED(simple(OP_MEM_ALLOC, bytes, 0, &r));
	if (!st)
		*d = r;
	return st;
}

EXPORT CUresult cuMemAlloc(CUdeviceptr *d, size_t bytes)
{
	return cuMemAlloc_v2(d, bytes);
}

EXPORT CUresult cuMemFree_v2(CUdeviceptr d)
{
	return LOCKED(simple(OP_MEM_FREE, d, 0, NULL));
}

EXPORT CUresult cuMemFree(CUdeviceptr d)
{
	return cuMemFree_v2(d);
}

static CUresult htod(CUdeviceptr d, const uint8_t *src, size_t n)
{
	size_t chunk;
	CUresult st = CUDA_SUCCESS;

	if (connect_server())
		return CUDA_ERROR_NOT_INITIALIZED;
	chunk = x.max - sizeof(struct req);
	for (size_t off = 0; off < n && !st; off += chunk) {
		uint64_t a[8] = { d + off };
		size_t k = n - off < chunk ? n - off : chunk;

		st = call(OP_MEMCPY_HTOD, a, src + off, k, NULL, NULL, NULL);
	}
	return st;
}

EXPORT CUresult cuMemcpyHtoD_v2(CUdeviceptr d, const void *src, size_t n)
{
	if (!src && n)
		return CUDA_ERROR_INVALID_VALUE;
	return LOCKED(htod(d, src, n));
}

EXPORT CUresult cuMemcpyHtoD(CUdeviceptr d, const void *src, size_t n)
{
	return cuMemcpyHtoD_v2(d, src, n);
}

static CUresult dtoh(uint8_t *dst, CUdeviceptr d, size_t n)
{
	size_t chunk;
	CUresult st = CUDA_SUCCESS;

	if (connect_server())
		return CUDA_ERROR_NOT_INITIALIZED;
	chunk = x.max - sizeof(struct rsp);
	for (size_t off = 0; off < n && !st; off += chunk) {
		uint64_t a[8] = { d + off, n - off < chunk ? n - off : chunk };
		const uint8_t *p;
		size_t plen;

		st = call(OP_MEMCPY_DTOH, a, NULL, 0, NULL, &p, &plen);
		if (!st && plen != a[1])
			st = CUDA_ERROR_UNKNOWN;
		if (!st)
			memcpy(dst + off, p, plen);
	}
	return st;
}

EXPORT CUresult cuMemcpyDtoH_v2(void *dst, CUdeviceptr d, size_t n)
{
	if (!dst && n)
		return CUDA_ERROR_INVALID_VALUE;
	return LOCKED(dtoh(dst, d, n));
}

EXPORT CUresult cuMemcpyDtoH(void *dst, CUdeviceptr d, size_t n)
{
	return cuMemcpyDtoH_v2(dst, d, n);
}

static CUresult launch(const struct rfunc *rf, const uint64_t a[8], void **params)
{
	uint8_t buf[4096];

	if (rf->bytes > sizeof(buf))	/* the Driver API caps parameters at 4 KiB */
		return CUDA_ERROR_INVALID_VALUE;
	if (rf->nparams && !params)
		return CUDA_ERROR_INVALID_VALUE;
	memset(buf, 0, rf->bytes);
	for (uint32_t i = 0; i < rf->nparams; i++)
		memcpy(buf + rf->params[i].offset, params[i], rf->params[i].size);
	return call(OP_LAUNCH, a, buf, rf->bytes, NULL, NULL, NULL);
}

EXPORT CUresult cuLaunchKernel(CUfunction f, unsigned int gx, unsigned int gy, unsigned int gz,
			       unsigned int bx, unsigned int by, unsigned int bz,
			       unsigned int shared, CUstream stream, void **params, void **extra)
{
	const struct rfunc *rf = f;
	uint64_t a[8];

	(void)stream;			/* every launch goes to the default stream */
	if (!rf)
		return CUDA_ERROR_INVALID_HANDLE;
	if (extra)
		return CUDA_ERROR_NOT_SUPPORTED;
	a[0] = rf->id;
	a[1] = gx; a[2] = gy; a[3] = gz;
	a[4] = bx; a[5] = by; a[6] = bz;
	a[7] = shared;
	return LOCKED(launch(rf, a, params));
}

static const struct {
	CUresult r;
	const char *name, *text;
} errors[] = {
	{ CUDA_SUCCESS, "CUDA_SUCCESS", "no error" },
	{ CUDA_ERROR_INVALID_VALUE, "CUDA_ERROR_INVALID_VALUE", "invalid argument" },
	{ CUDA_ERROR_OUT_OF_MEMORY, "CUDA_ERROR_OUT_OF_MEMORY", "out of memory" },
	{ CUDA_ERROR_NOT_INITIALIZED, "CUDA_ERROR_NOT_INITIALIZED",
	  "no g2g channel to the CUDA server" },
	{ CUDA_ERROR_INVALID_IMAGE, "CUDA_ERROR_INVALID_IMAGE", "device kernel image is invalid" },
	{ CUDA_ERROR_INVALID_HANDLE, "CUDA_ERROR_INVALID_HANDLE", "invalid resource handle" },
	{ CUDA_ERROR_NOT_SUPPORTED, "CUDA_ERROR_NOT_SUPPORTED", "operation not supported" },
	{ CUDA_ERROR_UNKNOWN, "CUDA_ERROR_UNKNOWN", "unknown error" },
};

EXPORT CUresult cuGetErrorName(CUresult r, const char **s)
{
	for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
		if (errors[i].r == r) {
			*s = errors[i].name;
			return CUDA_SUCCESS;
		}
	*s = "CUDA_ERROR (remote)";
	return CUDA_SUCCESS;
}

EXPORT CUresult cuGetErrorString(CUresult r, const char **s)
{
	for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
		if (errors[i].r == r) {
			*s = errors[i].text;
			return CUDA_SUCCESS;
		}
	*s = "error reported by the remote CUDA driver";
	return CUDA_SUCCESS;
}
