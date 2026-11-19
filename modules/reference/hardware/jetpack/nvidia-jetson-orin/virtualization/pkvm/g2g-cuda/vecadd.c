// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// g2g-vecadd: c = a + b on the GPU through the CUDA Driver API, checked on the
// CPU. The kernel is PTX text, JIT-compiled by the driver, so no nvcc is
// needed. libcuda.so.1 is loaded at run time: the real one in the GPU VM,
// the g2g-cuda one (LD_LIBRARY_PATH) in a VM without a GPU.
//
//   g2g-vecadd [elements]        default 1048576
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext, *CUmodule, *CUfunction, *CUstream;
typedef unsigned long long CUdeviceptr;

static const char ptx[] =
	".version 7.0\n"
	".target sm_80\n"
	".address_size 64\n"
	".visible .entry vecadd(.param .u64 pa, .param .u64 pb, .param .u64 pc, .param .u32 pn)\n"
	"{\n"
	"	.reg .pred %p<2>;\n"
	"	.reg .b32 %r<6>;\n"
	"	.reg .f32 %f<4>;\n"
	"	.reg .b64 %rd<11>;\n"
	"	ld.param.u64 %rd1, [pa];\n"
	"	ld.param.u64 %rd2, [pb];\n"
	"	ld.param.u64 %rd3, [pc];\n"
	"	ld.param.u32 %r2, [pn];\n"
	"	mov.u32 %r3, %ctaid.x;\n"
	"	mov.u32 %r4, %ntid.x;\n"
	"	mov.u32 %r5, %tid.x;\n"
	"	mad.lo.s32 %r1, %r3, %r4, %r5;\n"
	"	setp.ge.u32 %p1, %r1, %r2;\n"
	"	@%p1 bra DONE;\n"
	"	cvta.to.global.u64 %rd4, %rd1;\n"
	"	cvta.to.global.u64 %rd5, %rd2;\n"
	"	cvta.to.global.u64 %rd6, %rd3;\n"
	"	mul.wide.u32 %rd7, %r1, 4;\n"
	"	add.s64 %rd8, %rd4, %rd7;\n"
	"	add.s64 %rd9, %rd5, %rd7;\n"
	"	add.s64 %rd10, %rd6, %rd7;\n"
	"	ld.global.f32 %f1, [%rd8];\n"
	"	ld.global.f32 %f2, [%rd9];\n"
	"	add.f32 %f3, %f1, %f2;\n"
	"	st.global.f32 [%rd10], %f3;\n"
	"DONE:\n"
	"	ret;\n"
	"}\n";

static struct {
	CUresult (*Init)(unsigned int);
	CUresult (*DriverGetVersion)(int *);
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
	CUresult (*MemAlloc)(CUdeviceptr *, size_t);
	CUresult (*MemFree)(CUdeviceptr);
	CUresult (*MemcpyHtoD)(CUdeviceptr, const void *, size_t);
	CUresult (*MemcpyDtoH)(void *, CUdeviceptr, size_t);
	CUresult (*LaunchKernel)(CUfunction, unsigned int, unsigned int, unsigned int,
				 unsigned int, unsigned int, unsigned int, unsigned int,
				 CUstream, void **, void **);
	CUresult (*GetErrorString)(CUresult, const char **);
} cu;

#define CHECK(call)							\
	do {								\
		CUresult _r = (call);					\
		if (_r) {						\
			const char *_s = "?";				\
			if (cu.GetErrorString)				\
				cu.GetErrorString(_r, &_s);		\
			fprintf(stderr, "%s: error %d (%s)\n", #call, _r, _s); \
			return 1;					\
		}							\
	} while (0)

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int bind(void *h)
{
	static const struct {
		void **fn;
		const char *name;
	} s[] = {
		{ (void **)&cu.Init, "cuInit" },
		{ (void **)&cu.DriverGetVersion, "cuDriverGetVersion" },
		{ (void **)&cu.DeviceGet, "cuDeviceGet" },
		{ (void **)&cu.DeviceGetName, "cuDeviceGetName" },
		{ (void **)&cu.DeviceGetAttribute, "cuDeviceGetAttribute" },
		{ (void **)&cu.DeviceTotalMem, "cuDeviceTotalMem_v2" },
		{ (void **)&cu.PrimaryCtxRetain, "cuDevicePrimaryCtxRetain" },
		{ (void **)&cu.PrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2" },
		{ (void **)&cu.CtxSetCurrent, "cuCtxSetCurrent" },
		{ (void **)&cu.CtxSynchronize, "cuCtxSynchronize" },
		{ (void **)&cu.ModuleLoadData, "cuModuleLoadData" },
		{ (void **)&cu.ModuleUnload, "cuModuleUnload" },
		{ (void **)&cu.ModuleGetFunction, "cuModuleGetFunction" },
		{ (void **)&cu.MemAlloc, "cuMemAlloc_v2" },
		{ (void **)&cu.MemFree, "cuMemFree_v2" },
		{ (void **)&cu.MemcpyHtoD, "cuMemcpyHtoD_v2" },
		{ (void **)&cu.MemcpyDtoH, "cuMemcpyDtoH_v2" },
		{ (void **)&cu.LaunchKernel, "cuLaunchKernel" },
		{ (void **)&cu.GetErrorString, "cuGetErrorString" },
	};

	for (size_t i = 0; i < sizeof(s) / sizeof(s[0]); i++)
		if (!(*s[i].fn = dlsym(h, s[i].name))) {
			fprintf(stderr, "libcuda.so.1 has no %s\n", s[i].name);
			return -1;
		}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int n = argc > 1 ? strtoul(argv[1], NULL, 0) : 1u << 20;
	size_t bytes = (size_t)n * sizeof(float);
	const char *lib = getenv("G2G_LIBCUDA");
	float *a = malloc(bytes), *b = malloc(bytes), *c = malloc(bytes);
	CUdeviceptr da, db, dc;
	CUcontext ctx;
	CUmodule mod;
	CUfunction fn;
	CUdevice dev;
	int ver, major, minor;
	size_t mem;
	char name[256];
	void *h, *args[4];
	double t0, t1, t2, t3, t4;

	if (!a || !b || !c || !n)
		return 1;
	h = dlopen(lib ? lib : "libcuda.so.1", RTLD_NOW);
	if (!h) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	if (bind(h))
		return 1;
	for (unsigned int i = 0; i < n; i++) {
		a[i] = (float)i;
		b[i] = (float)(2 * i) + 0.5f;
	}

	t0 = now_ms();
	CHECK(cu.Init(0));
	CHECK(cu.DriverGetVersion(&ver));
	CHECK(cu.DeviceGet(&dev, 0));
	CHECK(cu.DeviceGetName(name, sizeof(name), dev));
	CHECK(cu.DeviceGetAttribute(&major, 75, dev));	/* COMPUTE_CAPABILITY_MAJOR */
	CHECK(cu.DeviceGetAttribute(&minor, 76, dev));	/* COMPUTE_CAPABILITY_MINOR */
	CHECK(cu.DeviceTotalMem(&mem, dev));
	printf("device: %s, sm_%d%d, %zu MiB, driver %d\n", name, major, minor, mem >> 20, ver);
	CHECK(cu.PrimaryCtxRetain(&ctx, dev));
	CHECK(cu.CtxSetCurrent(ctx));
	CHECK(cu.ModuleLoadData(&mod, ptx));
	CHECK(cu.ModuleGetFunction(&fn, mod, "vecadd"));
	CHECK(cu.MemAlloc(&da, bytes));
	CHECK(cu.MemAlloc(&db, bytes));
	CHECK(cu.MemAlloc(&dc, bytes));
	t1 = now_ms();
	CHECK(cu.MemcpyHtoD(da, a, bytes));
	CHECK(cu.MemcpyHtoD(db, b, bytes));
	t2 = now_ms();
	args[0] = &da;
	args[1] = &db;
	args[2] = &dc;
	args[3] = &n;
	CHECK(cu.LaunchKernel(fn, (n + 255) / 256, 1, 1, 256, 1, 1, 0, NULL, args, NULL));
	CHECK(cu.CtxSynchronize());
	t3 = now_ms();
	CHECK(cu.MemcpyDtoH(c, dc, bytes));
	t4 = now_ms();

	for (unsigned int i = 0; i < n; i++)
		if (c[i] != a[i] + b[i]) {
			fprintf(stderr, "c[%u] = %f, expected %f\n", i, c[i], a[i] + b[i]);
			return 1;
		}
	printf("vecadd of %u floats: correct\n", n);
	printf("  setup %.2f ms, copy in %.2f ms (%.1f MB/s), kernel %.2f ms, copy out %.2f ms (%.1f MB/s)\n",
	       t1 - t0, t2 - t1, 2 * bytes / (t2 - t1) / 1e3, t3 - t2, t4 - t3,
	       bytes / (t4 - t3) / 1e3);

	CHECK(cu.MemFree(da));
	CHECK(cu.MemFree(db));
	CHECK(cu.MemFree(dc));
	CHECK(cu.ModuleUnload(mod));
	CHECK(cu.CtxSetCurrent(NULL));
	CHECK(cu.PrimaryCtxRelease(dev));
	free(a);
	free(b);
	free(c);
	return 0;
}
