/* Host unit tests for the fixed-point math library. */
#define _GNU_SOURCE
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "../src/core/fixed.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double D(fx v) { return v / 65536.0; }

int main(void)
{
    int i;
    double maxerr;

    /* sin/cos */
    maxerr = 0;
    for (i = -7200; i <= 7200; i++) {
        double deg = i * 0.1;
        double e1 = fabs(D(fx_sin_deg(FX(deg))) - sin(deg * M_PI / 180));
        double e2 = fabs(D(fx_cos_deg(FX(deg))) - cos(deg * M_PI / 180));
        if (e1 > maxerr) maxerr = e1;
        if (e2 > maxerr) maxerr = e2;
    }
    printf("sin/cos max err %g\n", maxerr);
    CHECK(maxerr < 0.0005, "sin/cos accuracy");

    /* asin */
    maxerr = 0;
    for (i = -1000; i <= 1000; i++) {
        double v = i / 1000.0;
        double e = fabs(D(fx_asin_deg(FX(v))) - asin(v) * 180 / M_PI);
        if (e > maxerr) maxerr = e;
    }
    printf("asin max err %g deg\n", maxerr);
    CHECK(maxerr < 0.6, "asin accuracy");

    /* sqrt / rsqrt */
    maxerr = 0;
    for (i = 1; i < 20000; i++) {
        double v = i * 0.37;
        double r = D(fx_sqrt(FX(v)));
        double e = fabs(r - sqrt(v)) / sqrt(v);
        if (e > maxerr) maxerr = e;
    }
    printf("sqrt max rel err %g\n", maxerr);
    CHECK(maxerr < 0.003, "sqrt accuracy");
    maxerr = 0;
    for (i = 1; i < 20000; i++) {
        double v = i * 0.011;
        double r = D(fx_rsqrt(FX(v)));
        double e = fabs(r - 1 / sqrt(v)) / (1 / sqrt(v));
        if (e > maxerr) maxerr = e;
    }
    printf("rsqrt max rel err %g\n", maxerr);
    CHECK(maxerr < 0.003, "rsqrt accuracy");

    /* vector length / normalise (large values) */
    {
        vec3 v = { FX(300.0), FX(-400.0), FX(1200.0) };
        fx len = v3_length(&v);
        CHECK(fabs(D(len) - 1300.0) < 0.01, "v3_length %g", D(len));
        v3_normalize(&v);
        CHECK(fabs(D(v.x) - 300.0 / 1300) < 1e-3, "normalize x");
        CHECK(fabs(D(v.z) - 1200.0 / 1300) < 1e-3, "normalize z");
    }

    /* matrix rotations */
    {
        mat34 m; vec3 p = { FX_ONE, 0, 0 }, o;
        m34_rot_z(&m, FX(90.0));
        m34_apply_vec(&o, &m, &p);
        CHECK(fabs(D(o.x)) < 1e-3 && fabs(D(o.y) - 1) < 1e-3, "rot z 90: %g %g", D(o.x), D(o.y));
        m34_rot_y(&m, FX(90.0));
        m34_apply_vec(&o, &m, &p);
        CHECK(fabs(D(o.z) + 1) < 1e-3, "rot y 90: z=%g", D(o.z));
        {
            vec3 ax = { 0, FX_ONE, 0 };
            mat34 m2;
            m34_rot_axis(&m2, &ax, FX(90.0));
            m34_apply_vec(&o, &m2, &p);
            CHECK(fabs(D(o.z) + 1) < 1e-3, "rot axis y 90: z=%g", D(o.z));
        }
    }

    /* quaternion roundtrip */
    {
        mat34 m, m2; quat q; vec3 p = { FX(0.3), FX(0.5), FX(-0.7) }, o1, o2;
        mat34 a, b;
        m34_rot_x(&a, FX(35.0));
        m34_rot_y(&b, FX(-70.0));
        m34_mul(&m, &a, &b);
        q_from_mat(&q, &m);
        q_to_mat(&m2, &q);
        m34_apply_vec(&o1, &m, &p);
        m34_apply_vec(&o2, &m2, &p);
        CHECK(fabs(D(o1.x) - D(o2.x)) < 2e-3 && fabs(D(o1.y) - D(o2.y)) < 2e-3 && fabs(D(o1.z) - D(o2.z)) < 2e-3,
              "quat roundtrip %g %g %g vs %g %g %g", D(o1.x), D(o1.y), D(o1.z), D(o2.x), D(o2.y), D(o2.z));
        q_rotate(&o2, &q, &p);
        CHECK(fabs(D(o1.x) - D(o2.x)) < 2e-3 && fabs(D(o1.y) - D(o2.y)) < 2e-3 && fabs(D(o1.z) - D(o2.z)) < 2e-3,
              "q_rotate %g %g %g vs %g %g %g", D(o1.x), D(o1.y), D(o1.z), D(o2.x), D(o2.y), D(o2.z));
    }
    /* q_from_vectors */
    {
        vec3 a = { 0, 0, -FX_ONE }, b = { FX(0.6), 0, -FX(0.8) }, o; quat q;
        q_from_vectors(&q, &a, &b);
        q_rotate(&o, &q, &a);
        CHECK(fabs(D(o.x) - 0.6) < 2e-3 && fabs(D(o.z) + 0.8) < 2e-3, "q_from_vectors %g %g %g", D(o.x), D(o.y), D(o.z));
    }
    /* 1-exp(-x) */
    {
        double e = fabs(D(fx_one_minus_exp_neg(FX(0.5))) - (1 - exp(-0.5)));
        CHECK(e < 1e-3, "1-exp(-x) err %g", e);
    }
    /* drag table sanity: at 20 m/s, tux racer drag */
    printf("drag at 20 m/s = %g N\n", D(drag_force_tab[40]));
    CHECK(D(drag_force_tab[40]) > 5 && D(drag_force_tab[40]) < 200, "drag range");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("test_fixed: all OK\n");
    return 0;
}
