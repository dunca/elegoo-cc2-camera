// Per-stage timing of the timelapse encoder: file read, JPEG decode, RGB->YUV, x264 encode.
// Uses the exact code paths from src/timelapse_encoder.c (included directly).
#include "../src/timelapse_encoder.c"
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int n = argc - 1;
    if (n < 1) return 1;
    int W = 1280, H = 720;
    double tr = 0, td = 0, tc = 0, te = 0;

    x264_param_t p;
    x264_param_default_preset(&p, "ultrafast", "zerolatency");
    p.i_width = W; p.i_height = H; p.i_fps_num = 15; p.i_fps_den = 1; p.i_csp = X264_CSP_I420;
    p.i_keyint_max = 30; p.i_keyint_min = 15; p.b_vfr_input = 0; p.b_repeat_headers = 1; p.b_annexb = 1;
    p.i_threads = 1; p.i_lookahead_threads = 0; p.rc.i_lookahead = 0; p.i_bframe = 0;
    p.rc.i_rc_method = X264_RC_CRF; p.rc.f_rf_constant = 26.0;
    x264_param_apply_profile(&p, "baseline");
    x264_t *e = x264_encoder_open(&p);
    x264_picture_t pi, po;
    x264_picture_alloc(&pi, X264_CSP_I420, W, H);

    for (int i = 0; i < n; i++) {
        double t0 = now();
        FILE *f = fopen(argv[i + 1], "rb");
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        unsigned char *jb = malloc(sz);
        fread(jb, 1, sz, f);
        fclose(f);
        double t1 = now();
        int w, h, c;
        unsigned char *rgb = stbi_load_from_memory(jb, sz, &w, &h, &c, 3);
        free(jb);
        double t2 = now();
        rgb24_to_yuv420p(rgb, W, H, pi.img.plane[0], pi.img.plane[1], pi.img.plane[2]);
        stbi_image_free(rgb);
        double t3 = now();
        pi.i_pts = i;
        x264_nal_t *nals;
        int nn;
        x264_encoder_encode(e, &nals, &nn, &pi, &po);
        double t4 = now();
        tr += t1 - t0; td += t2 - t1; tc += t3 - t2; te += t4 - t3;
    }
    printf("frames=%d read=%.1fms decode=%.1fms rgb2yuv=%.1fms x264=%.1fms (per frame)\n",
           n, tr / n * 1e3, td / n * 1e3, tc / n * 1e3, te / n * 1e3);
    return 0;
}
