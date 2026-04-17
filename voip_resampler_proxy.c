#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Proxy replacement for libsamsungVoipResampler.so (32-bit vendor path).
 *
 * Runtime behavior:
 * - Derives own path from /proc/self/maps
 * - Loads:
 *     <dir>/libvoip_volume_fix.so
 *     <dir>/libsamsungVoipResampler_real.so
 * - Forwards all Voip_* exports to real library
 * - Calls shim Voip_write first, then falls back to real if needed
 */

typedef int (*fn_Voip_create_t)(void);
typedef void (*fn_Voip_destroy_t)(void);
typedef int (*fn_Voip_init_t)(void);
typedef int (*fn_Voip_isVoipMode_t)(void);
typedef int (*fn_Voip_read_t)(void *buf, int byte_size);
typedef int (*fn_Voip_setMode_t)(int mode);
typedef int (*fn_Voip_setRealCall_t)(int enable);
typedef int (*fn_Voip_setRx_t)(int a, int b, int c, int d, int e, int f);
typedef int (*fn_Voip_setRxHandle_t)(void *pcm_handle);
typedef int (*fn_Voip_setTx_t)(int a, int b, int c, int d, int e, int f);
typedef int (*fn_Voip_setTxHandle_t)(void *pcm_handle);
typedef int (*fn_Voip_setVoipMode_t)(int mode);
typedef int (*fn_Voip_stop_t)(void);
typedef int (*fn_Voip_write_t)(void *buf, int byte_size);

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void *g_real_handle = NULL;
static fn_Voip_write_t g_shim_voip_write = NULL;

static fn_Voip_create_t g_real_Voip_create = NULL;
static fn_Voip_destroy_t g_real_Voip_destroy = NULL;
static fn_Voip_init_t g_real_Voip_init = NULL;
static fn_Voip_isVoipMode_t g_real_Voip_isVoipMode = NULL;
static fn_Voip_read_t g_real_Voip_read = NULL;
static fn_Voip_setMode_t g_real_Voip_setMode = NULL;
static fn_Voip_setRealCall_t g_real_Voip_setRealCall = NULL;
static fn_Voip_setRx_t g_real_Voip_setRx = NULL;
static fn_Voip_setRxHandle_t g_real_Voip_setRxHandle = NULL;
static fn_Voip_setTx_t g_real_Voip_setTx = NULL;
static fn_Voip_setTxHandle_t g_real_Voip_setTxHandle = NULL;
static fn_Voip_setVoipMode_t g_real_Voip_setVoipMode = NULL;
static fn_Voip_stop_t g_real_Voip_stop = NULL;
static fn_Voip_write_t g_real_Voip_write = NULL;

static void proxy_log(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    int fd = open("/data/adb/voip_volume_fix.log", O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd >= 0) {
        dprintf(fd, "[proxy32] %s\n", line);
        close(fd);
        return;
    }

    fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        dprintf(fd, "voip_volume_fix_proxy32: %s\n", line);
        close(fd);
    }
}

static int find_self_path(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    FILE *fp = fopen("/proc/self/maps", "re");
    if (!fp) return -1;

    char line[1024];
    int rc = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "libsamsungVoipResampler.so")) continue;
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

static void build_sibling_path(const char *self_path, const char *name, char *out, size_t out_sz) {
    if (!self_path || !name || !out || out_sz == 0) return;
    const char *slash = strrchr(self_path, '/');
    if (!slash) {
        snprintf(out, out_sz, "%s", name);
        return;
    }

    size_t dir_len = (size_t)(slash - self_path);
    if (dir_len + 1 + strlen(name) + 1 > out_sz) {
        out[0] = '\0';
        return;
    }

    memcpy(out, self_path, dir_len);
    out[dir_len] = '/';
    strcpy(out + dir_len + 1, name);
}

static void resolve_symbol(const char *name, void **fn_ptr) {
    if (!g_real_handle || !name || !fn_ptr) return;
    *fn_ptr = dlsym(g_real_handle, name);
    if (!*fn_ptr) proxy_log("missing real symbol: %s", name);
}

static void proxy_init_once(void) {
    char self_path[512] = {0};
    char shim_path[512] = {0};
    char real_path[512] = {0};

    if (find_self_path(self_path, sizeof(self_path)) != 0) {
        snprintf(self_path, sizeof(self_path), "/vendor/lib/libsamsungVoipResampler.so");
    }

    build_sibling_path(self_path, "libvoip_volume_fix.so", shim_path, sizeof(shim_path));
    build_sibling_path(self_path, "libsamsungVoipResampler_real.so", real_path, sizeof(real_path));

    proxy_log("self=%s", self_path);
    proxy_log("shim=%s", shim_path);
    proxy_log("real=%s", real_path);

    void *shim = dlopen(shim_path, RTLD_NOW | RTLD_GLOBAL);
    if (!shim) {
        proxy_log("shim dlopen failed");
    } else {
        g_shim_voip_write = (fn_Voip_write_t)dlsym(shim, "Voip_write");
        if (!g_shim_voip_write) proxy_log("shim Voip_write not found");
    }

    g_real_handle = dlopen(real_path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_real_handle) {
        proxy_log("real dlopen failed");
        return;
    }

    resolve_symbol("Voip_create", (void **)&g_real_Voip_create);
    resolve_symbol("Voip_destroy", (void **)&g_real_Voip_destroy);
    resolve_symbol("Voip_init", (void **)&g_real_Voip_init);
    resolve_symbol("Voip_isVoipMode", (void **)&g_real_Voip_isVoipMode);
    resolve_symbol("Voip_read", (void **)&g_real_Voip_read);
    resolve_symbol("Voip_setMode", (void **)&g_real_Voip_setMode);
    resolve_symbol("Voip_setRealCall", (void **)&g_real_Voip_setRealCall);
    resolve_symbol("Voip_setRx", (void **)&g_real_Voip_setRx);
    resolve_symbol("Voip_setRxHandle", (void **)&g_real_Voip_setRxHandle);
    resolve_symbol("Voip_setTx", (void **)&g_real_Voip_setTx);
    resolve_symbol("Voip_setTxHandle", (void **)&g_real_Voip_setTxHandle);
    resolve_symbol("Voip_setVoipMode", (void **)&g_real_Voip_setVoipMode);
    resolve_symbol("Voip_stop", (void **)&g_real_Voip_stop);
    resolve_symbol("Voip_write", (void **)&g_real_Voip_write);

    proxy_log("init complete");
}

static inline void ensure_proxy_ready(void) {
    pthread_once(&g_once, proxy_init_once);
}

__attribute__((constructor))
static void proxy_ctor(void) {
    ensure_proxy_ready();
}

__attribute__((visibility("default")))
int Voip_create(void) {
    ensure_proxy_ready();
    return g_real_Voip_create ? g_real_Voip_create() : -1;
}

__attribute__((visibility("default")))
void Voip_destroy(void) {
    ensure_proxy_ready();
    if (g_real_Voip_destroy) g_real_Voip_destroy();
}

__attribute__((visibility("default")))
int Voip_init(void) {
    ensure_proxy_ready();
    return g_real_Voip_init ? g_real_Voip_init() : -1;
}

__attribute__((visibility("default")))
int Voip_isVoipMode(void) {
    ensure_proxy_ready();
    return g_real_Voip_isVoipMode ? g_real_Voip_isVoipMode() : 0;
}

__attribute__((visibility("default")))
int Voip_read(void *buf, int byte_size) {
    ensure_proxy_ready();
    return g_real_Voip_read ? g_real_Voip_read(buf, byte_size) : -1;
}

__attribute__((visibility("default")))
int Voip_setMode(int mode) {
    ensure_proxy_ready();
    return g_real_Voip_setMode ? g_real_Voip_setMode(mode) : -1;
}

__attribute__((visibility("default")))
int Voip_setRealCall(int enable) {
    ensure_proxy_ready();
    return g_real_Voip_setRealCall ? g_real_Voip_setRealCall(enable) : -1;
}

__attribute__((visibility("default")))
int Voip_setRx(int a, int b, int c, int d, int e, int f) {
    ensure_proxy_ready();
    return g_real_Voip_setRx ? g_real_Voip_setRx(a, b, c, d, e, f) : -1;
}

__attribute__((visibility("default")))
int Voip_setRxHandle(void *pcm_handle) {
    ensure_proxy_ready();
    return g_real_Voip_setRxHandle ? g_real_Voip_setRxHandle(pcm_handle) : -1;
}

__attribute__((visibility("default")))
int Voip_setTx(int a, int b, int c, int d, int e, int f) {
    ensure_proxy_ready();
    return g_real_Voip_setTx ? g_real_Voip_setTx(a, b, c, d, e, f) : -1;
}

__attribute__((visibility("default")))
int Voip_setTxHandle(void *pcm_handle) {
    ensure_proxy_ready();
    return g_real_Voip_setTxHandle ? g_real_Voip_setTxHandle(pcm_handle) : -1;
}

__attribute__((visibility("default")))
int Voip_setVoipMode(int mode) {
    ensure_proxy_ready();
    return g_real_Voip_setVoipMode ? g_real_Voip_setVoipMode(mode) : -1;
}

__attribute__((visibility("default")))
int Voip_stop(void) {
    ensure_proxy_ready();
    return g_real_Voip_stop ? g_real_Voip_stop() : -1;
}

__attribute__((visibility("default")))
int Voip_write(void *buf, int byte_size) {
    ensure_proxy_ready();
    if (g_shim_voip_write) {
        int rc = g_shim_voip_write(buf, byte_size);
        if (rc != -1) return rc;
    }
    return g_real_Voip_write ? g_real_Voip_write(buf, byte_size) : -1;
}

