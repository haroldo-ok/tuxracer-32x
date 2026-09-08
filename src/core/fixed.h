/*
 * Tux Racer 32X - 16.16 fixed-point math.
 *
 * The whole game core is float-free so the host build and the SH-2 build
 * compute bit-identical results.  On the SH-2 the multiply uses the
 * hardware DMULS.L (avoids GCC 12's long-long multiply-chain miscompile)
 * and divisions use the on-chip division unit (DIVU) instead of libgcc.
 */
#ifndef FIXED_H
#define FIXED_H

typedef int fx;                     /* 16.16 */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;

#define FX_SHIFT 16
#define FX_ONE   (1 << FX_SHIFT)
#define FX_HALF  (1 << (FX_SHIFT - 1))
#define FX(x)    ((fx)((x) * 65536.0))
#define FX_INT(x) ((x) >> FX_SHIFT)
#define FX_FROM_INT(i) ((fx)(i) << FX_SHIFT)
#define FX_MAX   0x7fffffff
#define FX_MIN   (-0x7fffffff - 1)

#ifdef __32X__
static inline fx fxmul(fx a, fx b)
{
    fx r;
    __asm__ volatile(
        "dmuls.l %1,%2\n\t"
        "sts mach,r1\n\t"
        "sts macl,%0\n\t"
        "xtrct r1,%0"
        : "=r"(r) : "r"(a), "r"(b) : "r1", "mach", "macl");
    return r;
}

/* 64/32 divide via the division unit: (a << 16) / b */
static inline fx fxdiv(fx a, fx b)
{
    volatile int *dvsr = (volatile int *)0xFFFFFF00;
    volatile int *dvdnth = (volatile int *)0xFFFFFF10;
    volatile int *dvdntl = (volatile int *)0xFFFFFF14;
    if (b == 0) return a >= 0 ? FX_MAX : FX_MIN;
    *dvsr = b;
    *dvdnth = a >> 16;
    *dvdntl = a << 16;
    return *dvdntl;
}

/* 32/32 integer divide via the division unit */
static inline int idiv(int a, int b)
{
    volatile int *dvsr = (volatile int *)0xFFFFFF00;
    volatile int *dvdnt = (volatile int *)0xFFFFFF04;
    if (b == 0) return 0;
    *dvsr = b;
    *dvdnt = a;
    return *dvdnt;
}
/* high word of the 64-bit product, rounded: (a*b + 2^31) >> 32.  With two
   16.16 operands this is the integer part of a*b and cannot overflow, which
   makes it the safe way to turn (coord * focal/z) into a pixel. */
static inline int fxmul_hi(fx a, fx b)
{
    int r;
    __asm__ volatile(
        "dmuls.l %1,%2\n\t"
        "sts macl,r1\n\t"
        "sts mach,%0\n\t"
        "shll r1\n\t"
        "mov #0,r1\n\t"
        "addc r1,%0"
        : "=&r"(r) : "r"(a), "r"(b) : "r1", "mach", "macl", "t");
    return r;
}
/* (a * b) >> 30 with a 64-bit intermediate (slopes from 2.30 reciprocals) */
static inline fx mul_shr30(fx a, fx b)
{
    fx r;
    __asm__ volatile(
        "dmuls.l %1,%2\n\t"
        "sts macl,%0\n\t"
        "rotl %0\n\t"
        "rotl %0\n\t"
        "and #3,%0\n\t"
        "sts mach,r1\n\t"
        "shll2 r1\n\t"
        "or r1,%0"
        : "=&z"(r) : "r"(a), "r"(b) : "r1", "mach", "macl", "t");
    return r;
}
#else
static inline fx fxmul(fx a, fx b)
{
    return (fx)(((long long)a * (long long)b) >> FX_SHIFT);
}
static inline fx mul_shr30(fx a, fx b)
{
    return (fx)(((long long)a * (long long)b) >> 30);
}
static inline int fxmul_hi(fx a, fx b)
{
    return (int)((((long long)a * (long long)b) + (1LL << 31)) >> 32);
}
static inline fx fxdiv(fx a, fx b)
{
    long long q;
    if (b == 0) return a >= 0 ? FX_MAX : FX_MIN;
    q = (((long long)a) << FX_SHIFT) / b;
    if (q > FX_MAX) return FX_MAX;
    if (q < FX_MIN) return FX_MIN;
    return (fx)q;
}
static inline int idiv(int a, int b)
{
    if (b == 0) return 0;
    return a / b;
}
#endif

static inline fx fabsx(fx a) { return a < 0 ? -a : a; }
static inline fx fminx(fx a, fx b) { return a < b ? a : b; }
static inline fx fmaxx(fx a, fx b) { return a > b ? a : b; }
static inline fx fclamp(fx v, fx lo, fx hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int iabs(int a) { return a < 0 ? -a : a; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* tables (src/gen/tables.c) */
extern const fx fx_sintab[4096];        /* sin(2*pi*i/4096) */
extern const fx fx_asintab[257];        /* asin(i/256) in degrees */
extern const fx drag_force_tab[257];    /* drag force (N) at speed i*0.5 m/s */
extern const fx fx_rsqrt_tab256[256];   /* 1/sqrt(1 + (i+.5)/256) */
extern const fx fx_sqrt_tab256[256];    /* sqrt(1 + (i+.5)/256) */
extern const fx recip30_tab[1024];      /* round(2^30 / i), [0] unused */
extern const fx proj_tab[4096];         /* FOCAL / (i/32 m) in 16.16, i >= 8 */

/* angle in degrees (16.16) -> sin/cos (16.16), linear interpolated */
fx fx_sin_deg(fx deg);
fx fx_cos_deg(fx deg);
/* asin/acos of a 16.16 value in [-1,1] -> degrees (16.16) */
fx fx_asin_deg(fx v);
fx fx_acos_deg(fx v);
/* sqrt of a 16.16 value (>= 0) -> 16.16 */
fx fx_sqrt(fx v);
/* 1/sqrt of a 16.16 value (> 0) -> 16.16 */
fx fx_rsqrt(fx v);
/* 10^x for x in 16.16 (used rarely) */
u32 fx_isqrt32(u32 v);
/* 1 - exp(-x) for x in [0, 8) (16.16) */
fx fx_one_minus_exp_neg(fx x);

/* --- 3-vectors (out-params only: no 12-byte struct returns on SH-2) --- */
typedef struct { fx x, y, z; } vec3;

static inline void v3_set(vec3 *o, fx x, fx y, fx z) { o->x = x; o->y = y; o->z = z; }
static inline void v3_add(vec3 *o, const vec3 *a, const vec3 *b) { o->x = a->x + b->x; o->y = a->y + b->y; o->z = a->z + b->z; }
static inline void v3_sub(vec3 *o, const vec3 *a, const vec3 *b) { o->x = a->x - b->x; o->y = a->y - b->y; o->z = a->z - b->z; }
static inline void v3_scale(vec3 *o, const vec3 *a, fx s) { o->x = fxmul(a->x, s); o->y = fxmul(a->y, s); o->z = fxmul(a->z, s); }
static inline void v3_madd(vec3 *o, const vec3 *a, const vec3 *b, fx s) { o->x = a->x + fxmul(b->x, s); o->y = a->y + fxmul(b->y, s); o->z = a->z + fxmul(b->z, s); }
static inline fx v3_dot(const vec3 *a, const vec3 *b) { return fxmul(a->x, b->x) + fxmul(a->y, b->y) + fxmul(a->z, b->z); }
static inline void v3_cross(vec3 *o, const vec3 *a, const vec3 *b)
{
    fx x = fxmul(a->y, b->z) - fxmul(a->z, b->y);
    fx y = fxmul(a->z, b->x) - fxmul(a->x, b->z);
    fx z = fxmul(a->x, b->y) - fxmul(a->y, b->x);
    o->x = x; o->y = y; o->z = z;
}
/* normalise in place, returns the previous length */
fx v3_normalize(vec3 *v);
fx v3_length(const vec3 *v);
/* project v into the plane with normal n (n must be unit): v - (v.n) n */
static inline void v3_project_plane(vec3 *o, const vec3 *n, const vec3 *v)
{
    fx d = v3_dot(n, v);
    o->x = v->x - fxmul(n->x, d); o->y = v->y - fxmul(n->y, d); o->z = v->z - fxmul(n->z, d);
}

/* --- 3x3 matrix + translation (row-major: m[row][col]) --- */
typedef struct { fx m[3][3]; vec3 t; } mat34;

void m34_identity(mat34 *m);
void m34_rot_x(mat34 *m, fx deg);
void m34_rot_y(mat34 *m, fx deg);
void m34_rot_z(mat34 *m, fx deg);
void m34_rot_axis(mat34 *m, const vec3 *axis, fx deg);   /* axis unit */
void m34_scale(mat34 *m, fx sx, fx sy, fx sz);
void m34_translate(mat34 *m, fx x, fx y, fx z);
/* o = a * b  (apply b first, then a); o may alias neither */
void m34_mul(mat34 *o, const mat34 *a, const mat34 *b);
/* o = a * b, where result written to a temp then copied so aliasing is ok */
void m34_mul_safe(mat34 *o, const mat34 *a, const mat34 *b);
void m34_apply(vec3 *o, const mat34 *m, const vec3 *p);       /* rotate+translate */
void m34_apply_vec(vec3 *o, const mat34 *m, const vec3 *p);   /* rotate only */
void m34_apply_vec_t(vec3 *o, const mat34 *m, const vec3 *p); /* rotate by transpose */

/* inline variants for hot loops (o must not alias p) */
static inline void m34_apply_i(vec3 *o, const mat34 *m, const vec3 *p)
{
    fx x = p->x, y = p->y, z = p->z;
    o->x = fxmul(m->m[0][0], x) + fxmul(m->m[0][1], y) + fxmul(m->m[0][2], z) + m->t.x;
    o->y = fxmul(m->m[1][0], x) + fxmul(m->m[1][1], y) + fxmul(m->m[1][2], z) + m->t.y;
    o->z = fxmul(m->m[2][0], x) + fxmul(m->m[2][1], y) + fxmul(m->m[2][2], z) + m->t.z;
}
static inline void m34_apply_vec_i(vec3 *o, const mat34 *m, const vec3 *p)
{
    fx x = p->x, y = p->y, z = p->z;
    o->x = fxmul(m->m[0][0], x) + fxmul(m->m[0][1], y) + fxmul(m->m[0][2], z);
    o->y = fxmul(m->m[1][0], x) + fxmul(m->m[1][1], y) + fxmul(m->m[1][2], z);
    o->z = fxmul(m->m[2][0], x) + fxmul(m->m[2][1], y) + fxmul(m->m[2][2], z);
}

/* --- quaternions --- */
typedef struct { fx x, y, z, w; } quat;
void q_from_mat(quat *q, const mat34 *m);
void q_to_mat(mat34 *m, const quat *q);
void q_nlerp(quat *o, const quat *a, const quat *b, fx t);
void q_rotate(vec3 *o, const quat *q, const vec3 *v);
void q_conj(quat *o, const quat *q);
/* quaternion rotating unit vector a onto unit vector b */
void q_from_vectors(quat *o, const vec3 *a, const vec3 *b);

#endif
