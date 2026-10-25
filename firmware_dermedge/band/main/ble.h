// The band's Bluetooth LE link to the DermEdge website (Web Bluetooth, Chrome/Edge), on the
// ESP32's own radio through NimBLE.
//
// Security: LE Secure Connections with a passkey the band shows on its screen and the user types
// on the laptop, then bonded. Every characteristic needs that encrypted, authenticated link, so a
// stranger in a school or hospital can't read the band or send it commands.
//
// Service a3c87510-8ed3-4bdf-8a39-a01bebede295, all text:
//   ...7511 status   read/notify, ~1 Hz: {"id","s","r","src","el","n","act","q","alert"}
//   ...7512 data     notify: one reading per message, already in the hub's /readings shape,
//                    e.g. {"r":3,"motion":[{"t":12,"activity":0.1,"scratch_energy":0.02}]}
//   ...7513 command  write: start[:label] | stop | dismiss | alert:<level>|<reason>|<tip> |
//                    token:<device token> | unpair
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { BLE_EV_LINK_UP, BLE_EV_LINK_DOWN, BLE_EV_PASSKEY, BLE_EV_COMMAND } ble_event_t;

// Called from the Bluetooth task: copy what you need and hand it to your own task
typedef void (*ble_cb_t)(ble_event_t ev, uint32_t arg, const char *text);

void ble_start(ble_cb_t cb);
bool ble_secure(void);      // paired, encrypted link up
bool ble_data_ready(void);  // ...and the website listens for readings
bool ble_notify_status(const char *json);
bool ble_notify_data(const char *json);  // false when the radio's buffers are full: retry later
const char *ble_device_id(void);         // "DE-" + the Bluetooth address, for pairing on the hub
void ble_forget_bonds(void);
