/* imgdiff A B [TOL] -- exit 0 if same size and every channel within TOL
 * (default 2; renderers use float maths, so allow last-bit differences). */
#include "pmark.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: imgdiff A B [TOL]\n"); return 2; }
    int tol = argc > 3 ? atoi(argv[3]) : 2;
    pm_image a, b;
    pm_status st;
    if ((st = pm_image_load(&a, argv[1])) != PM_OK) { fprintf(stderr, "%s: %s\n", argv[1], pm_strerror(st)); return 2; }
    if ((st = pm_image_load(&b, argv[2])) != PM_OK) { fprintf(stderr, "%s: %s\n", argv[2], pm_strerror(st)); return 2; }
    if (a.w != b.w || a.h != b.h) {
        fprintf(stderr, "imgdiff: size %dx%d vs %dx%d\n", a.w, a.h, b.w, b.h);
        return 1;
    }
    size_t n = (size_t)a.w * a.h * 4, bad = 0;
    int worst = 0;
    for (size_t i = 0; i < n; i++) {
        int d = abs(a.px[i] - b.px[i]);
        if (d > worst) worst = d;
        if (d > tol) bad++;
    }
    pm_image_free(&a);
    pm_image_free(&b);
    if (bad) {
        fprintf(stderr, "imgdiff: %zu channel values differ by more than %d (max %d)\n", bad, tol, worst);
        return 1;
    }
    return 0;
}
