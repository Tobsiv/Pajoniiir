#pragma once

// USB Host mass-storage support for the Rekordbox music drive(s).
//
// Brings up the ESP32-P4 USB host stack + MSC class driver and mounts up to
// USB_STORAGE_MAX_DEVICES flash drives at once (e.g. behind a hub). Slot 0
// mounts at "/usb", slot i at "/usb<i+1>". When a drive appears or goes away
// the registered callback fires with the slot identity so the app can
// (re)build the merged library and refresh the UI.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define USB_STORAGE_MOUNT_POINT   "/usb"
#define USB_STORAGE_MAX_DEVICES   3u
#define USB_STORAGE_PATH_MAX      12u

typedef struct {
    uint8_t index;                        // slot 0..USB_STORAGE_MAX_DEVICES-1
    char base_path[USB_STORAGE_PATH_MAX]; // "/usb", "/usb2", ...
    bool mounted;                         // true = just mounted, false = removed
} usb_storage_mount_event_t;

// Fired from the USB storage task when a drive mounts / unmounts. Runs in its
// own task context — callers that touch LVGL must take the UI lock themselves.
typedef void (*usb_storage_event_cb_t)(const usb_storage_mount_event_t *event);

// Start the USB host + MSC stack. Non-blocking: enumeration/mount happen later
// on a background task. `cb` may be NULL.
esp_err_t usb_storage_init(usb_storage_event_cb_t cb);

// Whether at least one drive is currently mounted.
bool usb_storage_is_mounted(void);

// Number of drives currently mounted.
unsigned usb_storage_mounted_count(void);

// Copy slot `index`'s mount path into `out`; false if that slot is not mounted.
bool usb_storage_mount_path(unsigned index, char *out, size_t len);

typedef struct {
    bool desired_connected;   // any slot has a connected device
    bool mounted;             // any slot mounted
    uint8_t devices_connected;
    uint8_t devices_mounted;
    uint32_t connect_events;
    uint32_t connect_accepted;
    uint32_t disconnect_events;
    uint32_t disconnect_accepted;
    uint32_t mount_attempts;
    uint32_t mount_successes;
    esp_err_t last_mount_result;
    uint32_t releases;
    esp_err_t last_unmount_result;
    esp_err_t last_uninstall_result;
} usb_storage_diagnostics_t;

// Lock-free/read-only runtime evidence for hot-plug diagnosis.
void usb_storage_get_diagnostics(usb_storage_diagnostics_t *out);
