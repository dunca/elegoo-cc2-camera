// Per-stage timing of the timelapse encoder on the printer, using the exact code in
// src/timelapse_encoder.c (included directly).
//
//   stagebench FRAME.jpeg...
//
// For every frame: the original path (stb_image -> RGB -> rgb24_to_yuv420p), the current
// load_frame_i420() path (libjpeg-turbo straight to YUV when built with HAVE_TURBOJPEG), the
// x264 encode, and the PSNR between the two decode paths' YUV output.
#include "../src/timelapse_encoder.c"
#include <math.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double plane_mse(const uint8_t *a, const uint8_t *b, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        s += d * d;
    }
    return s / n;
}

static double psnr(double mse) { return mse == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse); }

int main(int argc, char **argv) {
    int n = argc - 1;
    if (n < 1) return 1;
    int W = 1280, H = 720;
    double t_old = 0, t_new = 0, t_enc = 0, mse_y = 0, mse_c = 0, worst = 99;

    x264_param_t p;
    x264_param_default_preset(&p, "ultrafast", "zerolatency");
    p.i_width = W; p.i_height = H; p.i_fps_num = 15; p.i_fps_den = 1; p.i_csp = X264_CSP_I420;
    p.i_keyint_max = 30; p.i_keyint_min = 15; p.b_vfr_input = 0; p.b_repeat_headers = 1; p.b_annexb = 1;
    p.i_threads = 1; p.i_lookahead_threads = 0; p.rc.i_lookahead = 0; p.i_bframe = 0;
    p.rc.i_rc_method = X264_RC_CRF; p.rc.f_rf_constant = 26.0;
    x264_param_apply_profile(&p, "baseline");
    p.i_log_level = X264_LOG_NONE;
    x264_t *e = x264_encoder_open(&p);
    x264_picture_t pic_new, pic_old, po;
    x264_picture_alloc(&pic_new, X264_CSP_I420, W, H);
    x264_picture_alloc(&pic_old, X264_CSP_I420, W, H);
    frame_decoder_t dec;
    frame_decoder_init(&dec);

    for (int i = 0; i < n; i++) {
        // Original path, including reading the file as stbi_load() does.
        double t0 = now();
        int w, h, c;
        uint8_t *rgb = stbi_load(argv[i + 1], &w, &h, &c, 3);
        if (!rgb) return 2;
        rgb24_to_yuv420p(rgb, W, H, pic_old.img.plane[0], pic_old.img.plane[1], pic_old.img.plane[2]);
        stbi_image_free(rgb);
        double t1 = now();
        if (load_frame_i420(&dec, argv[i + 1], W, H, &pic_new) != 0) return 3;
        double t2 = now();
        pic_new.i_pts = i;
        x264_nal_t *nals;
        int nn;
        x264_encoder_encode(e, &nals, &nn, &pic_new, &po);
        double t3 = now();
        t_old += t1 - t0; t_new += t2 - t1; t_enc += t3 - t2;

        double my = plane_mse(pic_old.img.plane[0], pic_new.img.plane[0], (size_t)W * H);
        double mc = (plane_mse(pic_old.img.plane[1], pic_new.img.plane[1], (size_t)W * H / 4) +
                     plane_mse(pic_old.img.plane[2], pic_new.img.plane[2], (size_t)W * H / 4)) / 2;
        mse_y += my; mse_c += mc;
        if (psnr(my) < worst) worst = psnr(my);
    }
    frame_decoder_cleanup(&dec);
#ifdef HAVE_TURBOJPEG
    const char *path = "turbojpeg";
#else
    const char *path = "stb";
#endif
    printf("frames=%d old(stb+rgb2yuv)=%.1fms new(%s)=%.1fms x264=%.1fms per frame; "
           "new vs old PSNR Y=%.1fdB (worst %.1f) UV=%.1fdB\n",
           n, t_old / n * 1e3, path, t_new / n * 1e3, t_enc / n * 1e3, psnr(mse_y / n), worst, psnr(mse_c / n));
    return 0;
}
