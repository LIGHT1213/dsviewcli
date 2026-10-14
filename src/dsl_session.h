/*
 * dslab — thin wrapper over libsigrok4DSL's ds_* API.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_DSL_SESSION_H
#define DSLAB_DSL_SESSION_H

#include <libsigrok.h>

/* Initialize library with firmware resource dir. Must be called before any
 * other dsl_* helper. Returns SR error code (0 = ok). */
int dsl_global_init(const char *res_dir);
int dsl_global_exit(void);

/* Activate a device: model_hint==NULL/"auto" picks the first USB (non-demo)
 * device; otherwise matches ds_device_full_info.name containing the hint.
 * Returns SR error code; fills *out on success. */
int dsl_activate(const char *model_hint, struct ds_device_full_info *out);
int dsl_release(void);

int dsl_cfg_set_i16(int key, int16_t v);
int dsl_cfg_set_u64(int key, uint64_t v);
int dsl_cfg_set_bool(int key, gboolean v);
int dsl_cfg_get_u64(int key, uint64_t *v);
int dsl_cfg_get_i16(int key, int16_t *v);

/* Channel helpers (operate on the activated device). */
GSList *dsl_channels(void);
int dsl_enable_channel(int index, gboolean en);
int dsl_channel_count_enabled(void);

/* Last library error (ds_get_last_error) as string. */
const char *dsl_last_error_str(int rc);

#endif
