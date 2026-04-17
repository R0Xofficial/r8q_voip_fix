#define _GNU_SOURCE

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

/*
 * Property-driven VoIP volume shim (armeabi-v7a)
 *
 * - Interposes Voip_write() and applies S16LE software scaling
 * - Reads live volume index from vendor.voip.volume.idx (set by service.sh logcat watcher)
 * - Avoids code patching/hooking entirely (SELinux-safe path)
 */

static const char *k_voip_resampler_name = "libsamsungVoipResampler.so";
static const float k_index_vol_map[9] = {
    1.000000f, 0.875000f, 0.750000f, 0.625000f, 0.500000f,
    0.375000f, 0.250000f, 0.125000f, 0.125000f,
};

static _Atomic float g_volume = 1.0f;
static _Atomic int g_last_idx = -1;
static _Atomic uint64_t g_last_poll_ns = 0;

typedef int (*voip_write_fn_t)(void *buf, int byte_size);

static voip_write_fn_t g_real_voip_write = NULL;
static pthread_mutex_t g_voip_resolve_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_line(const char *msg) {
    __android_log_print(ANDROID_LOG_INFO, "voip_volume_fix", "%s", msg);

    int fd = open("/data/adb/voip_volume_fix.log", O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd >= 0) {
        dprintf(fd, "[shim-prop] %s\n", msg);
        close(fd);
    }
}

static inline void set_volume_from_index(int idx) {
    if (idx >= 0 && idx <= 8) {
        atomic_store_explicit(&g_volume, k_index_vol_map[idx], memory_order_relaxed);
        atomic_store_explicit(&g_last_idx, idx, memory_order_relaxed);
    }
}

static inline float current_volume(void) {
    return atomic_load_explicit(&g_volume, memory_order_relaxed);
}

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void refresh_volume_from_property(void) {
    const uint64_t now = monotonic_ns();
    const uint64_t last = atomic_load_explicit(&g_last_poll_ns, memory_order_relaxed);

    /* Poll at most every 50ms to keep overhead negligible in Voip_write path. */
    if (now - last < 50000000ULL) return;
    atomic_store_explicit(&g_last_poll_ns, now, memory_order_relaxed);

    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get("vendor.voip.volume.idx", v) <= 0) return;

    char *end = NULL;
    long idx = strtol(v, &end, 10);
    if (end == v || idx < 0 || idx > 8) return;

    if ((int)idx != atomic_load_explicit(&g_last_idx, memory_order_relaxed)) {
        set_volume_from_index((int)idx);
    }
}

static int find_module_path(const char *soname, char *out, size_t out_sz) {
    if (!soname || !out || out_sz == 0) return -1;

    FILE *fp = fopen("/proc/self/maps", "re");
    if (!fp) return -1;

    char line[1024];
    int rc = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, soname)) continue;
        char *path = strchr(line, '/');
        if (!path) continue;
        char *nl = strchr(path, '\n');
        if (nl) *nl = '\0';
        strncpy(out, path, out_sz - 1);
        out[out_sz - 1] = '\0';
        rc = 0;
        break;
    }
    fclose(fp);
    return rc;
}

static void resolve_real_voip_write(void) {
    if (g_real_voip_write) return;

    pthread_mutex_lock(&g_voip_resolve_lock);
    if (!g_real_voip_write) {
        g_real_voip_write = (voip_write_fn_t)dlsym(RTLD_NEXT, "Voip_write");
        if (!g_real_voip_write) {
            char path[512];
            if (find_module_path(k_voip_resampler_name, path, sizeof(path)) == 0) {
                void *h = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
                if (!h) h = dlopen(path, RTLD_NOW);
                if (h) g_real_voip_write = (voip_write_fn_t)dlsym(h, "Voip_write");
            }
        }
    }
    pthread_mutex_unlock(&g_voip_resolve_lock);
}

static void scale_buffer_s16le(void *buf_ptr, int byte_size, float volume) {
    if (!buf_ptr || byte_size <= 0) return;
    if (volume >= 0.999f) return;

    int16_t *samples = (int16_t *)buf_ptr;
    int count = byte_size / 2;
    for (int i = 0; i < count; ++i) {
        float scaled_f = (float)samples[i] * volume;
        long scaled = lroundf(scaled_f);
        if (scaled > 32767) scaled = 32767;
        else if (scaled < -32768) scaled = -32768;
        samples[i] = (int16_t)scaled;
    }
}

__attribute__((visibility("default")))
int Voip_write(void *buf, int byte_size) {
    resolve_real_voip_write();
    refresh_volume_from_property();

    if (buf && byte_size > 0 && byte_size < 65536) {
        scale_buffer_s16le(buf, byte_size, current_volume());
    }

    if (g_real_voip_write) return g_real_voip_write(buf, byte_size);
    return -1;
}

__attribute__((constructor))
static void voip_fix_init(void) {
    atomic_store_explicit(&g_volume, 1.0f, memory_order_relaxed);
    atomic_store_explicit(&g_last_idx, -1, memory_order_relaxed);
    atomic_store_explicit(&g_last_poll_ns, 0, memory_order_relaxed);
    refresh_volume_from_property();
    log_line("property mode init");
}
