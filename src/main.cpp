#include <Arduino.h>
#include <Wire.h>
#include <bluefruit.h>
#include <SensirionI2cScd4x.h>
#include <nrf_wdt.h>
#include "config.h"

// The wait states all end one cycle and start the next; they differ in what
// they do while waiting. ST_IDLE measures at the normal cadence, ST_ALERT is
// ST_IDLE with the LED burst repeating, ST_SAVE_BAT stretches the cadence,
// ST_HIBERNATE stops measuring. See idleState() for which one wins.
typedef enum {
  ST_WARMUP, ST_MEASURE, ST_TRANSMIT, ST_ALERT,
  ST_IDLE, ST_SAVE_BAT, ST_HIBERNATE
} State;

typedef struct {
  uint16_t co2;
  int16_t  temp_c100;
  uint16_t rh_c100;
  uint16_t vbat_mv;
  uint8_t  flags;
} Sample;

static SensirionI2cScd4x scd;

static State   state    = ST_WARMUP;
static Sample  sample   = {0};
static uint8_t failures = 0;
static uint8_t packetId = 0;

// Keeps counting through the tickless sleeps, pauses under a debugger. Once
// started it cannot be stopped, so every path in loop() must reach wdtFeed().
static void wdtBegin(void) {
  nrf_wdt_behaviour_set(NRF_WDT, NRF_WDT_BEHAVIOUR_RUN_SLEEP);
  nrf_wdt_reload_value_set(NRF_WDT, WDT_TIMEOUT_S * 32768 - 1);  // 32.768 kHz ticks --> LFCLK
  nrf_wdt_reload_request_enable(NRF_WDT, NRF_WDT_RR0);
  nrf_wdt_task_trigger(NRF_WDT, NRF_WDT_TASK_START);
}

static inline void wdtFeed(void) { nrf_wdt_reload_request_set(NRF_WDT, NRF_WDT_RR0); }
  
// Every wait in this firmware goes through here. delay() on this core is a
// tickless FreeRTOS sleep, so the MCU idles in System ON; the wait is chunked
// so the watchdog gets fed along the way. A bare delay() longer than
// WDT_TIMEOUT_S anywhere else would reset the board.
static void sleepMs(uint32_t ms) {
  while (ms) {
    uint32_t chunk = ms < SLEEP_CHUNK_MS ? ms : SLEEP_CHUNK_MS;
    delay(chunk);
    wdtFeed();
    ms -= chunk;
  }
}

static void blink(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(PIN_ALERT_LED, HIGH);
    delay(ALERT_BLINK_MS);
    digitalWrite(PIN_ALERT_LED, LOW);
    delay(ALERT_BLINK_MS);
  }
}

// Alert wait: the same wait ST_IDLE does, with the LED repeating its burst
// for the whole interval. One burst per measurement was easy to miss -- it
// only caught whoever happened to be looking when the reading landed.
static void alertWait(uint32_t ms) {
  const uint32_t burst = (uint32_t)ALERT_BLINKS * 2 * ALERT_BLINK_MS;
  while (ms) {
    uint32_t period = ms < ALERT_PERIOD_MS ? ms : ALERT_PERIOD_MS;
    if (period >= burst) {
      blink(ALERT_BLINKS);
      wdtFeed();
      sleepMs(period - burst);
    } else {
      sleepMs(period);   // tail too short for a burst: just wait it out
    }
    ms -= period;
  }
}

static uint16_t readVbatMv(void) {
  digitalWrite(VBAT_ENABLE, LOW);   // connects the divider
  delay(10);
  uint32_t raw = 0;
  for (uint8_t i = 0; i < VBAT_SAMPLES; i++) raw += analogRead(PIN_VBAT);
  digitalWrite(VBAT_ENABLE, HIGH);
  return (uint16_t)((raw / VBAT_SAMPLES) * ADC_MV_PER_CODE * VBAT_DIVIDER);
}

// Decisions read the last stored sample, never the sensor. Each latches with
// hysteresis -- it turns on at one threshold and off at another -- so a value
// sitting on a threshold cannot chatter between states. Calling one twice with
// the same sample returns the same answer, so they are safe to use anywhere.

// CO2 high enough to warrant the LED alert.
static bool co2IsDangerous(void) {
  static bool danger = false;
  if (sample.co2 >= CO2_ALERT_ON_PPM)       danger = true;
  else if (sample.co2 <= CO2_ALERT_OFF_PPM) danger = false;
  return danger;
}

// Battery low enough to stretch the measurement interval.
static bool powerSaveRequired(void) {
  static bool save = false;
  if (sample.vbat_mv <= VBAT_SAVER_ON_MV)       save = true;
  else if (sample.vbat_mv >= VBAT_SAVER_OFF_MV) save = false;
  return save;
}

// Battery too low to measure at all: enter ST_HIBERNATE, leave once it recovers.
static bool hibernateRequired(void) {
  static bool low = false;
  if (sample.vbat_mv <= VBAT_SHUTDOWN_MV)     low = true;
  else if (sample.vbat_mv >= VBAT_RECOVER_MV) low = false;
  return low;
}

// Which wait state follows a finished cycle. The alert outranks ST_SAVE_BAT:
// air this bad is a danger to the person in the room, so stretching the
// cadence to save a battery that still has charge in it can wait. It does not
// outrank ST_HIBERNATE -- a cell that flat cannot warn anyone for long, and a
// device that browns out mid-alert warns nobody at all, so it rides out the
// dark and comes back to alert once the harvester has caught up.
static State idleState(void) {
  if (hibernateRequired())  return ST_HIBERNATE;
  if (co2IsDangerous())     return ST_ALERT;
  if (powerSaveRequired())  return ST_SAVE_BAT;
  return ST_IDLE;
}

static void sensorBegin(void) {
  Wire.begin();
  scd.begin(Wire, SCD41_I2C_ADDR_62);
  scd.wakeUp();
  scd.stopPeriodicMeasurement();
  scd.reinit();
  uint64_t sn = 0;
  int16_t err = scd.getSerialNumber(sn);
  Serial.printf("scd41 serial=%08lX%08lX err=%d\n", (uint32_t)(sn >> 32), (uint32_t)sn, err);
  uint16_t asc = 0;
  // persistSettings() wears the sensor EEPROM, so only write when it differs.
  if (scd.getAutomaticSelfCalibrationEnabled(asc) == 0 && asc != 1) {
    scd.setAutomaticSelfCalibrationEnabled(1);
    scd.persistSettings();
  }
  scd.powerDown();
}

static void bleBegin(void) {
  Bluefruit.begin();
  Bluefruit.setTxPower(0);
  Bluefruit.setName(BLE_NAME);
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(244, 244);
  Bluefruit.ScanResponse.addName();
}

// Broadcast one BTHome frame, then go quiet again.
static void advertiseData(const uint8_t *d, uint8_t len) {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, d, len);
  Bluefruit.Advertising.start(0);
  sleepMs(ADV_WINDOW_MS);
  Bluefruit.Advertising.stop();
}

// BTHome v2 service data. The spec requires object ids in ascending order.
// Values are little-endian, which is the native layout, so the fields copy
// straight out of Sample.
static void advertise(void) {
  uint8_t d[21], i = 0;
  d[i++] = 0xD2; d[i++] = 0xFC;                                     // BTHome UUID
  d[i++] = BTHOME_DEVINFO;
  d[i++] = 0x00; d[i++] = ++packetId;                               // packet id
  d[i++] = 0x02; memcpy(&d[i], &sample.temp_c100, 2); i += 2;       // temperature
  d[i++] = 0x03; memcpy(&d[i], &sample.rh_c100, 2);   i += 2;       // humidity
  d[i++] = 0x0C; memcpy(&d[i], &sample.vbat_mv, 2);   i += 2;       // voltage
  d[i++] = 0x12; memcpy(&d[i], &sample.co2, 2);       i += 2;       // co2
  d[i++] = 0x15; d[i++] = powerSaveRequired() ? 1 : 0;              // battery low
  d[i++] = 0x26; d[i++] = (sample.flags & FLAG_SENSOR_ERR) ? 1 : 0; // problem
  advertiseData(d, i);
}

// Sent while the cell is too flat to measure. The measurement objects are left
// out on purpose: a consumer ages them out instead of showing the last reading
// as if it were fresh, while the device itself stays visible and explains why.
static void advertiseBattery(void) {
  uint8_t d[10], i = 0;
  d[i++] = 0xD2; d[i++] = 0xFC;
  d[i++] = BTHOME_DEVINFO;
  d[i++] = 0x00; d[i++] = ++packetId;                             // packet id
  d[i++] = 0x0C; memcpy(&d[i], &sample.vbat_mv, 2); i += 2;       // voltage
  d[i++] = 0x15; d[i++] = 1;                                      // battery low
  advertiseData(d, i);
}

void setup(void) {
  pinMode(PIN_ALERT_LED, OUTPUT);
  digitalWrite(PIN_ALERT_LED, LOW);
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, HIGH);
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < SERIAL_WAIT_MS) delay(10);

  sensorBegin();
  bleBegin();
  wdtBegin();
}

void loop(void) {
  switch (state) {
    case ST_WARMUP:
      // First single shot after wake is unreliable and is thrown away.
      scd.wakeUp();
      scd.measureSingleShot();
      state = ST_MEASURE;
      break;

    case ST_MEASURE: {
      uint16_t co2 = 0;
      float t = 0, rh = 0;
      int16_t err = scd.measureAndReadSingleShot(co2, t, rh);
      scd.powerDown();
      bool bad = (err != 0 || co2 == 0);

      if (bad && ++failures >= SENSOR_RETRY_MAX) {
        Serial.printf("sensor err=%d, reinit after %u failures\n", err, failures);
        failures = 0;
        sensorBegin();
      } else if (!bad) {
        failures         = 0;
        sample.co2       = co2;
        sample.temp_c100 = (int16_t)lroundf(t * 100.0f);
        sample.rh_c100   = (uint16_t)lroundf(rh * 100.0f);
      }
      sample.vbat_mv = readVbatMv();

      sample.flags = (co2IsDangerous()    ? FLAG_ALERT : 0) |
                     (powerSaveRequired() ? FLAG_SAVER : 0) |
                     (bad ? FLAG_SENSOR_ERR : 0);
      Serial.printf("co2=%u ppm  t=%.2f C  rh=%.2f %%  vbat=%u mV  flags=0x%02X\n",
                    sample.co2, sample.temp_c100 / 100.0f, sample.rh_c100 / 100.0f,
                    sample.vbat_mv, sample.flags);
      state = ST_TRANSMIT;
      break;
    }

    case ST_TRANSMIT:
      advertise();
      state = idleState();
      break;

    case ST_ALERT:
      alertWait(INTERVAL_NORMAL_MS);
      // A whole interval of bursts is the most expensive thing this firmware
      // does, so the cell can cross the shutdown threshold inside one alert
      // wait. Re-read it here: reaching ST_HIBERNATE through ST_MEASURE would
      // spend a sensor cycle on a reading we already know we cannot act on.
      sample.vbat_mv = readVbatMv();
      state = hibernateRequired() ? ST_HIBERNATE : ST_WARMUP;
      break;

    case ST_IDLE:
      sleepMs(INTERVAL_NORMAL_MS);
      state = ST_WARMUP;
      break;

    case ST_SAVE_BAT:
      sleepMs(INTERVAL_SAVER_MS);
      state = ST_WARMUP;
      break;

    case ST_HIBERNATE:
      // Cell is near empty: wait for the harvester without measuring, and keep
      // reporting the voltage so the device does not just vanish. Nothing to
      // shut down here -- the sensor was slept in ST_MEASURE, advertising was
      // stopped at the end of advertise(), and blink() leaves the LED off.
      // This is also where an alert goes to wait out a flat cell: the latch in
      // co2IsDangerous() holds, so the first cycle after the voltage recovers
      // goes straight back to ST_ALERT without needing a fresh bad reading.
      sleepMs(INTERVAL_SAVER_MS);
      sample.vbat_mv = readVbatMv();
      advertiseBattery();
      if (!hibernateRequired()) state = ST_WARMUP;
      break;
  }
  wdtFeed();
}
