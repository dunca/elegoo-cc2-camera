#include "db_sync.h"
#include "sqlite3.h"
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
        // Fallback for development / testing
        rc = sqlite3_open("/tmp/elegoo.db", &db);
        if (rc != SQLITE_OK) {
            return -1;
        }
    }

    // Prepare query to scan tasks
    sqlite3_stmt *stmt = NULL;
    const char *query = "SELECT id, task_name, time_lapse_video_status FROM task_record ORDER BY id DESC;";
    rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    int matched_id = -1;
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

        // Check if vname matches task_name or clean_task
        if (strstr(vname, task_name) != NULL || strstr(vname, clean_task) != NULL) {
            matched_id = id;
            break;
        }
    }
    sqlite3_finalize(stmt);

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
        if (!strstr(ent->d_name, ".mp4")) continue;

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
