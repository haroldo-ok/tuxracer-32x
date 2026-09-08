/*
 * Tux Racer 32X - fixed-point math implementation.
 */
#include "fixed.h"

extern const fx fx_expneg_tab[129];

/* degrees (16.16) -> table index in 1/4096 turns, with 16-bit fraction */
static inline fx sin_from_turns(u32 t)   /* t: 32-bit fraction of turn (0..2^32) */
{
    u32 idx = t >> 20;                   /* 0..4095 */
    u32 frac = (t >> 4) & 0xffff;        /* 16-bit sub-step fraction */
    fx a = fx_sintab[idx];
    fx b = fx_sintab[(idx + 1) & 4095];
    return a + (fx)(((long long)(b - a) * (long long)frac) >> 16);
}

/* On the SH-2 the inline above would use a long long multiply; the
   interpolation delta is small (< 101) so a 32-bit multiply is exact. */
static inline fx sin_from_turns32(u32 t)
{
    u32 idx = t >> 20;
    int frac = (int)((t >> 4) & 0xffff);
    fx a = fx_sintab[idx];
    fx b = fx_sintab[(idx + 1) & 4095];
    return a + (((b - a) * frac) >> 16);
}

/* deg (16.16) -> 32-bit turn fraction: deg/360 * 2^32 = deg * (2^32/360/65536) */
static inline u32 deg_to_turns(fx deg)
{
    /* 2^32 / (360 * 65536) = 182.04444... ; use 182.0444 * 2^16 = 11930464.7 */
    /* turns = deg * 11930465 >> 16 (unsigned wrap is what we want) */
    long long p = (long long)deg * 11930465LL;
    return (u32)(p >> 16);
}

fx fx_sin_deg(fx deg)
{
    (void)sin_from_turns;
    return sin_from_turns32(deg_to_turns(deg));
}

fx fx_cos_deg(fx deg)
{
    return sin_from_turns32(deg_to_turns(deg) + 0x40000000u);
}

fx fx_asin_deg(fx v)
{
    int neg = 0;
    fx r;
    int idx, frac;
    if (v < 0) { neg = 1; v = -v; }
    if (v >= FX_ONE) return neg ? -FX(90.0) : FX(90.0);
    if (v > FX(0.9)) {
        /* steep region: asin(v) = 90 - asin(sqrt(1 - v^2)) */
        fx s = fx_sqrt(FX_ONE - fxmul(v, v));
        r = FX(90.0) - fx_asin_deg(s);
        return neg ? -r : r;
    }
    idx = v >> 8;                 /* 0..255 */
    frac = v & 0xff;
    r = fx_asintab[idx] + (((fx_asintab[idx + 1] - fx_asintab[idx]) * frac) >> 8);
    return neg ? -r : r;
}

fx fx_acos_deg(fx v)
{
    return FX(90.0) - fx_asin_deg(v);
}

u32 fx_isqrt32(u32 v)
{
    u32 rem = 0, root = 0;
    int i;
    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;
        if (root < rem) {
            rem -= root | 1;
            root += 2;
        }
    }
    return root >> 1;
}

/* sqrt of 16.16 value: sqrt(v/65536)*65536 = sqrt(v*65536) = sqrt(v) * 256 */
fx fx_sqrt(fx v)
{
    u32 uv;
    int e;
    fx m;
    if (v <= 0) return 0;
    uv = (u32)v;
    /* normalise: uv = m * 2^e with m in [1,2) as 16.16 -> use table */
    e = 0;
    while (uv >= (2u << 16)) { uv >>= 1; e++; }
    while (uv < (1u << 16)) { uv <<= 1; e--; }
    /* uv in [65536, 131072): index by (uv - 65536) >> 8 */
    m = fx_sqrt_tab256[(uv - 65536) >> 8];
    /* sqrt(m * 2^e) = sqrt(m) * 2^(e/2); if e odd, multiply by sqrt(2) */
    if (e & 1) {
        m = fxmul(m, FX(1.41421356));
        e -= 1;
    }
    e >>= 1;
    if (e >= 0) return m << e;
    return m >> (-e);
}

fx fx_rsqrt(fx v)
{
    u32 uv;
    int e;
    fx m;
    if (v <= 0) return FX_MAX;
    uv = (u32)v;
    e = 0;
    while (uv >= (2u << 16)) { uv >>= 1; e++; }
    while (uv < (1u << 16)) { uv <<= 1; e--; }
    m = fx_rsqrt_tab256[(uv - 65536) >> 8];
    if (e & 1) {
        m = fxmul(m, FX(0.70710678));
        e -= 1;
    }
    e >>= 1;                     /* result = m * 2^(-e) */
    if (e <= 0) {
        if (-e >= 15) return FX_MAX;
        return m << (-e);
    }
    return m >> e;
}

fx fx_one_minus_exp_neg(fx x)
{
    int idx, frac;
    fx a, b;
    if (x <= 0) return 0;
    if (x >= FX(8.0)) return FX_ONE;
    idx = x >> 12;                /* x/16 in 16.16 -> idx = x / 4096 */
    frac = x & 0xfff;
    a = fx_expneg_tab[idx];
    b = fx_expneg_tab[idx + 1];
    return FX_ONE - (a + (((b - a) * frac) >> 12));
}

/* ---------------------------------------------------------------- vectors */

fx v3_length(const vec3 *v)
{
    /* compute in 64-bit to avoid overflow: |v|^2 in 32.32 */
    long long s = (long long)v->x * v->x + (long long)v->y * v->y + (long long)v->z * v->z;
    /* sqrt(s) where s = len^2 * 2^32  -> len*2^16 = sqrt(s) */
    u32 hi, lo, root;
    if (s <= 0) return 0;
    /* 64-bit integer sqrt */
    {
        unsigned long long rem = 0, r = 0, vv = (unsigned long long)s;
        int i;
        for (i = 0; i < 32; i++) {
            r <<= 1;
            rem = (rem << 2) | (vv >> 62);
            vv <<= 2;
            if (r < rem) {
                rem -= r | 1;
                r += 2;
            }
        }
        root = (u32)(r >> 1);
    }
    (void)hi; (void)lo;
    return (fx)root;
}

fx v3_normalize(vec3 *v)
{
    fx len = v3_length(v);
    if (len <= 0) return 0;
    if (len == FX_ONE) return len;
    if (len < FX(64.0)) {
        fx inv = fxdiv(FX_ONE, len);
        v->x = fxmul(v->x, inv);
        v->y = fxmul(v->y, inv);
        v->z = fxmul(v->z, inv);
    } else {
        v->x = fxdiv(v->x, len);
        v->y = fxdiv(v->y, len);
        v->z = fxdiv(v->z, len);
    }
    return len;
}

/* --------------------------------------------------------------- matrices */

void m34_identity(mat34 *m)
{
    m->m[0][0] = FX_ONE; m->m[0][1] = 0; m->m[0][2] = 0;
    m->m[1][0] = 0; m->m[1][1] = FX_ONE; m->m[1][2] = 0;
    m->m[2][0] = 0; m->m[2][1] = 0; m->m[2][2] = FX_ONE;
    m->t.x = m->t.y = m->t.z = 0;
}

void m34_rot_x(mat34 *m, fx deg)
{
    fx s = fx_sin_deg(deg), c = fx_cos_deg(deg);
    m34_identity(m);
    m->m[1][1] = c; m->m[1][2] = -s;
    m->m[2][1] = s; m->m[2][2] = c;
}

void m34_rot_y(mat34 *m, fx deg)
{
    fx s = fx_sin_deg(deg), c = fx_cos_deg(deg);
    m34_identity(m);
    m->m[0][0] = c; m->m[0][2] = s;
    m->m[2][0] = -s; m->m[2][2] = c;
}

void m34_rot_z(mat34 *m, fx deg)
{
    fx s = fx_sin_deg(deg), c = fx_cos_deg(deg);
    m34_identity(m);
    m->m[0][0] = c; m->m[0][1] = -s;
    m->m[1][0] = s; m->m[1][1] = c;
}

void m34_rot_axis(mat34 *m, const vec3 *u, fx deg)
{
    fx s = fx_sin_deg(deg), c = fx_cos_deg(deg);
    fx t = FX_ONE - c;
    fx x = u->x, y = u->y, z = u->z;
    fx tx = fxmul(t, x), ty = fxmul(t, y), tz = fxmul(t, z);
    m->m[0][0] = fxmul(tx, x) + c;
    m->m[0][1] = fxmul(tx, y) - fxmul(s, z);
    m->m[0][2] = fxmul(tx, z) + fxmul(s, y);
    m->m[1][0] = fxmul(tx, y) + fxmul(s, z);
    m->m[1][1] = fxmul(ty, y) + c;
    m->m[1][2] = fxmul(ty, z) - fxmul(s, x);
    m->m[2][0] = fxmul(tx, z) - fxmul(s, y);
    m->m[2][1] = fxmul(ty, z) + fxmul(s, x);
    m->m[2][2] = fxmul(tz, z) + c;
    m->t.x = m->t.y = m->t.z = 0;
}

void m34_scale(mat34 *m, fx sx, fx sy, fx sz)
{
    m34_identity(m);
    m->m[0][0] = sx; m->m[1][1] = sy; m->m[2][2] = sz;
}

void m34_translate(mat34 *m, fx x, fx y, fx z)
{
    m34_identity(m);
    m->t.x = x; m->t.y = y; m->t.z = z;
}

void m34_mul(mat34 *o, const mat34 *a, const mat34 *b)
{
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            o->m[i][j] = fxmul(a->m[i][0], b->m[0][j]) + fxmul(a->m[i][1], b->m[1][j]) + fxmul(a->m[i][2], b->m[2][j]);
        }
    }
    o->t.x = fxmul(a->m[0][0], b->t.x) + fxmul(a->m[0][1], b->t.y) + fxmul(a->m[0][2], b->t.z) + a->t.x;
    o->t.y = fxmul(a->m[1][0], b->t.x) + fxmul(a->m[1][1], b->t.y) + fxmul(a->m[1][2], b->t.z) + a->t.y;
    o->t.z = fxmul(a->m[2][0], b->t.x) + fxmul(a->m[2][1], b->t.y) + fxmul(a->m[2][2], b->t.z) + a->t.z;
}

void m34_mul_safe(mat34 *o, const mat34 *a, const mat34 *b)
{
    mat34 tmp;
    m34_mul(&tmp, a, b);
    *o = tmp;
}

void m34_apply(vec3 *o, const mat34 *m, const vec3 *p)
{
    fx x = fxmul(m->m[0][0], p->x) + fxmul(m->m[0][1], p->y) + fxmul(m->m[0][2], p->z) + m->t.x;
    fx y = fxmul(m->m[1][0], p->x) + fxmul(m->m[1][1], p->y) + fxmul(m->m[1][2], p->z) + m->t.y;
    fx z = fxmul(m->m[2][0], p->x) + fxmul(m->m[2][1], p->y) + fxmul(m->m[2][2], p->z) + m->t.z;
    o->x = x; o->y = y; o->z = z;
}

void m34_apply_vec(vec3 *o, const mat34 *m, const vec3 *p)
{
    fx x = fxmul(m->m[0][0], p->x) + fxmul(m->m[0][1], p->y) + fxmul(m->m[0][2], p->z);
    fx y = fxmul(m->m[1][0], p->x) + fxmul(m->m[1][1], p->y) + fxmul(m->m[1][2], p->z);
    fx z = fxmul(m->m[2][0], p->x) + fxmul(m->m[2][1], p->y) + fxmul(m->m[2][2], p->z);
    o->x = x; o->y = y; o->z = z;
}

void m34_apply_vec_t(vec3 *o, const mat34 *m, const vec3 *p)
{
    fx x = fxmul(m->m[0][0], p->x) + fxmul(m->m[1][0], p->y) + fxmul(m->m[2][0], p->z);
    fx y = fxmul(m->m[0][1], p->x) + fxmul(m->m[1][1], p->y) + fxmul(m->m[2][1], p->z);
    fx z = fxmul(m->m[0][2], p->x) + fxmul(m->m[1][2], p->y) + fxmul(m->m[2][2], p->z);
    o->x = x; o->y = y; o->z = z;
}

/* ------------------------------------------------------------ quaternions */

void q_from_mat(quat *q, const mat34 *m)
{
    /* m is a rotation matrix (row-major, columns are the basis vectors) */
    fx tr = m->m[0][0] + m->m[1][1] + m->m[2][2];
    fx s;
    if (tr > 0) {
        s = fx_sqrt(tr + FX_ONE);          /* s = 2w */
        q->w = s >> 1;
        s = fxdiv(FX_HALF, s);
        q->x = fxmul(m->m[2][1] - m->m[1][2], s);
        q->y = fxmul(m->m[0][2] - m->m[2][0], s);
        q->z = fxmul(m->m[1][0] - m->m[0][1], s);
    } else {
        int i = 0, j, k;
        fx qv[3];
        if (m->m[1][1] > m->m[0][0]) i = 1;
        if (m->m[2][2] > m->m[i][i]) i = 2;
        j = (i + 1) % 3;
        k = (j + 1) % 3;
        s = fx_sqrt(m->m[i][i] - m->m[j][j] - m->m[k][k] + FX_ONE);
        qv[i] = s >> 1;
        s = fxdiv(FX_HALF, s);
        q->w = fxmul(m->m[k][j] - m->m[j][k], s);
        qv[j] = fxmul(m->m[j][i] + m->m[i][j], s);
        qv[k] = fxmul(m->m[k][i] + m->m[i][k], s);
        q->x = qv[0]; q->y = qv[1]; q->z = qv[2];
    }
}

void q_to_mat(mat34 *m, const quat *q)
{
    fx xx = fxmul(q->x, q->x), yy = fxmul(q->y, q->y), zz = fxmul(q->z, q->z);
    fx xy = fxmul(q->x, q->y), xz = fxmul(q->x, q->z), yz = fxmul(q->y, q->z);
    fx wx = fxmul(q->w, q->x), wy = fxmul(q->w, q->y), wz = fxmul(q->w, q->z);
    m->m[0][0] = FX_ONE - 2 * (yy + zz);
    m->m[0][1] = 2 * (xy - wz);
    m->m[0][2] = 2 * (xz + wy);
    m->m[1][0] = 2 * (xy + wz);
    m->m[1][1] = FX_ONE - 2 * (xx + zz);
    m->m[1][2] = 2 * (yz - wx);
    m->m[2][0] = 2 * (xz - wy);
    m->m[2][1] = 2 * (yz + wx);
    m->m[2][2] = FX_ONE - 2 * (xx + yy);
    m->t.x = m->t.y = m->t.z = 0;
}

void q_nlerp(quat *o, const quat *a, const quat *b, fx t)
{
    fx cosom = fxmul(a->x, b->x) + fxmul(a->y, b->y) + fxmul(a->z, b->z) + fxmul(a->w, b->w);
    quat bb = *b;
    fx x, y, z, w, len;
    if (cosom < 0) { bb.x = -bb.x; bb.y = -bb.y; bb.z = -bb.z; bb.w = -bb.w; }
    x = a->x + fxmul(bb.x - a->x, t);
    y = a->y + fxmul(bb.y - a->y, t);
    z = a->z + fxmul(bb.z - a->z, t);
    w = a->w + fxmul(bb.w - a->w, t);
    len = fx_sqrt(fxmul(x, x) + fxmul(y, y) + fxmul(z, z) + fxmul(w, w));
    if (len > 0) {
        fx inv = fxdiv(FX_ONE, len);
        x = fxmul(x, inv); y = fxmul(y, inv); z = fxmul(z, inv); w = fxmul(w, inv);
    }
    o->x = x; o->y = y; o->z = z; o->w = w;
}

void q_conj(quat *o, const quat *q)
{
    o->x = -q->x; o->y = -q->y; o->z = -q->z; o->w = q->w;
}

void q_rotate(vec3 *o, const quat *q, const vec3 *v)
{
    /* o = v + 2w(q x v) + 2 q x (q x v) */
    vec3 qv, t, u;
    qv.x = q->x; qv.y = q->y; qv.z = q->z;
    v3_cross(&t, &qv, v);
    t.x *= 2; t.y *= 2; t.z *= 2;
    v3_cross(&u, &qv, &t);
    o->x = v->x + fxmul(q->w, t.x) + u.x;
    o->y = v->y + fxmul(q->w, t.y) + u.y;
    o->z = v->z + fxmul(q->w, t.z) + u.z;
}

void q_from_vectors(quat *o, const vec3 *a, const vec3 *b)
{
    /* q = (a x b, 1 + a.b) normalised */
    vec3 c;
    fx w, len;
    v3_cross(&c, a, b);
    w = FX_ONE + v3_dot(a, b);
    if (w < FX(0.0001)) {
        /* 180 degree turn: pick any perpendicular axis */
        if (fabsx(a->x) < FX(0.9)) { c.x = 0; c.y = -a->z; c.z = a->y; }
        else { c.x = -a->y; c.y = a->x; c.z = 0; }
        w = 0;
    }
    len = fx_sqrt(fxmul(c.x, c.x) + fxmul(c.y, c.y) + fxmul(c.z, c.z) + fxmul(w, w));
    if (len > 0) {
        fx inv = fxdiv(FX_ONE, len);
        o->x = fxmul(c.x, inv); o->y = fxmul(c.y, inv); o->z = fxmul(c.z, inv); o->w = fxmul(w, inv);
    } else {
        o->x = o->y = o->z = 0; o->w = FX_ONE;
    }
}
