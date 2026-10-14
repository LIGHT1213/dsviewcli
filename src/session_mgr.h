/*
 * dslab — capture session management (M2).
 * File-based IPC between the CLI and a detached worker process:
 *   runs/<session_id>/state.json  worker-maintained state (atomic rewrite)
 *   runs/<session_id>/stop        capture stop requests (existence = stop)
 *   runs/<session_id>/worker.log  worker stdout/stderr
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_SESSION_MGR_H
#define DSLAB_SESSION_MGR_H

#include <stdint.h>
#include <stddef.h>

/* States (plan §M2): created → configuring → starting → armed|capturing →
 * complete|timeout|cancelled|failed (terminal). */
#define SES_TERMINATED(s) (!strcmp((s), "complete") || !strcmp((s), "timeout") || \
                            !strcmp((s), "cancelled") || !strcmp((s), "failed"))

/* Generate "<yyyymmdd>_<hhmmss>_<hex6>" id into buf. */
void ses_gen_id(char buf[32], uint64_t extra_entropy);

/* Resolve "runs/<id>" into out (returns 0, -1 if dir/state missing). */
int ses_dir_for(const char *session_id, char *out, size_t outlen);
/* Same but without requiring state.json to exist yet. */
int ses_dir_for_create(const char *session_id, char *out, size_t outlen);

/* Returns a malloc'd copy of the state string (caller frees), or NULL.
 * Fills *pid and *heartbeat_unix_ms when present. */
char *ses_read_state(const char *session_dir, long *pid, int64_t *heartbeat_unix_ms);

/* Atomically write a full state.json. state: current state name. */
int ses_write_state(const char *session_dir, const char *session_id,
                    const char *state, long pid,
                    const char *config_json_fld,   /* may be NULL */
                    uint64_t bytes, uint64_t samples,
                    const char *result_json_fld);  /* may be NULL */

void ses_touch_heartbeat(const char *session_dir, const char *session_id,
                         long pid, const char *state,
                         uint64_t bytes, uint64_t samples,
                         const char *config_json_fld);

int ses_request_stop(const char *session_dir);
int ses_stop_requested(const char *session_dir);

/* Any session in runs/ still non-terminal AND (fresh heartbeat or live pid)?
 * Returns 1 and copies its id, 0 when none. */
int ses_any_active(char *out_id, size_t idlen);

/* Is a process with this pid alive? (heartbeat age is the portable truth;
 * this is a best-effort complement.) */
int ses_pid_alive(long pid);

/* Spawn a detached worker: argv[] is NULL-terminated, argv[0] = dslab path.
 * Returns pid or -1. The worker must call ses_worker_redirect_stdio(). */
long ses_spawn_detached(const char *exe, char *const argv[], const char *session_dir);

/* Worker-side: reopen stdout/stderr onto <session_dir>/worker.log. */
void ses_worker_redirect_stdio(const char *session_dir);

#endif
