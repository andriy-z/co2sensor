#pragma once

#define PIN_ALERT_LED        D7

// Measurement cadence. Lower INTERVAL_NORMAL_MS while bench-testing, e.g.
// (1UL * 60000UL) for one reading a minute.
#define INTERVAL_NORMAL_MS   (5UL * 60000UL)
#define INTERVAL_SAVER_MS    (30UL * 60000UL)

#define CO2_ALERT_ON_PPM     1200
#define CO2_ALERT_OFF_PPM    1000

#define VBAT_SAVER_ON_MV     3600
#define VBAT_SAVER_OFF_MV    3750
#define VBAT_SHUTDOWN_MV     3200
#define VBAT_RECOVER_MV      3500

// One ADC code in mV: full scale / codes. Full scale is AR_INTERNAL_3_0
// (0.6 V band-gap x gain 1/5), codes are 2^12 from analogReadResolution(12).
#define ADC_MV_PER_CODE      (3000.0f / 4096.0f)
// XIAO nRF52840 on-board divider: (1M + 510k) / 510k, undone to get V(bat).
#define VBAT_DIVIDER         2.9608f
#define VBAT_SAMPLES         8

#define ALERT_BLINKS         3
#define ALERT_BLINK_MS       120
// How often the burst repeats while the alert wait runs.
#define ALERT_PERIOD_MS      5000UL
#define SENSOR_RETRY_MAX     3
#define WDT_TIMEOUT_S        30
#define SLEEP_CHUNK_MS       10000UL
#define SERIAL_WAIT_MS       3000UL

#define BLE_NAME             "co2m"
#define ADV_WINDOW_MS        2000UL
// BTHome v2, unencrypted, regular interval: version 2 in bits 5-7.
#define BTHOME_DEVINFO       0x40

#define FLAG_ALERT           0x01
#define FLAG_SAVER           0x02
#define FLAG_SENSOR_ERR      0x04
