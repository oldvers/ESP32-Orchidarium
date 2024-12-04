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
#    define TIME_LOGT(t,s)  time_LogTime(t, s)
#else
#    define TIME_LOGI(...)
#    define TIME_LOGE(...)
#    define TIME_LOGW(...)
#    define TIME_LOGT(...)
#endif

//-------------------------------------------------------------------------------------------------

enum
{
    /* Calculated for Dec 21 2024 14:00:00 (seconds) */
    TIME_SHORTEST_DAY_DURATION_S   = 22222,
    /* Calculated for Jun 21 2024 14:00:00 (seconds) */
    TIME_LONGEST_DAY_DURATION_S    = 52666,
    TIME_STR_MAX_LEN               = 28,
    /* Whole 24 hours day duration (seconds) */
    TIME_FULL_DAY_DURATION_S       = (24 * 60 * 60),
    TIME_UV_BRIGHTNESS_MIN         = 25,
    TIME_UV_BRIGHTNESS_MAX         = 150,
    TIME_W_BRIGHTNESS_MIN          = 170,
    TIME_W_BRIGHTNESS_MAX          = 230,
    TIME_FAN_MORNING_PERCENT       = 4,
    TIME_FAN_EVENING_PERCENT       = 5,
    TIME_FAN_DAY_PERCENT_MIN       = 15,
    TIME_FAN_DAY_PERCENT_MAX       = 26,
    TIME_FAN_SLOT_DURATION_S       = (35 * 60),
    TIME_FAN_MARGIN_DURATION_S     = (5 * 60),
    TIME_HUMIDIFIER_MIN_DURATION_S = 23,
    TIME_HUMIDIFIER_MAX_DURATION_S = 60,
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
} rgb_tx_t, * rgb_tx_p;

typedef struct
{
    rgb_tx_p transition;
    uint32_t interval;
} rgb_point_t;

typedef struct
{
    led_command_t u_cmd;
    uint8_t       u_max;
    led_command_t w_cmd;
    uint8_t       w_max;
} uw_tx_t, * uw_tx_p;

typedef struct
{
    uw_tx_p  transition;
    uint32_t interval;
} uw_point_t;

typedef struct
{
    climate_command_t cmd;
    fan_speed_t       speed;
    bool              repeat;
    uint8_t           percent;
} fan_tx_t, * fan_tx_p;

typedef struct
{
    fan_tx_p transition;
    uint32_t interval;
} fan_point_t;

typedef struct
{
    climate_command_t cmd;
    bool              on;
    bool              repeat;
    uint8_t           duration;
} humidifier_tx_t, * humidifier_tx_p;

typedef struct
{
    humidifier_tx_p transition;
    uint32_t        interval;
} humidifier_point_t;

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

#if (1 == TIME_LOG)
static const char * const gcPointDescription[] =
{
    [TIME_IDX_MIDNIGHT]            = "Day Start",
    [TIME_IDX_MORNING_BLUE_HOUR]   = "Morning Blue Hour",
    [TIME_IDX_MORNING_GOLDEN_HOUR] = "Morning Golden Hour",
    [TIME_IDX_RISE]                = "Sun Rise",
    [TIME_IDX_DAY]                 = "Day",
    [TIME_IDX_NOON]                = "Noon",
    [TIME_IDX_EVENING_GOLDEN_HOUR] = "Evening Golden Hour",
    [TIME_IDX_SET]                 = "Sun Set",
    [TIME_IDX_EVENING_BLUE_HOUR]   = "Evening Blue Hour",
    [TIME_IDX_NIGHT]               = "Night",
};
#endif

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


static fan_tx_t gFanMorning = {CLIMATE_CMD_FAN, FAN_SPEED_LOW,    true, 0};
static fan_tx_t gFanOff     = {CLIMATE_CMD_FAN, FAN_SPEED_NONE,  false, 0};
static fan_tx_t gFanDay     = {CLIMATE_CMD_FAN, FAN_SPEED_MEDIUM, true, 0};
static fan_tx_t gFanEvening = {CLIMATE_CMD_FAN, FAN_SPEED_LOW,    true, 0};

static fan_point_t gFanPoints[] =
{
    [TIME_IDX_MIDNIGHT]            = {&gFanMorning, 0},
    [TIME_IDX_MORNING_BLUE_HOUR]   = {&gFanOff,     0},
    [TIME_IDX_MORNING_GOLDEN_HOUR] = {NULL,         0},
    [TIME_IDX_RISE]                = {NULL,         0},
    [TIME_IDX_DAY]                 = {&gFanDay,     0},
    [TIME_IDX_NOON]                = {NULL,         0},
    [TIME_IDX_EVENING_GOLDEN_HOUR] = {&gFanOff,     0},
    [TIME_IDX_SET]                 = {NULL,         0},
    [TIME_IDX_EVENING_BLUE_HOUR]   = {NULL,         0},
    [TIME_IDX_NIGHT]               = {&gFanEvening, 0},
};

static humidifier_tx_t gHumidifierOn  = {CLIMATE_CMD_HUMIDIFY,  true, false, 0};
static humidifier_tx_t gHumidifierOff = {CLIMATE_CMD_HUMIDIFY, false, false, 0};

static humidifier_point_t gHumidifierPoints[] =
{
    [TIME_IDX_MIDNIGHT]            = {&gHumidifierOff, 0},
    [TIME_IDX_MORNING_BLUE_HOUR]   = {&gHumidifierOn,  0},
    [TIME_IDX_MORNING_GOLDEN_HOUR] = {&gHumidifierOff, 0},
    [TIME_IDX_RISE]                = {NULL,            0},
    [TIME_IDX_DAY]                 = {NULL,            0},
    [TIME_IDX_NOON]                = {NULL,            0},
    [TIME_IDX_EVENING_GOLDEN_HOUR] = {NULL,            0},
    [TIME_IDX_SET]                 = {NULL,            0},
    [TIME_IDX_EVENING_BLUE_HOUR]   = {&gHumidifierOn,  0},
    [TIME_IDX_NIGHT]               = {&gHumidifierOff, 0},
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

static void time_GetTimeRefs(time_t t, time_t * p_start_t, time_t * p_ref_t)
{
    char      string[TIME_STR_MAX_LEN] = {0};
    struct tm dt                       = {0};
    struct tm tz                       = {0};
    time_t    zero_time                = 0;
    time_t    tz_offset                = 0;

    /* Determine the local time */
    localtime_r(&t, &dt);
    strftime(string, sizeof(string), "%c", &dt);
    TIME_LOGI("%-26s : %10llu : %s", "Current local time", t, string);
    TIME_LOGI("DST (Daylight Saving Time) : %10d", dt.tm_isdst);

    /* Determine the time zone offset */
    gmtime_r(&zero_time, &tz);
    tz.tm_isdst  = dt.tm_isdst;
    tz_offset = mktime(&tz);
    TIME_LOGI("%-26s : %10lld", "Time zone offset", tz_offset);

    /* Determine the reference UTC time for calculations */
    dt.tm_sec  = 0;
    dt.tm_min  = 1;
    dt.tm_hour = 12;
    *p_ref_t   = (mktime(&dt) - tz_offset);
    gmtime_r(p_ref_t, &dt);
    strftime(string, sizeof(string), "%c", &dt);
    TIME_LOGI("%-26s : %10llu : %s", "Calculated reference UTC", *p_ref_t, string);

    /* Determine the start of day time */
    localtime_r(&t, &dt);
    dt.tm_sec      = 0;
    dt.tm_min      = 0;
    dt.tm_hour     = 0;
    *p_start_t = mktime(&dt);
    strftime(string, sizeof(string), "%c", &dt);
    TIME_LOGI("%-26s : %10llu : %s", "Start of day time", *p_start_t, string);
}

//-------------------------------------------------------------------------------------------------

static void time_TimePointsCalculate(time_t start_t, time_t ref_t)
{
    char      string[TIME_STR_MAX_LEN] = {0};
    struct tm dt                       = {0};
    int       point                    = 0;

    TIME_LOGI("Calculation of Time points : -------------------------");
    TIME_LOGI("-------------------------- : Start      : Itrvl : Date");
    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        if (TIME_IDX_NIGHT == point)
        {
            gTimePoints[point].start     = time_SunNight(ref_t);
            gTimePoints[point].interval  = (start_t + TIME_FULL_DAY_DURATION_S);
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

        localtime_r(&gTimePoints[point].start, &dt);
        strftime(string, sizeof(string), "%c", &dt);
        TIME_LOGI
        (
            "[%d] %-23s: %10llu : %5lu : %s",
            point,
            gcPointDescription[point],
            gTimePoints[point].start,
            gTimePoints[point].interval,
            string
        );
    }
}

//-------------------------------------------------------------------------------------------------

static void time_RgbPointsCalculate(void)
{
    uint32_t interval = 0;
    int      point    = 0;

    TIME_LOGI("Calculation of RGB points  : ------------");
    TIME_LOGI("-------------------------- : - Interval");
    interval = 0;
    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        interval += gTimePoints[point].interval;
        if (NULL != gRgbPoints[point].transition)
        {
            gRgbPoints[point].interval = interval;
            interval = 0;
            TIME_LOGI
            (
                "[%d] %-23s: %10lu",
                point,
                gcPointDescription[point],
                gRgbPoints[point].interval
            );
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_UwPointsCalculate(void)
{
    uint32_t interval = 0;
    int      point    = 0;
    int32_t  value    = 0;

    TIME_LOGI("Calculation of UV/W points : ----------------------");
    TIME_LOGI("-------------------------- : - Interval :  UV :   W");
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
                value -= TIME_SHORTEST_DAY_DURATION_S;
                value *= (TIME_UV_BRIGHTNESS_MAX - TIME_UV_BRIGHTNESS_MIN);
                value /= (TIME_LONGEST_DAY_DURATION_S - TIME_SHORTEST_DAY_DURATION_S);
                value += TIME_UV_BRIGHTNESS_MIN;
                gUwPoints[point].transition->u_max = value;
            }

            if (LED_CMD_W_INDICATE_SINE == gUwPoints[point].transition->w_cmd)
            {
                value  = gTimePoints[TIME_IDX_EVENING_GOLDEN_HOUR].start;
                value -= gTimePoints[TIME_IDX_DAY].start;
                value -= TIME_SHORTEST_DAY_DURATION_S;
                value *= (TIME_W_BRIGHTNESS_MAX - TIME_W_BRIGHTNESS_MIN);
                value /= (TIME_LONGEST_DAY_DURATION_S - TIME_SHORTEST_DAY_DURATION_S);
                value += TIME_W_BRIGHTNESS_MIN;
                gUwPoints[point].transition->w_max = value;
            }

            TIME_LOGI
            (
                "[%d] %-23s: %10lu : %3d : %3d",
                point,
                gcPointDescription[point],
                gUwPoints[point].interval,
                gUwPoints[point].transition->u_max,
                gUwPoints[point].transition->w_max
            );
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_FanPointsCalculate(void)
{
    uint32_t interval = 0;
    int      point    = 0;
    uint32_t value    = 0;

    TIME_LOGI("Calculation of FAN points  : ----------------------");
    TIME_LOGI("-------------------------- : - Interval :   S :   %%");

    /* Set/calculate the FAN in time percents */
    gFanMorning.percent = TIME_FAN_MORNING_PERCENT;
    gFanEvening.percent = TIME_FAN_EVENING_PERCENT;
    value  = gTimePoints[TIME_IDX_EVENING_GOLDEN_HOUR].start;
    value -= gTimePoints[TIME_IDX_DAY].start;
    value -= TIME_SHORTEST_DAY_DURATION_S;
    value *= (TIME_FAN_DAY_PERCENT_MAX - TIME_FAN_DAY_PERCENT_MIN);
    value /= (TIME_LONGEST_DAY_DURATION_S - TIME_SHORTEST_DAY_DURATION_S);
    value += TIME_FAN_DAY_PERCENT_MIN;
    gFanDay.percent = value;

    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        interval += gTimePoints[point].interval;
        if (NULL != gFanPoints[point].transition)
        {
            gFanPoints[point].interval = interval;
            interval = 0;
            TIME_LOGI
            (
                "[%d] %-23s: %10lu : %3d : %3d",
                point,
                gcPointDescription[point],
                gFanPoints[point].interval,
                gFanPoints[point].transition->speed,
                gFanPoints[point].transition->percent
            );
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_HumidifierPointsCalculate(void)
{
    uint32_t interval = 0;
    int      point    = 0;
    uint32_t value    = 0;

    TIME_LOGI("Calculation of HUM points  : ----------------------");
    TIME_LOGI("-------------------------- : - Interval :  On : Dur");

    /* Calculate the Humidification duration */
    value  = gTimePoints[TIME_IDX_EVENING_GOLDEN_HOUR].start;
    value -= gTimePoints[TIME_IDX_DAY].start;
    value -= TIME_SHORTEST_DAY_DURATION_S;
    value *= (TIME_HUMIDIFIER_MAX_DURATION_S - TIME_HUMIDIFIER_MIN_DURATION_S);
    value /= (TIME_LONGEST_DAY_DURATION_S - TIME_SHORTEST_DAY_DURATION_S);
    value += TIME_HUMIDIFIER_MIN_DURATION_S;
    gHumidifierOn.duration = value;

    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        interval += gTimePoints[point].interval;
        if (NULL != gHumidifierPoints[point].transition)
        {
            gHumidifierPoints[point].interval = interval;
            interval = 0;
            TIME_LOGI
            (
                "[%d] %-23s: %10lu : %3d : %3d",
                point,
                gcPointDescription[point],
                gHumidifierPoints[point].interval,
                gHumidifierPoints[point].transition->on,
                gHumidifierPoints[point].transition->duration
            );
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_PointsCalculate(time_t t)
{
    time_t ref_utc_time   = t;
    time_t start_day_time = 0;

    time_GetTimeRefs(t, &start_day_time, &ref_utc_time);

    time_TimePointsCalculate(start_day_time, ref_utc_time);

    time_RgbPointsCalculate();

    time_UwPointsCalculate();

    time_FanPointsCalculate();

    time_HumidifierPointsCalculate();
}

//-------------------------------------------------------------------------------------------------

static void time_SunRgb(time_t t, FW_BOOLEAN pre_tx, led_message_p p_rgb_msg)
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
        if ((t >= gTimePoints[point].start) && (NULL != gRgbPoints[point].transition))
        {
            interval = (gRgbPoints[point].interval * 1000);
            duration = ((t - gTimePoints[point].start) * 1000);
            TIME_LOGI
            (
                "[%d] %-23s: %10lu : %8lu : RGB",
                point,
                gcPointDescription[point],
                interval,
                duration
            );

            /* Prepare the indication message */
            p_rgb_msg->command         = gRgbPoints[point].transition->cmd;
            p_rgb_msg->src.color.dword = gRgbPoints[point].transition->src.dword;
            p_rgb_msg->dst.color.dword = gRgbPoints[point].transition->dst.dword;
            p_rgb_msg->interval        = interval;
            p_rgb_msg->duration        = duration;

            if (FW_TRUE == pre_tx)
            {
                LED_Task_DetermineColor(p_rgb_msg, &color);
                pre_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
                pre_msg.dst.color.dword = color.dword;
                pre_msg.interval        = TRANSITION_INTERVAL;
                LED_Task_SendMsg(&pre_msg);
            }
            /* Skip the rest of time points */
            break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_SunUw(time_t t, FW_BOOLEAN pre_tx, led_message_p p_u_msg, led_message_p p_w_msg)
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
        if ((t >= gTimePoints[point].start) && (NULL != gUwPoints[point].transition))
        {
            interval = (gUwPoints[point].interval * 1000);
            duration = ((t - gTimePoints[point].start) * 1000);
            TIME_LOGI
            (
                "[%d] %-23s: %10lu : %8lu : U/W - %d/%d",
                point,
                gcPointDescription[point],
                interval,
                duration,
                gUwPoints[point].transition->u_max,
                gUwPoints[point].transition->w_max
            );

            /* Prepare the indication message */
            p_u_msg->command          = gUwPoints[point].transition->u_cmd;
            p_u_msg->src.brightness.v = 0;
            p_u_msg->src.brightness.a = 1;
            p_u_msg->dst.brightness.v = gUwPoints[point].transition->u_max;
            p_u_msg->dst.brightness.a = 1;
            p_u_msg->interval         = interval;
            p_u_msg->duration         = duration;

            p_w_msg->command          = gUwPoints[point].transition->w_cmd;
            p_w_msg->src.brightness.v = 0;
            p_u_msg->src.brightness.a = 1;
            p_w_msg->dst.brightness.v = gUwPoints[point].transition->w_max;
            p_u_msg->dst.brightness.a = 1;
            p_w_msg->interval         = interval;
            p_w_msg->duration         = duration;

            if (FW_TRUE == pre_tx)
            {
                LED_Task_DetermineColor(p_u_msg, &color);
                pre_msg.command          = LED_CMD_UV_INDICATE_BRIGHTNESS;
                pre_msg.dst.brightness.v = color.a;
                pre_msg.dst.brightness.a = 1;
                pre_msg.interval         = TRANSITION_INTERVAL;
                LED_Task_SendMsg(&pre_msg);

                LED_Task_DetermineColor(p_w_msg, &color);
                pre_msg.command          = LED_CMD_W_INDICATE_BRIGHTNESS;
                pre_msg.dst.brightness.v = color.a;
                pre_msg.dst.brightness.a = 1;
                LED_Task_SendMsg(&pre_msg);
            }
            /* Skip the rest of time points */
            break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

#if (1 == TIME_LOG)
static void time_LogTime(time_t t, char * p_str)
{
    struct tm dt                    = {0};
    char      str[TIME_STR_MAX_LEN] = {0};

    localtime_r(&t, &dt);
    strftime(str, sizeof(str), "%c", &dt);
    TIME_LOGI("%-26s : %10llu : %s", p_str, t, str);
}
#endif

//-------------------------------------------------------------------------------------------------

static void time_Sun(time_t t, FW_BOOLEAN pre_transition)
{
    enum
    {
        TRANSITION_TIMEOUT = (pdMS_TO_TICKS(1300)),
    };
    led_message_t rgb_msg = {0};
    led_message_t u_msg   = {0};
    led_message_t w_msg   = {0};

    time_SunRgb(t, pre_transition, &rgb_msg);
    time_SunUw(t, pre_transition, &u_msg, &w_msg);

    if (FW_TRUE == pre_transition)
    {
        if ((0 != rgb_msg.command) || (0 != u_msg.command) || (0 != w_msg.command))
        {
            vTaskDelay(TRANSITION_TIMEOUT);
        }
    }

    if (LED_CMD_EMPTY != rgb_msg.command)
    {
        LED_Task_SendMsg(&rgb_msg);
    } 
    
    if (LED_CMD_EMPTY != u_msg.command)
    {
        LED_Task_SendMsg(&u_msg);
    } 
    
    if (LED_CMD_EMPTY != w_msg.command)
    {
        LED_Task_SendMsg(&w_msg);
    }
}

//-------------------------------------------------------------------------------------------------

static void time_ClimateFan(time_t t, climate_message_p p_msg)
{
    uint8_t  count    = 0;
    int      point    = 0;
    uint32_t interval = UINT32_MAX;
    uint32_t duration = UINT32_MAX;

    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        /* Find the appropriate time point */
        if ((t >= gTimePoints[point].start) && (NULL != gFanPoints[point].transition))
        {
            /* Prepare the FAN message */
            p_msg->command = gFanPoints[point].transition->cmd;
            p_msg->speed   = gFanPoints[point].transition->speed;
            p_msg->repeat  = gFanPoints[point].transition->repeat;

            if (FAN_SPEED_NONE < gFanPoints[point].transition->speed)
            {
                /* The remaining time to the next time point */
                interval = (gTimePoints[point].start + gFanPoints[point].interval - t);
                /* Determine the count of remaining time slots */
                count = (interval / TIME_FAN_SLOT_DURATION_S);
                if (0 < count)
                {
                    /* Determine the FAN time interval */
                    interval = (1000 * ((interval + TIME_FAN_MARGIN_DURATION_S) / count));
                    /* Determine the FAN time duration */
                    duration = ((gFanPoints[point].transition->percent * interval) / 100);
                    /* Setup the message */
                    p_msg->interval = interval;
                    p_msg->duration = duration;
                }
                else
                {
                    p_msg->speed = FAN_SPEED_NONE;
                }
            }
            TIME_LOGI
            (
                "[%d] %-23s: %10lu : %8lu : FAN - S: %d",
                point,
                gcPointDescription[point],
                p_msg->interval,
                p_msg->duration,
                p_msg->speed
            );
            /* Skip the rest of time points */
            break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_ClimateHumidifier(time_t t, climate_message_p p_msg)
{
    enum
    {
        HUMIDIFICATION_TIMEOUT_S = (8 * 60),
    };
    uint32_t interval = UINT32_MAX;
    int      point    = 0;

    for (point = (TIME_IDX_MAX - 1); point >= 0; point--)
    {
        /* Find the appropriate time point */
        if ((t >= gTimePoints[point].start) && (NULL != gHumidifierPoints[point].transition))
        {
            /* The remaining time to the next time point */
            interval = (gTimePoints[point].start + gHumidifierPoints[point].interval - t);
            /* Prepare the Humidifier message */
            p_msg->command = gHumidifierPoints[point].transition->cmd;
            /* There should be at least a few minutes after the humidification */
            if ((true == gHumidifierPoints[point].transition->on) &&
                (HUMIDIFICATION_TIMEOUT_S < interval))
            {
                p_msg->on       = gHumidifierPoints[point].transition->on;
                p_msg->repeat   = gHumidifierPoints[point].transition->repeat;
                p_msg->interval = (gHumidifierPoints[point].interval * 1000);
                p_msg->duration = (gHumidifierPoints[point].transition->duration * 1000);
            }
            TIME_LOGI
            (
                "[%d] %-23s: %10lu : %8lu : HUM - On: %d",
                point,
                gcPointDescription[point],
                p_msg->interval,
                p_msg->duration,
                p_msg->on
            );
            /* Skip the rest of time points */
            break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_Climate(time_t t)
{
    climate_message_t humidifier_msg = {0};
    climate_message_t fan_msg        = {0};

    time_ClimateFan(t, &fan_msg);
    time_ClimateHumidifier(t, &humidifier_msg);

    if (CLIMATE_CMD_EMPTY != fan_msg.command)
    {
        Climate_Task_SendMsg(&fan_msg);
    }
    if (CLIMATE_CMD_EMPTY != humidifier_msg.command)
    {
        Climate_Task_SendMsg(&humidifier_msg);
    }
}

//-------------------------------------------------------------------------------------------------

static void time_SetAlarm(time_t t)
{
    int32_t point = 0;

    for (point = 0; point < TIME_IDX_MAX; point++)
    {
        if (t < gTimePoints[point].start)
        {
            gAlarm = gTimePoints[point].start;
            TIME_LOGT(gAlarm, "Alarm set to next time");
            break;
        }
    }
    if (TIME_IDX_MAX == point)
    {
        gAlarm = LONG_MAX;
        TIME_LOGT(t, "Alarm cleared");
    }
}

//-------------------------------------------------------------------------------------------------

static void time_ProcessMsg(time_message_t * p_msg, time_t t)
{
    gCommand = p_msg->command;

    if (TIME_CMD_SUN_ENABLE == gCommand)
    {
        time_PointsCalculate(t);
        time_SetAlarm(t);
        TIME_LOGT(t, "Current local time");
        time_Sun(t, FW_TRUE);
        time_Climate(t);
    }
}

//-------------------------------------------------------------------------------------------------

static void time_CheckForAlarms(time_t t)
{
    struct tm dt = {0};

    if (TIME_CMD_SUN_ENABLE == gCommand)
    {
        /* If the alarm is not set */
        if (LONG_MAX == gAlarm)
        {
            /* Check for midnight */
            localtime_r(&t, &dt);
            if ((0 == dt.tm_hour) && (0 == dt.tm_min) && (0 <= dt.tm_sec))
            {
                TIME_LOGT(t, "Midnight detected!");
                time_PointsCalculate(t);
                time_SetAlarm(t);
                TIME_LOGT(t, "Current local time");
                time_Sun(t, FW_TRUE);
                time_Climate(t);
            }
        }
        else
        {
            /* Check for alarm */
            if (t >= gAlarm)
            {
                TIME_LOGT(t, "Alarm detected!");
                time_SetAlarm(t);
                TIME_LOGT(t, "Current local time");
                time_Sun(t, FW_TRUE);
                time_Climate(t);
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
    BaseType_t     status   = pdFAIL;
    time_message_t msg      = {0};
    time_t         now      = 0;
    struct tm      datetime = {0};
    uint32_t       retry    = 0;
    static uint8_t sync_ok  = FW_FALSE;

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
            if (FW_FALSE == sync_ok)
            {
                TIME_LOGT(now, "Sync OK!");
                sync_ok = FW_TRUE;
            }

            status = xQueueReceive(gTimeQueue, (void *)&msg, TIME_TASK_TICK_MS);
            if (pdTRUE == status)
            {
                time_ProcessMsg(&msg, now);
            }
            time_CheckForAlarms(now);
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

static void time_Test_Time_Calculations(void)
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

    char                   string[TIME_STR_MAX_LEN] = {0};
    struct tm              dt                       = {0};
    time_t                 zero_time                = 0;
    time_t                 tz_offset                = 0;
    time_t                 current_time             = sun.current.time;
    time_t                 ref_utc_time             = sun.current.time;
    uint32_t               test                     = 0;
    uint32_t               offset_sec               = 0;
    uint32_t               offset_min               = 0;
    time_t                 calculated_time          = 0;
    transition_s_t const * p_trans                  = NULL;
    int                    trans_index              = sun.count;

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
    time_t         now                      = 0;
    struct tm      datetime                 = {0};
    char           string[TIME_STR_MAX_LEN] = {0};
    time_t         zero_time                = 0;
    time_t         tz_offset                = 0;
    led_message_t  led_msg                  = {0};
    struct timeval tv                       = {0};

    /* Set the timezone */
    TIME_LOGI("Set timezone to - %s", gTZ);
    setenv("TZ", gTZ, 1);
    tzset();

    /* Determine the time zone offset */
    gmtime_r(&zero_time, &datetime);
    datetime.tm_isdst = -1;
    tz_offset = mktime(&datetime);
    TIME_LOGI("Time zone offset           : %10lld", tz_offset);
    (void)tz_offset;

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
    TIME_LOGI("Test time                  : %10lld : %s", now, string);

    /* Transition to DST color for 1100 ms */
    led_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst.color.r     = 255;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.dword = 0;
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

    fan_tx_t fanMorning = {CLIMATE_CMD_FAN, FAN_SPEED_LOW,    true, 33};
    fan_tx_t fanOff     = {CLIMATE_CMD_FAN, FAN_SPEED_NONE,  false,  0};
    fan_tx_t fanDay     = {CLIMATE_CMD_FAN, FAN_SPEED_MEDIUM, true, 35};
    fan_tx_t fanEvening = {CLIMATE_CMD_FAN, FAN_SPEED_LOW,    true, 35};

    fan_point_t fan_points[] =
    {
        [TIME_IDX_MIDNIGHT]            = {&fanMorning, 429},
        [TIME_IDX_MORNING_BLUE_HOUR]   = {&fanOff,      76},
        [TIME_IDX_MORNING_GOLDEN_HOUR] = {NULL,          0},
        [TIME_IDX_RISE]                = {NULL,          0},
        [TIME_IDX_DAY]                 = {&fanDay,     572},
        [TIME_IDX_NOON]                = {NULL,          0},
        [TIME_IDX_EVENING_GOLDEN_HOUR] = {&fanOff,      77},
        [TIME_IDX_SET]                 = {NULL,          0},
        [TIME_IDX_EVENING_BLUE_HOUR]   = {NULL,          0},
        [TIME_IDX_NIGHT]               = {&fanEvening, 287},
    };

    humidifier_tx_t humidifierOn  = {CLIMATE_CMD_HUMIDIFY,  true, false, 20};
    humidifier_tx_t humidifierOff = {CLIMATE_CMD_HUMIDIFY, false, false,  0};

    humidifier_point_t humidifier_points[] =
    {
        [TIME_IDX_MIDNIGHT]            = {&humidifierOff, 429},
        [TIME_IDX_MORNING_BLUE_HOUR]   = {&humidifierOn,   13},
        [TIME_IDX_MORNING_GOLDEN_HOUR] = {&humidifierOff, 699},
        [TIME_IDX_RISE]                = {NULL,             0},
        [TIME_IDX_DAY]                 = {NULL,             0},
        [TIME_IDX_NOON]                = {NULL,             0},
        [TIME_IDX_EVENING_GOLDEN_HOUR] = {NULL,             0},
        [TIME_IDX_SET]                 = {NULL,             0},
        [TIME_IDX_EVENING_BLUE_HOUR]   = {&humidifierOn,   13},
        [TIME_IDX_NIGHT]               = {&humidifierOff, 287},
    };

    for (p = 0; p < TIME_IDX_MAX; p++)
    {
        uint32_t timeout = (time_points[p].interval) * 100;

        if (NULL != rgb_points[p].transition)
        {
            memset(&led_msg, 0, sizeof(led_msg));
            led_msg.command         = rgb_points[p].transition->cmd;
            led_msg.src.color.dword = rgb_points[p].transition->src.dword;
            led_msg.dst.color.dword = rgb_points[p].transition->dst.dword;
            led_msg.interval        = (rgb_points[p].interval * 100);
            led_msg.duration        = 0;
            LED_Task_SendMsg(&led_msg);
        }

        if (NULL != uw_points[p].transition)
        {
            memset(&led_msg, 0, sizeof(led_msg));
            led_msg.src.color.a = 0;
            led_msg.interval    = (uw_points[p].interval * 100);
            led_msg.duration    = 0;

            led_msg.command     = uw_points[p].transition->u_cmd;
            led_msg.dst.color.a = uw_points[p].transition->u_max;
            LED_Task_SendMsg(&led_msg);

            led_msg.command     = uw_points[p].transition->w_cmd;
            led_msg.dst.color.a = uw_points[p].transition->w_max;
            LED_Task_SendMsg(&led_msg);
        }

        if (NULL != fan_points[p].transition)
        {
            memset(&clt_msg, 0, sizeof(clt_msg));
            clt_msg.command  = fan_points[p].transition->cmd;
            clt_msg.speed    = fan_points[p].transition->speed;
            clt_msg.interval = 6000;
            clt_msg.duration = 2000;
            clt_msg.repeat   = fan_points[p].transition->repeat;
            Climate_Task_SendMsg(&clt_msg);
        }

        if (NULL != humidifier_points[p].transition)
        {
            memset(&clt_msg, 0, sizeof(clt_msg));
            clt_msg.command  = humidifier_points[p].transition->cmd;
            clt_msg.on       = humidifier_points[p].transition->on;
            clt_msg.interval = 6000;
            clt_msg.duration = 3000;
            clt_msg.repeat   = humidifier_points[p].transition->repeat;
            Climate_Task_SendMsg(&clt_msg);
        }

        vTaskDelay(pdMS_TO_TICKS(timeout));
    }
}

//-------------------------------------------------------------------------------------------------

#define GT(yr,mn,dy,hr,mi,sc,t,o) \
do \
{ \
    char str[TIME_STR_MAX_LEN] = {0}; \
    struct tm dt = {0}; \
    struct tm tz = {0}; \
    time_t ztime = 0; \
    dt.tm_sec = sc; \
    dt.tm_min = mi; \
    dt.tm_hour = hr; \
    dt.tm_mday = dy; \
    dt.tm_mon = (mn - 1); \
    dt.tm_year = (yr - 1900); \
    dt.tm_wday = 0; \
    dt.tm_yday = 0; \
    dt.tm_isdst = -1; \
    t = mktime(&dt); \
    strftime(str, sizeof(str), "%c", &dt); \
    TIME_LOGI("DST (Daylight Saving Time) : %10d", dt.tm_isdst); \
    TIME_LOGI("%-26s : %10lld : %s", "Test time", t, str); \
    gmtime_r(&ztime, &tz); \
    tz.tm_isdst = dt.tm_isdst; \
    o = mktime(&tz); \
    TIME_LOGI("%-26s : %10lld", "Time zone offset", o); \
} \
while (0)

#define CHECK_TZ(x,v) \
do \
{ \
    if (v != x) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Must be", v); \
    } \
} \
while (0)

#define CHECK_RGB(x,i,d) \
do \
{ \
    if (i != x.interval) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Interval must be", i); \
    } \
    if (d != x.duration) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Duration must be", d); \
    } \
} \
while (0)

#define CHECK_UW(x,i,d,b) \
do \
{ \
    if (i != x.interval) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Interval must be", i); \
    } \
    if (d != x.duration) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Duration must be", d); \
    } \
    if (b != x.dst.brightness.v) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Brightness must be", b); \
    } \
} \
while (0)

#define CHECK_FAN(x,i,d,s) \
do \
{ \
    if (i != x.interval) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Interval must be", i); \
    } \
    if (d != x.duration) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Duration must be", d); \
    } \
    if (s != x.speed) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Speed must be", s); \
    } \
} \
while (0)

#define CHECK_HUM(x,i,d,o) \
do \
{ \
    if (i != x.interval) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Interval must be", i); \
    } \
    if (d != x.duration) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! Duration must be", d); \
    } \
    if (o != x.on) \
    { \
        TIME_LOGE("%-26s : %10d", "FAIL! \"On\" must be", o); \
    } \
} \
while (0)

static void time_Test_Point_Calculations(void)
{
    typedef struct
    {
        led_message_t     rgb;
        led_message_t     u;
        led_message_t     w;
        climate_message_t fan;
        climate_message_t hum;
    } test_msgs_t;

    test_msgs_t msgs      = {0};
    time_t      now       = 0;
    time_t      tz_offset = 0;

    /* Set the timezone */
    TIME_LOGI("%-26s : %s", "Set timezone to", gTZ);
    setenv("TZ", gTZ, 1);
    tzset();

    TIME_LOGI("---------------------------------------------------------");
    /* Determine the date/time - Sep 30 12:13:58 2024 */
    GT(2024, 9, 30, 12, 13, 58, now, tz_offset);
    CHECK_TZ(tz_offset, -10800);

    TIME_LOGI("---------------------------------------------------------");
    /* Determine the date/time - Nov 30 12:13:58 2024 */
    GT(2024, 11, 30, 12, 13, 58, now, tz_offset);
    CHECK_TZ(tz_offset, -7200);

    time_PointsCalculate(now);

    TIME_LOGI("---------------------------------------------------------");
    /* Nov 30 12:13:58 2024 - Noon */
    GT(2024, 11, 30, 12, 13, 58, now, tz_offset);
    TIME_LOGI("-------------------------- : - Interval : Duration : Info");
    memset(&msgs, 0, sizeof(msgs));

    time_SunRgb(now, FW_FALSE, &msgs.rgb);
    CHECK_RGB(msgs.rgb, 23895000, 11947000);

    time_SunUw(now, FW_FALSE, &msgs.u, &msgs.w);
    CHECK_UW(msgs.u, 23895000, 11947000, 31);
    CHECK_UW(msgs.w, 23895000, 11947000, 173);

    time_ClimateFan(now, &msgs.fan);
    CHECK_FAN(msgs.fan, 2449000, 367350, FAN_SPEED_MEDIUM);

    time_ClimateHumidifier(now, &msgs.hum);
    CHECK_HUM(msgs.hum, 0, 0, 0);


    TIME_LOGI("---------------------------------------------------------");
    /* Nov 30 15:30:06 2024 - Just before the Evening Golden Hour*/
    GT(2024, 11, 30, 15, 30, 06, now, tz_offset);
    TIME_LOGI("-------------------------- : - Interval : Duration : Info");
    memset(&msgs, 0, sizeof(msgs));

    time_SunRgb(now, FW_FALSE, &msgs.rgb);
    CHECK_RGB(msgs.rgb, 23895000, 23715000);

    time_SunUw(now, FW_FALSE, &msgs.u, &msgs.w);
    CHECK_UW(msgs.u, 23895000, 23715000, 31);
    CHECK_UW(msgs.w, 23895000, 23715000, 173);

    time_ClimateFan(now, &msgs.fan);
    CHECK_FAN(msgs.fan, 0, 0, FAN_SPEED_NONE);

    time_ClimateHumidifier(now, &msgs.hum);
    CHECK_HUM(msgs.hum, 0, 0, 0);

    TIME_LOGI("---------------------------------------------------------");
    /* Nov 30 07:36:56 2024 - Just before Sun rise */
    GT(2024, 11, 30, 7, 36, 56, now, tz_offset);
    TIME_LOGI("-------------------------- : - Interval : Duration : Info");
    memset(&msgs, 0, sizeof(msgs));

    time_SunRgb(now, FW_FALSE, &msgs.rgb);
    CHECK_RGB(msgs.rgb, 844000, 833000);

    time_SunUw(now, FW_FALSE, &msgs.u, &msgs.w);
    CHECK_UW(msgs.u, 32091000, 27416000, 0);
    CHECK_UW(msgs.w, 32091000, 27416000, 0);

    time_ClimateFan(now, &msgs.fan);
    CHECK_FAN(msgs.fan, 0, 0, FAN_SPEED_NONE);

    time_ClimateHumidifier(now, &msgs.hum);
    CHECK_HUM(msgs.hum, 0, 0, 0);

    TIME_LOGI("---------------------------------------------------------");
    /* Nov 30 07:28:56 2024 - Enough for humidification */
    GT(2024, 11, 30, 7, 28, 56, now, tz_offset);
    TIME_LOGI("-------------------------- : - Interval : Duration : Info");
    memset(&msgs, 0, sizeof(msgs));

    time_SunRgb(now, FW_FALSE, &msgs.rgb);
    CHECK_RGB(msgs.rgb, 844000, 353000);

    time_SunUw(now, FW_FALSE, &msgs.u, &msgs.w);
    CHECK_UW(msgs.u, 32091000, 26936000, 0);
    CHECK_UW(msgs.w, 32091000, 26936000, 0);

    time_ClimateFan(now, &msgs.fan);
    CHECK_FAN(msgs.fan, 0, 0, FAN_SPEED_NONE);

    time_ClimateHumidifier(now, &msgs.hum);
    CHECK_HUM(msgs.hum, 844000, 25000, 1);
}

//-------------------------------------------------------------------------------------------------

void Time_Task_Test(void)
{
    time_Test_Time_Calculations();
    time_Test_Alarm();
    time_Test_DayNight();
    time_Test_Point_Calculations();
}

//-------------------------------------------------------------------------------------------------
