#include "db_sync.h"
#include "sqlite3.h"
#include "timelapse_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

#define DB_PATH "/opt/usr/record/elegoo.db"

int db_sync_update_timelapse(const char *video_filename, long video_size) {
    if (!video_filename || !video_filename[0]) return -1;

    char base_copy[256];
    snprintf(base_copy, sizeof(base_copy), "%s", video_filename);
    char *vname = basename(base_copy);

    sqlite3 *db = NULL;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        // Fallback for testing
        rc = sqlite3_open("/tmp/elegoo.db", &db);
        if (rc != SQLITE_OK) {
            return -1;
        }
    }

    char stem[256];
    snprintf(stem, sizeof(stem), "%s", vname);
    char *mp4_ext = strstr(stem, ".mp4");
    if (mp4_ext) *mp4_ext = '\0';

    char expected_pic_url[300];
    snprintf(expected_pic_url, sizeof(expected_pic_url), "picture/%s", stem);
    char expected_vid_url[300];
    snprintf(expected_vid_url, sizeof(expected_vid_url), "video/%s", vname);

    int matched_id = -1;

    // Try exact match on time_lapse_video_url where status is 1 or 2
    const char *query_exact = "SELECT id FROM task_record WHERE (time_lapse_video_url = ? OR time_lapse_video_url = ?) AND time_lapse_video_status IN (1, 2) ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, query_exact, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, expected_pic_url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, expected_vid_url, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            matched_id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // Else fallback to pending tasks (status == 1) matching task_name prefix
    // Don't match tasks with time_lapse_video_status == 0 (which had timelapse disabled)
    if (matched_id < 0) {
        const char *query_pending = "SELECT id, task_name FROM task_record WHERE time_lapse_video_status = 1 ORDER BY id DESC;";
        if (sqlite3_prepare_v2(db, query_pending, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int id = sqlite3_column_int(stmt, 0);
                const unsigned char *task_name_raw = sqlite3_column_text(stmt, 1);
                if (!task_name_raw) continue;

                char task_name[256];
                snprintf(task_name, sizeof(task_name), "%s", (const char *)task_name_raw);

                char clean_task[256];
                snprintf(clean_task, sizeof(clean_task), "%s", task_name);
                char *ext = strstr(clean_task, ".gcode");
                if (ext) *ext = '\0';

                if (strstr(vname, task_name) != NULL || strstr(vname, clean_task) != NULL) {
                    matched_id = id;
                    break;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    if (matched_id >= 0) {
        char v_url[300];
        snprintf(v_url, sizeof(v_url), "video/%s", vname);

        const char *update_sql = "UPDATE task_record SET time_lapse_video_status = 2, "
                                 "time_lapse_video_url = ?, time_lapse_video_size = ?, "
                                 "time_lapse_video_duration = 20 WHERE id = ?;";
        sqlite3_stmt *up_stmt = NULL;
        if (sqlite3_prepare_v2(db, update_sql, -1, &up_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(up_stmt, 1, v_url, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(up_stmt, 2, (sqlite3_int64)video_size);
            sqlite3_bind_int(up_stmt, 3, matched_id);
            if (sqlite3_step(up_stmt) == SQLITE_DONE) {
                printf("[db_sync] Successfully linked Task %d to '%s' (%ld bytes) in %s\n",
                       matched_id, v_url, video_size, DB_PATH);
            }
            sqlite3_finalize(up_stmt);
        }
    }

    sqlite3_close(db);
    return (matched_id >= 0) ? 0 : -1;
}

int db_sync_all_timelapses(void) {
    DIR *d = opendir("/opt/usr/video");
    if (!d) return -1;

    int updated_count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t n = strlen(ent->d_name);
        if (n < 4 || strcmp(ent->d_name + n - 4, ".mp4") != 0) continue; // not .mp4.part

        char full[512];
        snprintf(full, sizeof(full), "/opt/usr/video/%s", ent->d_name);
        if (!timelapse_mp4_is_complete(full)) {
            printf("[db_sync] Skipping incomplete video '%s'\n", full);
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "/opt/usr/video/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            if (db_sync_update_timelapse(ent->d_name, (long)st.st_size) == 0) {
                updated_count++;
            }
        }
    }
    closedir(d);
    return updated_count;
}

int db_sync_cleanup_ghost_entries(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        rc = sqlite3_open("/tmp/elegoo.db", &db);
        if (rc != SQLITE_OK) {
            return -1;
        }
    }

    struct stat st_vdir;
    int has_video_dir = (stat("/opt/usr/video", &st_vdir) == 0 && S_ISDIR(st_vdir.st_mode));
    struct stat st_pdir;
    int has_picture_dir = (stat("/opt/usr/picture", &st_pdir) == 0 && S_ISDIR(st_pdir.st_mode));

    // Fallback checks for testing
    if (!has_video_dir && stat("/tmp/video", &st_vdir) == 0 && S_ISDIR(st_vdir.st_mode)) {
        has_video_dir = 2;
    }
    if (!has_picture_dir && stat("/tmp/picture", &st_pdir) == 0 && S_ISDIR(st_pdir.st_mode)) {
        has_picture_dir = 2;
    }

    const char *query = "SELECT id, task_name, time_lapse_video_status, time_lapse_video_url FROM task_record WHERE time_lapse_video_status > 0;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    int capacity = 64;
    int ghost_count = 0;
    int *ghost_ids = malloc(capacity * sizeof(int));
    if (!ghost_ids) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *task_name_raw = sqlite3_column_text(stmt, 1);
        const char *tname = task_name_raw ? (const char *)task_name_raw : "";
        const unsigned char *url_raw = sqlite3_column_text(stmt, 3);
        const char *url = url_raw ? (const char *)url_raw : "";

        int is_ghost = 0;

        if (url[0] == '\0') {
            is_ghost = 1;
        } else if (strcmp(url, "picture/") == 0 || strcmp(url, "picture") == 0 ||
                   strcmp(url, "video/") == 0 || strcmp(url, "video") == 0) {
            is_ghost = 1;
        } else if (strncmp(url, "picture/", 8) == 0) {
            const char *folder = url + 8;
            if (folder[0] == '\0') {
                is_ghost = 1;
            } else if (has_picture_dir || has_video_dir) {
                char p_path[512];
                snprintf(p_path, sizeof(p_path), "%s/%s",
                         has_picture_dir == 2 ? "/tmp/picture" : "/opt/usr/picture", folder);
                char v_path[512];
                snprintf(v_path, sizeof(v_path), "%s/%s.mp4",
                         has_video_dir == 2 ? "/tmp/video" : "/opt/usr/video", folder);

                struct stat st_p, st_v;
                int p_exists = (stat(p_path, &st_p) == 0 && S_ISDIR(st_p.st_mode));
                int v_exists = (stat(v_path, &st_v) == 0 && S_ISREG(st_v.st_mode) && st_v.st_size > 0);

                if (!p_exists && !v_exists) {
                    is_ghost = 1;
                }
            }
        } else if (strncmp(url, "video/", 6) == 0) {
            const char *vname = url + 6;
            if (vname[0] == '\0') {
                is_ghost = 1;
            } else if (has_video_dir) {
                char v_path[512];
                snprintf(v_path, sizeof(v_path), "%s/%s",
                         has_video_dir == 2 ? "/tmp/video" : "/opt/usr/video", vname);

                struct stat st_v;
                if (stat(v_path, &st_v) != 0 || !S_ISREG(st_v.st_mode) || st_v.st_size <= 0) {
                    is_ghost = 1;
                }
            }
        } else {
            // Check direct/absolute path if any
            struct stat st;
            if (stat(url, &st) != 0 || st.st_size <= 0) {
                is_ghost = 1;
            }
        }

        if (is_ghost) {
            if (ghost_count >= capacity) {
                capacity *= 2;
                int *new_ids = realloc(ghost_ids, capacity * sizeof(int));
                if (!new_ids) break;
                ghost_ids = new_ids;
            }
            ghost_ids[ghost_count++] = id;
            printf("[db_sync] Detected ghost timelapse for Task %d ('%s', url='%s')\n",
                   id, tname, url);
        }
    }
    sqlite3_finalize(stmt);

    int cleaned = 0;
    if (ghost_count > 0) {
        sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
        const char *reset_sql = "UPDATE task_record SET time_lapse_video_status = 0, "
                                "time_lapse_video_url = '', time_lapse_video_size = 0, "
                                "time_lapse_video_duration = 0 WHERE id = ?;";
        sqlite3_stmt *up_stmt = NULL;
        if (sqlite3_prepare_v2(db, reset_sql, -1, &up_stmt, NULL) == SQLITE_OK) {
            for (int i = 0; i < ghost_count; i++) {
                sqlite3_reset(up_stmt);
                sqlite3_bind_int(up_stmt, 1, ghost_ids[i]);
                if (sqlite3_step(up_stmt) == SQLITE_DONE) {
                    cleaned++;
                }
            }
            sqlite3_finalize(up_stmt);
        }
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    }

    free(ghost_ids);
    sqlite3_close(db);
    return cleaned;
}

