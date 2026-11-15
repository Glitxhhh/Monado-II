// [XCDE] Native baseline FMA compatibility: Windows UCRT returned NaN for
// finite tiny-product + finite-addend inputs in authored SIMD differential tests.
// Adapted from the musl source bundled by pinned ziglang 0.14.1. Exact upstream
// file hashes and modifications: third_party/musl_math/PROVENANCE.json.
// Copyright (c) 2005-2020 Rich Felker, et al. MIT license; full musl copyright
// and permission notice: third_party/musl_math/COPYRIGHT. fmaf retains its
// original FreeBSD two-clause license below. No new host CPU feature required.
#ifndef _WIN32
#error "This compatibility implementation is native Windows only"
#endif
#ifdef __FAST_MATH__
#error "Fused guest arithmetic requires strict floating-point semantics"
#endif
#pragma STDC FP_CONTRACT OFF
#include <stdint.h>
#include <stdbool.h>
#include <float.h>
#include <math.h>
#include <cpuid.h>
#include <immintrin.h>
#include <stdatomic.h>


#define ASUINT64(x) ((union {double f; uint64_t i;}){x}).i
#define ZEROINFNAN (0x7ff-0x3ff-52-1)

struct num { uint64_t m; int e; int sign; };

static struct num normalize(double x)
{
	uint64_t ix = ASUINT64(x);
	int e = ix>>52;
	int sign = e & 0x800;
	e &= 0x7ff;
	if (!e) {
		ix = ASUINT64(x*0x1p63);
		e = ix>>52 & 0x7ff;
		e = e ? e-63 : 0x800;
	}
	ix &= (1ull<<52)-1;
	ix |= 1ull<<52;
	ix <<= 1;
	e -= 0x3ff + 52 + 1;
	return (struct num){ix,e,sign};
}

static void mul(uint64_t *hi, uint64_t *lo, uint64_t x, uint64_t y)
{
	uint64_t t1,t2,t3;
	uint64_t xlo = (uint32_t)x, xhi = x>>32;
	uint64_t ylo = (uint32_t)y, yhi = y>>32;

	t1 = xlo*ylo;
	t2 = xlo*yhi + xhi*ylo;
	t3 = xhi*yhi;
	*lo = t1 + (t2<<32);
	*hi = t3 + (t2>>32) + (t1 > *lo);
}

static double software_fma(double x, double y, double z)
{
	#pragma STDC FENV_ACCESS ON

	/* normalize so top 10bits and last bit are 0 */
	struct num nx, ny, nz;
	nx = normalize(x);
	ny = normalize(y);
	nz = normalize(z);

	if (nx.e >= ZEROINFNAN || ny.e >= ZEROINFNAN)
		return x*y + z;
	if (nz.e >= ZEROINFNAN) {
		/* Multiplying by finite nonzero factors is the entire exact result
		 * when z is zero. Adding z after underflow could erase -0. Earlier
		 * operand classification already handles zero/infinite/NaN factors. */
		if (nz.e > ZEROINFNAN) /* z==0, factors finite and nonzero */
			return x*y;
		return z;
	}

	/* mul: r = x*y */
	uint64_t rhi, rlo, zhi, zlo;
	mul(&rhi, &rlo, nx.m, ny.m);
	/* either top 20 or 21 bits of rhi and last 2 bits of rlo are 0 */

	/* align exponents */
	int e = nx.e + ny.e;
	int d = nz.e - e;
	/* shift bits z<<=kz, r>>=kr, so kz+kr == d, set e = e+kr (== ez-kz) */
	if (d > 0) {
		if (d < 64) {
			zlo = nz.m<<d;
			zhi = nz.m>>(64-d);
		} else {
			zlo = 0;
			zhi = nz.m;
			e = nz.e - 64;
			d -= 64;
			if (d == 0) {
			} else if (d < 64) {
				rlo = rhi<<(64-d) | rlo>>d | !!(rlo<<(64-d));
				rhi = rhi>>d;
			} else {
				rlo = 1;
				rhi = 0;
			}
		}
	} else {
		zhi = 0;
		d = -d;
		if (d == 0) {
			zlo = nz.m;
		} else if (d < 64) {
			zlo = nz.m>>d | !!(nz.m<<(64-d));
		} else {
			zlo = 1;
		}
	}

	/* add */
	int sign = nx.sign^ny.sign;
	int samesign = !(sign^nz.sign);
	int nonzero = 1;
	if (samesign) {
		/* r += z */
		rlo += zlo;
		rhi += zhi + (rlo < zlo);
	} else {
		/* r -= z */
		uint64_t t = rlo;
		rlo -= zlo;
		rhi = rhi - zhi - (t < rlo);
		if (rhi>>63) {
			rlo = -rlo;
			rhi = -rhi-!!rlo;
			sign = !sign;
		}
		nonzero = !!rhi;
	}

	/* set rhi to top 63bit of the result (last bit is sticky) */
	if (nonzero) {
		e += 64;
		d = __builtin_clzll(rhi)-1;
		/* note: d > 0 */
		rhi = rhi<<d | rlo>>(64-d) | !!(rlo<<d);
	} else if (rlo) {
		d = __builtin_clzll(rlo)-1;
		if (d < 0)
			rhi = rlo>>1 | (rlo&1);
		else
			rhi = rlo<<d;
	} else {
		/* exact +-0 */
		return x*y + z;
	}
	e -= d;

	/* convert to double */
	int64_t i = rhi; /* i is in [1<<62,(1<<63)-1] */
	if (sign)
		i = -i;
	double r = i; /* |r| is in [0x1p62,0x1p63] */

	if (e < -1022-62) {
		/* result is subnormal before rounding */
		if (e == -1022-63) {
			double c = 0x1p63;
			if (sign)
				c = -c;
			if (r == c) {
				/* min normal after rounding, underflow depends
				   on arch behaviour which can be imitated by
				   a double to float conversion */
				float fltmin = 0x0.ffffff8p-63*FLT_MIN * r;
				return DBL_MIN/FLT_MIN * fltmin;
			}
			/* one bit is lost when scaled, add another top bit to
			   only round once at conversion if it is inexact */
			if (rhi << 53) {
				i = rhi>>1 | (rhi&1) | 1ull<<62;
				if (sign)
					i = -i;
				r = i;
				r = 2*r - c; /* remove top bit */

				/* raise underflow portably, such that it
				   cannot be optimized away */
				{
					double_t tiny = DBL_MIN/FLT_MIN * r;
					r += (double)(tiny*tiny) * (r-r);
				}
			}
		} else {
			/* only round once when scaled */
			d = 10;
			i = ( rhi>>d | !!(rhi<<(64-d)) ) << d;
			if (sign)
				i = -i;
			r = i;
		}
	}
	return scalbn(r, e);
}

/* origin: FreeBSD /usr/src/lib/msun/src/s_fmaf.c */
/*-
 * Copyright (c) 2005-2011 David Schultz <das@FreeBSD.ORG>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <fenv.h>
#include <math.h>
#include <stdint.h>

/*
 * Fused multiply-add: Compute x * y + z with a single rounding error.
 *
 * A double has more than twice as much precision than a float, so
 * direct double-precision arithmetic suffices, except where double
 * rounding occurs.
 */
static float software_fmaf(float x, float y, float z)
{
	#pragma STDC FENV_ACCESS ON
	double xy, result;
	union {double f; uint64_t i;} u;
	int e;

	xy = (double)x * y;
	result = xy + z;
	u.f = result;
	e = u.i>>52 & 0x7ff;
	/* Common case: The double precision result is fine. */
	if ((u.i & 0x1fffffff) != 0x10000000 || /* not a halfway case */
		e == 0x7ff ||                   /* NaN */
		(result - xy == z && result - z == xy) || /* exact */
		fegetround() != FE_TONEAREST)       /* not round-to-nearest */
	{
		/*
		underflow may not be raised correctly, example:
		fmaf(0x1p-120f, 0x1p-120f, 0x1p-149f)
		*/
#if defined(FE_INEXACT) && defined(FE_UNDERFLOW)
		if (e < 0x3ff-126 && e >= 0x3ff-149 && fetestexcept(FE_INEXACT)) {
			feclearexcept(FE_INEXACT);
			/* TODO: gcc and clang bug workaround */
			volatile float vz = z;
			result = xy + vz;
			if (fetestexcept(FE_INEXACT))
				feraiseexcept(FE_UNDERFLOW);
			else
				feraiseexcept(FE_INEXACT);
		}
#endif
		z = result;
		return z;
	}

	/*
	 * If result is inexact, and exactly halfway between two float values,
	 * we need to adjust the low-order bit in the direction of the error.
	 */
	double err;
	int neg = u.i >> 63;
	if (neg == (z > xy))
		err = xy - result + z;
	else
		err = z - result + xy;
	if (neg == (err < 0))
		u.i++;
	else
		u.i--;
	z = u.f;
	return z;
}

// Baseline instructions remain x86-64-v2. The two explicit FMA instructions
// below are reached only after BOTH CPU and OS state checks; the compiler is
// never given a global AVX/FMA target which could spill into an ungated path.
// In particular OSXSAVE alone is insufficient: XCR0 must enable XMM and YMM.
static bool fma_capable(unsigned ecx, uint64_t xcr0)
{
	const unsigned required = (1u << 12) | (1u << 26) | (1u << 27) | (1u << 28);
	return (ecx & required) == required && (xcr0 & 6) == 6;
}

static int detect_fma(void)
{
	unsigned a, b, c, d;
	if (!__get_cpuid(1, &a, &b, &c, &d) || !fma_capable(c, 6)) return 1;
	unsigned lo, hi;
	// Executed only when CPUID confirms XSAVE + OSXSAVE. Do not introduce an
	// unconditional XGETBV instruction into startup on an older supported CPU.
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return fma_capable(c, ((uint64_t)hi << 32) | lo) ? 2 : 1;
}

static atomic_int fma_dispatch;
static bool hardware_enabled(void)
{
	int mode = atomic_load_explicit(&fma_dispatch, memory_order_relaxed);
	if (!mode) {
		mode = detect_fma();
		atomic_store_explicit(&fma_dispatch, mode, memory_order_relaxed);
	}
	return mode == 2;
}

double fma(double x, double y, double z)
{
	// Retain the existing software NaN/signalling/infinity policy exactly. The
	// hardware instruction handles normal operands with a single correctly
	// rounded FMA, obeying MXCSR rounding without changing its control bits.
	// Zero/subnormal inputs and zero/subnormal results retain the software path.
	// Normal inputs/results are independent of FTZ/DAZ; this avoids an expensive
	// per-call MXCSR read while retaining the caller's nonbaseline FTZ/DAZ policy.
	unsigned ex=(ASUINT64(x)>>52)&0x7ff,ey=(ASUINT64(y)>>52)&0x7ff,ez=(ASUINT64(z)>>52)&0x7ff;
	if (hardware_enabled() && ex && ex!=0x7ff && ey && ey!=0x7ff && ez && ez!=0x7ff) {
		double result=x;
		__asm__("vfmadd213sd %2, %1, %0" : "+x"(result) : "x"(y), "xm"(z));
		if ((ASUINT64(result)&0x7ff0000000000000ull)!=0) return result;
	}
	return software_fma(x, y, z);
}

float fmaf(float x, float y, float z)
{
	union { float f; uint32_t u; } a = {x}, b = {y}, c = {z};
	unsigned ex=a.u&0x7f800000,ey=b.u&0x7f800000,ez=c.u&0x7f800000;
	if (hardware_enabled() && ex && ex!=0x7f800000 && ey && ey!=0x7f800000 && ez && ez!=0x7f800000) {
		float result=x;
		__asm__("vfmadd213ss %2, %1, %0" : "+x"(result) : "x"(y), "xm"(z));
		union {float f;uint32_t u;} out={result};
		if ((out.u&0x7f800000)!=0) return result;
	}
	return software_fmaf(x, y, z);
}

#ifdef MONADO_FMA_TEST
// Authored fixture controls are absent from production. They can remove a CPU
// capability, never manufacture support or execute FMA on an unsupported host.
int monado_fma_test_supported(void) { return detect_fma() == 2; }
int monado_fma_test_capable(unsigned ecx, uint64_t xcr0) { return fma_capable(ecx, xcr0); }
void monado_fma_test_software(int enabled) { atomic_store(&fma_dispatch, enabled ? 1 : 0); }
int monado_fma_test_hardware(void) { return hardware_enabled(); }
#endif
