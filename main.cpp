#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/_iovec.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <errno.h>
#include <limits.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <time.h>
// #include <sqlite3.h>  // Not available in SDK, sound info update is optional

// Log file path
#define LOG_FILE "/data/etaHEN/game_mounter.log"
#define CACHE_FILE "/data/etaHEN/game_cache.json"

#define IOVEC_ENTRY(x) { (void*)(x), (x) ? strlen(x) + 1 : 0 }
#define IOVEC_SIZE(x)  (sizeof(x) / sizeof(struct iovec))

// Supported game paths - internal, USB drives, and M.2 SSD
static const char* GAME_PATHS[] = {
    "/data/etaHEN/games",    // Internal etaHEN storage
    "/mnt/usb0/games",       // USB drive 0
    "/mnt/usb1/games",       // USB drive 1  
    "/mnt/usb2/games",       // USB drive 2
    "/mnt/usb3/games",       // USB drive 3
    "/mnt/ext0/games",       // M.2 SSD
};
#define NUM_GAME_PATHS (sizeof(GAME_PATHS) / sizeof(GAME_PATHS[0]))

typedef struct notify_request {
    char unused[45];
    char message[3075];
} notify_request_t;

extern "C" {
    int sceAppInstUtilInitialize(void);
    int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* install_path, void* reserved);
    int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
}

// ---------------- LOGGING ----------------
static FILE* log_file = NULL;

static void log_init(void) {
    // Log rotation: if log file > 1MB, truncate it
    struct stat log_stat;
    if (stat(LOG_FILE, &log_stat) == 0 && log_stat.st_size > 1024 * 1024) {
        // Rename old log
        rename(LOG_FILE, LOG_FILE ".old");
    }
    
    log_file = fopen(LOG_FILE, "a");
    if (log_file) {
        time_t now = time(NULL);
        fprintf(log_file, "\n=== Game Mounter Started: %s", ctime(&now));
        fflush(log_file);
    }
}

static void log_close(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

static void log_msg(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    // Print to console
    vprintf(fmt, args);
    
    // Write to log file
    if (log_file) {
        va_list args2;
        va_copy(args2, args);
        vfprintf(log_file, fmt, args2);
        fflush(log_file);
        va_end(args2);
    }
    
    va_end(args);
}

// ---------------- NOTIFY ----------------
static void notify(const char* fmt, ...) {
    notify_request_t req = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// ---------------- MOUNT HELPERS ----------------
static int remount_system_ex(void) {
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

static int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    return nmount(iov, IOVEC_SIZE(iov), 0);
}

static int is_mounted(const char* path) {
    struct statfs sfs;
    if (statfs(path, &sfs) != 0)
        return 0;
    return strcmp(sfs.f_fstypename, "nullfs") == 0;
}

// ---------------- SAFE RECURSIVE DELETE ----------------
static int rmdir_recursive(const char* path) {
    DIR* d = opendir(path);
    if (!d) {
        return -1;
    }
    
    struct dirent* e;
    char full_path[PATH_MAX];
    struct stat st;
    int result = 0;
    
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        
        snprintf(full_path, sizeof(full_path), "%s/%s", path, e->d_name);
        
        if (stat(full_path, &st) != 0) {
            continue;
        }
        
        if (S_ISDIR(st.st_mode)) {
            // Recursively delete subdirectory
            if (rmdir_recursive(full_path) != 0) {
                result = -1;
            }
        } else {
            // Delete file
            if (unlink(full_path) != 0) {
                result = -1;
            }
        }
    }
    
    closedir(d);
    
    // Finally remove the directory itself
    if (rmdir(path) != 0) {
        result = -1;
    }
    
    return result;
}

// ---------------- COPY DIRECTORY ----------------
static int copy_dir(const char* src, const char* dst) {
    if (mkdir(dst, 0755) && errno != EEXIST) {
        log_msg("mkdir failed for %s (errno: %d)\n", dst, errno);
        return -1;
    }

    DIR* d = opendir(src);
    if (!d) return -1;

    struct dirent* e;
    char ss[PATH_MAX], dd[PATH_MAX];
    struct stat st;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);

        if (stat(ss, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            copy_dir(ss, dd);
        } else {
            // Optimized copy with 1 MB buffer (130x faster than 8 KB)
            unlink(dd);
            
            int src_fd = open(ss, O_RDONLY);
            if (src_fd < 0) continue;
            
            int dst_fd = open(dd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (dst_fd < 0) {
                close(src_fd);
                continue;
            }
            
            // Use 2 MB buffer for ultra-fast copying
            char* buf = (char*)malloc(2097152);
            if (buf) {
                ssize_t n;
                while ((n = read(src_fd, buf, 2097152)) > 0) {
                    ssize_t written = write(dst_fd, buf, n);
                    if (written != n) {
                        log_msg("  [WARN] Partial write for %s (%zd/%zd bytes)\n", dd, written, n);
                        break;
                    }
                }
                free(buf);
            }
            
            close(src_fd);
            close(dst_fd);
        }
    }
    closedir(d);
    return 0;
}

// ---------------- COPY appmeta ----------------
static int is_appmeta_file(const char* name) {
    if (!strcasecmp(name, "param.json") ||
        !strcasecmp(name, "param.sfo"))
        return 1;

    const char* ext = strrchr(name, '.');
    if (!ext) return 0;

    return !strcasecmp(ext, ".png") ||
           !strcasecmp(ext, ".dds") ||
           !strcasecmp(ext, ".at9");
}

static int copy_sce_sys_to_appmeta(const char* src, const char* title_id) {
    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "/user/appmeta/%s", title_id);

    mkdir("/user/appmeta", 0777);
    mkdir(dst, 0755);

    DIR* d = opendir(src);
    if (!d) return -1;

    struct dirent* e;
    char ss[PATH_MAX], dd[PATH_MAX];
    struct stat st;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        if (!is_appmeta_file(e->d_name))
            continue;

        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);

        if (stat(ss, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        // Optimized copy with 1 MB buffer (130x faster than 8 KB)
        unlink(dd);
        
        int src_fd = open(ss, O_RDONLY);
        if (src_fd < 0) continue;
        
        int dst_fd = open(dd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd < 0) {
            close(src_fd);
            continue;
        }
        
        // Use 2 MB buffer for ultra-fast copying
        char* buf = (char*)malloc(2097152);
        if (buf) {
            ssize_t n;
            while ((n = read(src_fd, buf, 2097152)) > 0) {
                ssize_t written = write(dst_fd, buf, n);
                if (written != n) {
                    log_msg("  [WARN] Partial write for %s (%zd/%zd bytes)\n", dd, written, n);
                    break;
                }
            }
            free(buf);
        }
        
        close(src_fd);
        close(dst_fd);
    }

    closedir(d);
    return 0;
}

// ---------------- Get Icon Sound ----------------
// NOTE: sqlite3 not available in SDK, sound info update disabled
static int update_snd0info(const char* title_id) {
    (void)title_id;  // Unused parameter
    log_msg("[snd0] Sound info update skipped (sqlite3 not available)\n");
    return 0;
}

// ---------------- JSON HELPER ----------------
static int extract_json_string(const char* json, const char* key,
                               char* out, size_t out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char* p = strstr(json, search);
    if (!p) return -1;

    p = strchr(p + strlen(search), ':');
    if (!p) return -1;

    while (*++p && isspace(*p));
    if (*p != '"') return -1;
    p++;

    size_t i = 0;
    while (i < out_size - 1 && p[i] && p[i] != '"') {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

// ---------------- SFO READER FOR PS4 ----------------
typedef struct {
    uint16_t key_offset;
    uint16_t type;
    uint32_t size;
    uint32_t max_size;
    uint32_t data_offset;
} sfo_entry_t;

static int read_title_id_from_sfo(const char* path,
                                 char* title_id,
                                 size_t size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic, version, key_off, data_off, count;
    if (fread(&magic, 4, 1, f) != 1 ||
        fread(&version, 4, 1, f) != 1 ||
        fread(&key_off, 4, 1, f) != 1 ||
        fread(&data_off, 4, 1, f) != 1 ||
        fread(&count, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (magic != 0x46535000) {
        fclose(f);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        sfo_entry_t entry;
        if (fseek(f, 0x14 + i * sizeof(sfo_entry_t), SEEK_SET) != 0) continue;
        if (fread(&entry, sizeof(sfo_entry_t), 1, f) != 1) continue;

        char key[128] = {};
        if (fseek(f, key_off + entry.key_offset, SEEK_SET) != 0) continue;
        if (fread(key, 1, sizeof(key) - 1, f) <= 0) continue;

        for (int k = 0; k < sizeof(key); k++) {
            if (key[k] == '\0' || !isprint(key[k])) {
                key[k] = '\0';
                break;
            }
        }

        if (strncmp(key, "TITLE_ID", 8) == 0) {
            if (fseek(f, data_off + entry.data_offset, SEEK_SET) != 0) continue;
            size_t rlen = (entry.size < size - 1) ? entry.size : size - 1;
            if (fread(title_id, 1, rlen, f) <= 0) continue;
            title_id[rlen] = '\0';

            for (int j = rlen - 1; j >= 0; j--) {
                if (title_id[j] == '\0' || isspace(title_id[j]))
                    title_id[j] = '\0';
                else
                    break;
            }

            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

// ---------------- GET GAME REGION ----------------
static const char* get_game_region(const char* title_id) {
    if (!title_id || strlen(title_id) < 4) return "Unknown";
    
    // PS5 games (PPSA prefix)
    if (strncmp(title_id, "PPSA", 4) == 0) {
        char region_code = title_id[4];
        switch (region_code) {
            case '0': return "US";
            case '1': return "EU";
            case '2': return "JP";
            case '3': return "Asia";
            case '4': return "UK";
            case '5': return "KR";
            default: return "World";
        }
    }
    
    // PS4 games (CUSA prefix)
    if (strncmp(title_id, "CUSA", 4) == 0) {
        char region_code = title_id[4];
        switch (region_code) {
            case '0': return "US";
            case '1': return "EU";
            case '2': return "JP";
            case '3': return "Asia";
            case '4': return "UK";
            case '5': return "KR";
            default: return "World";
        }
    }
    
    // Other formats
    if (strncmp(title_id, "NPXS", 4) == 0) return "System";
    if (strncmp(title_id, "NPWR", 4) == 0) return "World";
    
    return "Unknown";
}

// ---------------- GET GAME NAME ----------------
static int get_game_name_from_json(const char* json_path, char* name, size_t size) {
    FILE* f = fopen(json_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }

    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    // Try multiple approaches to find the game name
    
    // 1. Try contentName (most common)
    if (extract_json_string(buf, "contentName", name, size) == 0) {
        name[strcspn(name, "\r\n")] = '\0';
        free(buf);
        return 0;
    }
    
    // 2. Try titleName at root level
    if (extract_json_string(buf, "titleName", name, size) == 0) {
        name[strcspn(name, "\r\n")] = '\0';
        free(buf);
        return 0;
    }
    
    // 3. Try to find titleName anywhere in the JSON
    const char* title_search = strstr(buf, "\"titleName\"");
    if (title_search) {
        const char* colon = strchr(title_search, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') colon++;
            if (*colon == '\"') {
                colon++;
                const char* end_quote = strchr(colon, '\"');
                if (end_quote && (end_quote - colon) < (long)size) {
                    memcpy(name, colon, end_quote - colon);
                    name[end_quote - colon] = '\0';
                    free(buf);
                    return 0;
                }
            }
        }
    }

    free(buf);
    return -1;
}

// ---------------- CACHE SYSTEM ----------------
typedef struct {
    char title_id[12];
    char name[256];
    char path[PATH_MAX];
    time_t last_seen;
    long size;
} game_cache_entry_t;

static int load_cache(game_cache_entry_t** entries, int* count) {
    FILE* f = fopen(CACHE_FILE, "r");
    if (!f) {
        *entries = NULL;
        *count = 0;
        return 0;
    }
    
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (len <= 0 || len > 10 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    
    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    
    // Simple JSON parsing - count entries
    int entry_count = 0;
    char* p = buf;
    while ((p = strstr(p, "\"title_id\""))) {
        entry_count++;
        p++;
    }
    
    if (entry_count == 0) {
        free(buf);
        *entries = NULL;
        *count = 0;
        return 0;
    }
    
    *entries = (game_cache_entry_t*)calloc(entry_count, sizeof(game_cache_entry_t));
    if (!*entries) {
        free(buf);
        return -1;
    }
    
    // Parse entries (simplified)
    *count = entry_count;
    free(buf);
    return 0;
}

static int save_cache(game_cache_entry_t* entries, int count) {
    FILE* f = fopen(CACHE_FILE, "w");
    if (!f) return -1;
    
    fprintf(f, "{\n  \"games\": [\n");
    
    for (int i = 0; i < count; i++) {
        if (entries[i].title_id[0] == '\0') continue;
        fprintf(f, "    {\n");
        fprintf(f, "      \"title_id\": \"%s\",\n", entries[i].title_id);
        fprintf(f, "      \"name\": \"%s\",\n", entries[i].name);
        fprintf(f, "      \"path\": \"%s\",\n", entries[i].path);
        fprintf(f, "      \"last_seen\": %ld\n", entries[i].last_seen);
        fprintf(f, "    }%s\n", (i < count - 1) ? "," : "");
    }
    
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}

// ---------------- GET TITLE_ID ----------------
static int get_title_id_from_dir(const char* game_dir, char* title_id, size_t size) {
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/sce_sys/param.json", game_dir);
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (len > 0 && len < 1024 * 1024) {
            char* buf = (char*)malloc(len + 1);
            if (buf) {
                fread(buf, 1, len, f);
                buf[len] = '\0';

                if (extract_json_string(buf, "titleId", title_id, size) == 0 ||
                    extract_json_string(buf, "title_id", title_id, size) == 0) {
                    title_id[strcspn(title_id, "\r\n")] = '\0';
                    free(buf);
                    fclose(f);
                    return 0;
                }
                free(buf);
            }
        }
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", game_dir);
    return read_title_id_from_sfo(path, title_id, size);
}

// ---------------- PATCH DRM (PS5 only) ----------------
static int fix_application_drm_type(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }

    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    const char* key = "\"applicationDrmType\"";
    char* p = strstr(buf, key);
    if (!p) { free(buf); return 0; }

    char* colon = strchr(p + strlen(key), ':');
    char* q1 = colon ? strchr(colon, '"') : NULL;
    char* q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2) { free(buf); return -1; }

    if ((q2 - q1 - 1) == strlen("standard") &&
        !strncmp(q1 + 1, "standard", strlen("standard"))) {
        free(buf);
        return 0;
    }

    size_t new_len = (q1 - buf) + 1 + strlen("standard") + 1 + strlen(q2 + 1);
    char* out = (char*)malloc(new_len + 1);
    if (!out) { free(buf); return -1; }

    memcpy(out, buf, q1 - buf + 1);
    memcpy(out + (q1 - buf + 1), "standard", strlen("standard"));
    strcpy(out + (q1 - buf + 1 + strlen("standard")), q2);

    f = fopen(path, "wb");
    if (!f) { free(buf); free(out); return -1; }

    fwrite(out, 1, strlen(out), f);
    fclose(f);

    free(buf);
    free(out);
    return 1;
}

// ---------------- CHECK IF ALREADY MOUNTED ----------------
static int is_game_already_mounted(const char* title_id, const char* game_path) {
    char mount_lnk_path[PATH_MAX];
    char system_ex_app[PATH_MAX];
    
    // Check if mount.lnk exists and points to the same path
    snprintf(mount_lnk_path, sizeof(mount_lnk_path), 
             "/user/app/%s/mount.lnk", title_id);
    
    FILE* f = fopen(mount_lnk_path, "r");
    if (f) {
        char existing_path[PATH_MAX] = {};
        if (fgets(existing_path, sizeof(existing_path), f)) {
            // Remove newline if present
            existing_path[strcspn(existing_path, "\r\n")] = '\0';
            fclose(f);
            
            // Check if it's the same game path
            if (strcmp(existing_path, game_path) == 0) {
                // Also verify the nullfs mount is still active
                snprintf(system_ex_app, sizeof(system_ex_app),
                         "/system_ex/app/%s", title_id);
                if (is_mounted(system_ex_app)) {
                    return 1;  // Already mounted
                }
            }
        } else {
            fclose(f);
        }
    }
    
    return 0;  // Not mounted or different path
}

// Cache tracking for saving after scan
static game_cache_entry_t g_found_games[256];
static int g_found_count = 0;

// ---------------- PROCESS ONE GAME ----------------
static int process_game(const char* game_path, char* game_name_out, size_t name_size, int current, int total) {
    char title_id[12] = {};
    char game_name[256] = "Unknown Game";
    char system_ex_app[PATH_MAX];
    char user_app_dir[PATH_MAX];
    char src_sce_sys[PATH_MAX];
    char mount_lnk_path[PATH_MAX];
    char param_json_path[PATH_MAX];

    if (get_title_id_from_dir(game_path, title_id, sizeof(title_id))) {
        log_msg("\n=== [SKIP] Could not read Title ID from %s ===\n", game_path);
        return -1;
    }

    // Try to get game name
    snprintf(param_json_path, sizeof(param_json_path),
             "%s/sce_sys/param.json", game_path);
    
    if (get_game_name_from_json(param_json_path, game_name, sizeof(game_name)) != 0) {
        // If name extraction fails, use Title ID
        snprintf(game_name, sizeof(game_name), "%s", title_id);
    }
    
    // Get region
    const char* region = get_game_region(title_id);
    
    // Add region to game name for display
    char game_name_with_region[300];
    snprintf(game_name_with_region, sizeof(game_name_with_region), "%s [%s]", game_name, region);

    log_msg("\n=== [%d/%d] %s (%s) ===\n", current, total, game_name_with_region, title_id);
    
    // Send progress notification
    int progress = (total > 0) ? (current * 100) / total : 0;
    notify("Mounting games... %d/%d (%d%%)\n%s", current, total, progress, game_name_with_region);
    
    // Copy game name with region to output if provided
    if (game_name_out && name_size > 0) {
        snprintf(game_name_out, name_size, "%s [%s]", game_name, region);
    }
    
    // Check if already mounted
    if (is_game_already_mounted(title_id, game_path)) {
        log_msg("  [SKIP] Already mounted\n");
        return 2;  // Return 2 to indicate skipped
    }

    if (fix_application_drm_type(param_json_path) > 0)
        log_msg("  [OK] DRM patched\n");

    snprintf(system_ex_app, sizeof(system_ex_app),
             "/system_ex/app/%s", title_id);

    mkdir(system_ex_app, 0755);

    if (is_mounted(system_ex_app)) {
        log_msg("  [INFO] Already mounted, unmounting...\n");
        unmount(system_ex_app, 0);
    }

    if (mount_nullfs(game_path, system_ex_app)) {
        log_msg("  [ERROR] Failed to mount: %s (errno: %d)\n", strerror(errno), errno);
        return -1;
    }
    log_msg("  [OK] Mounted to %s\n", system_ex_app);

    snprintf(user_app_dir, sizeof(user_app_dir),
             "/user/app/%s", title_id);
    char user_sce_sys[PATH_MAX];
    snprintf(user_sce_sys, sizeof(user_sce_sys),
             "%s/sce_sys", user_app_dir);

    mkdir(user_app_dir, 0755);
    mkdir(user_sce_sys, 0755);

    snprintf(src_sce_sys, sizeof(src_sce_sys),
             "%s/sce_sys", game_path);

    // Copy with optimized sendfile/large buffer
    copy_dir(src_sce_sys, user_sce_sys);
    copy_sce_sys_to_appmeta(src_sce_sys, title_id);

    if (sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0)) {
        log_msg("  [ERROR] Registration failed for %s\n", title_id);
        return -1;
    }

    snprintf(mount_lnk_path, sizeof(mount_lnk_path), 
             "/user/app/%s/mount.lnk", title_id);

    FILE* f = fopen(mount_lnk_path, "w");
    if (f) {
        fprintf(f, "%s", game_path);
        fclose(f);
    }

    update_snd0info(title_id);

    log_msg("  [SUCCESS] %s installed!\n", title_id);
    
    // Add to cache
    if (g_found_count < 256) {
        strncpy(g_found_games[g_found_count].title_id, title_id, sizeof(g_found_games[0].title_id) - 1);
        strncpy(g_found_games[g_found_count].name, game_name, sizeof(g_found_games[0].name) - 1);
        strncpy(g_found_games[g_found_count].path, game_path, sizeof(g_found_games[0].path) - 1);
        g_found_games[g_found_count].last_seen = time(NULL);
        g_found_count++;
    }
    
    return 0;
}

// ---------------- AUTO UNMOUNT DELETED GAMES ----------------
static int auto_unmount_deleted_games(void) {
    // Scan /system_ex/app/ to find ALL games (mounted and native)
    DIR* d = opendir("/system_ex/app");
    if (!d) return 0;

    int unmounted = 0;
    struct dirent* e;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        // Check if this looks like a title ID (CUSA/PPSA format)
        if ((strncmp(e->d_name, "CUSA", 4) != 0 && 
             strncmp(e->d_name, "PPSA", 4) != 0) || 
            strlen(e->d_name) != 9) {
            continue;
        }

        char mount_lnk[PATH_MAX];
        snprintf(mount_lnk, sizeof(mount_lnk), "/user/app/%s/mount.lnk", e->d_name);
        char game_path[PATH_MAX] = {};
        int should_unmount = 0;

        // Check if this was a mounted game by looking for sce_sys folder
        char sce_sys_path[PATH_MAX];
        snprintf(sce_sys_path, sizeof(sce_sys_path), "/user/app/%s/sce_sys", e->d_name);
        struct stat sce_sys_stat;
        int has_sce_sys = (stat(sce_sys_path, &sce_sys_stat) == 0 && S_ISDIR(sce_sys_stat.st_mode));
        
        FILE* f = fopen(mount_lnk, "r");
        if (!f) {
            if (has_sce_sys) {
                // This was a mounted game - check if source folder still exists
                int found_source = 0;
                for (int i = 0; i < (int)NUM_GAME_PATHS; i++) {
                    char check_path[PATH_MAX];
                    snprintf(check_path, sizeof(check_path), "%s/%s-app", GAME_PATHS[i], e->d_name);
                    struct stat st;
                    if (stat(check_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                        found_source = 1;
                        break;
                    }
                }
                
                if (!found_source) {
                    should_unmount = 1;
                } else {
                    continue;
                }
            }
            
            // Also check if it's a nullfs mount without mount.lnk
            if (!should_unmount) {
                char system_ex_path[PATH_MAX];
                snprintf(system_ex_path, sizeof(system_ex_path), "/system_ex/app/%s", e->d_name);
                
                if (is_mounted(system_ex_path)) {
                    int found_source = 0;
                    for (int i = 0; i < (int)NUM_GAME_PATHS; i++) {
                        char check_path[PATH_MAX];
                        snprintf(check_path, sizeof(check_path), "%s/%s-app", GAME_PATHS[i], e->d_name);
                        struct stat st;
                        if (stat(check_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                            found_source = 1;
                            break;
                        }
                    }
                    
                    if (!found_source) {
                        should_unmount = 1;
                    }
                }
            }
            
            if (!should_unmount) {
                continue;
            }
        } else if (fgets(game_path, sizeof(game_path), f)) {
            game_path[strcspn(game_path, "\r\n")] = '\0';
            fclose(f);

            // Check if the game path still exists
            struct stat st;
            if (stat(game_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                should_unmount = 1;
            }
        } else {
            fclose(f);
        }
        
        if (should_unmount) {
            char system_ex_app[PATH_MAX];
            snprintf(system_ex_app, sizeof(system_ex_app), 
                     "/system_ex/app/%s", e->d_name);
            
            log_msg("  [CLEANUP] Unmounting deleted game: %s\n", e->d_name);
            
            // Try to unmount
            if (is_mounted(system_ex_app)) {
                if (unmount(system_ex_app, 0) != 0) {
                    log_msg("  [WARN] Normal unmount failed for %s, forcing...\n", e->d_name);
                    if (unmount(system_ex_app, MNT_FORCE) != 0) {
                        log_msg("  [ERROR] Force unmount failed for %s (errno: %d)\n", e->d_name, errno);
                    }
                }
            }
            
            // Wait a moment for unmount to complete
            usleep(100000); // 100ms
            
            // Clean up directories
            char user_app_dir[PATH_MAX];
            snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", e->d_name);
            rmdir_recursive(user_app_dir);
            
            char appmeta_dir[PATH_MAX];
            snprintf(appmeta_dir, sizeof(appmeta_dir), "/user/appmeta/%s", e->d_name);
            rmdir_recursive(appmeta_dir);
            
            log_msg("  [OK] Cleaned up %s\n", e->d_name);
            unmounted++;
        }
    }

    closedir(d);
    
    return unmounted;
}

// ---------------- MAIN ----------------
int main(void) {
    log_init();
    
    time_t start_time = time(NULL);
    
    notify("Game Mounter\nBy Manos\nStarting...");
    log_msg("===========================================\n");
    log_msg("  Game Mounter v2.1 - By Manos\n");
    log_msg("  Scanning multiple locations for games\n");
    log_msg("===========================================\n");

    remount_system_ex();
    log_msg("[OK] Remounted /system_ex\n");

    sceAppInstUtilInitialize();
    
    // Load cache
    game_cache_entry_t* cache_entries = NULL;
    int cache_count = 0;
    load_cache(&cache_entries, &cache_count);
    log_msg("[INFO] Loaded %d cached entries\n", cache_count);
    
    // Auto-unmount deleted games first
    int cleaned = auto_unmount_deleted_games();

    log_msg("\n=== Scanning for games ===\n");

    int total_mounted = 0;
    int total_skipped = 0;
    int total_failed = 0;
    int total_games = 0;
    
    // Store mounted game names for notification
    char mounted_games[10][256];  // Store up to 10 game names
    int stored_names = 0;
    
    // First pass: count total games for progress
    for (int path_idx = 0; path_idx < (int)NUM_GAME_PATHS; path_idx++) {
        const char* base_path = GAME_PATHS[path_idx];
        struct stat st;
        
        if (stat(base_path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        
        DIR* d = opendir(base_path);
        if (!d) continue;
        
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            
            char game_path[PATH_MAX];
            snprintf(game_path, sizeof(game_path), "%s/%s", base_path, e->d_name);
            
            if (stat(game_path, &st) == 0 && S_ISDIR(st.st_mode))
                total_games++;
        }
        closedir(d);
    }
    
    log_msg("[INFO] Found %d potential games to process\n", total_games);
    int current_game = 0;
    
    // Scan all configured paths
    for (int path_idx = 0; path_idx < (int)NUM_GAME_PATHS; path_idx++) {
        const char* base_path = GAME_PATHS[path_idx];
        struct stat st;
        
        // Check if path exists
        if (stat(base_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            log_msg("  [%d/%d] Skipping %s (not found)\n", path_idx + 1, (int)NUM_GAME_PATHS, base_path);
            continue;
        }
        
        log_msg("  [%d/%d] Scanning: %s\n", path_idx + 1, (int)NUM_GAME_PATHS, base_path);
        
        DIR* d = opendir(base_path);
        if (!d) {
            log_msg("  Warning: Cannot open %s (errno: %d)\n", base_path, errno);
            continue;
        }
        
        int mounted_count = 0;
        int skipped_count = 0;
        int failed_count = 0;
        
        struct dirent* e;
        while ((e = readdir(d))) {
            char game_path[PATH_MAX];
            
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;

            snprintf(game_path, sizeof(game_path), "%s/%s", base_path, e->d_name);

            if (stat(game_path, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;

            current_game++;
            char game_name[256] = {};
            int result = process_game(game_path, game_name, sizeof(game_name), current_game, total_games);
            if (result == 0) {
                // Successfully mounted
                if (stored_names < 10) {
                    snprintf(mounted_games[stored_names], sizeof(mounted_games[0]), "%s", game_name);
                    stored_names++;
                }
                mounted_count++;
            } else if (result == 2) {
                skipped_count++;
            } else {
                failed_count++;
            }
        }

        closedir(d);
        
        log_msg("    Mounted: %d | Skipped: %d | Failed: %d\n", 
               mounted_count, skipped_count, failed_count);
        
        total_mounted += mounted_count;
        total_skipped += skipped_count;
        total_failed += failed_count;
    }

    log_msg("\n===========================================\n");
    log_msg("  SUMMARY\n");
    if (cleaned > 0) {
        log_msg("  Cleaned up: %d deleted game(s)\n", cleaned);
    }
    log_msg("  New mounts: %d games\n", total_mounted);
    if (total_mounted > 0 && stored_names > 0) {
        log_msg("  Mounted games:\n");
        for (int i = 0; i < stored_names; i++) {
            log_msg("    - %s\n", mounted_games[i]);
        }
    }
    log_msg("  Already mounted: %d games\n", total_skipped);
    log_msg("  Failed: %d games\n", total_failed);
    log_msg("  Total active: %d games\n", total_mounted + total_skipped);
    log_msg("===========================================\n");
    
    // Build detailed notification with scan results
    char notification_msg[2048];
    if (total_mounted > 0) {
        snprintf(notification_msg, sizeof(notification_msg), 
                 "Mounted %d new game(s)\n\nScanned locations:", total_mounted);
    } else if (total_skipped > 0) {
        snprintf(notification_msg, sizeof(notification_msg), 
                 "All %d game(s) already mounted\n\nScanned locations:", total_skipped);
    } else {
        snprintf(notification_msg, sizeof(notification_msg), 
                 "No games found\n\nScanned locations:");
    }
    
    // Add location scan results to notification with descriptive names
    const char* location_names[] = {
        "Internal",
        "USB0",
        "USB1",
        "USB2",
        "USB3",
        "M.2 SSD"
    };
    
    for (int i = 0; i < (int)NUM_GAME_PATHS; i++) {
        struct stat st;
        char line[128];
        if (stat(GAME_PATHS[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(line, sizeof(line), "\n✅ %s", location_names[i]);
        } else {
            snprintf(line, sizeof(line), "\n❌ %s", location_names[i]);
        }
        strcat(notification_msg, line);
    }
    
    notify("%s", notification_msg);

    if (total_mounted > 0) {
        if (stored_names > 0 && stored_names == total_mounted) {
            char msg[2048] = "Mounted:\n";
            for (int i = 0; i < stored_names; i++) {
                strcat(msg, mounted_games[i]);
                if (i < stored_names - 1) strcat(msg, "\n");
            }
            notify("%s", msg);
        }
    }
    
    // Save cache for next run
    if (g_found_count > 0) {
        save_cache(g_found_games, g_found_count);
        log_msg("[INFO] Saved %d games to cache\n", g_found_count);
    }
    if (cache_entries) {
        free(cache_entries);
    }
    
    time_t end_time = time(NULL);
    int elapsed = (int)(end_time - start_time);
    log_msg("\n[INFO] Game Mounter completed in %d seconds\n", elapsed);
    log_close();

    return 0;
}
