#include "usb_storage.h"
#include "usb_storage_recovery.h"
#include "usb_storage_session.h"
#include "media_io_gate.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb_media_mount.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "usb_storage";

#define USB_LIB_TASK_STACK       4096
#define USB_LIB_TASK_PRIO        4
#define MSC_TASK_STACK           4096
#define MSC_TASK_PRIO            5
/* The mount callback runs library_init() (PDB parse) + UI refresh. */
#define STORAGE_TASK_STACK       (16 * 1024)
#define STORAGE_TASK_PRIO        3

#define CONNECT_STABLE_MS        350u
#define RECONCILE_POLL_MS        500u
#define MOUNT_RETRY_INITIAL_MS   500u
#define MOUNT_RETRY_MAX_MS       30000u

/* Root-port recovery for a drive that remained powered across a software reset. */
#define ROOT_PORT_SETTLE_MS      150u
#define ROOT_PORT_RETRY_MS       900u
#define ROOT_PORT_MAX_CYCLES     8u
#define ROOT_PORT_SLOW_MS        30000u

#ifndef USB_STORAGE_DEVICE_ROUTE_ALLOWED
#define USB_STORAGE_DEVICE_ROUTE_ALLOWED(address) (true)
#endif

#ifndef USB_STORAGE_REQUEST_ROOT_RECOVERY
#define USB_STORAGE_REQUEST_ROOT_RECOVERY(why) (false)
#endif

/* One physical drive. s_state_mux guards `session`; the storage task is the sole
 * owner of `handle` / `mount` / `announced` / backoff. `base_path` is fixed at
 * init: slot 0 -> "/usb", slot i -> "/usb<i+1>". */
struct usb_dev_slot {
    usb_storage_session_t session;
    msc_host_device_handle_t handle;
    usb_media_mount_t *mount;
    char base_path[USB_STORAGE_PATH_MAX];
    bool announced;
    uint32_t retry_ms;
    TickType_t next_attempt;
};

static TaskHandle_t             s_storage_task;
static TaskHandle_t             s_usb_lib_task;
static usb_storage_event_cb_t   s_cb;
static struct usb_dev_slot      s_devs[USB_STORAGE_MAX_DEVICES];

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint_fast32_t s_connect_events;
static atomic_uint_fast32_t s_connect_accepted;
static atomic_uint_fast32_t s_disconnect_events;
static atomic_uint_fast32_t s_disconnect_accepted;
static atomic_uint_fast32_t s_mount_attempts;
static atomic_uint_fast32_t s_mount_successes;
static atomic_int s_last_mount_result;
static atomic_uint_fast32_t s_releases;
static atomic_int s_last_unmount_result;
static atomic_int s_last_uninstall_result;

/* ── slot state snapshots (under s_state_mux) ─────────────────────────────── */

static usb_storage_session_t desired_snapshot(unsigned slot)
{
    usb_storage_session_t snapshot;
    portENTER_CRITICAL(&s_state_mux);
    snapshot = s_devs[slot].session;
    portEXIT_CRITICAL(&s_state_mux);
    return snapshot;
}

static bool desired_matches(unsigned slot, uint32_t epoch, uint8_t dev_addr)
{
    bool matches;
    portENTER_CRITICAL(&s_state_mux);
    matches = usb_storage_session_matches(&s_devs[slot].session, epoch, dev_addr);
    portEXIT_CRITICAL(&s_state_mux);
    return matches;
}

/* Caller holds s_state_mux. */
static int slot_for_addr_locked(uint8_t dev_addr)
{
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVICES; ++i) {
        if (s_devs[i].session.connected &&
            s_devs[i].session.dev_addr == dev_addr) {
            return (int)i;
        }
    }
    return -1;
}

static int free_slot_locked(void)
{
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVICES; ++i) {
        if (!s_devs[i].session.connected) {
            return (int)i;
        }
    }
    return -1;
}

/* An enumeration bounce retires an address before install_device() binds a
 * handle; the driver then has no device object to report DEV_GONE through. Only
 * supersede such a stale unbound slot, and only when no fresh slot is free. */
static int stale_unbound_slot_locked(void)
{
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVICES; ++i) {
        if (s_devs[i].session.connected &&
            s_devs[i].session.accepted_handle == 0u &&
            !s_devs[i].session.mounted) {
            return (int)i;
        }
    }
    return -1;
}

static unsigned devices_connected(void)
{
    unsigned n = 0;
    portENTER_CRITICAL(&s_state_mux);
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVICES; ++i) {
        if (s_devs[i].session.connected) n++;
    }
    portEXIT_CRITICAL(&s_state_mux);
    return n;
}

static unsigned devices_mounted(void)
{
    unsigned n = 0;
    portENTER_CRITICAL(&s_state_mux);
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVICES; ++i) {
        if (s_devs[i].session.mounted) n++;
    }
    portEXIT_CRITICAL(&s_state_mux);
    return n;
}

static void refresh_gate_availability(void)
{
    media_io_gate_set_available(devices_mounted() > 0);
}

static void notify_storage_owner(void)
{
    TaskHandle_t task = s_storage_task;
    if (task) {
        xTaskNotifyGive(task);
    }
}

/* ── driver-context desired-state publishers ─────────────────────────────── */

static void publish_desired_connect(uint8_t dev_addr)
{
    atomic_fetch_add_explicit(&s_connect_events, 1u, memory_order_relaxed);

    usb_storage_connect_result_t result = USB_STORAGE_CONNECT_IGNORED_SECONDARY;
    portENTER_CRITICAL(&s_state_mux);
    int slot = slot_for_addr_locked(dev_addr);
    if (slot < 0) {
        slot = free_slot_locked();
    }
    if (slot < 0) {
        slot = stale_unbound_slot_locked();
    }
    if (slot >= 0) {
        result = usb_storage_session_on_connect(&s_devs[slot].session, dev_addr);
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (result == USB_STORAGE_CONNECT_IGNORED_SECONDARY) {
        ESP_LOGW(TAG, "USB storage device ignored, all %u slots busy (addr=%u)",
                 (unsigned)USB_STORAGE_MAX_DEVICES, (unsigned)dev_addr);
        return;
    }
    if (result == USB_STORAGE_CONNECT_ACCEPTED) {
        atomic_fetch_add_explicit(&s_connect_accepted, 1u, memory_order_relaxed);
        notify_storage_owner();
    }
}

static void publish_desired_disconnect(msc_host_device_handle_t handle)
{
    atomic_fetch_add_explicit(&s_disconnect_events, 1u, memory_order_relaxed);

    bool accepted = false;
    portENTER_CRITICAL(&s_state_mux);
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVICES; ++i) {
        if (!s_devs[i].session.connected) {
            continue;
        }
        usb_storage_disconnect_result_t r = usb_storage_session_on_disconnect(
            &s_devs[i].session, (uintptr_t)handle);
        if (r == USB_STORAGE_DISCONNECT_ACCEPTED) {
            accepted = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (!accepted) {
        ESP_LOGW(TAG, "disconnect for unknown USB storage handle ignored");
        return;
    }
    atomic_fetch_add_explicit(&s_disconnect_accepted, 1u, memory_order_relaxed);
    /* The owner task performs the safe unmount and recomputes gate availability
     * from the remaining mounts. A read that races the dying drive fails at the
     * MSC layer rather than being blanket-blocked for every stick. */
    notify_storage_owner();
}

static void root_port_power_cycle(const char *why)
{
    ESP_LOGI(TAG, "root port power cycle (%s)", why ? why : "");
    if (USB_STORAGE_REQUEST_ROOT_RECOVERY(why)) {
        return;
    }
    esp_err_t rc = usb_host_lib_set_root_port_power(false);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "root port power off: %s", esp_err_to_name(rc));
    }
    vTaskDelay(pdMS_TO_TICKS(ROOT_PORT_SETTLE_MS));
    rc = usb_host_lib_set_root_port_power(true);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "root port power on: %s", esp_err_to_name(rc));
    }
}

/* ── storage-task-owned mount lifecycle (per slot) ───────────────────────── */

static void release_device(unsigned slot)
{
    /* Detach the sole-owner handles before entering either teardown routine.
     * A DEV_GONE edge can be published while the MSC/VFS layers are being
     * dismantled; no reconcile pass may observe and release the same handles
     * twice. */
    usb_media_mount_t *released_mount = s_devs[slot].mount;
    msc_host_device_handle_t released_handle = s_devs[slot].handle;
    s_devs[slot].mount = NULL;
    s_devs[slot].handle = NULL;
    atomic_fetch_add_explicit(&s_releases, 1u, memory_order_relaxed);

    if (released_handle) {
        portENTER_CRITICAL(&s_state_mux);
        usb_storage_session_release_handle(
            &s_devs[slot].session, (uintptr_t)released_handle);
        portEXIT_CRITICAL(&s_state_mux);
    }

    media_io_gate_begin();
    if (released_mount) {
        esp_err_t rc = usb_media_unmount(released_mount);
        atomic_store_explicit(&s_last_unmount_result, rc, memory_order_relaxed);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "usb_media_unmount(%s): %s",
                     s_devs[slot].base_path, esp_err_to_name(rc));
        }
    }
    if (released_handle) {
        esp_err_t rc = msc_host_uninstall_device(released_handle);
        atomic_store_explicit(&s_last_uninstall_result, rc, memory_order_relaxed);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "msc_host_uninstall_device: %s", esp_err_to_name(rc));
        }
    }
    media_io_gate_end();
}

static void publish_unmounted(unsigned slot)
{
    bool notify = false;
    portENTER_CRITICAL(&s_state_mux);
    usb_storage_session_mark_unmounted(&s_devs[slot].session);
    if (s_devs[slot].announced) {
        s_devs[slot].announced = false;
        notify = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    refresh_gate_availability();
    if (notify && s_cb) {
        usb_storage_mount_event_t ev = { .index = (uint8_t)slot, .mounted = false };
        strncpy(ev.base_path, s_devs[slot].base_path, sizeof(ev.base_path) - 1u);
        s_cb(&ev);
    }
}

static bool commit_mounted(unsigned slot, uint32_t epoch, uint8_t dev_addr)
{
    bool committed = false;
    portENTER_CRITICAL(&s_state_mux);
    if (usb_storage_session_commit_mounted(
            &s_devs[slot].session, epoch, dev_addr)) {
        s_devs[slot].announced = true;
        committed = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (!committed) {
        return false;
    }
    refresh_gate_availability();
    if (s_cb) {
        usb_storage_mount_event_t ev = { .index = (uint8_t)slot, .mounted = true };
        strncpy(ev.base_path, s_devs[slot].base_path, sizeof(ev.base_path) - 1u);
        s_cb(&ev);
    }
    return true;
}

bool usb_storage_is_mounted(void)
{
    return devices_mounted() > 0;
}

unsigned usb_storage_mounted_count(void)
{
    return devices_mounted();
}

bool usb_storage_mount_path(unsigned index, char *out, size_t len)
{
    if (index >= USB_STORAGE_MAX_DEVICES || !out || len == 0u) {
        return false;
    }
    bool mounted;
    portENTER_CRITICAL(&s_state_mux);
    mounted = s_devs[index].session.mounted;
    portEXIT_CRITICAL(&s_state_mux);
    if (!mounted) {
        return false;
    }
    strncpy(out, s_devs[index].base_path, len - 1u);
    out[len - 1u] = '\0';
    return true;
}

void usb_storage_get_diagnostics(usb_storage_diagnostics_t *out)
{
    if (!out) {
        return;
    }
    unsigned connected = devices_connected();
    unsigned mounted = devices_mounted();
    *out = (usb_storage_diagnostics_t) {
        .desired_connected = connected > 0,
        .mounted = mounted > 0,
        .devices_connected = (uint8_t)connected,
        .devices_mounted = (uint8_t)mounted,
        .connect_events = (uint32_t)atomic_load_explicit(
            &s_connect_events, memory_order_relaxed),
        .connect_accepted = (uint32_t)atomic_load_explicit(
            &s_connect_accepted, memory_order_relaxed),
        .disconnect_events = (uint32_t)atomic_load_explicit(
            &s_disconnect_events, memory_order_relaxed),
        .disconnect_accepted = (uint32_t)atomic_load_explicit(
            &s_disconnect_accepted, memory_order_relaxed),
        .mount_attempts = (uint32_t)atomic_load_explicit(
            &s_mount_attempts, memory_order_relaxed),
        .mount_successes = (uint32_t)atomic_load_explicit(
            &s_mount_successes, memory_order_relaxed),
        .last_mount_result = (esp_err_t)atomic_load_explicit(
            &s_last_mount_result, memory_order_relaxed),
        .releases = (uint32_t)atomic_load_explicit(
            &s_releases, memory_order_relaxed),
        .last_unmount_result = (esp_err_t)atomic_load_explicit(
            &s_last_unmount_result, memory_order_relaxed),
        .last_uninstall_result = (esp_err_t)atomic_load_explicit(
            &s_last_uninstall_result, memory_order_relaxed),
    };
}

/* MSC callback runs in the driver task. It never blocks on mount I/O and never
 * relies on a finite queue whose disconnect edge could be dropped. */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (!event) {
        return;
    }
    if (event->event == MSC_DEVICE_CONNECTED) {
        publish_desired_connect(event->device.address);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        publish_desired_disconnect(event->device.handle);
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    const usb_host_config_t host_cfg = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .root_port_unpowered = true,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority = MSC_TASK_PRIO,
        .stack_size = MSC_TASK_STACK,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));

    root_port_power_cycle("initial bring-up");
    ESP_LOGI(TAG, "USB host + MSC installed; waiting for drives on the HS USB port");

    bool any_connected = devices_connected() > 0;
    usb_storage_recovery_t recovery;
    usb_storage_recovery_init(&recovery, any_connected, 0u,
                              (uint32_t)xTaskGetTickCount(), 1u);

    for (;;) {
        uint32_t flags = 0u;
        esp_err_t rc = usb_host_lib_handle_events(pdMS_TO_TICKS(RECONCILE_POLL_MS),
                                                  &flags);
        if (rc == ESP_OK && (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) {
            usb_host_device_free_all();
        }

        /* Recovery only runs while the bus is truly empty. "Empty" means no
         * enumerated device of any kind, not merely no mounted MSC session: an
         * external hub enumerates first and only then resets its downstream
         * ports, so a stick behind a hub can take several seconds to appear as
         * an MSC session. usb_host_lib_info().num_devices counts every
         * enumerated device (the hub included), so once anything is on the bus
         * we stop power-cycling the root port — otherwise the 900 ms recovery
         * cycle keeps resetting the hub mid-enumeration
         * (EXT_PORT: PortN disabled / CHECK_SHORT_DEV_DESC FAILED). */
        usb_host_lib_info_t lib_info = {0};
        bool bus_populated = (usb_host_lib_info(&lib_info) == ESP_OK &&
                              lib_info.num_devices > 0);
        any_connected = devices_connected() > 0 || bus_populated;
        uint32_t now = (uint32_t)xTaskGetTickCount();
        /* One root port: recovery only runs while nothing is attached. Feed it a
         * synthetic epoch that advances whenever the attach state flips. */
        static uint32_t s_recovery_epoch;
        static bool s_recovery_prev;
        if (any_connected != s_recovery_prev) {
            s_recovery_prev = any_connected;
            s_recovery_epoch++;
        }
        usb_storage_recovery_observe(&recovery, any_connected, s_recovery_epoch, now);
        if (!usb_storage_recovery_cycle_due(
                &recovery, now,
                (uint32_t)pdMS_TO_TICKS(ROOT_PORT_RETRY_MS),
                ROOT_PORT_MAX_CYCLES,
                (uint32_t)pdMS_TO_TICKS(ROOT_PORT_SLOW_MS))) {
            continue;
        }

        bool was_slow = usb_storage_recovery_uses_slow_cadence(
            &recovery, ROOT_PORT_MAX_CYCLES);
        usb_storage_recovery_mark_cycle(&recovery, now);
        bool now_slow = usb_storage_recovery_uses_slow_cadence(
            &recovery, ROOT_PORT_MAX_CYCLES);
        if (!was_slow && now_slow) {
            ESP_LOGW(TAG,
                     "USB enumeration recovery exhausted %u fast cycles; "
                     "continuing every %u ms",
                     (unsigned)ROOT_PORT_MAX_CYCLES, (unsigned)ROOT_PORT_SLOW_MS);
        }
        root_port_power_cycle("no active storage session");
    }
}

static bool wait_for_stable_connection(unsigned slot, uint32_t epoch,
                                       uint8_t dev_addr)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONNECT_STABLE_MS);
    for (;;) {
        if (!desired_matches(slot, epoch, dev_addr)) {
            return false;
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return true;
        }
        (void)ulTaskNotifyTake(pdTRUE, deadline - now);
    }
}

static void log_device_info(unsigned slot, const char *prefix)
{
    msc_host_device_info_t info = {0};
    if (!s_devs[slot].handle ||
        msc_host_get_device_info(s_devs[slot].handle, &info) != ESP_OK) {
        return;
    }
    uint64_t mb = ((uint64_t)info.sector_size * info.sector_count) /
                  (1024u * 1024u);
    ESP_LOGI(TAG, "%s (%s): %llu MB, sector=%u bytes (VID:0x%04X PID:0x%04X)",
             prefix, s_devs[slot].base_path,
             (unsigned long long)mb, (unsigned)info.sector_size,
             info.idVendor, info.idProduct);
}

static void log_mount_layout(unsigned slot)
{
    usb_media_mount_info_t mount_info;
    if (s_devs[slot].mount &&
        usb_media_mount_get_info(s_devs[slot].mount, &mount_info)) {
        ESP_LOGI(TAG,
                 "USB media mounted at %s: base_lba=%u sectors=%u sector_size=%u exfat=%u gpt=%u",
                 s_devs[slot].base_path,
                 (unsigned)mount_info.base_lba, (unsigned)mount_info.sector_count,
                 (unsigned)mount_info.sector_size,
                 mount_info.exfat ? 1u : 0u, mount_info.gpt ? 1u : 0u);
    }

    DIR *dir = opendir(s_devs[slot].base_path);
    if (!dir) {
        ESP_LOGW(TAG, "opendir(%s) failed: %s",
                 s_devs[slot].base_path, strerror(errno));
        return;
    }
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < 24) {
        ESP_LOGI(TAG, "  %s/%s", s_devs[slot].base_path, entry->d_name);
        count++;
    }
    closedir(dir);
}

static esp_err_t mount_desired_device(unsigned slot, uint32_t epoch,
                                      uint8_t dev_addr)
{
    atomic_fetch_add_explicit(&s_mount_attempts, 1u, memory_order_relaxed);
    if (!desired_matches(slot, epoch, dev_addr)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!USB_STORAGE_DEVICE_ROUTE_ALLOWED(dev_addr)) {
        atomic_store_explicit(&s_last_mount_result, ESP_ERR_NOT_SUPPORTED,
                              memory_order_relaxed);
        ESP_LOGW(TAG, "rejecting MSC addr=%u outside the USB0 direct root",
                 (unsigned)dev_addr);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* A previous transient attempt may have left a partially opened handle. */
    if (s_devs[slot].mount || s_devs[slot].handle) {
        release_device(slot);
    }

    ESP_LOGI(TAG, "drive connected (addr=%u), mounting at %s",
             (unsigned)dev_addr, s_devs[slot].base_path);
    esp_err_t rc = msc_host_install_device(dev_addr, &s_devs[slot].handle);
    if (rc != ESP_OK) {
        atomic_store_explicit(&s_last_mount_result, rc, memory_order_relaxed);
        ESP_LOGW(TAG, "msc_host_install_device: %s", esp_err_to_name(rc));
        s_devs[slot].handle = NULL;
        return rc;
    }
    if (!desired_matches(slot, epoch, dev_addr)) {
        release_device(slot);
        return ESP_ERR_INVALID_STATE;
    }

    bool handle_bound;
    portENTER_CRITICAL(&s_state_mux);
    handle_bound = usb_storage_session_bind_handle(
        &s_devs[slot].session, epoch, dev_addr, (uintptr_t)s_devs[slot].handle);
    portEXIT_CRITICAL(&s_state_mux);
    if (!handle_bound) {
        release_device(slot);
        return ESP_ERR_INVALID_STATE;
    }

    log_device_info(slot, "USB MSC device");

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 8192,
    };
    rc = usb_media_mount(s_devs[slot].handle, s_devs[slot].base_path,
                         &mount_cfg, &s_devs[slot].mount);
    if (rc != ESP_OK) {
        atomic_store_explicit(&s_last_mount_result, rc, memory_order_relaxed);
        ESP_LOGW(TAG, "usb_media_mount(%s): %s",
                 s_devs[slot].base_path, esp_err_to_name(rc));
        ESP_LOGW(TAG,
                 "mount retry scheduled; supported media is FAT32/exFAT on superfloppy, MBR, or GPT");
        release_device(slot);
        return rc;
    }

    if (!desired_matches(slot, epoch, dev_addr)) {
        release_device(slot);
        return ESP_ERR_INVALID_STATE;
    }

    log_mount_layout(slot);
    log_device_info(slot, "mounted");

    if (!commit_mounted(slot, epoch, dev_addr)) {
        atomic_store_explicit(&s_last_mount_result, ESP_ERR_INVALID_STATE,
                              memory_order_relaxed);
        release_device(slot);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_fetch_add_explicit(&s_mount_successes, 1u, memory_order_relaxed);
    atomic_store_explicit(&s_last_mount_result, ESP_OK, memory_order_relaxed);
    return ESP_OK;
}

static void reconcile_slot(unsigned slot)
{
    usb_storage_session_t desired = desired_snapshot(slot);

    if (!desired.connected) {
        bool had_device = s_devs[slot].announced || s_devs[slot].mount ||
                          s_devs[slot].handle;
        if (had_device) {
            ESP_LOGW(TAG, "drive on %s disconnected; reconciling",
                     s_devs[slot].base_path);
        }
        publish_unmounted(slot);
        if (s_devs[slot].mount || s_devs[slot].handle) {
            release_device(slot);
        }
        s_devs[slot].retry_ms = MOUNT_RETRY_INITIAL_MS;
        s_devs[slot].next_attempt = 0;
        return;
    }

    if (desired.mounted) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_devs[slot].next_attempt != 0 &&
        (int32_t)(s_devs[slot].next_attempt - now) > 0) {
        return;
    }

    desired = desired_snapshot(slot);
    if (!desired.connected ||
        !wait_for_stable_connection(slot, desired.epoch, desired.dev_addr)) {
        return;
    }

    esp_err_t rc = mount_desired_device(slot, desired.epoch, desired.dev_addr);
    if (rc == ESP_OK) {
        s_devs[slot].retry_ms = MOUNT_RETRY_INITIAL_MS;
        s_devs[slot].next_attempt = 0;
        return;
    }
    if (!desired_matches(slot, desired.epoch, desired.dev_addr)) {
        s_devs[slot].retry_ms = MOUNT_RETRY_INITIAL_MS;
        s_devs[slot].next_attempt = 0;
        return;
    }

    ESP_LOGW(TAG, "USB mount attempt on %s failed (%s); retrying in %u ms",
             s_devs[slot].base_path, esp_err_to_name(rc),
             (unsigned)s_devs[slot].retry_ms);
    s_devs[slot].next_attempt =
        xTaskGetTickCount() + pdMS_TO_TICKS(s_devs[slot].retry_ms);
    if (s_devs[slot].retry_ms < MOUNT_RETRY_MAX_MS) {
        uint32_t doubled = s_devs[slot].retry_ms * 2u;
        s_devs[slot].retry_ms =
            doubled > MOUNT_RETRY_MAX_MS ? MOUNT_RETRY_MAX_MS : doubled;
    }
}

static void storage_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (unsigned slot = 0; slot < USB_STORAGE_MAX_DEVICES; ++slot) {
            reconcile_slot(slot);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RECONCILE_POLL_MS));
    }
}

esp_err_t usb_storage_init(usb_storage_event_cb_t cb)
{
    if (s_storage_task || s_usb_lib_task) {
        return ESP_ERR_INVALID_STATE;
    }

    media_io_gate_set_available(false);
    s_cb = cb;
    portENTER_CRITICAL(&s_state_mux);
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVICES; ++i) {
        usb_storage_session_reset(&s_devs[i].session);
        s_devs[i].handle = NULL;
        s_devs[i].mount = NULL;
        s_devs[i].announced = false;
        s_devs[i].retry_ms = MOUNT_RETRY_INITIAL_MS;
        s_devs[i].next_attempt = 0;
    }
    portEXIT_CRITICAL(&s_state_mux);

    /* Slot 0 -> "/usb"; slot i -> "/usb<i+1>". */
    strncpy(s_devs[0].base_path, USB_STORAGE_MOUNT_POINT,
            sizeof(s_devs[0].base_path) - 1u);
    for (unsigned i = 1; i < USB_STORAGE_MAX_DEVICES; ++i) {
        snprintf(s_devs[i].base_path, sizeof(s_devs[i].base_path),
                 "%s%u", USB_STORAGE_MOUNT_POINT, i + 1u);
    }

    if (xTaskCreate(storage_task, "usb_store", STORAGE_TASK_STACK,
                    NULL, STORAGE_TASK_PRIO, &s_storage_task) != pdPASS) {
        s_storage_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(usb_lib_task, "usb_lib", USB_LIB_TASK_STACK,
                    NULL, USB_LIB_TASK_PRIO, &s_usb_lib_task) != pdPASS) {
        vTaskDelete(s_storage_task);
        s_storage_task = NULL;
        s_usb_lib_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
