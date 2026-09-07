#ifndef DB_SYNC_H
#define DB_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Updates a single timelapse video entry in /opt/usr/record/elegoo.db
 *
 * @param video_filename Base filename of the video
 * @param video_size Size in bytes of the MP4 file
 * @return 0 on success, negative on error
 */
int db_sync_update_timelapse(const char *video_filename, long video_size);

/**
 * Scans /opt/usr/video for mp4s and updates all matching task_record rows in /opt/usr/record/elegoo.db
 *
 * @return Number of records updated, or negative on error
 */
int db_sync_all_timelapses(void);

/**
 * Removes ghost timelapse records (where status > 0 but URL is invalid or video file doesn't exist)
 * by resetting their time_lapse_video_* columns in elegoo.db
 *
 * @return Number of ghost records cleaned, or negative on error
 */
int db_sync_cleanup_ghost_entries(void);

#ifdef __cplusplus
}
#endif

#endif // DB_SYNC_H
