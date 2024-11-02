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

//-------------------------------------------------------------------------------------------------

#define TIME_TASK_TICK_MS   (pdMS_TO_TICKS(1000))

#define TIME_POINT_COUNT    (7)
#define RGBA(rv,gv,bv,av)   {.r=rv,.g=gv,.b=bv,.a=av}
#define TIME_SECONDS_IN_DAY (24*60*60)

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

typedef struct
{
    time_t        start;
    uint32_t      duration;
    led_color_t   src;
    led_color_t   dst;
    led_command_t cmd;
    uint8_t       done;
} time_point_t;

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

/* Start             -    0 minutes - RGB(  0,   0,  32) - RGB(  0,   0,  44) - Smooth      */
/* MorningBlueHour   -  429 minutes - RGB(  0,   0,  44) - RGB( 64,   0,  56) - Rainbow CW  */
/* MorningGoldenHour -  442 minutes - RGB( 64,   0,  56) - RGB(220, 220,   0) - Rainbow CW  */
/* Rise              -  461 minutes                                                         */
/* Day               -  505 minutes - RGB(220, 220,   0) - RGB(255, 255, 255) - Sine        */
/* Noon              -  791 minutes                                                         */
/* EveningGoldenHour - 1076 minutes - RGB(220, 220,   0) - RGB( 64,   0,  56) - Rainbow CCW */
/* Set               - 1120 minutes                                                         */
/* EveningBlueHour   - 1140 minutes - RGB( 64,   0,  56) - RGB(  0,   0,  44) - Rainbow CCW */
/* Night             - 1153 minutes - RGB(  0,   0,  44) - RGB(  0,   0,  32) - None        */
static time_point_t gPoints[TIME_POINT_COUNT] =
{
    {0, 0, RGBA(  0,   0,  32, 1), RGBA(  0,   0,  44, 1), LED_CMD_INDICATE_COLOR,   0},
    {0, 0, RGBA(  0,   0,  44, 0), RGBA( 64,   0,  56, 1), LED_CMD_INDICATE_RAINBOW, 0},
    {0, 0, RGBA( 64,   0,  56, 0), RGBA(220, 220,   0, 1), LED_CMD_INDICATE_RAINBOW, 0},
    {0, 0, RGBA(220, 220,   0, 1), RGBA(255, 255, 255, 1), LED_CMD_INDICATE_SINE,    0},
    {0, 0, RGBA(220, 220,   0, 1), RGBA( 64,   0,  56, 0), LED_CMD_INDICATE_RAINBOW, 0},
    {0, 0, RGBA( 64,   0,  56, 1), RGBA(  0,   0,  44, 0), LED_CMD_INDICATE_RAINBOW, 0},
    {0, 0, RGBA(  0,   0,  44, 1), RGBA(  0,   0,  32, 1), LED_CMD_INDICATE_COLOR,   0},
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

static void time_PointsCalculate(time_t t, struct tm * p_dt, char * p_str)
{
    char    string[28]   = {0};
    time_t  zero_time    = 0;
    time_t  tz_offset    = 0;
    time_t  current_time = t;
    time_t  ref_utc_time = t;
    int     point        = 0;

    TIME_LOGI("Current local time         : %12llu - %s", current_time, p_str);

    /* Determine the time zone offset */
    gmtime_r(&zero_time, p_dt);
    p_dt->tm_isdst = 1;
    tz_offset = mktime(p_dt);
    TIME_LOGI("Time zone offset           : %10llu s", tz_offset);

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

    for (point = (TIME_POINT_COUNT - 1); point >= 0; point--)
    {
        if (6 == point)
        {
            gPoints[point].start     = time_SunNight(ref_utc_time);
            gPoints[point].duration  = (start_day_time + TIME_SECONDS_IN_DAY);
            gPoints[point].duration -= gPoints[point].start;
        }
        else
        {
            switch (point)
            {
                case 5:
                    gPoints[point].start = time_SunEveningBlueHour(ref_utc_time);
                    break;
                case 4:
                    gPoints[point].start = time_SunEveningGoldenHour(ref_utc_time);
                    break;
                case 3:
                    gPoints[point].start = time_SunDay(ref_utc_time);
                    break;
                case 2:
                    gPoints[point].start = time_SunMorningGoldenHour(ref_utc_time);
                    break;
                case 1:
                    gPoints[point].start = time_SunMorningBlueHour(ref_utc_time);
                    break;
                case 0:
                    gPoints[point].start = start_day_time;
                    break;
            }
            gPoints[point].duration = (gPoints[point + 1].start - gPoints[point].start);
        }

        localtime_r(&gPoints[point].start, p_dt);
        strftime(string, sizeof(string), "%c", p_dt);
        TIME_LOGI
        (
            "[%d] - Duration: %5lu s    : %12llu - %s",
            point,
            gPoints[point].duration,
            gPoints[point].start,
            string
        );
    }
}

//-------------------------------------------------------------------------------------------------

static void time_Indicate(time_t t, struct tm * p_dt, char * p_str, FW_BOOLEAN pre_transition)
{
    enum
    {
        TRANSITION_INTERVAL = 1200,
        TRANSITION_TIMEOUT  = (pdMS_TO_TICKS(1300)),
    };
    led_message_t led_msg      = {0};
    time_t        current_time = t;
    int           point        = 0;
    uint32_t      interval     = UINT32_MAX;
    uint32_t      duration     = UINT32_MAX;

    TIME_LOGI("Current local time         : %12llu - %s", current_time, p_str);

    for (point = (TIME_POINT_COUNT - 1); point >= 0; point--)
    {
        /* Find the offset inside the time range */
        if (current_time >= gPoints[point].start)
        {
            interval    = (gPoints[point].duration * 1000);
            duration    = ((current_time - gPoints[point].start) * 1000);
            TIME_LOGI("[%d] - Interval/Duration    : %12lu - %lu", point, interval, duration);

            /* Prepare the indication message */
            led_msg.command         = gPoints[point].cmd;
            led_msg.src_color.dword = gPoints[point].src.dword;
            led_msg.dst_color.dword = gPoints[point].dst.dword;
            led_msg.interval        = interval;
            led_msg.duration        = duration;

            if (FW_TRUE == pre_transition)
            {
                led_color_t   color   = {0};
                led_message_t pre_msg = {0};
                LED_Task_DetermineColor(&led_msg, &color);
                pre_msg.command         = LED_CMD_INDICATE_COLOR;
                pre_msg.dst_color.dword = color.dword;
                pre_msg.interval        = TRANSITION_INTERVAL;
                LED_Task_SendMsg(&pre_msg);
                vTaskDelay(TRANSITION_TIMEOUT);
            }

            LED_Task_SendMsg(&led_msg);

            break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void time_SetAlarm(time_t t, struct tm * p_dt, char * p_str)
{
    char    string[28]   = {0};
    time_t  current_time = t;
    int32_t point        = 0;

    for (point = 0; point < TIME_POINT_COUNT; point++)
    {
        if (current_time < gPoints[point].start)
        {
            gAlarm = gPoints[point].start;
            localtime_r(&gAlarm, p_dt);
            strftime(string, sizeof(string), "%c", p_dt);
            TIME_LOGI("Alarm set to next time     : %12llu - %s", gAlarm, string);
            break;
        }
    }
    if (TIME_POINT_COUNT == point)
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
        time_Indicate(t, p_dt, p_str, FW_TRUE);
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
                time_Indicate(t, p_dt, p_str, FW_TRUE);
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
                time_Indicate(t, p_dt, p_str, FW_TRUE);
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
    BaseType_t     status     = pdFAIL;
    time_message_t msg        = {0};
    time_t         now        = 0;
    struct tm      datetime   = {0};
    char           string[28] = {0};
    uint32_t       retry      = 0;
    static uint8_t sync_ok    = FW_FALSE;

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
    xTaskCreate(vTime_Task, "TIME", 2048, NULL, 5, NULL);

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
    TIME_LOGI("Time zone offset           : %10llu s", tz_offset);

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
    led_msg.command         = LED_CMD_INDICATE_COLOR;
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
    now        = (gPoints[3].start - 10);
    tv.tv_sec  = now;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    /* Wait till the alarm happens */
    vTaskDelay(15 * TIME_TASK_TICK_MS);
}

//-------------------------------------------------------------------------------------------------

void Time_Task_Test(void)
{
    time_Test_Calculations();
    time_Test_Alarm();
}

//-------------------------------------------------------------------------------------------------
