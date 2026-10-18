// check_host.c
// Funciones para verificar el estado de los hosts registrados en Moonlight Vita
// Muestra: verde (en línea), rojo (desconectado), amarillo (IP cambió pero es el mismo host)

#include "check_host.h"
#include "device.h"
#include "config.h"
#include "util.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include "debug.h"

#ifdef __vita__
#include "udp_sniffer_vita.h"
#include <psp2/kernel/threadmgr.h>
#define MAX_HOSTS 16

// The mDNS sniffer (third_party/mdnsniff) keeps one static socket, callback
// and host table, and isn't thread-safe: its poll creates the socket lazily
// and its deinit closes it. The host scan thread, the Search thread and
// find_host_ip_mdns() all use it, so each takes this lock around its use.
// Without it, two threads polling at once could each open a socket (leaking
// one, still a multicast member), and one thread's deinit closed another's
// socket mid-poll.
static SceUID sniffer_mutex = -1;

bool mdns_sniffer_lock(SceUInt timeout_us) {
    SceUID mutex = __atomic_load_n(&sniffer_mutex, __ATOMIC_ACQUIRE);
    if (mutex < 0) {
        // Created on first use; if two threads race here, one keeps its mutex
        SceUID created = sceKernelCreateMutex("mdns_sniffer", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
        if (created < 0) {
            vita_debug_log("[mDNS] couldn't create the sniffer lock: 0x%08x", created);
            return false;
        }
        SceUID expected = -1;
        if (__atomic_compare_exchange_n(&sniffer_mutex, &expected, created, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            mutex = created;
        } else {
            sceKernelDeleteMutex(created);
            mutex = expected;
        }
    }
    SceUInt timeout = timeout_us;
    return sceKernelLockMutex(mutex, 1, &timeout) >= 0;
}

void mdns_sniffer_unlock(void) {
    sceKernelUnlockMutex(__atomic_load_n(&sniffer_mutex, __ATOMIC_ACQUIRE), 1);
}

// Variables estáticas para comunicación con el callback
static volatile int mdns_found = 0;
static char mdns_found_ip[64] = "";
static char mdns_hostname_ref[256] = "";

static void mdns_found_cb(int idx, const char* host, const char* pcname, const char* ip, int port) {
    if (strcmp(pcname, mdns_hostname_ref) == 0) {
        strncpy((char*)mdns_found_ip, ip, sizeof(mdns_found_ip)-1);
        mdns_found = 1;
    }
}

// Busca la IP actual de un host por mDNS (sniffer Vita)
bool find_host_ip_mdns(const char *hostname, char *out_ip, size_t out_len) {
    mdns_found = 0;
    mdns_found_ip[0] = '\0';
    strncpy(mdns_hostname_ref, hostname, sizeof(mdns_hostname_ref)-1);
    mdns_hostname_ref[sizeof(mdns_hostname_ref)-1] = '\0';
    // Normally free, since the host scan and Search are stopped by then; if
    // not, fall back to the other checks rather than hold up the caller
    if (!mdns_sniffer_lock(2 * 1000 * 1000)) {
        vita_debug_log("[mDNS] sniffer busy, skipping the mDNS lookup of %s", hostname);
        return false;
    }
    udp_sniffer_vita_deinit();
    udp_sniffer_vita_init();
    udp_sniffer_vita_set_callback(mdns_found_cb);
    int ticks = 0, max_ticks = 30; // 3 segundos
    while (!mdns_found && ticks < max_ticks) {
        udp_sniffer_vita_poll();
        sceKernelDelayThread(1000 * 100); // 100ms
        ticks++;
    }
    udp_sniffer_vita_set_callback(NULL);
    udp_sniffer_vita_deinit();
    mdns_sniffer_unlock();
    if (mdns_found) {
        strncpy(out_ip, mdns_found_ip, out_len-1);
        out_ip[out_len-1] = '\0';
        return true;
    }
    return false;
}

volatile int g_host_scan_thread_status = 0;
struct host_status g_host_status[MAX_HOSTS];
// The one host scan thread, until stop_host_scan_thread() has seen it end
static SceUID host_scan_thread_id = -1;

// Declaración anticipada para evitar warning/errores
static bool ping_host(const char *ip, uint16_t port);

// Callback estático para el hilo de escaneo de hosts
static void host_scan_mdns_cb(int idx, const char* host, const char* pcname, const char* ip, int port) {
    if (idx < 0 || idx >= MAX_HOSTS) return;
    if (strcmp(pcname, known_devices.devices[idx].name) == 0) {
        strncpy(g_host_status[idx].current_ip, ip, sizeof(g_host_status[idx].current_ip)-1);
        g_host_status[idx].current_ip[sizeof(g_host_status[idx].current_ip)-1] = '\0';
        // Se puede agregar más lógica si se requiere
    }
}

static volatile int scan_mdns_found = 0;
static char scan_mdns_found_ip[64] = "";
static char scan_mdns_hostname_ref[256] = "";

static void scan_mdns_found_cb(int idx, const char* host, const char* pcname, const char* ip, int port) {
    if (strcmp(pcname, scan_mdns_hostname_ref) == 0) {
        strncpy((char*)scan_mdns_found_ip, ip, sizeof(scan_mdns_found_ip)-1);
        scan_mdns_found = 1;
    }
}

volatile int g_host_status_changed = 0;

// Checked between every step, so stopping the scan doesn't wait for a whole pass
#define SCAN_RUNNING() (g_host_scan_thread_status == 1)

// Takes the sniffer lock for the scan, giving up if the scan is stopped meanwhile
static bool scan_lock_sniffer(void) {
    while (SCAN_RUNNING()) {
        if (mdns_sniffer_lock(100 * 1000)) {
            return true;
        }
    }
    return false;
}

static int host_scan_thread(SceSize args, void *argp) {
    int ticks = 0;
    vita_debug_log("[SCAN] Hilo de escaneo iniciado\n");
    if (scan_lock_sniffer()) {
        udp_sniffer_vita_deinit();
        udp_sniffer_vita_init();
        mdns_sniffer_unlock();
    }
    while (SCAN_RUNNING() && ticks < 1000) { // 100s máx
        for (int i = 0; i < known_devices.count && i < MAX_HOSTS && SCAN_RUNNING(); i++) {
            device_info_t *info = &known_devices.devices[i];
            if (!info->paired) {
                if (g_host_status[i].status != HOST_OFFLINE) {
                    vita_debug_log("[SCAN] Host %s marcado como OFFLINE (no paired)\n", info->name);
                    g_host_status_changed = 1;
                }
                g_host_status[i].status = HOST_OFFLINE;
                g_host_status[i].current_ip[0] = '\0';
                continue;
            }
            // --- mDNS polling manual usando variables globales temporales ---
            scan_mdns_found = 0;
            scan_mdns_found_ip[0] = '\0';
            strncpy(scan_mdns_hostname_ref, info->name, sizeof(scan_mdns_hostname_ref)-1);
            scan_mdns_hostname_ref[sizeof(scan_mdns_hostname_ref)-1] = '\0';
            if (!scan_lock_sniffer()) {
                break;
            }
            udp_sniffer_vita_set_callback(scan_mdns_found_cb);
            int mdns_ticks = 0;
            while (!scan_mdns_found && mdns_ticks < 10 && SCAN_RUNNING()) { // 1s máx
                udp_sniffer_vita_poll();
                sceKernelDelayThread(1000 * 100); // 100ms
                mdns_ticks++;
            }
            udp_sniffer_vita_set_callback(NULL);
            mdns_sniffer_unlock();
            if (!SCAN_RUNNING()) {
                break;
            }
            if (scan_mdns_found) {
                vita_debug_log("[SCAN] Host %s encontrado por mDNS: IP=%s\n", info->name, scan_mdns_found_ip);
                if (strcmp(scan_mdns_found_ip, info->internal) == 0) {
                    if (ping_host(scan_mdns_found_ip, info->port)) {
                        if (g_host_status[i].status != HOST_ONLINE) {
                            vita_debug_log("[SCAN] Host %s ONLINE (mDNS IP igual a internal)\n", info->name);
                            g_host_status_changed = 1;
                        }
                        g_host_status[i].status = HOST_ONLINE;
                        strncpy(g_host_status[i].current_ip, scan_mdns_found_ip, sizeof(g_host_status[i].current_ip)-1);
                        g_host_status[i].current_ip[sizeof(g_host_status[i].current_ip)-1] = '\0';
                        continue;
                    }
                } else {
                    if (ping_host(scan_mdns_found_ip, info->port)) {
                        if (g_host_status[i].status != HOST_IP_CHANGED) {
                            vita_debug_log("[SCAN] Host %s IP CAMBIADA: old=%s, new=%s\n", info->name, info->internal, scan_mdns_found_ip);
                            g_host_status_changed = 1;
                        }
                        g_host_status[i].status = HOST_IP_CHANGED;
                        strncpy(g_host_status[i].current_ip, scan_mdns_found_ip, sizeof(g_host_status[i].current_ip)-1);
                        g_host_status[i].current_ip[sizeof(g_host_status[i].current_ip)-1] = '\0';
                        // Setear flags globales para el menú
                        extern int pending_ip_update_idx;
                        extern char pending_ip_update[64];
                        pending_ip_update_idx = i;
                        strncpy(pending_ip_update, scan_mdns_found_ip, sizeof(pending_ip_update)-1);
                        pending_ip_update[sizeof(pending_ip_update)-1] = '\0';
                        // Detener el hilo de escaneo y sniffer al detectar cambio de IP
                        vita_debug_log("[SCAN] Cambio de IP detectado, deteniendo hilo de escaneo y sniffer\n");
                        g_host_scan_thread_status = 0; // Salir del hilo inmediatamente
                        break;
                    }
                }
            }
            if (!SCAN_RUNNING()) {
                break;
            }
            if (ping_host(info->internal, info->port)) {
                if (g_host_status[i].status != HOST_ONLINE) {
                    vita_debug_log("[SCAN] Host %s ONLINE (por IP interna)\n", info->name);
                    g_host_status_changed = 1;
                }
                g_host_status[i].status = HOST_ONLINE;
                strncpy(g_host_status[i].current_ip, info->internal, sizeof(g_host_status[i].current_ip)-1);
                g_host_status[i].current_ip[sizeof(g_host_status[i].current_ip)-1] = '\0';
                continue;
            }
            if (g_host_status[i].status != HOST_OFFLINE) {
                vita_debug_log("[SCAN] Host %s OFFLINE (no responde)\n", info->name);
                g_host_status_changed = 1;
            }
            g_host_status[i].status = HOST_OFFLINE;
            strncpy(g_host_status[i].current_ip, info->internal, sizeof(g_host_status[i].current_ip)-1);
            g_host_status[i].current_ip[sizeof(g_host_status[i].current_ip)-1] = '\0';
        }
        for (int wait = 0; wait < 5 && SCAN_RUNNING(); wait++) {
            sceKernelDelayThread(1000 * 100); // 500ms
        }
        ticks++;
    }
    // The sniffer may be someone else's by now; then they close it
    if (mdns_sniffer_lock(2 * 1000 * 1000)) {
        udp_sniffer_vita_deinit();
        mdns_sniffer_unlock();
    }
    vita_debug_log("[SCAN] Hilo de escaneo detenido\n");
    g_host_scan_thread_status = 0;
    return 0;
}

void start_host_scan_thread() {
    if (g_host_scan_thread_status == 1 && host_scan_thread_id >= 0) return;
    // Reaps a scan that ended by itself, or waits for one still stopping, so
    // there's never more than one
    stop_host_scan_thread();
    if (host_scan_thread_id >= 0) {
        vita_debug_log("[SCAN] El hilo anterior sigue activo, no se creará otro\n");
        return;
    }
    for (int i = 0; i < MAX_HOSTS; i++) {
        g_host_status[i].status = HOST_OFFLINE;
        g_host_status[i].current_ip[0] = '\0';
    }
    SceUID tid = sceKernelCreateThread("hostscan", host_scan_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (tid < 0) {
        vita_debug_log("[SCAN] Error creando hilo de escaneo: 0x%08x\n", tid);
        return;
    }
    // Set before it starts, so the thread can't miss a stop sent right away
    g_host_scan_thread_status = 1;
    if (sceKernelStartThread(tid, 0, 0) < 0) {
        g_host_scan_thread_status = 0;
        sceKernelDeleteThread(tid);
        return;
    }
    host_scan_thread_id = tid;
}

void stop_host_scan_thread() {
    SceUID tid = host_scan_thread_id;
    if (tid < 0) return;
    if (g_host_scan_thread_status == 1) {
        g_host_scan_thread_status = 2;
    }
    // The thread checks for the stop every 100 ms, and a ping takes at most
    // a second, so this normally returns well within the limit
    for (int waited = 0; waited < 50; waited++) {
        SceUInt timeout = 100 * 1000; // 100ms
        if (sceKernelWaitThreadEnd(tid, NULL, &timeout) >= 0) {
            sceKernelDeleteThread(tid);
            host_scan_thread_id = -1;
            g_host_scan_thread_status = 0;
            return;
        }
    }
    // Kept, so start_host_scan_thread() doesn't start a second one beside it
    vita_debug_log("[SCAN] El hilo de escaneo no terminó en 5 s\n");
}
#endif

// El struct host_status y los defines ya están en check_host.h, no se redeclaran aquí

// Intenta conectar al host por IP y puerto, retorna true si responde
static bool ping_host(const char *ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(sock);
        return false;
    }
    // SO_SNDTIMEO may not bound connect(), so connect without blocking and
    // wait up to a second, as moonlight-common-c's connectTcpSocket() does
#ifdef SO_NONBLOCK
    int nonblock = 1;
    setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, (const char*)&nonblock, sizeof nonblock);
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
#endif
    bool connected = false;
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        connected = true;
    } else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN) {
        struct pollfd pfd = { .fd = sock, .events = POLLOUT };
        if (poll(&pfd, 1, 1000) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            connected = getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0
                        && !(pfd.revents & POLLERR);
        }
    }
    close(sock);
    return connected;
}

// Busca la IP actual de un host por nombre (DNS o mDNS)
static bool resolve_host_ip(const char *hostname, char *out_ip, size_t out_len) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) return false;
    struct sockaddr_in *addr = (struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, out_ip, out_len);
    freeaddrinfo(res);
    return true;
}

// Chequea el estado de un host registrado
struct host_status check_host_status(const device_info_t *info) {
    struct host_status result = {0};
#ifdef __vita__
    // 1. Intentar mDNS primero
    char mdns_ip[64] = "";
    if (find_host_ip_mdns(info->name, mdns_ip, sizeof(mdns_ip))) {
        if (strcmp(mdns_ip, info->internal) == 0) {
            if (ping_host(mdns_ip, info->port)) {
                result.status = HOST_ONLINE;
                strncpy(result.current_ip, mdns_ip, sizeof(result.current_ip));
                return result;
            }
        } else {
            if (ping_host(mdns_ip, info->port)) {
                result.status = HOST_IP_CHANGED;
                strncpy(result.current_ip, mdns_ip, sizeof(result.current_ip));
                return result;
            }
        }
    }
#endif
    // 2. Intentar ping a la IP registrada
    if (ping_host(info->internal, info->port)) {
        result.status = HOST_ONLINE;
        strncpy(result.current_ip, info->internal, sizeof(result.current_ip));
        return result;
    }
    // 3. Intentar resolver por nombre (DNS estándar)
    if (strcmp(info->name, info->internal) != 0) {
        char resolved_ip[64] = "";
        if (resolve_host_ip(info->name, resolved_ip, sizeof(resolved_ip))) {
            if (ping_host(resolved_ip, info->port)) {
                result.status = HOST_IP_CHANGED;
                strncpy(result.current_ip, resolved_ip, sizeof(result.current_ip));
                return result;
            }
        }
    }
    // 4. No responde
    result.status = HOST_OFFLINE;
    strncpy(result.current_ip, info->internal, sizeof(result.current_ip));
    return result;
}
