#ifndef TIMELAPSE_ENCODER_H
#define TIMELAPSE_ENCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*timelapse_start_cb)(const char *output_mp4, void *user_data);
typedef void (*timelapse_progress_cb)(int current_frame, int total_frames, void *user_data);
typedef void (*timelapse_finish_cb)(const char *output_mp4, long file_size, int success, void *user_data);

/**
 * Encodes all JPEG images in `image_dir` into an H.264 MP4 video file at `output_mp4`.
 * The video is written to `<output_mp4>.part` and renamed only once it is complete, so an
 * interrupted encode never leaves a truncated `.mp4` behind. If no frame can be read, nothing
 * is written and an existing `output_mp4` is left as it is.
 */
int timelapse_encode_directory(const char *image_dir, const char *output_mp4, int fps, int width, int height,
                               timelapse_progress_cb prog_cb, void *user_data);

/**
 * Starts encoding in a detached background thread with optional initial delay and callbacks.
 * The encode itself runs in a child process (this binary with --encoder-child), one at a time.
 * A job whose video is already complete is skipped, so two requests for the same print (the
 * service's own encode on print stop and a composite request) never encode it twice.
 */
int timelapse_encode_directory_async(const char *image_dir, const char *output_mp4, int fps,
                                     int delay_sec,
                                     timelapse_start_cb start_cb,
                                     timelapse_progress_cb prog_cb,
                                     timelapse_finish_cb finish_cb,
                                     void *user_data);

/** timelapse_encode_directory() result when none of the frames could be read. */
#define TIMELAPSE_ERR_NO_FRAMES (-10)

/** Returns 1 if `path` is a finished MP4: top-level boxes cover the file exactly and a moov box exists. */
int timelapse_mp4_is_complete(const char *path);

/** Background-encode status for /health. */
typedef struct {
    char state[16];          // idle | encoding
    char last_result[32];    // "" | ok | already_encoded | no_frames | failed | killed_signal_N
    char last_video[160];
    long last_avail_kb;      // MemAvailable when the encode started
    long last_min_avail_kb;  // lowest MemAvailable seen while it ran
    long last_peak_kb;       // encoder process peak RSS (-1 if it ran in-process)
    double last_seconds;     // encode wall time
    long long last_end;      // epoch seconds, 0 = never
} timelapse_status_t;

void timelapse_get_status(timelapse_status_t *out);

/** Persistent one-line-per-encode log (survives reboots, rotated at 64 KB). */
#define TIMELAPSE_ENCODE_LOG "/opt/usr/timelapse-encode.log"

#ifdef __cplusplus
}
#endif

#endif // TIMELAPSE_ENCODER_H
