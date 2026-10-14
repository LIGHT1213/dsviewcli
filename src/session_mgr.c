#include "session_mgr.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#endif

void ses_gen_id(char buf[32], uint64_t extra_entropy)
{
    char ts[40];
    dslab_now_iso8601(ts);
    /* 2026-10-14T15:30:00.123Z -> 20261014_153000 */
    char compact[16];
    int j = 0;
    for (const char *p = ts; *p && j < 14; p++)
        if (isdigit((unsigned char)*p) && (p - ts) < 19)
            compact[j++] = *p;
    compact[j] = '\0';
    uint64_t rnd = ((uint64_t)(uintptr_t)&buf ^ dslab_now_ms() ^ extra_entropy);
    rnd = rnd * 6364136223846793005ULL + 1442695040888963407ULL;
    snprintf(buf, 32, "%s_%06llx", compact, (unsigned long long)((rnd >> 33) & 0xFFFFFFULL));
}

int ses_dir_for(const char *session_id, char *out, size_t outlen)
{
    if (ses_dir_for_create(session_id, out, outlen) != 0)
        return -1;
    struct stat st;
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/state.json", out);
    if (stat(state_path, &st) != 0)
        return -1;
    return 0;
}

int ses_dir_for_create(const char *session_id, char *out, size_t outlen)
{
    if (!session_id || !*session_id || strlen(session_id) > 28)
        return -1;
    for (const char *p = session_id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_')
            return -1;
    snprintf(out, outlen, "runs/%s", session_id);
    return 0;
}

char *ses_read_state(const char *session_dir, long *pid, int64_t *heartbeat_unix_ms)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/state.json", session_dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    /* Minimal field extraction: "state":"...", "pid":N, "heartbeat_ms":N.
     * state.json is written by us, so a lightweight scanner is safe here. */
    char *state = NULL;
    const char *keys[] = { "\"state\":", "\"pid\":", "\"heartbeat_ms\":" };
    for (int k = 0; k < 3; k++) {
        const char *p = strstr(buf, keys[k]);
        if (!p)
            continue;
        p += strlen(keys[k]);
        while (*p == ' ')
            p++;
        if (k == 0) {
            if (*p != '"')
                continue;
            const char *e = strchr(p + 1, '"');
            if (!e)
                continue;
            state = malloc((size_t)(e - p) + 1);
            if (state) {
                memcpy(state, p + 1, (size_t)(e - p - 1));
                state[e - p - 1] = '\0';
            }
        } else {
            if (k == 1) {
                long v = strtol(p, NULL, 10);
                if (pid)
                    *pid = v;
            } else {
                long long v = strtoll(p, NULL, 10);
                if (heartbeat_unix_ms)
                    *heartbeat_unix_ms = (int64_t)v;
            }
        }
    }
    free(buf);
    return state; /* caller frees */
}

static int write_state_file(const char *path, const char *content)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    size_t n = strlen(content);
    if (fwrite(content, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    fclose(f);
#ifdef _WIN32
    /* replace() via remove+rename keeps it simple enough for single-writer */
    remove(path);
#endif
    if (rename(tmp, path) != 0)
        return -1;
    return 0;
}

int ses_write_state(const char *session_dir, const char *session_id,
                    const char *state, long pid,
                    const char *config_json_fld,
                    uint64_t bytes, uint64_t samples,
                    const char *result_json_fld)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/state.json", session_dir);

    char *buf = malloc(16384);
    if (!buf)
        return -1;
    int n = snprintf(buf, 16384,
        "{\"schema_version\":\"1.0\",\"session_id\":\"%s\",\"state\":\"%s\","
        "\"pid\":%ld,\"heartbeat_ms\":%llu,"
        "\"progress\":{\"bytes\":%llu,\"samples\":%llu}%s%s%s%s}",
        session_id, state, pid,
        (unsigned long long)dslab_unix_ms(),
        (unsigned long long)bytes, (unsigned long long)samples,
        config_json_fld ? ",\"config\":" : "", config_json_fld ? config_json_fld : "",
        result_json_fld ? ",\"result\":" : "", result_json_fld ? result_json_fld : "");
    if (n < 0 || n >= 16384) {
        free(buf);
        return -1;
    }
    int rc = write_state_file(path, buf);
    free(buf);
    return rc;
}

void ses_touch_heartbeat(const char *session_dir, const char *session_id,
                         long pid, const char *state,
                         uint64_t bytes, uint64_t samples,
                         const char *config_json_fld)
{
    ses_write_state(session_dir, session_id, state, pid, config_json_fld,
                    bytes, samples, NULL);
}

int ses_request_stop(const char *session_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/stop", session_dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "stop");
    fclose(f);
    return 0;
}

int ses_stop_requested(const char *session_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/stop", session_dir);
    struct stat st;
    return stat(path, &st) == 0;
}

int ses_any_active(char *out_id, size_t idlen)
{
    DIR *d = opendir("runs");
    if (!d)
        return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char dir[512];
        if (ses_dir_for_create(de->d_name, dir, sizeof(dir)) != 0)
            continue;
        long pid = 0;
        int64_t hb = 0;
        char *st = ses_read_state(dir, &pid, &hb);
        if (!st)
            continue;
        if (!SES_TERMINATED(st)) {
            uint64_t age = hb ? (int64_t)dslab_unix_ms() - hb : ~0ULL;
            if (age > 15000 && !ses_pid_alive(pid)) {
                /* stale leftover: treat as inactive */
            } else {
                snprintf(out_id, idlen, "%s", de->d_name);
                found = 1;
                free(st);
                break;
            }
        }
        free(st);
    }
    closedir(d);
    return found;
}

int ses_pid_alive(long pid)
{
    if (pid <= 0)
        return 0;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        return 0;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0;
#endif
}

long ses_spawn_detached(const char *exe, char *const argv[], const char *session_dir)
{
#ifdef _WIN32
    /* IMPORTANT: do NOT inherit the parent's handles. The parent may be a
     * short-lived CLI whose stdout is a pipe watched by an agent; an
     * inherited pipe write-handle in this long-lived worker would keep the
     * pipe open and block the reader until the capture ends. The worker
     * reopens its own stdout/stderr onto worker.log at startup. */
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    char cmd[4096];
    size_t off = 0;
    for (int i = 0; argv[i]; i++) {
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, "%s\"%s\"",
                                i ? " " : "", argv[i]);
        if (off >= sizeof(cmd) - 4)
            return -1;
    }
    wchar_t wcmd[4200];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 4200);

    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
                             DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                             NULL, NULL, &si, &pi);
    if (!ok)
        return -1;
    CloseHandle(pi.hThread);
    long pid = (long)pi.dwProcessId;
    CloseHandle(pi.hProcess);
    return pid;
#else
    (void)session_dir;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setsid();
        char logpath[512];
        snprintf(logpath, sizeof(logpath), "%s/worker.log", session_dir);
        int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        execv(exe, argv);
        _exit(127);
    }
    return (long)pid;
#endif
}

/* Worker-side redirection of stdio into <session_dir>/worker.log. */
void ses_worker_redirect_stdio(const char *session_dir)
{
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/worker.log", session_dir);
    freopen(logpath, "ab", stdout);
    freopen(logpath, "ab", stderr);
}
