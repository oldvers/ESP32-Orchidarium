#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_sntp.h"

#include "time_task.h"
#include "led_task.h"
#include "climate_task.h"

//-------------------------------------------------------------------------------------------------

#define TIME_TASK_TICK_MS   (pdMS_TO_TICKS(1000))

#define RGBA(rv,gv,bv,av)   {.r=rv,.g=gv,.b=bv,.a=av}

#define TIME_LOG  1

#if (1 == TIME_LOG)
static const char * gTAG = "TIME";
#    define TIME_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define TIME_LOGE(...)  ESP_LOGE(gTAG, __VA_ARGS__)
#    define TIME_LOGW(...)  ESP_LOGV(gTAG, __VA_ARGS__)
#else
#    define TIME_LOGI(...)
#    define TIME_LOGE(...)
#    define TIME_LOGW(...)
#endif

//-------------------------------------------------------------------------------------------------

enum
{
    /* Calculated for Dec 21 2024 14:00:00 (seconds) */
    TIME_SHORTEST_DAY_DURATION = 22222,
    /* Calculated for Jun 21 2024 14:00:00 (seconds) */
    TIME_LONGEST_DAY_DURATION  = 52666,
    TIME_STR_MAX_LEN           = 28,
    /* Whole 24 hours day duration (seconds) */
    TIME_FULL_DAY_DURATION     = (24 * 60 * 60),
    TIME_UV_MIN                = 25,
    TIME_UV_MAX                = 150,
    TIME_W_MIN                 = 170,
    TIME_W_MAX                 = 230,
};

enum
{
    TIME_IDX_MIDNIGHT = 0,
    TIME_IDX_MORNING_BLUE_HOUR,  
    TIME_IDX_MORNING_GOLDEN_HOUR,
    TIME_IDX_RISE,
    TIME_IDX_DAY,
    TIME_IDX_NOON,
    TIME_IDX_EVENING_GOLDEN_HOUR,
    TIME_IDX_SET,
    TIME_IDX_EVENING_BLUE_HOUR,
    TIME_IDX_NIGHT,
    TIME_IDX_MAX,
};

typedef struct
{
    time_t   start;
    uint32_t interval;
} time_point_t;

typedef struct
{
    led_color_t   src;
    led_color_t   dst;
    led_command_t cmd;
} rgb_tx_t;

typedef struct
{
    rgb_tx_t * transition;
    uint32_t   interval;
} rgb_point_t;

typedef struct
{
    led_command_t u_cmd;
    uint8_t       u_max;
    led_command_t w_cmd;
    uint8_t       w_max;
} uw_tx_t;

typedef struct
{
    uw_tx_t * transition;
    uint32_t  interval;
} uw_point_t;

typedef struct
{
    climate_command_t cmd;
    fan_speed_t       speed;
    bool              repeat;
    uint32_t          interval;
    uint32_t          duration;
} fan_tx_t, * fan_tx_p;

typedef struct
{
    climate_command_t cmd;
    bool              on;
    bool              repeat;
    uint32_t          interval;
    uint32_t          duration;
} humidifier_tx_t, * humidifier_tx_p;

//-------------------------------------------------------------------------------------------------

/* Time zone */
/* https://remotemonitoringsystems.ca/time-zone-abbreviations.php */
/* Europe -Kyiv,Ukraine - EET-2EEST,M3.5.0/3,M10.5.0/4 */
/* https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv */
/* Europe/Kiev - EET-2EEST,M3.5.0/3,M10.5.0/4 */
static const char * gTZ  = "EET-2EEST,M3.5.0/3,M10.5.0/4";
static const double gPi  = 3.14159265;
static const double gLat = 49.839684;
static const double gLon = 24.029716;

static QueueHandle_t  gTimeQueue = {0};
static time_command_t gCommand   = TIME_CMD_EMPTY;
static time_t         gAlarm     = LONG_MAX;

/* Start             -    0 minutes */
/* MorningBlueHour   -  429 minutes */
/* MorningGoldenHour -  442 minutes */
/* Rise              -  461 minutes */
/* Day               -  505 minutes */
/* Noon              -  791 minutes */
/* EveningGoldenHour - 1076 minutes */
/* Set               - 1120 minutes */
/* EveningBlueHour   - 1140 minutes */
/* Night             - 1153 minutes */

static time_point_t gTimePoints[] =
{
    [TIME_IDX_MIDNIGHT]            = {0, 0},
    [TIME_IDX_MORNING_BLUE_HOUR]   = {0, 0},
    [TIME_IDX_MORNING_GOLDEN_HOUR] = {0, 0},
    [TIME_IDX_RISE]                = {0, 0},
    [TIME_IDX_DAY]                 = {0, 0},
    [TIME_IDX_NOON]                = {0, 0},
    [TIME_IDX_EVENING_GOLDEN_HOUR] = {0, 0},
    [TIME_IDX_SET]                 = {0, 0},
    [TIME_IDX_EVENING_BLUE_HOUR]   = {0, 0},
    [TIME_IDX_NIGHT]               = {0, 0},
};

/* Start             - RGB(0,0,32)    - RGB(0,0,44)    - Smooth      -  429 */
/* MorningBlueHour   - RGB(0,0,44)    - RGB(64,0,56)   - Rainbow CW  -   13 */
/* MorningGoldenHour - RGB(64,0,56)   - RGB(220,220,0) - Rainbow CW  -   19 */
/* Rise              -                -                -             -  +44 */
/* Day               - RGB(220,220,0) - RGB(10,10,10)  - Sine        -  286 */
/* Noon              -                -                -             - +286 */
/* EveningGoldenHour - RGB(220,220,0) - RGB( 64,0,56)  - Rainbow CCW -   44 */
/* Set               -                -                -             -  +20 */
/* EveningBlueHour   - RGB(64,0,56)   - RGB(0,0,44)    - Rainbow CCW -   13 */
/* Night             - RGB(0,0,44)    - RGB(0,0,32)    - None        -  287 */

static rgb_tx_t gRgbStart =
{
    RGBA(0, 0, 32, 1),
    RGBA(0, 0, 44, 1),
    LED_CMD_RGB_INDICATE_COLOR
};
static rgb_tx_t gRgbMorningBlueHour =
{
    RGBA( 0, 0, 44, 0),
    RGBA(64, 0, 56, 1),
    LED_CMD_RGB_INDICATE_RAINBOW
};
static rgb_tx_t gRgbMorningGoldenHour =
{
    RGBA( 64,   0, 56, 0),
    RGBA(220, 220,  0, 1),
    LED_CMD_RGB_INDICATE_RAINBOW
};
static rgb_tx_t gRgbDay =
{
    RGBA(220, 220, 0, 1),
    RGBA( 10,  10, 10,1),
    LED_CMD_RGB_INDICATE_SINE
};
static rgb_tx_t gRgbEveningGoldenHour =
{
    RGBA(220, 220,  0, 1),
    RGBA( 64,   0, 56, 0),
    LED_CMD_RGB_INDICATE_RAINBOW
};
static rgb_tx_t gRgbEveningBlueHour =
{
    RGBA(64, 0, 56, 1),
    RGBA( 0, 0, 44, 0),
    LED_CMD_RGB_INDICATE_RAINBOW
};
static rgb_tx_t gRgbNight =
{
    RGBA(0, 0, 44, 1),
    RGBA(0, 0, 32, 1),
    LED_CMD_RGB_INDICATE_COLOR
};

static rgb_point_t gRgbPoints[] =
{
    [TIME_IDX_MIDNIGHT]            = {&gRgbStart,             0},
    [TIME_IDX_MORNING_BLUE_HOUR]   = {&gRgbMorningBlueHour,   0},
    [TIME_IDX_MORNING_GOLDEN_HOUR] = {&gRgbMorningGoldenHour, 0},
    [TIME_IDX_RISE]                = {NULL,                   0},
    [TIME_IDX_DAY]                 = {&gRgbDay,               0},
    [TIME_IDX_NOON]                = {NULL,                   0},
    [TIME_IDX_EVENING_GOLDEN_HOUR] = {&gRgbEveningGoldenHour, 0},
    [TIME_IDX_SET]                 = {NULL,                   0},
    [TIME_IDX_EVENING_BLUE_HOUR]   = {&gRgbEveningBlueHour,   0},
    [TIME_IDX_NIGHT]               = {&gRgbNight,             0},
};

/*                        ---------                         <---- Longest Day (UV/W max)    */
/*                    ----         ----                                                     */
/*                 ---                 ---                                                  */
/*              ---                       ---                                               */
/*            --                             --                                             */
/*          --                                 --                                           */
/*       ---              ---------              ---        <---- Shortest Day (UV/W min)   */
/*    ---     ------------         ------------     ---                                     */
/* -----------                                 -----------                                  */
/* Day -----------------------------------------------> Evening Golden Hour                 */
/*                                                                                          */
/* duration = (evening_golden_hour - day);                                                  */
/* percent = (duration-SHORTEST_DAY_DURATION)/(LONGEST_DAY_DURATION-SHORTEST_DAY_DURATION); */
/* UV = (UV_MIN + (UV_MAX - UV_MIN) * percent);                                             */
/* W = (W_MIN + (W_MAX - W_MIN) * percent);                                                 */
/*                                                                                          */
/* Start             - UW(0,0)     - Smooth -  429 */
/* MorningBlueHour   -             -        -  +13 */
/* MorningGoldenHour -             -        -  +19 */
/* Rise              -             -        -  +44 */
/* Day               - UW(110,180) - Sine   -  286 */
/* Noon              -             -        - +286 */
/* EveningGoldenHour - UW(0,0)     - Smooth -   44 */
/* Set               -             -        -  +20 */
/* EveningBlueHour   -             -        -  +13 */
/* Night             -             -        - +287 */

static uw_tx_t gUwStart =
{
    LED_CMD_UV_INDICATE_BRIGHTNESS,
    0,
    LED_CMD_W_INDICATE_BRIGHTNESS,
    0
};
static uw_tx_t gUwDay =
{
    LED_CMD_UV_INDICATE_SINE,
    0,
    LED_CMD_W_INDICATE_SINE,
    0
};
static uw_tx_t gUwEveningGoldenHour =
{
    LED_CMD_UV_INDICATE_BRIGHTNESS,
    0,
    LED_CMD_W_INDICATE_BRIGHTNESS,
    0
};

static uw_point_t gUwPoints[] =
{
    [TIME_IDX_MIDNIGHT]            = {&gUwStart,             0},
    [TIME_IDX_MORNING_BLUE_HOUR]   = {NULL,                  0},
    [TIME_IDX_MORNING_GOLDEN_HOUR] = {NULL,                  0},
    [TIME_IDX_RISE]                = {NULL,                  0},
    [TIME_IDX_DAY]                 = {&gUwDay,               0},
    [TIME_IDX_NOON]                = {NULL,                  0},
    [TIME_IDX_EVENING_GOLDEN_HOUR] = {&gUwEveningGoldenHour, 0},
    [TIME_IDX_SET]                 = {NULL,                  0},
    [TIME_IDX_EVENING_BLUE_HOUR]   = {NULL,                  0},
    [TIME_IDX_NIGHT]               = {NULL,                  0},
};

//-------------------------------------------------------------------------------------------------

static void time_SunCalculate(time_t time, double angle, time_t * p_m, time_t * p_e)
{
    /* Convert Unix Time Stamp to Julian Day */
    time_t Jdate = (time_t)(time / 86400.0 + 2440587.5);
    /* Number of days since Jan 1st, 2000 12:00 */
    double n = (double)Jdate - 2451545.0 + 0.0008;
    /* Mean solar noon */
    double Jstar = -gLon / 360 + n;
    /* Solar mean anomaly */
    double M = fmod((357.5291 + 0.98560028 * Jstar), 360);
    /* Equation of the center */
    double C = 0.0003 * sin(3 * M * 360 * 2 * gPi);
    C += 0.02 * sin(2 * M / 360 * 2 * gPi);
    C += 1.9148 * sin(M / 360 * 2 * gPi);
    /* Ecliptic longitude */
    double lambda = fmod((M + C + 180 + 102.9372), 360);
    /* Solar transit */
    double Jtransit = 0.0053 * sin(M / 360.0 * 2.0 * gPi);
    Jtransit -= 0.0069 * sin(2.0 * (lambda / 360.0 * 2.0 * gPi));
    Jtransit += Jstar;
    /* Declination of the Sun */
    double delta = sin(lambda / 360 * 2 * gPi) * sin(23.44 / 360 * 2 * gPi);
    delta = asin(delta) / (2 * gPi) * 360;
    /* Hour angle */
    double omega0 = sin(gLat / 360 * 2 * gPi) * sin(delta / 360 * 2 * gPi);
    omega0 = (sin(angle / 360 * 2 * gPi) - omega0);
    omega0 /= (cos(gLat / 360 * 2 * gPi) * cos(delta / 360 * 2 * gPi));
    omega0 = 360 / (2 * gPi) * acos(omega0);
    /* Julian day sunrise, sunset */
    double Jevening = Jtransit + omega0 / 360;
    double Jmorning = Jtransit - omega0 / 360;
    /* Convert to Unix Timestamp */
    time_t morning = (time_t)(Jmorning * 86400 + 946728000);
    time_t evening = (time_t)(Jevening * 86400 + 946728000);
    *p_m = morning;
    *p_e = evening;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunMorningBlueHour(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, -6.0, &morning, &evening);
    return morning;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunMorningGoldenHour(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, -4, &morning, &evening);
    return morning;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunRise(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, -0.83, &morning, &evening);
    return morning;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunDay(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, 6, &morning, &evening);
    return morning;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunNoon(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, -0.83, &morning, &evening);
    return (time_t)(((uint32_t)evening + (uint32_t)morning) / 2);
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunEveningGoldenHour(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, 6, &morning, &evening);
    return evening;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunSet(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, -0.83, &morning, &evening);
    return evening;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunEveningBlueHour(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, -4, &morning, &evening);
    return evening;
}

//-------------------------------------------------------------------------------------------------

static time_t time_SunNight(time_t time)
{
    time_t morning = 0, evening = 0;
    time_SunCalculate(time, -6, &morning, &evening);
    return evening;
}

//-------------------------------------------------------------------------------------------------

static void time_TimePointsCalculate(time_t start_t, time_t ref_t, struct tm * p_dt, char * p_str)
{
    int point = 0;

    TIME_LOGI("Calculation of Time points : -----------");
    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        if (TIME_IDX_NIGHT == point)
        {
            gTimePoints[point].start     = time_SunNight(ref_t);
            gTimePoints[point].interval  = (start_t + TIME_FULL_DAY_DURATION);
            gTimePoints[point].interval -= gTimePoints[point].start;
        }
        else
        {
            switch (point)
            {
                case TIME_IDX_EVENING_BLUE_HOUR:
                    gTimePoints[point].start = time_SunEveningBlueHour(ref_t);
                    break;
                case TIME_IDX_SET:
                    gTimePoints[point].start = time_SunSet(ref_t);
                    break;
                case TIME_IDX_EVENING_GOLDEN_HOUR:
                    gTimePoints[point].start = time_SunEveningGoldenHour(ref_t);
                    break;
                case TIME_IDX_NOON:
                    gTimePoints[point].start = time_SunNoon(ref_t);
                    break;
                case TIME_IDX_DAY:
                    gTimePoints[point].start = time_SunDay(ref_t);
                    break;
                case TIME_IDX_RISE:
                    gTimePoints[point].start = time_SunRise(ref_t);
                    break;
                case TIME_IDX_MORNING_GOLDEN_HOUR:
                    gTimePoints[point].start = time_SunMorningGoldenHour(ref_t);
                    break;
                case TIME_IDX_MORNING_BLUE_HOUR:
                    gTimePoints[point].start = time_SunMorningBlueHour(ref_t);
                    break;
                case TIME_IDX_MIDNIGHT:
                    gTimePoints[point].start = start_t;
                    break;
            }
            gTimePoints[point].interval  = gTimePoints[point + 1].start;
            gTimePoints[point].interval -= gTimePoints[point].start;
        }

        localtime_r(&gTimePoints[point].start, p_dt);
        strftime(p_str, TIME_STR_MAX_LEN, "%c", p_dt);
        TIME_LOGI
        (
            "[%d] - Interval: %5lu s    : %12llu - %s",
            point,
            gTimePoints[point].interval,
            gTimePoints[point].start,
            p_str
        );
    }
}

//-------------------------------------------------------------------------------------------------

static void time_RgbPointsCalculate(void)
{
    uint32_t interval = 0;
    int      point    = 0;

    TIME_LOGI("Calculation of RGB points  : -----------");
    interval = 0;
    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        interval += gTimePoints[point].interval;
        if (NULL != gRgbPoints[point].transition)
        {
            gRgbPoints[point].interval = interval;
            interval = 0;
            TIME_LOGI("[%d] - Interval: %5lu s", point, gRgbPoints[point].interval);
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_UwPointsCalculate(void)
{
    uint32_t interval = 0;
    int      point    = 0;
    int32_t  value    = 0;

    TIME_LOGI("Calculation of UV/W points : -----------");
    interval = 0;
    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        interval += gTimePoints[point].interval;
        if (NULL != gUwPoints[point].transition)
        {
            gUwPoints[point].interval = interval;
            interval = 0;
        
            if (LED_CMD_UV_INDICATE_SINE == gUwPoints[point].transition->u_cmd)
            {
                value  = gTimePoints[TIME_IDX_EVENING_GOLDEN_HOUR].start;
                value -= gTimePoints[TIME_IDX_DAY].start;
                value -= TIME_SHORTEST_DAY_DURATION;
                value *= (TIME_UV_MAX - TIME_UV_MIN);
                value /= (TIME_LONGEST_DAY_DURATION - TIME_SHORTEST_DAY_DURATION);
                value += TIME_UV_MIN;
                gUwPoints[point].transition->u_max = value;
            }

            if (LED_CMD_W_INDICATE_SINE == gUwPoints[point].transition->w_cmd)
            {
                value  = gTimePoints[TIME_IDX_EVENING_GOLDEN_HOUR].start;
                value -= gTimePoints[TIME_IDX_DAY].start;
                value -= TIME_SHORTEST_DAY_DURATION;
                value *= (TIME_W_MAX - TIME_W_MIN);
                value /= (TIME_LONGEST_DAY_DURATION - TIME_SHORTEST_DAY_DURATION);
                value += TIME_W_MIN;
                gUwPoints[point].transition->w_max = value;
            }

            if ((0 == gUwPoints[point].transition->u_max) &&
                (0 == gUwPoints[point].transition->w_max))
            {
                gUwPoints[point].interval = 0;
            }

            TIME_LOGI
            (
                "[%d] - Interval: %5lu s - UV: %3d - W: %3d",
                point,
                gUwPoints[point].interval,
                gUwPoints[point].transition->u_max,
                gUwPoints[point].transition->w_max
            );
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_PointsCalculate(time_t t, struct tm * p_dt, char * p_str)
{
    char     string[TIME_STR_MAX_LEN] = {0};
    time_t   zero_time                = 0;
    time_t   tz_offset                = 0;
    time_t   current_time             = t;
    time_t   ref_utc_time             = t;

    TIME_LOGI("Current local time         : %12llu - %s", current_time, p_str);

    /* Determine the time zone offset */
    gmtime_r(&zero_time, p_dt);
    p_dt->tm_isdst = 1;
    tz_offset = mktime(p_dt);
    TIME_LOGI("Time zone offset           : %10lld s", tz_offset);

    localtime_r(&ref_utc_time, p_dt);
    p_dt->tm_sec   = 0;
    p_dt->tm_min   = 1;
    p_dt->tm_hour  = 12;
    p_dt->tm_isdst = 1;
    ref_utc_time   = (mktime(p_dt) - tz_offset);
    gmtime_r(&ref_utc_time, p_dt);
    strftime(string, sizeof(string), "%c", p_dt);
    TIME_LOGI("Calculation reference UTC  : %12llu - %s", ref_utc_time, string);

    localtime_r(&current_time, p_dt);
    p_dt->tm_sec   = 0;
    p_dt->tm_min   = 0;
    p_dt->tm_hour  = 0;
    p_dt->tm_isdst = 1;
    time_t start_day_time = mktime(p_dt);
    strftime(string, sizeof(string), "%c", p_dt);
    TIME_LOGI("Start of day time          : %12llu - %s", start_day_time, string);

    time_TimePointsCalculate(start_day_time, ref_utc_time, p_dt, string);

    time_RgbPointsCalculate();

    time_UwPointsCalculate();
}

//-------------------------------------------------------------------------------------------------

static void time_SunRgb(time_t curr_t, FW_BOOLEAN pre_tx, led_message_p p_rgb_msg)
{
    enum
    {
        TRANSITION_INTERVAL = 1200,
    };
    led_message_t pre_msg  = {0};
    led_color_t   color    = {0};
    int           point    = 0;
    uint32_t      interval = UINT32_MAX;
    uint32_t      duration = UINT32_MAX;

    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        /* Find the appropriate time point */
        if ((curr_t >= gTimePoints[point].start) && (NULL != gRgbPoints[point].transition))
        {
            interval = (gRgbPoints[point].interval * 1000);
            duration = ((curr_t - gTimePoints[point].start) * 1000);
            TIME_LOGI("[%d] - Interval/Duration RGB: %12lu - %lu", point, interval, duration);

            /* Prepare the indication message */
            p_rgb_msg->command         = gRgbPoints[point].transition->cmd;
            p_rgb_msg->src_color.dword = gRgbPoints[point].transition->src.dword;
            p_rgb_msg->dst_color.dword = gRgbPoints[point].transition->dst.dword;
            p_rgb_msg->interval        = interval;
            p_rgb_msg->duration        = duration;

            if (FW_TRUE == pre_tx)
            {
                LED_Task_DetermineColor(p_rgb_msg, &color);
                pre_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
                pre_msg.dst_color.dword = color.dword;
                pre_msg.interval        = TRANSITION_INTERVAL;
                LED_Task_SendMsg(&pre_msg);
            }
            /* Skip the rest of time points */
            break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_SunUw
(
    time_t curr_t,
    FW_BOOLEAN pre_tx,
    led_message_p p_u_msg,
    led_message_p p_w_msg
)
{
    enum
    {
        TRANSITION_INTERVAL = 1200,
    };
    led_message_t pre_msg  = {0};
    led_color_t   color    = {0};
    int           point    = 0;
    uint32_t      interval = UINT32_MAX;
    uint32_t      duration = UINT32_MAX;

    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        /* Find the appropriate time point */
        if ((curr_t >= gTimePoints[point].start) && (NULL != gUwPoints[point].transition))
        {
            interval = (gUwPoints[point].interval * 1000);
            duration = ((curr_t - gTimePoints[point].start) * 1000);
            TIME_LOGI("[%d] - Interval/Duration U/W: %12lu - %lu", point, interval, duration);

            /* Prepare the indication message */
            p_u_msg->command     = gUwPoints[point].transition->u_cmd;
            p_u_msg->src_color.a = 0;
            p_u_msg->dst_color.a = gUwPoints[point].transition->u_max;
            p_u_msg->interval    = interval;
            p_u_msg->duration    = duration;

            p_w_msg->command     = gUwPoints[point].transition->w_cmd;
            p_w_msg->src_color.a = 0;
            p_w_msg->dst_color.a = gUwPoints[point].transition->w_max;
            p_w_msg->interval    = interval;
            p_w_msg->duration    = duration;

            if (FW_TRUE == pre_tx)
            {
                LED_Task_DetermineColor(p_u_msg, &color);
                pre_msg.command     = LED_CMD_UV_INDICATE_BRIGHTNESS;
                pre_msg.dst_color.a = color.a;
                pre_msg.interval    = TRANSITION_INTERVAL;
                LED_Task_SendMsg(&pre_msg);

                LED_Task_DetermineColor(p_w_msg, &color);
                pre_msg.command     = LED_CMD_W_INDICATE_BRIGHTNESS;
                pre_msg.dst_color.a = color.a;
                LED_Task_SendMsg(&pre_msg);
            }
            /* Skip the rest of time points */
            break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_Sun(time_t t, struct tm * p_dt, char * p_str, FW_BOOLEAN pre_transition)
{
    enum
    {
        TRANSITION_TIMEOUT  = (pdMS_TO_TICKS(1300)),
    };
    led_message_t rgb_msg      = {0};
    led_message_t u_msg        = {0};
    led_message_t w_msg        = {0};
    time_t        current_time = t;

    TIME_LOGI("Current local time         : %12llu - %s", current_time, p_str);

    time_SunRgb(current_time, pre_transition, &rgb_msg);
    time_SunUw(current_time, pre_transition, &u_msg, &w_msg);

    if (FW_TRUE == pre_transition)
    {
        if ((0 != rgb_msg.command) || (0 != u_msg.command) || (0 != w_msg.command))
        {
            vTaskDelay(TRANSITION_TIMEOUT);
        }
    }

    if (0 != rgb_msg.command)
    {
        LED_Task_SendMsg(&rgb_msg);
    } 
    
    if (0 != u_msg.command)
    {
        LED_Task_SendMsg(&u_msg);
    } 
    
    if (0 != w_msg.command)
    {
        LED_Task_SendMsg(&w_msg);
    }
}

//-------------------------------------------------------------------------------------------------

static void time_SetAlarm(time_t t, struct tm * p_dt, char * p_str)
{
    char    string[TIME_STR_MAX_LEN] = {0};
    time_t  current_time             = t;
    int32_t point                    = 0;

    for (point = 0; point < TIME_IDX_MAX; point++)
    {
        if (current_time < gTimePoints[point].start)
        {
            gAlarm = gTimePoints[point].start;
            localtime_r(&gAlarm, p_dt);
            strftime(string, sizeof(string), "%c", p_dt);
            TIME_LOGI("Alarm set to next time     : %12llu - %s", gAlarm, string);
            break;
        }
    }
    if (TIME_IDX_MAX == point)
    {
        gAlarm = LONG_MAX;
        TIME_LOGI("Alarm cleared              : %12llu - %s", t, p_str);
    }
}

//-------------------------------------------------------------------------------------------------

static void time_ProcessMsg(time_message_t * p_msg, time_t t, struct tm * p_dt, char * p_str)
{
    gCommand = p_msg->command;

    if (TIME_CMD_SUN_ENABLE == gCommand)
    {
        time_PointsCalculate(t, p_dt, p_str);
        time_SetAlarm(t, p_dt, p_str);
        time_Sun(t, p_dt, p_str, FW_TRUE);
    }
}

//-------------------------------------------------------------------------------------------------

static void time_CheckForAlarms(time_t t, struct tm * p_dt, char * p_str)
{
    time_t current_time = t;

    if (TIME_CMD_SUN_ENABLE == gCommand)
    {
        /* If the alarm is not set */
        if (LONG_MAX == gAlarm)
        {
            /* Check for midnight */
            localtime_r(&current_time, p_dt);
            if ((0 == p_dt->tm_hour) && (0 == p_dt->tm_min) && (0 <= p_dt->tm_sec))
            {
                TIME_LOGI
                (
                    "Midnight detected!         : %12llu - %s",
                    current_time,
                    p_str
                );
                time_PointsCalculate(t, p_dt, p_str);
                time_SetAlarm(t, p_dt, p_str);
                time_Sun(t, p_dt, p_str, FW_TRUE);
            }
        }
        else
        {
            /* Check for alarm */
            if (current_time >= gAlarm)
            {
                TIME_LOGI
                (
                    "Alarm detected!            : %12llu - %s",
                    current_time,
                    p_str
                );
                time_SetAlarm(t, p_dt, p_str);
                time_Sun(t, p_dt, p_str, FW_TRUE);
            }
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void vTime_Task(void * pvParameters)
{
    enum
    {
        RETRY_COUNT = 20,
    };
    BaseType_t     status                   = pdFAIL;
    time_message_t msg                      = {0};
    time_t         now                      = 0;
    struct tm      datetime                 = {0};
    char           string[TIME_STR_MAX_LEN] = {0};
    uint32_t       retry                    = 0;
    static uint8_t sync_ok                  = FW_FALSE;

    /* Initialize the SNTP client which gets the time periodicaly */
    TIME_LOGI("Time Task Started...");
    TIME_LOGI("Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    /* Set the timezone */
    TIME_LOGI("Set timezone to - %s", gTZ);
    setenv("TZ", gTZ, 1);
    tzset();

    while (FW_TRUE)
    {
        /* Update the 'now' variable with current time */
        time(&now);
        localtime_r(&now, &datetime);

        /* If time is already in sync with the server */
        if ((2024 - 1900) <= datetime.tm_year)
        {
            strftime(string, sizeof(string), "%c", &datetime);

            if (FW_FALSE == sync_ok)
            {
                sync_ok = FW_TRUE;
                TIME_LOGI("Sync OK: Now - %llu - %s", now, string);
            }

            status = xQueueReceive(gTimeQueue, (void *)&msg, TIME_TASK_TICK_MS);
            if (pdTRUE == status)
            {
                time_ProcessMsg(&msg, now, &datetime, string);
            }
            time_CheckForAlarms(now, &datetime, string);
        }
        else
        {
            retry++;
            if (RETRY_COUNT == retry)
            {
                TIME_LOGE("Retry to sync the date/time");
                retry = 0;
                sntp_restart();
            }
            TIME_LOGE("The current date/time error");
            vTaskDelay(TIME_TASK_TICK_MS);
        }
    }
}

//-------------------------------------------------------------------------------------------------

void Time_Task_Init(void)
{
    time_message_t msg = {TIME_CMD_SUN_ENABLE};

    gTimeQueue = xQueueCreate(20, sizeof(time_message_t));

    /* SNTP service uses LwIP, large stack space should be allocated  */
    (void)xTaskCreatePinnedToCore(vTime_Task, "TIME", 4096, NULL, 3, NULL, CORE0);

    Time_Task_SendMsg(&msg);
}

//-------------------------------------------------------------------------------------------------

void Time_Task_SendMsg(time_message_t * p_msg)
{
    (void)xQueueSendToBack(gTimeQueue, (void *)p_msg, (TickType_t)0);
}

//-------------------------------------------------------------------------------------------------

FW_BOOLEAN Time_Task_IsInSunImitationMode(void)
{
    /* This call is not thread safe but this is acceptable */
    return (TIME_CMD_SUN_ENABLE == gCommand);
}

//-------------------------------------------------------------------------------------------------
//--- Tests ---------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void time_Test_Calculations(void)
{
    typedef struct
    {
        char *    name;
        time_t    time;
        uint32_t  offset;
        time_t (* getter)(time_t c);
    } transition_s_t;

    const transition_s_t tranzition[] =
    {
        /* Start             : 2024-02-29 00:00:00 - 1709157600.000000 -    0 minutes */
        /* Current           : 2024-02-29 16:38:46 - 1709217526.398973 -  998 minutes */
        /* MorningBlueHour   : 2024-02-29 06:37:40 - 1709181460.232813 -  397 minutes */
        /* MorningGoldenHour : 2024-02-29 06:50:09 - 1709182209.010621 -  410 minutes */
        /* Rise              : 2024-02-29 07:10:04 - 1709183404.703780 -  430 minutes */
        /* Day               : 2024-02-29 07:54:04 - 1709186044.106600 -  474 minutes */
        /* Noon              : 2024-02-29 12:37:43 - 1709203063.360857 -  757 minutes */
        /* EveningGoldenHour : 2024-02-29 17:21:22 - 1709220082.615113 - 1041 minutes */
        /* Set               : 2024-02-29 18:05:22 - 1709222722.017933 - 1085 minutes */
        /* EveningBlueHour   : 2024-02-29 18:25:17 - 1709223917.711092 - 1105 minutes */
        /* Night             : 2024-02-29 18:37:46 - 1709224666.488901 - 1117 minutes */
        {"Morning Blue Hour",   1709181460,  397, time_SunMorningBlueHour},
        {"Morning Golden Hour", 1709182209,  410, time_SunMorningGoldenHour},
        {"Rise",                1709183404,  430, time_SunRise},
        {"Day",                 1709186044,  474, time_SunDay},
        {"Noon",                1709203063,  757, time_SunNoon},
        {"Evening Golden Hour", 1709220082, 1041, time_SunEveningGoldenHour},
        {"Set",                 1709222722, 1085, time_SunSet},
        {"Evening Blue Hour",   1709223917, 1105, time_SunEveningBlueHour},
        {"Night",               1709224666, 1117, time_SunNight},
    };

    typedef struct
    {
        transition_s_t         start;
        transition_s_t         current;
        uint32_t               count;
        transition_s_t const * transition;
        uint32_t               trans_index;
        uint32_t               trans_duration;
    } sun_transitions_s_t;

    const sun_transitions_s_t sun =
    {
        /* Start   : 2024-02-29 00:00:00 - 1709157600.000000 -   0 minutes */
        /* Current : 2024-02-29 16:38:46 - 1709217526.398973 - 998 minutes */
        .start          = {"Start",   1709157600,   0, NULL},
        .current        = {"Current", 1709217526, 998, NULL},
        .count          = (sizeof(tranzition) / sizeof(transition_s_t)),
        .transition     = tranzition,
        .trans_index    = 4,
        .trans_duration = 241,
    };

    char                   string[28]      = {0};
    struct tm              dt              = {0};
    time_t                 zero_time       = 0;
    time_t                 tz_offset       = 0;
    time_t                 current_time    = sun.current.time;
    time_t                 ref_utc_time    = sun.current.time;
    uint32_t               test            = 0;
    uint32_t               offset_sec      = 0;
    uint32_t               offset_min      = 0;
    time_t                 calculated_time = 0;
    transition_s_t const * p_trans         = NULL;
    int                    trans_index     = sun.count;

    /* Set the time zone */
    setenv("TZ", gTZ, 1);
    tzset();

    /* Determine the time zone offset */
    gmtime_r(&zero_time, &dt);
    tz_offset = mktime(&dt);
    TIME_LOGI("Time zone offset           : %10lld s", tz_offset);

    localtime_r(&current_time, &dt);
    strftime(string, sizeof(string), "%c", &dt);
    TIME_LOGI("Current local time         : %12llu - %s", current_time, string);

    gmtime_r(&ref_utc_time, &dt);
    dt.tm_sec    = 0;
    dt.tm_min    = 1;
    dt.tm_hour   = 12;
    ref_utc_time = (mktime(&dt) - tz_offset);
    gmtime_r(&ref_utc_time, &dt);
    strftime(string, sizeof(string), "%c", &dt);
    TIME_LOGI("Calculation reference UTC  : %12llu - %s", ref_utc_time, string);

    localtime_r(&current_time, &dt);
    dt.tm_sec  = 0;
    dt.tm_min  = 0;
    dt.tm_hour = 0;
    time_t start_day_time = mktime(&dt);
    strftime(string, sizeof(string), "%c", &dt);
    TIME_LOGI("Start of day time          : %12llu - %s", start_day_time, string);

    if (sun.start.time == start_day_time)
    {
        TIME_LOGI("Start of day test          : %12llu - PASS", start_day_time);
    }
    else
    {
        TIME_LOGE("Start of day test          : %12llu - FAIL", start_day_time);
    }

    for (test = 0; test < sun.count; test++)
    {
        p_trans = &sun.transition[test];

        calculated_time = p_trans->getter(ref_utc_time);
        localtime_r(&calculated_time, &dt);
        strftime(string, sizeof(string), "%c", &dt);
        TIME_LOGI("- %-24s : %12llu - %s", p_trans->name, p_trans->time, string);

        offset_sec = (uint32_t)(calculated_time - start_day_time);
        offset_min = (offset_sec / 60);

        if ((p_trans->time == calculated_time) && (p_trans->offset == offset_min))
        {
            TIME_LOGI(" -- Time: %llu - Offset: %lu - PASS", calculated_time, offset_sec);
        }
        else
        {
            TIME_LOGE(" -- Time: %llu - Offset: %lu - FAIL", calculated_time, offset_sec);
        }

        /* Find the offset inside the transition time range */
        if ((trans_index == sun.count) && (sun.current.offset < p_trans->offset))
        {
            trans_index = (test - 1);
        }
    }

    if (trans_index == sun.count)
    {
        trans_index = (sun.count - 1);
    }
    offset_min = (sun.current.offset - sun.transition[trans_index].offset);
    if ((trans_index == sun.trans_index) && (offset_min == sun.trans_duration))
    {
        TIME_LOGI("Offset test (I:%d O:%4lu)   : - PASS", trans_index, offset_min);
    }
    else
    {
        TIME_LOGE("Offset test (I:%d O:%4lu)   : - FAIL", trans_index, offset_min);
    }
}

//-------------------------------------------------------------------------------------------------

static void time_Test_Alarm(void)
{
    time_t         now        = 0;
    struct tm      datetime   = {0};
    char           string[28] = {0};
    time_t         zero_time  = 0;
    time_t         tz_offset  = 0;
    led_message_t  led_msg    = {0};
    struct timeval tv         = {0};

    /* Set the timezone */
    TIME_LOGI("Set timezone to - %s", gTZ);
    setenv("TZ", gTZ, 1);
    tzset();

    /* Determine the time zone offset */
    gmtime_r(&zero_time, &datetime);
    datetime.tm_isdst = 1;
    tz_offset = mktime(&datetime);
    TIME_LOGI("Time zone offset           : %10lld s", tz_offset);

    /* Determine the date/time before midnight */
    datetime.tm_sec   = 46;
    datetime.tm_min   = 59;
    datetime.tm_hour  = 23;
    datetime.tm_mday  = 17;
    datetime.tm_mon   = 10 - 1;
    datetime.tm_year  = 2024 - 1900;
    datetime.tm_wday  = 0;
    datetime.tm_yday  = 0;
    datetime.tm_isdst = 1;
    now = mktime(&datetime);
    strftime(string, sizeof(string), "%c", &datetime);
    TIME_LOGI("Test time                  : %12lld - %s", now, string);

    /* Transition to DST color for 1100 ms */
    led_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst_color.r     = 255;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.dword = 0;
    led_msg.interval        = 1100;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Set the time */
    tv.tv_sec  = now;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    /* Wait till the Time task will be in sync */
    vTaskDelay(7 * TIME_TASK_TICK_MS);

    /* Enable the Sun emulation */
    time_message_t msg = {TIME_CMD_SUN_ENABLE};
    Time_Task_SendMsg(&msg);

    /* Wait till the Time task indicate the night and go through the midnight */
    vTaskDelay(15 * TIME_TASK_TICK_MS);

    /* Set the date/time before calculated alarm */
    now        = (gTimePoints[3].start - 10);
    tv.tv_sec  = now;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    /* Wait till the alarm happens */
    vTaskDelay(15 * TIME_TASK_TICK_MS);
}

//-------------------------------------------------------------------------------------------------

static void time_Test_DayNight(void)
{
    led_message_t     led_msg = {0};
    climate_message_t clt_msg = {0};
    uint8_t           p       = 0;

    /* Start             -    0 minutes */
    /* MorningBlueHour   -  429 minutes */
    /* MorningGoldenHour -  442 minutes */
    /* Rise              -  461 minutes */
    /* Day               -  505 minutes */
    /* Noon              -  791 minutes */
    /* EveningGoldenHour - 1076 minutes */
    /* Set               - 1120 minutes */
    /* EveningBlueHour   - 1140 minutes */
    /* Night             - 1153 minutes */

    time_point_t time_points[] =
    {
        [TIME_IDX_MIDNIGHT]            = {   0,  429},
        [TIME_IDX_MORNING_BLUE_HOUR]   = { 429,   13},
        [TIME_IDX_MORNING_GOLDEN_HOUR] = { 442,   19},
        [TIME_IDX_RISE]                = { 461,   44},
        [TIME_IDX_DAY]                 = { 505,  286},
        [TIME_IDX_NOON]                = { 791,  285},
        [TIME_IDX_EVENING_GOLDEN_HOUR] = {1076,   44},
        [TIME_IDX_SET]                 = {1120,   20},
        [TIME_IDX_EVENING_BLUE_HOUR]   = {1140,   13},
        [TIME_IDX_NIGHT]               = {1153,  287},
    };

    /* Start             - RGB(0,0,32)    - RGB(0,0,44)      - Smooth      -  429 */
    /* MorningBlueHour   - RGB(0,0,44)    - RGB(64,0,56)     - Rainbow CW  -   13 */
    /* MorningGoldenHour - RGB(64,0,56)   - RGB(220,220,0)   - Rainbow CW  -   19 */
    /* Rise              -                -                  -             -  +44 */
    /* Day               - RGB(220,220,0) - RGB(255,255,255) - Sine        -  286 */
    /* Noon              -                -                  -             - +286 */
    /* EveningGoldenHour - RGB(220,220,0) - RGB( 64,0,56)    - Rainbow CCW -   44 */
    /* Set               -                -                  -             -  +20 */
    /* EveningBlueHour   - RGB(64,0,56)   - RGB(0,0,44)      - Rainbow CCW -   13 */
    /* Night             - RGB(0,0,44)    - RGB(0,0,32)      - None        -  287 */

    rgb_tx_t rgbStart             = {RGBA(0,0,32,1),    RGBA(0,0,44,1),    LED_CMD_RGB_INDICATE_COLOR};
    rgb_tx_t rgbMorningBlueHour   = {RGBA(0,0,44,0),    RGBA(64,0,56,1),   LED_CMD_RGB_INDICATE_RAINBOW};
    rgb_tx_t rgbMorningGoldenHour = {RGBA(64,0,56,0),   RGBA(220,220,0,1), LED_CMD_RGB_INDICATE_RAINBOW};
    rgb_tx_t rgbDay               = {RGBA(220,220,0,1), RGBA(10,10,10,1),  LED_CMD_RGB_INDICATE_SINE};
    rgb_tx_t rgbEveningGoldenHour = {RGBA(220,220,0,1), RGBA(64,0,56,0),   LED_CMD_RGB_INDICATE_RAINBOW};
    rgb_tx_t rgbEveningBlueHour   = {RGBA(64,0,56,1),   RGBA(0,0,44,0),    LED_CMD_RGB_INDICATE_RAINBOW};
    rgb_tx_t rgbNight             = {RGBA(0,0,44,1),    RGBA(0,0,32,1),    LED_CMD_RGB_INDICATE_COLOR};

    rgb_point_t rgb_points[] =
    {
        [TIME_IDX_MIDNIGHT]            = {&rgbStart,             429},
        [TIME_IDX_MORNING_BLUE_HOUR]   = {&rgbMorningBlueHour,    13},
        [TIME_IDX_MORNING_GOLDEN_HOUR] = {&rgbMorningGoldenHour,  63},
        [TIME_IDX_RISE]                = {NULL,                    0},
        [TIME_IDX_DAY]                 = {&rgbDay,               572},
        [TIME_IDX_NOON]                = {NULL,                    0},
        [TIME_IDX_EVENING_GOLDEN_HOUR] = {&rgbEveningGoldenHour,  64},
        [TIME_IDX_SET]                 = {NULL,                    0},
        [TIME_IDX_EVENING_BLUE_HOUR]   = {&rgbEveningBlueHour,    13},
        [TIME_IDX_NIGHT]               = {&rgbNight,             287},
    };

    /*                        ---------                         <---- Longest Day (UV/W max)    */
    /*                    ----         ----                                                     */
    /*                 ---                 ---                                                  */
    /*              ---                       ---                                               */
    /*            --                             --                                             */
    /*          --                                 --                                           */
    /*       ---              ---------              ---        <---- Shortest Day (UV/W min)   */
    /*    ---     ------------         ------------     ---                                     */
    /* -----------                                 -----------                                  */
    /* Day ------------------------------------------------> Evening Golden Hour                */
    /*                                                                                          */
    /* duration = (evening_golden_hour - day);                                                  */
    /* percent = (duration-SHORTEST_DAY_DURATION)/(LONGEST_DAY_DURATION-SHORTEST_DAY_DURATION); */
    /* UV = (UV_MIN + (UV_MAX - UV_MIN) * percent);                                             */
    /* W = (W_MIN + (W_MAX - W_MIN) * percent);                                                 */
    /*                                                                                          */
    /* Start             - UW(0,0)     - Smooth -  429 */
    /* MorningBlueHour   -             -        -  +13 */
    /* MorningGoldenHour -             -        -  +19 */
    /* Rise              -             -        -  +44 */
    /* Day               - UW(110,180) - Sine   -  286 */
    /* Noon              -             -        - +286 */
    /* EveningGoldenHour - UW(0,0)     - Smooth -   44 */
    /* Set               -             -        -  +20 */
    /* EveningBlueHour   -             -        -  +13 */
    /* Night             -             -        - +287 */

    uw_tx_t uwStart             = {LED_CMD_UV_INDICATE_BRIGHTNESS, 0, LED_CMD_W_INDICATE_BRIGHTNESS, 0};
    uw_tx_t uwDay               = {LED_CMD_UV_INDICATE_SINE, 110, LED_CMD_W_INDICATE_SINE, 180};
    uw_tx_t uwEveningGoldenHour = {LED_CMD_UV_INDICATE_BRIGHTNESS, 0, LED_CMD_W_INDICATE_BRIGHTNESS, 0};

    uw_point_t uw_points[] =
    {
        [TIME_IDX_MIDNIGHT]            = {&uwStart,             0},
        [TIME_IDX_MORNING_BLUE_HOUR]   = {NULL,                 0},
        [TIME_IDX_MORNING_GOLDEN_HOUR] = {NULL,                 0},
        [TIME_IDX_RISE]                = {NULL,                 0},
        [TIME_IDX_DAY]                 = {&uwDay,             572},
        [TIME_IDX_NOON]                = {NULL,                 0},
        [TIME_IDX_EVENING_GOLDEN_HOUR] = {&uwEveningGoldenHour, 0},
        [TIME_IDX_SET]                 = {NULL,                 0},
        [TIME_IDX_EVENING_BLUE_HOUR]   = {NULL,                 0},
        [TIME_IDX_NIGHT]               = {NULL,                 0},
    };

    fan_tx_t fanMorning = {CLIMATE_CMD_FAN, FAN_SPEED_LOW,    true, 33, 10};
    fan_tx_t fanOff     = {CLIMATE_CMD_FAN, FAN_SPEED_NONE,  false, 0,  0};
    fan_tx_t fanDay     = {CLIMATE_CMD_FAN, FAN_SPEED_MEDIUM, true, 33, 11};
    fan_tx_t fanEvening = {CLIMATE_CMD_FAN, FAN_SPEED_LOW,    true, 36, 10};

    fan_tx_p fan_points[] =
    {
        [TIME_IDX_MIDNIGHT]            = &fanMorning,
        [TIME_IDX_MORNING_BLUE_HOUR]   = &fanOff,
        [TIME_IDX_MORNING_GOLDEN_HOUR] = NULL,
        [TIME_IDX_RISE]                = NULL,
        [TIME_IDX_DAY]                 = &fanDay,
        [TIME_IDX_NOON]                = NULL,
        [TIME_IDX_EVENING_GOLDEN_HOUR] = &fanOff,
        [TIME_IDX_SET]                 = NULL,
        [TIME_IDX_EVENING_BLUE_HOUR]   = NULL,
        [TIME_IDX_NIGHT]               = &fanEvening,
    };

    humidifier_tx_t humidifierOn  = {CLIMATE_CMD_HUMIDIFY,  true, false, 35, 20};
    humidifier_tx_t humidifierOff = {CLIMATE_CMD_HUMIDIFY, false, false,  0,  0};

    humidifier_tx_p humidifier_points[] =
    {
        [TIME_IDX_MIDNIGHT]            = &humidifierOff,
        [TIME_IDX_MORNING_BLUE_HOUR]   = &humidifierOn,
        [TIME_IDX_MORNING_GOLDEN_HOUR] = NULL,
        [TIME_IDX_RISE]                = NULL,
        [TIME_IDX_DAY]                 = NULL,
        [TIME_IDX_NOON]                = NULL,
        [TIME_IDX_EVENING_GOLDEN_HOUR] = NULL,
        [TIME_IDX_SET]                 = NULL,
        [TIME_IDX_EVENING_BLUE_HOUR]   = &humidifierOn,
        [TIME_IDX_NIGHT]               = NULL,
    };

    for (p = 0; p < TIME_IDX_MAX; p++)
    {
        uint32_t timeout = (time_points[p].interval) * 100;

        if (NULL != rgb_points[p].transition)
        {
            memset(&led_msg, 0, sizeof(led_msg));
            led_msg.command         = rgb_points[p].transition->cmd;
            led_msg.src_color.dword = rgb_points[p].transition->src.dword;
            led_msg.dst_color.dword = rgb_points[p].transition->dst.dword;
            led_msg.interval        = (rgb_points[p].interval * 100);
            led_msg.duration        = 0;
            LED_Task_SendMsg(&led_msg);
        }

        if (NULL != uw_points[p].transition)
        {
            memset(&led_msg, 0, sizeof(led_msg));
            led_msg.src_color.a = 0;
            led_msg.interval    = (uw_points[p].interval * 100);
            led_msg.duration    = 0;

            led_msg.command     = uw_points[p].transition->u_cmd;
            led_msg.dst_color.a = uw_points[p].transition->u_max;
            LED_Task_SendMsg(&led_msg);

            led_msg.command     = uw_points[p].transition->w_cmd;
            led_msg.dst_color.a = uw_points[p].transition->w_max;
            LED_Task_SendMsg(&led_msg);
        }

        if (NULL != fan_points[p])
        {
            memset(&clt_msg, 0, sizeof(clt_msg));
            clt_msg.command  = fan_points[p]->cmd;
            clt_msg.speed    = fan_points[p]->speed;
            clt_msg.interval = (fan_points[p]->interval * 100);
            clt_msg.duration = (fan_points[p]->duration * 100);
            clt_msg.repeat   = fan_points[p]->repeat;
            Climate_Task_SendMsg(&clt_msg);
        }

        if (NULL != humidifier_points[p])
        {
            memset(&clt_msg, 0, sizeof(clt_msg));
            clt_msg.command  = humidifier_points[p]->cmd;
            clt_msg.on       = humidifier_points[p]->on;
            clt_msg.interval = (humidifier_points[p]->interval * 100);
            clt_msg.duration = (humidifier_points[p]->duration * 100);
            clt_msg.repeat   = humidifier_points[p]->repeat;
            Climate_Task_SendMsg(&clt_msg);
        }

        vTaskDelay(pdMS_TO_TICKS(timeout));
    }
}

//-------------------------------------------------------------------------------------------------

void Time_Task_Test(void)
{
    time_Test_Calculations();
    time_Test_Alarm();
    time_Test_DayNight();
}

//-------------------------------------------------------------------------------------------------
