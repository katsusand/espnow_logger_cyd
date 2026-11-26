#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "error_log_store.h"
#include "espnow_log_format.h"
#include "espnow_logger.h"
#include "espnow_sensor_packet.h"
#include "nvs_health.h"
#include "nvs_schema.h"
#include "sd_card_status.h"
#include "sd_card_storage.h"
#include "sd_card_writer.h"

#define TAG "espnow_logger"

/* APP_WIFI_STA_ENABLED is defined by the top-level CMakeLists.txt. */
#ifndef APP_WIFI_STA_ENABLED
#define APP_WIFI_STA_ENABLED 1
#endif

/* This component owns this data, so the namespace is declared here.
   The "ftr_" prefix is what lets nvs_schema classify it by scanning flash:
   the radio settings survive swapping the foreground app. */
NVS_SCHEMA_DECLARE_NS(NVS_NS, "ftr_espnow");
static const nvs_key_descriptor_t NVS_KEY_CHANNEL = {.ns = NVS_NS, .key = "channel"};
static const nvs_key_descriptor_t NVS_KEY_PEER_MAC = {.ns = NVS_NS, .key = "peer_mac"};
static const nvs_key_descriptor_t NVS_KEY_AUTO_LOG = {.ns = NVS_NS, .key = "auto_log"};

#define MIN_VALID_EPOCH_SEC 1704067200LL
#define STREAM_CLOSE_WAIT_TICKS 200
#define SENSOR_BUFFER_BYTES ((size_t)CONFIG_ESPNOW_LOGGER_SENSOR_BUFFER_KB * 1024U)
#define CSV_BUFFER_BYTES ((size_t)CONFIG_ESPNOW_LOGGER_CSV_BUFFER_KB * 1024U)

typedef struct {
    bool started;
    bool peer_configured;
    bool utc_anchor_valid;
    uint8_t local_mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    uint8_t peer_mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    uint64_t utc_anchor_gps_ms;
    uint64_t utc_anchor_ts_ms;
    /* The last UTC time packet's sender, and when the same sender last sent IMU frames (0 = never). */
    bool time_source_valid;
    uint8_t time_source_mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    uint64_t time_source_ts_ms;
    uint64_t time_source_imu_ts_ms;
    TaskHandle_t periodic_task;
    uint32_t event_sequence;
    size_t event_count;
    size_t event_head;
    espnow_logger_event_t events[CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE];
    portMUX_TYPE lock; /* events, UTC anchor, peer, and the stream gate below */
} espnow_logger_context_t;

/*
 * SD log state. `streams_open` and `writers` form a gate: the Wi-Fi callbacks
 * and senders write only while they hold it, and the control task closes the
 * streams only after the gate is shut and the last writer has left. Both are
 * protected by s_logger.lock.
 */
typedef struct {
    TaskHandle_t task;
    bool requested;
    bool auto_start;
    bool streams_open;
    bool closing;
    bool failed;
    uint32_t writers;
    uint32_t opened_revision;
    uint32_t retry_after_ms;
    esp_err_t last_error;
    uint32_t sensor_seq;
    uint32_t frames_not_logged;
    sd_card_writer_stream_t *sensor_stream;
    sd_card_writer_stream_t *csv_stream;
    sd_card_writer_stats_t sensor_final;
    sd_card_writer_stats_t csv_final;
    /* File numbering; touched only on the writer task (the path functions) and
       reset by the control task while no stream exists. */
    unsigned session_index;
    bool first_file_used[ESPNOW_LOG_FILE_COUNT];
} espnow_logger_log_t;

static espnow_logger_context_t s_logger = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static espnow_logger_log_t s_log;

static const uint8_t s_broadcast_mac[ESPNOW_LOGGER_MAC_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

#define RETURN_ON_ERROR_LOG(expr, msg)                \
    do {                                              \
        esp_err_t _err = (expr);                      \
        if (_err != ESP_OK) {                         \
            ERROR_LOG(_err, "%s", msg);               \
            return _err;                              \
        }                                             \
    } while (0)

static uint64_t timestamp_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static bool is_broadcast_mac(const uint8_t *mac)
{
    return mac != NULL && memcmp(mac, s_broadcast_mac, ESPNOW_LOGGER_MAC_ADDR_LEN) == 0;
}

/* ---------------------------------------------------------------- NVS */

static esp_err_t nvs_read_u8(const nvs_key_descriptor_t *key, uint8_t *value)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_descriptor(key->ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open nvs failed");
    err = nvs_get_u8(handle, key->key, value);
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t nvs_write_u8(const nvs_key_descriptor_t *key, uint8_t value)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open_descriptor(key->ns, NVS_READWRITE, &handle), TAG, "open nvs failed");
    esp_err_t err = nvs_set_u8(handle, key->key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_read_mac(uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN])
{
    nvs_handle_t handle = 0;
    size_t size = ESPNOW_LOGGER_MAC_ADDR_LEN;
    esp_err_t err = nvs_open_descriptor(NVS_KEY_PEER_MAC.ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open nvs failed");
    err = nvs_get_blob(handle, NVS_KEY_PEER_MAC.key, mac, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK && size != ESPNOW_LOGGER_MAC_ADDR_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

/* mac == NULL erases the key. */
static esp_err_t nvs_write_mac(const uint8_t *mac)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open_descriptor(NVS_KEY_PEER_MAC.ns, NVS_READWRITE, &handle), TAG, "open nvs failed");
    esp_err_t err = ESP_OK;
    if (mac != NULL) {
        err = nvs_set_blob(handle, NVS_KEY_PEER_MAC.key, mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
    } else {
        err = nvs_erase_key(handle, NVS_KEY_PEER_MAC.key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* Saved channel if it is a real one, else the Kconfig default. */
static uint8_t load_channel(void)
{
    uint8_t channel = CONFIG_ESPNOW_LOGGER_WIFI_CHANNEL;
    uint8_t saved = 0;
    esp_err_t err = nvs_read_u8(&NVS_KEY_CHANNEL, &saved);

    if (err == ESP_OK) {
        if (saved >= ESPNOW_LOGGER_CHANNEL_MIN && saved <= ESPNOW_LOGGER_CHANNEL_MAX) {
            channel = saved;
        } else {
            nvs_health_report_invalid(&NVS_KEY_CHANNEL, ESP_ERR_INVALID_ARG, "invalid espnow channel");
        }
    } else if (err != ESP_ERR_NOT_FOUND) {
        nvs_health_report_invalid(&NVS_KEY_CHANNEL, err, "read espnow channel failed");
    }
    return channel;
}

/* Saved peer if there is one, else the Kconfig default (may be empty). */
static void load_peer(void)
{
    uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    bool configured = false;
    esp_err_t err = nvs_read_mac(mac);

    if (err == ESP_OK) {
        configured = true;
    } else if (err == ESP_ERR_NOT_FOUND) {
        configured = espnow_log_parse_mac(CONFIG_ESPNOW_LOGGER_UNICAST_PEER_MAC, mac) != 0;
        if (!configured && CONFIG_ESPNOW_LOGGER_UNICAST_PEER_MAC[0] != '\0') {
            ERROR_LOG_MSG("CONFIG_ESPNOW_LOGGER_UNICAST_PEER_MAC is not a MAC address");
        }
    } else {
        nvs_health_report_invalid(&NVS_KEY_PEER_MAC, err, "invalid espnow peer mac");
    }

    portENTER_CRITICAL(&s_logger.lock);
    s_logger.peer_configured = configured;
    if (configured) {
        memcpy(s_logger.peer_mac, mac, sizeof(mac));
    }
    portEXIT_CRITICAL(&s_logger.lock);
}

static void load_auto_start(void)
{
    uint8_t value = 0;
    esp_err_t err = nvs_read_u8(&NVS_KEY_AUTO_LOG, &value);

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        nvs_health_report_invalid(&NVS_KEY_AUTO_LOG, err, "read espnow auto log failed");
    }
    s_log.auto_start = (err == ESP_OK && value != 0U);
}

/* ---------------------------------------------------------------- UTC */

static void seed_utc_anchor_from_system_time(void)
{
    time_t now = time(NULL);

    if (now < (time_t)MIN_VALID_EPOCH_SEC) {
        return;
    }
    portENTER_CRITICAL(&s_logger.lock);
    s_logger.utc_anchor_valid = true;
    s_logger.utc_anchor_gps_ms = (uint64_t)now * 1000ULL;
    s_logger.utc_anchor_ts_ms = timestamp_ms();
    portEXIT_CRITICAL(&s_logger.lock);
}

/* Sets the system clock from time a sender reported, and keeps it as the anchor for CSV stamps. */
static esp_err_t apply_utc_epoch_ms(uint64_t epoch_ms, const char *source)
{
    struct timeval tv = {0};

    if (epoch_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_logger.lock);
    bool same = s_logger.utc_anchor_valid && s_logger.utc_anchor_gps_ms == epoch_ms;
    portEXIT_CRITICAL(&s_logger.lock);
    if (same) {
        return ESP_OK;
    }

    tv.tv_sec = (time_t)(epoch_ms / 1000ULL);
    tv.tv_usec = (suseconds_t)((epoch_ms % 1000ULL) * 1000ULL);
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_logger.lock);
    s_logger.utc_anchor_valid = true;
    s_logger.utc_anchor_gps_ms = epoch_ms;
    s_logger.utc_anchor_ts_ms = timestamp_ms();
    portEXIT_CRITICAL(&s_logger.lock);
    ESP_LOGI(TAG, "applied UTC epoch from %s epoch_ms=%" PRIu64, source, epoch_ms);
    return ESP_OK;
}

/* "" when the clock is not known yet. */
static void format_event_utc(uint64_t ts_ms, char out[ESPNOW_LOG_UTC_STR_LEN])
{
    out[0] = '\0';
    portENTER_CRITICAL(&s_logger.lock);
    bool valid = s_logger.utc_anchor_valid && ts_ms >= s_logger.utc_anchor_ts_ms;
    uint64_t epoch_ms = valid ? s_logger.utc_anchor_gps_ms + (ts_ms - s_logger.utc_anchor_ts_ms) : 0;
    portEXIT_CRITICAL(&s_logger.lock);
    if (valid) {
        espnow_log_format_utc_iso8601(epoch_ms, out, ESPNOW_LOG_UTC_STR_LEN);
    }
}

/* ---------------------------------------------------------------- file names (writer task) */

/* The largest number among our files in logs/ (0 if there are none). */
static unsigned scan_max_index(void)
{
    char dir_path[64];
    const char *mount = sd_card_storage_get_mount_point();
    unsigned max_index = 0;

    if (mount == NULL) {
        return 0;
    }
    int written = snprintf(dir_path, sizeof(dir_path), "%s/%s", mount, ESPNOW_LOG_DIR);
    if (written <= 0 || (size_t)written >= sizeof(dir_path)) {
        return 0;
    }
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return 0; /* the first file creates the directory */
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        espnow_log_file_kind_t kind;
        unsigned index = 0;
        if (espnow_log_file_parse_name(entry->d_name, &kind, &index) && index > max_index) {
            max_index = index;
        }
    }
    closedir(dir);
    return max_index;
}

/*
 * The first file of each kind in a logging session shares one number (sensor_0007.bin
 * goes with espnow_0007.csv). A file started by rotation takes the next number above
 * every file on the card. Numbers are never reused.
 */
static esp_err_t pick_file(espnow_log_file_kind_t kind, char *path, size_t path_size)
{
    unsigned index = 0;

    if (!s_log.first_file_used[kind] && s_log.session_index != 0) {
        index = s_log.session_index;
    } else {
        index = scan_max_index() + 1U;
        if (s_log.session_index == 0) {
            s_log.session_index = index;
        }
    }
    s_log.first_file_used[kind] = true;
    ESP_RETURN_ON_FALSE(espnow_log_file_path(kind, index, path, path_size) != 0, ESP_ERR_INVALID_SIZE, TAG, "log path too long");
    return ESP_OK;
}

static esp_err_t sensor_path_fn(char *path, size_t path_size, void *ctx)
{
    (void)ctx;
    return pick_file(ESPNOW_LOG_FILE_SENSOR, path, path_size);
}

static esp_err_t csv_target_fn(sd_card_writer_target_t *target, void *ctx)
{
    (void)ctx;
    ESP_RETURN_ON_ERROR(pick_file(ESPNOW_LOG_FILE_ESPNOW, target->path, sizeof(target->path)), TAG, "pick espnow csv failed");
    target->append = false; /* never write into a file of an earlier session */
    target->header_len = strlen(ESPNOW_LOG_CSV_HEADER);
    memcpy(target->header, ESPNOW_LOG_CSV_HEADER, target->header_len);
    return ESP_OK;
}

/* ---------------------------------------------------------------- stream gate */

static bool stream_enter(void)
{
    bool open = false;

    portENTER_CRITICAL(&s_logger.lock);
    if (s_log.streams_open) {
        s_log.writers++;
        open = true;
    }
    portEXIT_CRITICAL(&s_logger.lock);
    return open;
}

static void stream_leave(void)
{
    portENTER_CRITICAL(&s_logger.lock);
    s_log.writers--;
    portEXIT_CRITICAL(&s_logger.lock);
}

static void note_frame_not_logged(void)
{
    portENTER_CRITICAL(&s_logger.lock);
    if (s_log.requested) {
        s_log.frames_not_logged++;
    }
    portEXIT_CRITICAL(&s_logger.lock);
}

static void write_sensor_record(const espnow_sensor_packet_t *packet, uint64_t ts_ms)
{
    uint8_t record[ESPNOW_SENSOR_RECORD_MAX_LEN];

    if (!stream_enter()) {
        note_frame_not_logged();
        return;
    }
    /* Only the Wi-Fi receive callback writes sensor records, so the sequence needs no lock.
       A record the card cannot take is dropped by the stream and shows as a gap in seq. */
    size_t length = espnow_sensor_record_encode(packet, ++s_log.sensor_seq, ts_ms, record, sizeof(record));
    if (length != 0) {
        (void)sd_card_writer_write(s_log.sensor_stream, record, length, 0);
    }
    stream_leave();
}

static void write_csv_row(uint64_t ts_ms, const uint8_t *src_mac, const uint8_t *payload, size_t len)
{
    char utc[ESPNOW_LOG_UTC_STR_LEN];
    char line[ESPNOW_LOG_CSV_LINE_MAX];

    if (!stream_enter()) {
        note_frame_not_logged();
        return;
    }
    format_event_utc(ts_ms, utc);
    size_t length = espnow_log_format_csv_line(utc, ts_ms, src_mac, payload, len, line, sizeof(line));
    if (length != 0) {
        (void)sd_card_writer_write(s_log.csv_stream, line, length, 0);
    }
    stream_leave();
}

/* ---------------------------------------------------------------- log control task */

static bool stream_has_failed(sd_card_writer_stream_t *stream)
{
    sd_card_writer_stats_t stats;

    return stream != NULL && sd_card_writer_get_stats(stream, &stats) == ESP_OK && stats.failed;
}

static void close_streams(void)
{
    portENTER_CRITICAL(&s_logger.lock);
    s_log.streams_open = false;
    s_log.closing = true;
    portEXIT_CRITICAL(&s_logger.lock);

    /* Callbacks that got in before the gate shut finish within microseconds. */
    for (int i = 0; i < STREAM_CLOSE_WAIT_TICKS; ++i) {
        portENTER_CRITICAL(&s_logger.lock);
        uint32_t writers = s_log.writers;
        portEXIT_CRITICAL(&s_logger.lock);
        if (writers == 0) {
            break;
        }
        vTaskDelay(1);
    }

    /* The totals stay readable after the streams are gone. */
    if (s_log.sensor_stream != NULL) {
        (void)sd_card_writer_get_stats(s_log.sensor_stream, &s_log.sensor_final);
        esp_err_t err = sd_card_writer_close_stream(s_log.sensor_stream);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sensor stream closed with %s", esp_err_to_name(err));
        }
        s_log.sensor_stream = NULL;
    }
    if (s_log.csv_stream != NULL) {
        (void)sd_card_writer_get_stats(s_log.csv_stream, &s_log.csv_final);
        esp_err_t err = sd_card_writer_close_stream(s_log.csv_stream);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "espnow csv stream closed with %s", esp_err_to_name(err));
        }
        s_log.csv_stream = NULL;
    }

    portENTER_CRITICAL(&s_logger.lock);
    s_log.closing = false;
    portEXIT_CRITICAL(&s_logger.lock);
}

static esp_err_t open_streams(uint32_t revision)
{
    const sd_card_writer_stream_config_t sensor_config = {
        .name = "sensor",
        .path_fn = sensor_path_fn,
        .buffer_size = SENSOR_BUFFER_BYTES,
        .flush_interval_ms = CONFIG_ESPNOW_LOGGER_SD_SYNC_INTERVAL_MS,
        .max_file_size = (uint32_t)CONFIG_ESPNOW_LOGGER_SENSOR_MAX_FILE_KB * 1024U,
    };
    const sd_card_writer_stream_config_t csv_config = {
        .name = "espnow",
        .target_fn = csv_target_fn,
        .buffer_size = CSV_BUFFER_BYTES,
        .flush_interval_ms = CONFIG_ESPNOW_LOGGER_SD_SYNC_INTERVAL_MS,
        .max_file_size = (uint32_t)CONFIG_ESPNOW_LOGGER_CSV_MAX_FILE_KB * 1024U,
    };
    sd_card_writer_stream_t *sensor = NULL;
    sd_card_writer_stream_t *csv = NULL;

    esp_err_t err = sd_card_writer_open_stream(&sensor_config, &sensor);
    if (err == ESP_OK) {
        err = sd_card_writer_open_stream(&csv_config, &csv);
        if (err != ESP_OK) {
            (void)sd_card_writer_close_stream(sensor);
            sensor = NULL;
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    s_log.session_index = 0;
    memset(s_log.first_file_used, 0, sizeof(s_log.first_file_used));
    memset(&s_log.sensor_final, 0, sizeof(s_log.sensor_final));
    memset(&s_log.csv_final, 0, sizeof(s_log.csv_final));
    s_log.sensor_seq = 0;
    s_log.sensor_stream = sensor;
    s_log.csv_stream = csv;
    s_log.opened_revision = revision;
    portENTER_CRITICAL(&s_logger.lock);
    s_log.frames_not_logged = 0;
    s_log.streams_open = true;
    portEXIT_CRITICAL(&s_logger.lock);
    return ESP_OK;
}

/*
 * Brings the streams in line with "logging is wanted AND the card is OK". The card is
 * sd_card_status's: when it leaves OK the streams are closed here, and a card that
 * comes back (a new revision) gets new streams and new file numbers.
 */
static void reconcile_log(void)
{
    const uint32_t now_ms = (uint32_t)timestamp_ms();
    const bool want = s_log.requested;
    const bool card_ok = sd_card_status_get_state() == SD_CARD_STATUS_OK && sd_card_writer_is_running();
    const uint32_t revision = sd_card_status_get_revision();

    if (s_log.streams_open) {
        const bool failed = stream_has_failed(s_log.sensor_stream) || stream_has_failed(s_log.csv_stream);

        if (want && card_ok && !failed && revision == s_log.opened_revision) {
            return;
        }
        close_streams();
        if (want && card_ok && failed) {
            /* The writer has already put the reason in the error log. */
            s_log.failed = true;
            s_log.last_error = ESP_FAIL;
            s_log.retry_after_ms = now_ms + CONFIG_ESPNOW_LOGGER_LOG_RETRY_MS;
            ESP_LOGW(TAG, "SD log stream failed; retrying in %d ms", CONFIG_ESPNOW_LOGGER_LOG_RETRY_MS);
        }
        return;
    }

    if (!want || !card_ok) {
        if (!card_ok) {
            s_log.failed = false; /* what is wrong is the card, and the icon says so */
        }
        return;
    }
    if (s_log.failed && (int32_t)(now_ms - s_log.retry_after_ms) < 0) {
        return;
    }
    esp_err_t err = open_streams(revision);
    if (err != ESP_OK) {
        s_log.failed = true;
        s_log.last_error = err;
        s_log.retry_after_ms = now_ms + CONFIG_ESPNOW_LOGGER_LOG_RETRY_MS;
        ERROR_LOG(err, "SD log streams could not be opened");
        return;
    }
    s_log.failed = false;
    s_log.last_error = ESP_OK;
    ESP_LOGI(TAG, "SD logging started");
}

static void log_control_task(void *ctx)
{
    (void)ctx;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_ESPNOW_LOGGER_LOG_POLL_MS));
        reconcile_log();
    }
}

/* ---------------------------------------------------------------- events */

static void record_event(espnow_logger_event_direction_t direction,
                         espnow_logger_event_status_t status,
                         uint8_t channel,
                         const uint8_t *src_mac,
                         const uint8_t *dst_mac,
                         int rssi,
                         const uint8_t *payload,
                         size_t len,
                         espnow_logger_event_t *out_event)
{
    size_t payload_len = len > ESPNOW_LOGGER_MAX_DATA_LEN ? ESPNOW_LOGGER_MAX_DATA_LEN : len;

    portENTER_CRITICAL(&s_logger.lock);
    size_t slot = s_logger.event_head;
    s_logger.event_head = (s_logger.event_head + 1U) % CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE;
    if (s_logger.event_count < CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE) {
        s_logger.event_count++;
    }
    s_logger.event_sequence++;

    espnow_logger_event_t *event = &s_logger.events[slot];
    memset(event, 0, sizeof(*event));
    event->sequence = s_logger.event_sequence;
    event->timestamp_ms = timestamp_ms();
    event->direction = direction;
    event->status = status;
    event->channel = channel;
    event->rssi = (int16_t)rssi;
    event->data_len = (uint16_t)len;
    event->payload_len = (uint16_t)payload_len;
    event->is_broadcast = is_broadcast_mac(dst_mac);
    if (src_mac != NULL) {
        memcpy(event->src_mac, src_mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
    }
    if (dst_mac != NULL) {
        memcpy(event->dst_mac, dst_mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
    }
    if (payload != NULL && payload_len > 0) {
        memcpy(event->payload, payload, payload_len);
    }
    if (out_event != NULL) {
        *out_event = *event;
    }
    portEXIT_CRITICAL(&s_logger.lock);
}

static uint8_t get_runtime_channel(void)
{
    uint8_t primary = CONFIG_ESPNOW_LOGGER_WIFI_CHANNEL;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;

    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
        primary = CONFIG_ESPNOW_LOGGER_WIFI_CHANNEL;
    }
    return primary;
}

/* ---------------------------------------------------------------- who sends the time */

/* Wi-Fi task. A time packet makes its sender the time source; IMU frames from that sender mark it streaming. */
static void note_time_source(espnow_sensor_kind_t kind, const uint8_t *src_mac, uint64_t ts_ms)
{
    portENTER_CRITICAL(&s_logger.lock);
    if (kind == ESPNOW_SENSOR_KIND_UTC_TIME) {
        if (!s_logger.time_source_valid || memcmp(s_logger.time_source_mac, src_mac, ESPNOW_LOGGER_MAC_ADDR_LEN) != 0) {
            memcpy(s_logger.time_source_mac, src_mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
            s_logger.time_source_imu_ts_ms = 0;
        }
        s_logger.time_source_valid = true;
        s_logger.time_source_ts_ms = ts_ms;
    } else if (s_logger.time_source_valid &&
               memcmp(s_logger.time_source_mac, src_mac, ESPNOW_LOGGER_MAC_ADDR_LEN) == 0) {
        s_logger.time_source_imu_ts_ms = ts_ms;
    }
    portEXIT_CRITICAL(&s_logger.lock);
}

bool espnow_logger_get_time_source(espnow_logger_time_source_t *out)
{
    const uint64_t now = timestamp_ms();
    bool present;

    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_logger.lock);
    present = s_logger.time_source_valid && now - s_logger.time_source_ts_ms <= ESPNOW_LOGGER_TIME_SOURCE_WINDOW_MS;
    if (present) {
        memcpy(out->mac, s_logger.time_source_mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
        out->imu_streaming = s_logger.time_source_imu_ts_ms != 0 &&
                             now - s_logger.time_source_imu_ts_ms <= ESPNOW_LOGGER_IMU_STREAM_WINDOW_MS;
    }
    portEXIT_CRITICAL(&s_logger.lock);
    return present;
}

/* ---------------------------------------------------------------- what arrives goes to the card */

/*
 * One RX frame: sensor packets become sensor.bin records (and may set the clock);
 * everything else, including time and control packets, becomes an espnow.csv row.
 * Decode errors go to the shared error log as well, only while logging is wanted.
 *
 * English contract: runs on the Wi-Fi task. It copies into streams with no wait.
 */
static void log_rx_frame(uint64_t ts_ms, const uint8_t *src_mac, const uint8_t *payload, size_t len)
{
    espnow_sensor_packet_t packet;

    espnow_sensor_packet_parse(payload, len, &packet);

    if (packet.utc_epoch_ms != 0 && apply_utc_epoch_ms(packet.utc_epoch_ms, "sensor packet") != ESP_OK) {
        ESP_LOGW(TAG, "could not set the clock from a sensor packet");
    }

    if (src_mac != NULL && (packet.kind == ESPNOW_SENSOR_KIND_UTC_TIME || packet.kind == ESPNOW_SENSOR_KIND_IMU ||
                            packet.kind == ESPNOW_SENSOR_KIND_IMU_BATCH ||
                            packet.kind == ESPNOW_SENSOR_KIND_IMU_BATCH_NOSUMMARY ||
                            packet.kind == ESPNOW_SENSOR_KIND_ATTITUDE)) {
        note_time_source(packet.kind, src_mac, ts_ms);
    }

    switch (packet.kind) {
    case ESPNOW_SENSOR_KIND_IMU:
    case ESPNOW_SENSOR_KIND_GPS:
    case ESPNOW_SENSOR_KIND_IMU_BATCH:
    case ESPNOW_SENSOR_KIND_IMU_BATCH_NOSUMMARY:
    case ESPNOW_SENSOR_KIND_ATTITUDE:
        write_sensor_record(&packet, ts_ms);
        return;
    case ESPNOW_SENSOR_KIND_DECODE_ERROR:
        if (s_log.requested) {
            char mac[ESPNOW_LOG_MAC_STR_LEN];
            espnow_log_format_mac(src_mac, mac);
            ERROR_LOG_MSG("%s src=%s", packet.error, mac);
        }
        break;
    default:
        break;
    }
    write_csv_row(ts_ms, src_mac, payload, len);
}

static void log_frame(const char *direction,
                      const char *status,
                      espnow_logger_event_direction_t event_direction,
                      espnow_logger_event_status_t event_status,
                      uint8_t channel,
                      const uint8_t *src_mac,
                      const uint8_t *dst_mac,
                      int rssi,
                      const uint8_t *payload,
                      size_t len)
{
    char src_str[ESPNOW_LOG_MAC_STR_LEN];
    char dst_str[ESPNOW_LOG_MAC_STR_LEN];
    char payload_hex[ESPNOW_LOG_HEX_MAX];
    espnow_logger_event_t event = {0};

    espnow_log_format_mac(src_mac, src_str);
    espnow_log_format_mac(dst_mac, dst_str);
    espnow_log_format_hex(payload, len, payload_hex, sizeof(payload_hex));
    record_event(event_direction, event_status, channel, src_mac, dst_mac, rssi, payload, len, &event);

    if (payload != NULL && len > 0) {
        if (event_direction == ESPNOW_LOGGER_EVENT_DIR_RX && event_status == ESPNOW_LOGGER_EVENT_STATUS_RECV) {
            log_rx_frame(event.timestamp_ms, src_mac, payload, len);
        } else {
            write_csv_row(event.timestamp_ms, src_mac, payload, len);
        }
    }

    ESP_LOGI(TAG,
             "%s status=%s ts_ms=%" PRIu64 " ch=%u src=%s dst=%s rssi=%d len=%u payload=%s",
             direction,
             status,
             event.timestamp_ms,
             channel,
             src_str,
             dst_str,
             rssi,
             (unsigned)len,
             payload_hex);
}

/* ---------------------------------------------------------------- ESP-NOW */

static esp_err_t ensure_peer(const uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN])
{
    esp_now_peer_info_t peer = {0};

    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "peer mac is null");
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    memcpy(peer.peer_addr, mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

static esp_err_t send_internal(const uint8_t dest_mac[ESPNOW_LOGGER_MAC_ADDR_LEN], const void *payload, size_t len)
{
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(dest_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "dest mac is null");
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_INVALID_ARG, TAG, "payload is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "payload is empty");
    ESP_RETURN_ON_FALSE(len <= ESPNOW_LOGGER_MAX_DATA_LEN, ESP_ERR_INVALID_SIZE, TAG, "payload too large: %u", (unsigned)len);
    ESP_RETURN_ON_FALSE(s_logger.started, ESP_ERR_INVALID_STATE, TAG, "logger not started");

    err = ensure_peer(dest_mac);
    if (err != ESP_OK) {
        ERROR_LOG(err, "peer register failed");
        return err;
    }
    log_frame("TX",
              "queued",
              ESPNOW_LOGGER_EVENT_DIR_TX,
              ESPNOW_LOGGER_EVENT_STATUS_QUEUED,
              get_runtime_channel(),
              s_logger.local_mac,
              dest_mac,
              0,
              (const uint8_t *)payload,
              len);
    err = esp_now_send(dest_mac, (const uint8_t *)payload, len);
    if (err != ESP_OK) {
        ERROR_LOG(err, "esp_now_send failed");
    }
    return err;
}

static void send_callback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    log_frame("TX",
              status == ESP_NOW_SEND_SUCCESS ? "done" : "fail",
              ESPNOW_LOGGER_EVENT_DIR_TX,
              status == ESP_NOW_SEND_SUCCESS ? ESPNOW_LOGGER_EVENT_STATUS_DONE : ESPNOW_LOGGER_EVENT_STATUS_FAIL,
              get_runtime_channel(),
              s_logger.local_mac,
              tx_info != NULL ? tx_info->des_addr : NULL,
              0,
              NULL,
              0);
}

static void recv_callback(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
    if (info == NULL || info->rx_ctrl == NULL || data == NULL || data_len < 0) {
        return;
    }
    log_frame("RX",
              "recv",
              ESPNOW_LOGGER_EVENT_DIR_RX,
              ESPNOW_LOGGER_EVENT_STATUS_RECV,
              info->rx_ctrl->channel,
              info->src_addr,
              info->des_addr,
              info->rx_ctrl->rssi,
              data,
              (size_t)data_len);
}

static bool copy_peer(uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN])
{
    portENTER_CRITICAL(&s_logger.lock);
    bool configured = s_logger.peer_configured;
    if (configured) {
        memcpy(mac, s_logger.peer_mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
    }
    portEXIT_CRITICAL(&s_logger.lock);
    return configured;
}

static void periodic_task(void *ctx)
{
    const TickType_t interval_ticks = pdMS_TO_TICKS(CONFIG_ESPNOW_LOGGER_PERIODIC_TX_INTERVAL_MS);
    const char *broadcast_payload = CONFIG_ESPNOW_LOGGER_BROADCAST_PAYLOAD;
    const char *unicast_payload = CONFIG_ESPNOW_LOGGER_UNICAST_PAYLOAD;
    (void)ctx;

    while (true) {
        vTaskDelay(interval_ticks);

        if (broadcast_payload[0] != '\0') {
            (void)espnow_logger_send_broadcast(broadcast_payload, strlen(broadcast_payload));
        }
        if (unicast_payload[0] != '\0') {
            (void)espnow_logger_send_to_configured_peer(unicast_payload, strlen(unicast_payload));
        }
    }
}

esp_err_t espnow_logger_start(void)
{
#if !CONFIG_ESPNOW_LOGGER_ENABLED
    ESP_LOGW(TAG, "CONFIG_ESPNOW_LOGGER_ENABLED is disabled");
    return ESP_OK;
#else
    uint32_t version = 0;

    if (APP_WIFI_STA_ENABLED) {
        /* Until coexistence is worked out, two owners of the Wi-Fi driver would fight. */
        ESP_LOGE(TAG, "not started: this service owns the Wi-Fi driver, build with APP_WIFI_STA=0");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_logger.started) {
        return ESP_OK;
    }

    const uint8_t channel = load_channel();
    load_peer();
    load_auto_start();
    s_log.requested = s_log.auto_start;

    RETURN_ON_ERROR_LOG(esp_netif_init(), "esp_netif init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        RETURN_ON_ERROR_LOG(err, "event loop init failed");
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    RETURN_ON_ERROR_LOG(esp_wifi_init(&wifi_init), "Wi-Fi init failed");
    RETURN_ON_ERROR_LOG(esp_wifi_set_storage(WIFI_STORAGE_RAM), "Wi-Fi storage set failed");
    RETURN_ON_ERROR_LOG(esp_wifi_set_mode(WIFI_MODE_STA), "Wi-Fi mode set failed");
    RETURN_ON_ERROR_LOG(esp_wifi_set_ps(WIFI_PS_NONE), "Wi-Fi power save disable failed");
    RETURN_ON_ERROR_LOG(esp_wifi_start(), "Wi-Fi start failed");
    RETURN_ON_ERROR_LOG(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), "Wi-Fi channel set failed");
    RETURN_ON_ERROR_LOG(esp_wifi_get_mac(WIFI_IF_STA, s_logger.local_mac), "Wi-Fi MAC read failed");
    RETURN_ON_ERROR_LOG(esp_now_init(), "ESP-NOW init failed");
    RETURN_ON_ERROR_LOG(esp_now_get_version(&version), "ESP-NOW version read failed");
    RETURN_ON_ERROR_LOG(esp_now_register_send_cb(send_callback), "send callback register failed");
    RETURN_ON_ERROR_LOG(esp_now_register_recv_cb(recv_callback), "recv callback register failed");
    RETURN_ON_ERROR_LOG(ensure_peer(s_broadcast_mac), "broadcast peer add failed");

    uint8_t peer_mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    if (copy_peer(peer_mac)) {
        RETURN_ON_ERROR_LOG(ensure_peer(peer_mac), "peer add failed");
    }

    seed_utc_anchor_from_system_time();
    s_logger.started = true;

    BaseType_t task_ok = xTaskCreate(log_control_task,
                                     "espnow_log",
                                     CONFIG_ESPNOW_LOGGER_LOG_TASK_STACK_SIZE,
                                     NULL,
                                     CONFIG_ESPNOW_LOGGER_LOG_TASK_PRIORITY,
                                     &s_log.task);
    if (task_ok != pdPASS) {
        RETURN_ON_ERROR_LOG(ESP_ERR_NO_MEM, "log control task create failed");
    }

    char local_mac_str[ESPNOW_LOG_MAC_STR_LEN];
    char peer_mac_str[ESPNOW_LOG_MAC_STR_LEN];
    espnow_log_format_mac(s_logger.local_mac, local_mac_str);
    espnow_log_format_mac(copy_peer(peer_mac) ? peer_mac : NULL, peer_mac_str);
    ESP_LOGI(TAG,
             "started ts_ms=%" PRIu64 " espnow_ver=%" PRIu32 " ch=%u local_mac=%s peer=%s periodic_tx_ms=%d auto_log=%d",
             timestamp_ms(),
             version,
             channel,
             local_mac_str,
             peer_mac_str,
             CONFIG_ESPNOW_LOGGER_PERIODIC_TX_INTERVAL_MS,
             (int)s_log.auto_start);

#if CONFIG_ESPNOW_LOGGER_SEND_STARTUP_FRAMES
    {
        const char *broadcast_payload = CONFIG_ESPNOW_LOGGER_BROADCAST_PAYLOAD;
        const char *unicast_payload = CONFIG_ESPNOW_LOGGER_UNICAST_PAYLOAD;

        if (broadcast_payload[0] != '\0') {
            RETURN_ON_ERROR_LOG(espnow_logger_send_broadcast(broadcast_payload, strlen(broadcast_payload)),
                                "startup broadcast failed");
        }
        if (unicast_payload[0] != '\0' && espnow_logger_has_configured_peer()) {
            RETURN_ON_ERROR_LOG(espnow_logger_send_to_configured_peer(unicast_payload, strlen(unicast_payload)),
                                "startup unicast failed");
        }
    }
#endif

    if (CONFIG_ESPNOW_LOGGER_PERIODIC_TX_INTERVAL_MS > 0) {
        task_ok = xTaskCreate(periodic_task,
                              "espnow_logger",
                              CONFIG_ESPNOW_LOGGER_TASK_STACK_SIZE,
                              NULL,
                              CONFIG_ESPNOW_LOGGER_TASK_PRIORITY,
                              &s_logger.periodic_task);
        if (task_ok != pdPASS) {
            RETURN_ON_ERROR_LOG(ESP_ERR_NO_MEM, "periodic task create failed");
        }
    }
    return ESP_OK;
#endif
}

bool espnow_logger_is_started(void)
{
    return s_logger.started;
}

uint8_t espnow_logger_get_channel(void)
{
    return s_logger.started ? get_runtime_channel() : load_channel();
}

esp_err_t espnow_logger_set_channel(uint8_t channel)
{
    ESP_RETURN_ON_FALSE(channel >= ESPNOW_LOGGER_CHANNEL_MIN && channel <= ESPNOW_LOGGER_CHANNEL_MAX,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid channel: %u",
                        channel);
    ESP_RETURN_ON_FALSE(s_logger.started, ESP_ERR_INVALID_STATE, TAG, "logger not started");

    RETURN_ON_ERROR_LOG(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), "Wi-Fi channel set failed");
    ESP_LOGI(TAG, "channel changed ts_ms=%" PRIu64 " ch=%u", timestamp_ms(), channel);

    uint8_t saved = 0;
    if (nvs_read_u8(&NVS_KEY_CHANNEL, &saved) == ESP_OK && saved == channel) {
        return ESP_OK;
    }
    esp_err_t err = nvs_write_u8(&NVS_KEY_CHANNEL, channel);
    if (err != ESP_OK) {
        ERROR_LOG(err, "saving the channel failed");
    }
    return err;
}

bool espnow_logger_has_configured_peer(void)
{
    uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN];

    return copy_peer(mac);
}

esp_err_t espnow_logger_get_local_mac(uint8_t mac[6])
{
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac is null");
    ESP_RETURN_ON_FALSE(s_logger.started, ESP_ERR_INVALID_STATE, TAG, "logger not started");
    memcpy(mac, s_logger.local_mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
    return ESP_OK;
}

esp_err_t espnow_logger_get_configured_peer_mac(uint8_t mac[6])
{
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac is null");
    if (!copy_peer(mac)) {
        memset(mac, 0, ESPNOW_LOGGER_MAC_ADDR_LEN);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t espnow_logger_set_configured_peer_mac(const uint8_t mac[6])
{
    uint8_t old_mac[ESPNOW_LOGGER_MAC_ADDR_LEN];

    ESP_RETURN_ON_FALSE(s_logger.started, ESP_ERR_INVALID_STATE, TAG, "logger not started");
    ESP_RETURN_ON_FALSE(mac == NULL || !is_broadcast_mac(mac), ESP_ERR_INVALID_ARG, TAG, "broadcast is not a peer");

    ESP_RETURN_ON_ERROR(nvs_write_mac(mac), TAG, "saving the peer failed");
    if (mac != NULL) {
        RETURN_ON_ERROR_LOG(ensure_peer(mac), "peer add failed");
    }
    bool had_old = copy_peer(old_mac);

    portENTER_CRITICAL(&s_logger.lock);
    s_logger.peer_configured = (mac != NULL);
    if (mac != NULL) {
        memcpy(s_logger.peer_mac, mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
    }
    portEXIT_CRITICAL(&s_logger.lock);

    if (had_old && (mac == NULL || memcmp(old_mac, mac, sizeof(old_mac)) != 0) && esp_now_is_peer_exist(old_mac)) {
        (void)esp_now_del_peer(old_mac);
    }
    return ESP_OK;
}

esp_err_t espnow_logger_send_broadcast(const void *payload, size_t len)
{
    return send_internal(s_broadcast_mac, payload, len);
}

esp_err_t espnow_logger_send_to_configured_peer(const void *payload, size_t len)
{
    uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN];

    if (!copy_peer(mac)) {
        return ESP_ERR_NOT_FOUND;
    }
    return send_internal(mac, payload, len);
}

esp_err_t espnow_logger_send_to_mac(const uint8_t dest_mac[6], const void *payload, size_t len)
{
    return send_internal(dest_mac, payload, len);
}

static esp_err_t send_imu_control(const uint8_t dest_mac[6], uint8_t command)
{
    espnow_sensor_imu_control_packet_t packet;

    ESP_RETURN_ON_FALSE(dest_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "dest mac is null");
    espnow_sensor_packet_build_imu_control(command, &packet);
    ESP_RETURN_ON_ERROR(send_internal(dest_mac, &packet, sizeof(packet)), TAG, "imu control send failed");
    ESP_LOGI(TAG,
             "sent IMU stream %s request to %02X:%02X:%02X:%02X:%02X:%02X",
             command == ESPNOW_SENSOR_IMU_CONTROL_START ? "start" : "stop",
             dest_mac[0],
             dest_mac[1],
             dest_mac[2],
             dest_mac[3],
             dest_mac[4],
             dest_mac[5]);
    return ESP_OK;
}

/* The configured peer when there is one, else everyone. */
static esp_err_t send_imu_control_default(uint8_t command)
{
    uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN];

    return send_imu_control(copy_peer(mac) ? mac : s_broadcast_mac, command);
}

esp_err_t espnow_logger_send_imu_stream_start_request(void)
{
    return send_imu_control_default(ESPNOW_SENSOR_IMU_CONTROL_START);
}

esp_err_t espnow_logger_send_imu_stream_stop_request(void)
{
    return send_imu_control_default(ESPNOW_SENSOR_IMU_CONTROL_STOP);
}

esp_err_t espnow_logger_send_imu_stream_request_to_mac(const uint8_t dest_mac[6], bool start)
{
    return send_imu_control(dest_mac, start ? ESPNOW_SENSOR_IMU_CONTROL_START : ESPNOW_SENSOR_IMU_CONTROL_STOP);
}

uint32_t espnow_logger_get_event_sequence(void)
{
    portENTER_CRITICAL(&s_logger.lock);
    uint32_t sequence = s_logger.event_sequence;
    portEXIT_CRITICAL(&s_logger.lock);
    return sequence;
}

size_t espnow_logger_get_event_count(void)
{
    portENTER_CRITICAL(&s_logger.lock);
    size_t count = s_logger.event_count;
    portEXIT_CRITICAL(&s_logger.lock);
    return count;
}

size_t espnow_logger_copy_recent_events(espnow_logger_event_t *events, size_t max_events)
{
    if (events == NULL || max_events == 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_logger.lock);
    size_t copy_count = s_logger.event_count < max_events ? s_logger.event_count : max_events;
    for (size_t i = 0; i < copy_count; ++i) {
        size_t index = (s_logger.event_head + CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE - 1U - i) %
                       CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE;
        events[i] = s_logger.events[index];
    }
    portEXIT_CRITICAL(&s_logger.lock);
    return copy_count;
}

void espnow_logger_clear_events(void)
{
    portENTER_CRITICAL(&s_logger.lock);
    memset(s_logger.events, 0, sizeof(s_logger.events));
    s_logger.event_count = 0;
    s_logger.event_head = 0;
    s_logger.event_sequence++;
    portEXIT_CRITICAL(&s_logger.lock);
}

/* ---------------------------------------------------------------- SD log API */

esp_err_t espnow_logger_start_logging(void)
{
    ESP_RETURN_ON_FALSE(s_logger.started, ESP_ERR_INVALID_STATE, TAG, "logger not started");
    s_log.requested = true;
    xTaskNotifyGive(s_log.task);
    return ESP_OK;
}

esp_err_t espnow_logger_stop_logging(void)
{
    ESP_RETURN_ON_FALSE(s_logger.started, ESP_ERR_INVALID_STATE, TAG, "logger not started");
    s_log.requested = false;
    xTaskNotifyGive(s_log.task);
    return ESP_OK;
}

static void copy_stream_stats(const sd_card_writer_stats_t *from, espnow_logger_stream_stats_t *to)
{
    to->bytes_written = from->bytes_written;
    to->bytes_dropped = from->bytes_dropped;
    to->records_dropped = from->records_dropped;
    to->files_opened = from->files_opened;
    to->failed = from->failed;
}

esp_err_t espnow_logger_get_log_status(espnow_logger_log_status_t *status)
{
    sd_card_writer_stats_t sensor = s_log.sensor_final;
    sd_card_writer_stats_t csv = s_log.csv_final;

    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    memset(status, 0, sizeof(*status));

    /* While the gate is held the streams cannot be closed under us. */
    bool open = stream_enter();
    if (open) {
        (void)sd_card_writer_get_stats(s_log.sensor_stream, &sensor);
        (void)sd_card_writer_get_stats(s_log.csv_stream, &csv);
        stream_leave();
    }

    portENTER_CRITICAL(&s_logger.lock);
    const bool closing = s_log.closing;
    status->frames_not_logged = s_log.frames_not_logged;
    portEXIT_CRITICAL(&s_logger.lock);

    status->requested = s_log.requested;
    status->last_error = s_log.last_error;
    status->sensor_records = s_log.sensor_seq;
    copy_stream_stats(&sensor, &status->sensor);
    copy_stream_stats(&csv, &status->espnow_csv);

    if (open) {
        status->state = s_log.requested ? ESPNOW_LOGGER_LOG_ACTIVE : ESPNOW_LOGGER_LOG_STOPPING;
    } else if (closing) {
        status->state = ESPNOW_LOGGER_LOG_STOPPING;
    } else if (!s_log.requested) {
        status->state = ESPNOW_LOGGER_LOG_STOPPED;
    } else {
        status->state = s_log.failed ? ESPNOW_LOGGER_LOG_FAILED : ESPNOW_LOGGER_LOG_WAITING_CARD;
    }
    return ESP_OK;
}

bool espnow_logger_get_auto_start_log_enabled(void)
{
    return s_log.auto_start;
}

esp_err_t espnow_logger_set_auto_start_log_enabled(bool enabled)
{
    ESP_RETURN_ON_ERROR(nvs_write_u8(&NVS_KEY_AUTO_LOG, enabled ? 1U : 0U), TAG, "save auto start setting failed");
    s_log.auto_start = enabled;
    return ESP_OK;
}
