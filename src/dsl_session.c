#include "dsl_session.h"

#include <string.h>
#include <stdio.h>

int dsl_global_init(const char *res_dir)
{
    /* Resource dir must be set before ds_lib_init(): the first scan happens
     * inside init (lib_main.c:172) and may need to upload firmware. */
    ds_set_firmware_resource_dir(res_dir);
    return ds_lib_init();
}

int dsl_global_exit(void)
{
    return ds_lib_exit();
}

int dsl_activate(const char *model_hint, struct ds_device_full_info *out)
{
    struct ds_device_base_info *list = NULL;
    int count = 0;
    int rc = ds_get_device_list(&list, &count);
    if (rc != SR_OK)
        return rc;

    ds_device_handle best = NULL_HANDLE;
    int best_index = -1;
    for (int i = 0; i < count; i++) {
        struct ds_device_full_info info;
        memset(&info, 0, sizeof(info));
        if (ds_active_device(list[i].handle) != SR_OK) {
            /* try next candidate */
            continue;
        }
        if (ds_get_actived_device_info(&info) != SR_OK)
            continue;
        /* Only USB hardware is auto-selected. The bundled demo device is
         * reachable ONLY via an explicit "--device demo" so a synthetic
         * capture can never be mistaken for hardware output. */
        int want_demo = model_hint && !strcmp(model_hint, "demo");
        if (want_demo ? (info.dev_type != DEV_TYPE_DEMO)
                      : (info.dev_type != DEV_TYPE_USB))
            continue;
        int match;
        if (!model_hint || !strcmp(model_hint, "auto") || !*model_hint)
            match = 1; /* first USB device wins */
        else if (want_demo)
            match = 1; /* the only demo device in the list */
        else
            match = strstr(info.name, model_hint) != NULL;
        if (match) {
            best = list[i].handle;
            best_index = i;
            if (out)
                *out = info;
            break;
        }
    }

    if (best == NULL_HANDLE) {
        /* leave the library in a clean state: release whatever we activated
         * while probing (last probed candidate) */
        ds_release_actived_device();
        if (list)
            free(list);
        return SR_ERR;
    }

    /* The probing loop may have ended on a different device than `best`
     * when `best` came before it: reactivate the winner. */
    if (best_index >= 0 && ds_get_actived_device_index() != best_index) {
        if (ds_active_device(best) != SR_OK || !out) {
            if (out) {
                memset(out, 0, sizeof(*out));
                ds_get_actived_device_info(out);
            }
        }
    }

    if (list)
        free(list);
    return SR_OK;
}

int dsl_release(void)
{
    ds_release_actived_device();
    return SR_OK;
}

int dsl_cfg_set_i16(int key, int16_t v)
{
    GVariant *g = g_variant_new_int16(v);
    return ds_set_actived_device_config(NULL, NULL, key, g);
}

int dsl_cfg_set_u64(int key, uint64_t v)
{
    GVariant *g = g_variant_new_uint64(v);
    return ds_set_actived_device_config(NULL, NULL, key, g);
}

int dsl_cfg_set_bool(int key, gboolean v)
{
    GVariant *g = g_variant_new_boolean(v);
    return ds_set_actived_device_config(NULL, NULL, key, g);
}

int dsl_cfg_get_u64(int key, uint64_t *v)
{
    GVariant *g = NULL;
    int rc = ds_get_actived_device_config(NULL, NULL, key, &g);
    if (rc != SR_OK)
        return rc;
    *v = g_variant_get_uint64(g);
    g_variant_unref(g);
    return SR_OK;
}

int dsl_cfg_get_i16(int key, int16_t *v)
{
    GVariant *g = NULL;
    int rc = ds_get_actived_device_config(NULL, NULL, key, &g);
    if (rc != SR_OK)
        return rc;
    *v = g_variant_get_int16(g);
    g_variant_unref(g);
    return SR_OK;
}

GSList *dsl_channels(void)
{
    return ds_get_actived_device_channels();
}

int dsl_enable_channel(int index, gboolean en)
{
    return ds_enable_device_channel_index(index, en);
}

int dsl_channel_count_enabled(void)
{
    int n = 0;
    for (GSList *l = dsl_channels(); l; l = l->next) {
        struct sr_channel *ch = l->data;
        if (ch->enabled)
            n++;
    }
    return n;
}

const char *dsl_last_error_str(int rc)
{
    (void)rc;
    int e = ds_get_last_error();
    static char buf[64];
    snprintf(buf, sizeof(buf), "sr_err=%d", e);
    return buf;
}
