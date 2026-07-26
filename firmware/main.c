/* ============================================================
 * Theme: SKIF-A7 SECURITY
 * ============================================================
 */

#ifndef WITH_LOAD
#define WITH_LOAD 1

#endif

#define INJECT_MUTEX_BLOCK 0    //switch for implementing Task A mutex take and never give

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <stdbool.h>    //included to use bools

#define BUTTON_GPIO   GPIO_NUM_18      /* input — button to GND */
#define ISR_PULSE_GPIO GPIO_NUM_19     /* output — scope this for latency */
#define DEBOUNCE_US   200


#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "SKIF-A7";

static void update_security_state_locked(void);

/* Signaling primitive */
static TaskHandle_t      task_notif_handle;  /* direct notification path */

/* Latency telemetry */
static volatile int64_t isr_entry_time_us;
static volatile uint32_t presses_observed;
static volatile uint64_t latency_max_notif_us;

static SemaphoreHandle_t security_mutex; 

/* Added for Task A */
typedef enum {
    SECURITY_NORMAL,
    SECURITY_SUSPICIOUS,
    SECURITY_DEGRADED,
    SECURITY_ALERT
} security_state_t;

static security_state_t security_state = SECURITY_NORMAL;
static uint8_t tamper_score = 0;
static bool tamper_detected = false;

/* Added for Task B */
static float vibration_score = 0.0f;
static bool vibration_alert = false;

/* Added for Task C */
static uint32_t integrity_failures = 0;
static bool integrity_fault = false;

/* Added for Task D */
#define SECURITY_EVENT_COUNT 64

typedef struct {
    uint32_t timestamp_ms;
    uint16_t severity;
    uint16_t source;
} security_event_t;

static security_event_t security_events[SECURITY_EVENT_COUNT];

const char *state_name[] =
{
    "NORMAL",
    "SUSPICIOUS",
    "DEGRADED",
    "ALERT"
};

/* Bottom-half variables */
static volatile uint32_t tamper_event = 0;
static volatile bool button_tamper_latched = false;

/* Debounce — track time of last accepted edge */
static volatile int64_t last_edge_us;

/* ============================================================
 *  ISR — runs in interrupt context. IRAM_ATTR avoids the
 *  first-execution cache-fill penalty from flash.
 * ============================================================ */
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();

    /* Debounce: drop edges within DEBOUNCE_US of last accepted one. */
    if (now - last_edge_us < DEBOUNCE_US) return;
    last_edge_us = now;

    /* 1. Toggle the scope output HIGH so the logic analyzer can see ISR entry. */
    gpio_set_level(ISR_PULSE_GPIO, 1);

    isr_entry_time_us = now;
    presses_observed++;

    BaseType_t higher_woken = pdFALSE;

    /* 3. Signal via direct task notification.
     *    Faster than the semaphore on most ports; one-to-one. */
    vTaskNotifyGiveFromISR(task_notif_handle, &higher_woken);

    /* 4. Toggle scope output LOW — ISR is about to return. */
    gpio_set_level(ISR_PULSE_GPIO, 0);

    /* 5. Request a context switch on ISR exit if a higher-priority task is ready. */
    portYIELD_FROM_ISR(higher_woken);
}

/* ============================================================
 *  Bottom-half task: direct-notification path
 * ============================================================ */
static void btn_task_notif(void *arg)
{
    for (;;) {
        uint32_t count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (count == 0) continue;

        int64_t wake = esp_timer_get_time();
        int64_t lat = wake - isr_entry_time_us;
        ESP_LOGI(TAG, "Wake latency = %lld us", lat);
        if ((uint64_t)lat > latency_max_notif_us) latency_max_notif_us = (uint64_t)lat;
        tamper_event++;

        /* OpenAI ChatGPT https://chatgpt.com/share/6a64ff67-9ec0-83ea-889e-00f505be035f */
        ESP_LOGI(TAG, "Button task waiting for security mutex");

        if (xSemaphoreTake(security_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "Button task acquired security mutex");
          /* Toggle the latched tamper state */
          if (!button_tamper_latched) {

              /* First press: trigger alarm */
              button_tamper_latched = true;
              tamper_score = 100;
              tamper_detected = true;

              ESP_LOGW(TAG, "TAMPER DETECTED");

          } else {

              /* Second press: acknowledge/reset alarm */
              button_tamper_latched = false;
              tamper_score = 0;
              tamper_detected = false;

              ESP_LOGI(TAG, "TAMPER RESET");
          }
           update_security_state_locked();
           xSemaphoreGive(security_mutex);
        }
        else {
              ESP_LOGE(TAG, "Security mutex timeout in button task");
        }
      }
    }

#if WITH_LOAD
/* ============================================================
 *  BACKGROUND LOAD  (WITH_LOAD = 1)
 * ============================================================
 *
 * These four tasks are based on App 2's scheduler demo: four
 * periodic tasks pinned to Core 1 on the rate-monotonic ladder. 
 * Here they exist to put Core 1 under realistic contention while you measure ISR
 * response latency. 
 * You are not studying these bodies in App 3 — you are studing what
 * their presence does to your wake latency.
 *
 * Why these default code segments (the same rules App 2 fixed on):
 *   (1) DEAD-CODE ELIMINATION. Each kernel ends by writing a `volatile` sink
 *       and seeds itself from that sink, so -O2/-Os cannot delete the work.
 *   (2) INITIALIZE BUFFERS ONCE. Large buffers are static and filled a single
 *       time in load_init_buffers(), never inside the period.
 *   (3) float, NOT double. The ESP32 FPU is single-precision; double is
 *       software-emulated and runs with data-dependent timing.
 *   (4) WOKWI != SILICON. The *_ITERS / *_N / *_LEN knobs are 240 MHz ballpark.
 *       Tune them if you want a specific load level; the defaults give a light,
 *       comfortably schedulable set (~15-20% utilization).
 *
 * Per-task heartbeat counters and a WCET-max helper are included so you can
 * confirm the load is actually running (heartbeats climbing) and, if you want,
 * report the load's own WCET. Single 32-bit reads are atomic on Xtensa, so the
 * heartbeats need no mutex yet (App 6 changes that).
 */
static volatile uint32_t hb_a, hb_b, hb_c, hb_d;
static uint64_t wcet_a_max_us, wcet_b_max_us, wcet_c_max_us, wcet_d_max_us;
static uint64_t wcet_a_sum_us, wcet_b_sum_us, wcet_c_sum_us, wcet_d_sum_us;   //sum of wcet per task for mean

#define MEASURE_WCET(_max_var, _sum_var, _body) do {                       \
    int64_t _t0 = esp_timer_get_time();                          \
    _body;                                                        \
    int64_t _dt = esp_timer_get_time() - _t0;                    \
    if ((uint64_t)_dt > (_max_var)) (_max_var) = (uint64_t)_dt;  \
    _sum_var += _dt;} while (0)    //added from App#2 to compute mean WCET

/* ---- Task A  priority 15  period 10 ms : Security Tamper Detection---- */
#define A_ITERS 150 
#define TAMPER_THRESHOLD 80 
static volatile uint32_t a_sink;
static void tamper_detect(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(10);
    for (;;) {
        MEASURE_WCET(wcet_a_max_us, wcet_a_sum_us,{
            uint32_t x = a_sink ? a_sink : 0xACE1u;   /* seed from sink (observable) */
            for (int i = 0; i < A_ITERS; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            }

            /* OpenAI ChatGPT https://chatgpt.com/share/6a64e6cf-c704-83ea-b720-20d09eb4bca2 */
            bool raw_tamper = ((x & 0xFFu) > 245u);

        if (xSemaphoreTake(security_mutex, portMAX_DELAY) == pdTRUE) {
            #if INJECT_MUTEX_BLOCK
              ESP_LOGE(TAG, "FAULT: Task A holding mutex indefinitely");
              for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000));
              }
            #endif

            if (raw_tamper) {
                if (tamper_score <= 95) {
                    tamper_score += 5;
                }
                else {
                    tamper_score = 100;
                }
            }
            else if (tamper_score > 0 && !button_tamper_latched) {
                tamper_score--;
            }

            if (button_tamper_latched) {
                tamper_score = 100;
            }

            tamper_detected =
                button_tamper_latched ||
                tamper_score >= TAMPER_THRESHOLD;

            update_security_state_locked();

            xSemaphoreGive(security_mutex);
        }
            a_sink = x;
        });
        hb_a++;
        vTaskDelayUntil(&last, period);
      }
}

/* ---- Load Task B  priority 10  period 20 ms : Intrusion Analysis ---- */
#define B_SAMP 64                      /* power of two for the index mask */    //reduced load because of watchdog error
#define B_TAPS 32                       /* <= B_SAMP */   //reduced load because of watchdog error
#define VIBRATION_THRESHOLD 25.0f

static float b_buf[B_SAMP];
static float b_coef[B_TAPS];
static volatile float b_sink;

static void intrusion_analysis(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);
    for (;;) {
        MEASURE_WCET(wcet_b_max_us, wcet_b_sum_us,{
            float acc = 0.0f;          /* seed */
            for (int n = 0; n < B_SAMP; n++)
                for (int k = 0; k < B_TAPS; k++)
                    acc += b_buf[(n + B_SAMP - k) & (B_SAMP - 1)] * b_coef[k];
            b_sink = acc;

            /* OpenAI ChatGPT https://chatgpt.com/share/6a64ebb4-2368-83ea-a60b-ea3ce9f14ee7 */
            bool new_vibration_alert =
            acc > VIBRATION_THRESHOLD ||
            acc < -VIBRATION_THRESHOLD;

        if (xSemaphoreTake(security_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
            vibration_score = acc;
            vibration_alert = new_vibration_alert;

            update_security_state_locked();

            xSemaphoreGive(security_mutex);
        }
        });

        hb_b++;
        vTaskDelayUntil(&last, period);
    }
}

/* ---- Load Task C  priority 5  period 50 ms : Integrity Verification---- */
#define C_LEN 256                    /* bytes; raise toward 49152 to lengthen */   //reduced load because of watchdog error
#define EXPECTED_PACKET_CRC 0x2144DF1Cu

static uint8_t c_pkt[C_LEN];
static volatile uint32_t c_sink;

static void integrity_ver(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(50);
    for (;;) {
        MEASURE_WCET(wcet_c_max_us, wcet_c_sum_us,{
            uint32_t crc = 0xFFFFFFFFu;     // seed
            for (int n = 0; n < C_LEN; n++) {
                crc ^= c_pkt[n];
                for (int b = 0; b < 8; b++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
            }
            uint32_t final_crc = crc ^ 0xFFFFFFFFu;
            c_sink = final_crc;

            /* OpenAI ChatGPT https://chatgpt.com/share/6a64efbf-64b0-83ea-847c-8e2fdf0d5b2c */
             bool packet_valid = (final_crc == EXPECTED_PACKET_CRC);
             bool new_integrity_fault = !packet_valid;

          if (xSemaphoreTake(security_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
              integrity_fault = new_integrity_fault;

              if (new_integrity_fault) {
                  integrity_failures++;
              }

              update_security_state_locked();

              xSemaphoreGive(security_mutex);
          }
        });
        hb_c++;
        vTaskDelayUntil(&last, period);
    }
}

/* ---- Load Task D  priority 2  period 100 ms : Pending Event Prioritization ---- */
#define D_N SECURITY_EVENT_COUNT 

static volatile int d_sink;

static void pending_events(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(100);
    for(;;){
      security_state_t state_snapshot = SECURITY_DEGRADED;
      bool tamper_snapshot = false;
      bool vibration_snapshot = false;
      bool integrity_snapshot = true;

      MEASURE_WCET(wcet_d_max_us, wcet_d_sum_us, {
      
        /* OpenAI ChatGPThttps://chatgpt.com/share/6a64efbf-64b0-83ea-847c-8e2fdf0d5b2c */

        if (xSemaphoreTake(security_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            state_snapshot = security_state;
            tamper_snapshot = tamper_detected;
            vibration_snapshot = vibration_alert;
            integrity_snapshot = integrity_fault;

            xSemaphoreGive(security_mutex);
        }
        else {
            state_snapshot = SECURITY_DEGRADED;
            tamper_snapshot = false;
            vibration_snapshot = false;
            integrity_snapshot = true;
        }
                    /* 2. Generate pending security events */
            for (int i = 0; i < D_N; i++) {
                security_events[i].timestamp_ms =
                    (uint32_t)(esp_timer_get_time() / 1000);

                security_events[i].severity =
                    (uint16_t)((i * 37u + d_sink) % 100u);

                security_events[i].source =
                    (uint16_t)((i % 3) + 1);
            }

            /* 3. Prioritize events by severity */
            for (int i = 1; i < D_N; i++) {
                security_event_t key = security_events[i];
                int j = i - 1;

                while (j >= 0 &&
                       security_events[j].severity < key.severity) {

                    security_events[j + 1] =
                        security_events[j];

                    j--;
                }

                security_events[j + 1] = key;
            }

            /* Observable sink prevents optimization */
            d_sink = security_events[0].severity;
        });
        hb_d++;

        //Added form App #2 to log task WCETs
        if(hb_a > 0 && hb_b > 0 && hb_c > 0 && hb_d > 0){
          if(hb_d % 2 == 0){
            ESP_LOGI(TAG, "SECURITY STATUS=%s "
            "tamper=%d vibration=%d integrity=%d\n"
            "WCET us\n"
            "A HB=%lu mean=%llu max=%llu\n"
            "B HB=%lu mean=%llu max=%llu\n" 
            "C HB=%lu mean=%llu max=%llu\n"
            "D HB=%lu mean=%llu max=%llu\n",
            state_name[state_snapshot],
            tamper_snapshot,
            vibration_snapshot,
            integrity_snapshot,    
            hb_a, wcet_a_sum_us/hb_a, wcet_a_max_us,
            hb_b, wcet_b_sum_us/hb_b, wcet_b_max_us,
            hb_c, wcet_c_sum_us/hb_c, wcet_c_max_us,
            hb_d, wcet_d_sum_us/hb_d, wcet_d_max_us);
          }
    }

        vTaskDelayUntil(&last, period);
    }
}

/* Fill the load buffers exactly once, off the periodic path. OpenAI ChatGPT https://chatgpt.com/share/6a64efbf-64b0-83ea-847c-8e2fdf0d5b2c */
static void load_init_buffers(void)
{
    /* Initialize Task B vibration-analysis data. */
    for (int i = 0; i < B_SAMP; i++) {
        b_buf[i] = (float)((i % 11) - 5);
    }

    for (int i = 0; i < B_TAPS; i++) {
        b_coef[i] = 1.0f / (float)B_TAPS;
    }

    /* Initialize Task C CRC packet. */
    for (int i = 0; i < C_LEN; i++) {
        c_pkt[i] = (uint8_t)(i * 31 + 7);
    }
}

static void start_background_load(void)
{
    load_init_buffers();
    /* Rate-monotonic ladder, all on Core 1, mirroring App 2. These priorities
     * are FIXED here (this is a load fixture). Note A=15 outranks your
     * bottom-half tasks (12); B/C/D do not. */
    xTaskCreatePinnedToCore(tamper_detect, "load_a", 2048, NULL, 15, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(intrusion_analysis, "load_b", 2048, NULL, 10, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(integrity_ver, "load_c", 2048, NULL,  5, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(pending_events, "load_d", 4096, NULL,  2, NULL, APP_CPU_NUM);
}
#endif /* WITH_LOAD */

/* OpenAI ChatGPT */
static void update_security_state_locked(void)
{
    if (button_tamper_latched || tamper_detected) {
        security_state = SECURITY_ALERT;
    }
    else if (integrity_fault) {
        security_state = SECURITY_DEGRADED;
    }
    else if (vibration_alert) {
        security_state = SECURITY_SUSPICIOUS;
    }
    else {
        security_state = SECURITY_NORMAL;
    }
}

/* ============================================================
 *  app_main — wire everything up
 * ============================================================ */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== CAPSTONE [SKIF-A7 SECURITY] starting — ISR + bottom-half ====");

#if WITH_LOAD
    ESP_LOGI(TAG, "Run mode: UNDER LOAD (WITH_LOAD=1) — CAPSTONE's 4 tasks on Core 1");
#else
    ESP_LOGI(TAG, "Run mode: IDLE (WITH_LOAD=0) — baseline latency, no background tasks");
#endif

security_mutex = xSemaphoreCreateMutex();   //OpenAI ChatGPT

if (security_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create security mutex");
    return;
}

    /* Bottom-half task. Pinned to Core 1
     * they're the "real-time response" path. */
    xTaskCreatePinnedToCore(btn_task_notif,"btn_notif", 4096, NULL, 12,
                            &task_notif_handle, APP_CPU_NUM);    //increase stack size due to overflow

#if WITH_LOAD
    /* Bring CAPSONE's periodic tasks online as a Core-1 load fixture. */
    start_background_load();
#endif

    /* Configure GPIOs. */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,    /* button pulls low when pressed */
    };
    gpio_config(&btn_cfg);

    gpio_config_t pulse_cfg = {
        .pin_bit_mask = 1ULL << ISR_PULSE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pulse_cfg);
    gpio_set_level(ISR_PULSE_GPIO, 0);

    /* Install GPIO ISR service. Flags = 0 means default (low) priority. */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    ESP_LOGI(TAG, "Press the button on GPIO %d. Scope GPIO %d to time the ISR.",
             BUTTON_GPIO, ISR_PULSE_GPIO);

    /* app_main returns; tasks continue. */
}
