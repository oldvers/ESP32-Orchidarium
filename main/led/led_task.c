#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

#include "types.h"
#include "led_task.h"
#include "led_strip_rgb.h"
#include "led_strip_uwf.h"

#include "esp_timer.h"
#include "esp_log.h"

//-------------------------------------------------------------------------------------------------

typedef void (* iterate_fp_t)(void);

typedef struct
{
    double h;
    double s;
    double v;
} hsv_t;

typedef struct
{
    uint16_t interval;
    uint16_t counter;
} led_tick_t;

typedef struct
{
    uint32_t interval;
    uint32_t duration;
    uint32_t delta;
} led_time_t;

typedef struct
{
    led_command_t command;
    led_color_t   dst_color;
    led_color_t   src_color;
    led_tick_t    tick;
    led_time_t    time;
    uint16_t      offset;
    uint16_t      led;
    hsv_t         hsv;
    iterate_fp_t  fp_iterate;
    uint8_t       buffer[LED_TASK_PIXELS_COUNT * 3];
} leds_t;

//-------------------------------------------------------------------------------------------------

#define LED_TASK_TICK_MS (10)

#define LED_TASK_LOG  0

#if (1 == LED_TASK_LOG)
static const char * gTAG = "LED";
#    define LED_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define LED_LOGE(...)  ESP_LOGE(gTAG, __VA_ARGS__)
#    define LED_LOGV(...)  ESP_LOGV(gTAG, __VA_ARGS__)
#else
#    define LED_LOGI(...)
#    define LED_LOGE(...)
#    define LED_LOGV(...)
#endif

//-------------------------------------------------------------------------------------------------

static const double  gPi       = 3.1415926;

static QueueHandle_t gLedQueue = {0};
static leds_t        gLeds     = {0};

//-------------------------------------------------------------------------------------------------

static void led_RGBtoHSV(led_color_t * p_color, hsv_t * p_hsv)
{
    double min, max, delta;

    min = (p_color->r < p_color->g) ? p_color->r : p_color->g;
    min = (min < p_color->b) ? min : p_color->b;

    max = (p_color->r > p_color->g) ? p_color->r : p_color->g;
    max = (max > p_color->b) ? max : p_color->b;

    /* Value */
    p_hsv->v = max / 255.0;

    delta = max - min;

    if (max != 0)
    {
        /* Saturation */
        p_hsv->s = (1.0 * delta / max);
    }
    else
    {
        /* r = g = b = 0 */
        /* s = 0, v is undefined */
        p_hsv->s = 0;
        p_hsv->h = 0;
        return;
    }

    /* Hue */
    if (p_color->r == max)
    {
        p_hsv->h = (p_color->g - p_color->b) / delta;
    }
    else if (p_color->g == max)
    {
        p_hsv->h = 2 + (p_color->b - p_color->r) / delta;
    }
    else
    {
        p_hsv->h = 4 + (p_color->r - p_color->g) / delta;
    }

    /* Convert hue to degrees and back */
    p_hsv->h *= 60;
    if (p_hsv->h < 0)
    {
        p_hsv->h += 360;
    }
    p_hsv->h /= 360;
}

//-------------------------------------------------------------------------------------------------

static void led_HSVtoRGB(hsv_t * p_hsv, led_color_t * p_color)
{
    double r = 0, g = 0, b = 0;

    int i = (int)(p_hsv->h * 6);
    double f = p_hsv->h * 6 - i;
    double p = p_hsv->v * (1 - p_hsv->s);
    double q = p_hsv->v * (1 - f * p_hsv->s);
    double t = p_hsv->v * (1 - (1 - f) * p_hsv->s);

    switch(i % 6)
    {
        case 0: r = p_hsv->v, g = t, b = p; break;
        case 1: r = q, g = p_hsv->v, b = p; break;
        case 2: r = p, g = p_hsv->v, b = t; break;
        case 3: r = p, g = q, b = p_hsv->v; break;
        case 4: r = t, g = p, b = p_hsv->v; break;
        case 5: r = p_hsv->v, g = p, b = q; break;
    }

    p_color->r = (uint8_t)(r * 255);
    p_color->g = (uint8_t)(g * 255);
    p_color->b = (uint8_t)(b * 255);
}

//-------------------------------------------------------------------------------------------------

/* Performs linear interpolation between two values */
static double led_LinearInterpolation(double a, double b, double t)
{
    return a + (b - a) * t;
}

//-------------------------------------------------------------------------------------------------

/* Calculates smooth color transition between two RGB colors */
static void led_SmoothColorTransition
(
    led_color_t * p_a,
    led_color_t * p_b,
    double prgs,
    led_color_t * p_r
)
{
    // Clamp progress value between 0 and 1
    if (prgs < 0) prgs = 0;
    if (prgs > 1) prgs = 1;

    // Interpolate each RGB component separately
    p_r->r = (uint8_t)led_LinearInterpolation(p_a->r, p_b->r, prgs);
    p_r->g = (uint8_t)led_LinearInterpolation(p_a->g, p_b->g, prgs);
    p_r->b = (uint8_t)led_LinearInterpolation(p_a->b, p_b->b, prgs);
}

//-------------------------------------------------------------------------------------------------

// Calculates rainbow color transition between two RGB colors
static void led_RainbowColorTransition
(
    led_color_t * p_a,
    led_color_t * p_b,
    double prgs,
    led_color_t * p_r
)
{
    hsv_t       src_hsv = {0};
    hsv_t       dst_hsv = {0};
    hsv_t       hsv     = {0};

    /* Determine the SRC/DST HSVs */
    led_RGBtoHSV(p_a, &src_hsv);
    led_RGBtoHSV(p_b, &dst_hsv);

    /* Calculate Hue */
    if ((1 == p_b->a) && (dst_hsv.h < src_hsv.h))
    {
        dst_hsv.h += 1.0;
    }
    if ((0 == p_b->a) && (src_hsv.h < dst_hsv.h))
    {
        src_hsv.h += 1.0;
    }
    hsv.h = ((dst_hsv.h - src_hsv.h) * prgs + src_hsv.h);
    if (1.0 < hsv.h)
    {
        hsv.h -= 1.0;
    }

    /* Calculate Value */
    hsv.v = ((dst_hsv.v - src_hsv.v) * prgs + src_hsv.v);
    /* Calculate Saturation */
    hsv.s = ((dst_hsv.s - src_hsv.s) * prgs + src_hsv.s);
    led_HSVtoRGB(&hsv, p_r);
}

//-------------------------------------------------------------------------------------------------
//--- Simple Color Indication ---------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void led_IterateIndication_Color(void)
{
    led_color_t result  = {0};
    double      percent = (1.0 * gLeds.time.duration / gLeds.time.interval);

    if ((gLeds.dst_color.dword != gLeds.src_color.dword) &&
        (gLeds.time.duration < gLeds.time.interval))
    {
        led_SmoothColorTransition(&gLeds.src_color, &gLeds.dst_color, percent, &result);
        gLeds.time.duration += gLeds.time.delta;
    }
    else
    {
        result.dword          = gLeds.dst_color.dword;
        gLeds.src_color.dword = gLeds.dst_color.dword;
        gLeds.command         = LED_CMD_EMPTY;
    }
    LED_Strip_RGB_SetColor(&result);
    LED_Strip_RGB_Update();

    LED_LOGI("C(%d.%d.%d)-P:%d", result.r, result.g, result.b, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void led_SetIndication_Color(led_message_t * p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    gLeds.dst_color.dword = 0;
    gLeds.dst_color.r     = p_msg->dst_color.r;
    gLeds.dst_color.g     = p_msg->dst_color.g;
    gLeds.dst_color.b     = p_msg->dst_color.b;

    /* Set the default tick interval to 30 ms */
    gLeds.tick.interval = 3;
    gLeds.tick.counter  = gLeds.tick.interval;
    gLeds.time.delta    = (gLeds.tick.interval * LED_TASK_TICK_MS);

    /* Determine the SRC color */
    gLeds.src_color.dword = 0;
    if (0 == p_msg->src_color.a)
    {
        LED_Strip_RGB_GetAverageColor(&gLeds.src_color);
    }
    else
    {
        gLeds.src_color.r = p_msg->src_color.r;
        gLeds.src_color.g = p_msg->src_color.g;
        gLeds.src_color.b = p_msg->src_color.b;
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        gLeds.time.interval = p_msg->interval;
        gLeds.time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        gLeds.time.interval = MIN_TRANSITION_TIME_MS;
        gLeds.time.duration = 0;
    }
    gLeds.fp_iterate = led_IterateIndication_Color;
    gLeds.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- RGB Circulation LED Indication --------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void led_IterateIndication_RgbCirculation(void)
{
    LED_Strip_RGB_Update();
    LED_Strip_RGB_Rotate(false);
    if (UINT16_MAX != gLeds.led)
    {
        gLeds.led = ((gLeds.led + 1) % (sizeof(gLeds.buffer) / 3));
        /* Switch the color R -> G -> B */
        if (0 == gLeds.led)
        {
            gLeds.dst_color.bytes[gLeds.offset++] = 0;
            gLeds.offset %= 3;
            gLeds.dst_color.bytes[gLeds.offset] = UINT8_MAX;
            LED_Strip_RGB_SetPixelColor(gLeds.led, &gLeds.dst_color);
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void led_SetIndication_RgbCirculation(led_message_t * p_msg)
{
    LED_Strip_RGB_Clear();

    gLeds.dst_color.dword = 0;
    gLeds.dst_color.r     = p_msg->dst_color.r;
    gLeds.dst_color.g     = p_msg->dst_color.g;
    gLeds.dst_color.b     = p_msg->dst_color.b;

    /* Set the color depending on color settings */
    if (0 == gLeds.dst_color.dword)
    {
        gLeds.offset      = 0;
        gLeds.led         = 0;
        gLeds.dst_color.r = UINT8_MAX;
        LED_Strip_RGB_SetPixelColor(gLeds.led, &gLeds.dst_color);
    }
    else
    {
        gLeds.offset = UINT8_MAX;
        gLeds.led    = UINT16_MAX;
        LED_Strip_RGB_SetPixelColor(0, &gLeds.dst_color);
    }
    /* Set the default tick interval to 40 ms */
    gLeds.tick.interval = 4;
    gLeds.tick.counter  = gLeds.tick.interval;
    gLeds.fp_iterate    = led_IterateIndication_RgbCirculation;
    gLeds.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- Fade LED Indication -------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void led_IterateIndication_Fade(void)
{
    enum
    {
        MAX_FADE_LEVEL = 30,
    };

    led_HSVtoRGB(&gLeds.hsv, &gLeds.dst_color);
    LED_Strip_RGB_SetColor(&gLeds.dst_color);
    LED_Strip_RGB_Update();

    gLeds.led++;
    if (MAX_FADE_LEVEL == gLeds.led)
    {
        gLeds.led    = 0;
        gLeds.offset = ((gLeds.offset + 1) % 2);
    }
    if (0 == gLeds.offset)
    {
        gLeds.hsv.v = (gLeds.led * 0.02);
    }
    else
    {
        gLeds.hsv.v = ((MAX_FADE_LEVEL - gLeds.led - 1) * 0.02);
    }
}

//-------------------------------------------------------------------------------------------------

static void led_SetIndication_Fade(led_message_t * p_msg)
{
    LED_Strip_RGB_Clear();

    gLeds.dst_color.dword = 0;
    gLeds.dst_color.r     = p_msg->dst_color.r;
    gLeds.dst_color.g     = p_msg->dst_color.g;
    gLeds.dst_color.b     = p_msg->dst_color.b;

    led_RGBtoHSV(&gLeds.dst_color, &gLeds.hsv);
    gLeds.hsv.v         = 0.0;
    gLeds.offset        = 0;
    gLeds.led           = 0;
    /* Set the default tick interval to 30 ms */
    gLeds.tick.interval = 3;
    gLeds.tick.counter  = gLeds.tick.interval;
    gLeds.fp_iterate    = led_IterateIndication_Fade;
    gLeds.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- PingPong LED Indication ---------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void led_IterateIndication_PingPong(void)
{
    LED_Strip_RGB_Update();
    LED_Strip_RGB_Rotate(0 == gLeds.offset);

    gLeds.led++;
    if ((sizeof(gLeds.buffer) / 3) == gLeds.led)
    {
        gLeds.led    = 0;
        gLeds.offset = ((gLeds.offset + 1) % 2);
    }
}

//-------------------------------------------------------------------------------------------------

static void led_SetIndication_PingPong(led_message_t * p_msg)
{
    LED_Strip_RGB_Clear();

    gLeds.dst_color.dword = 0;
    gLeds.dst_color.r     = p_msg->dst_color.r;
    gLeds.dst_color.g     = p_msg->dst_color.g;
    gLeds.dst_color.b     = p_msg->dst_color.b;
    gLeds.offset          = 0;
    gLeds.led             = 0;
    LED_Strip_RGB_SetPixelColor(gLeds.led, &gLeds.dst_color);
    /* Set the default tick interval to 40 ms */
    gLeds.tick.interval = 4;
    gLeds.tick.counter  = gLeds.tick.interval;
    gLeds.fp_iterate    = led_IterateIndication_PingPong;
    gLeds.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- Rainbow LED Indication ---------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void led_IterateIndication_RainbowCirculation(void)
{
    LED_Strip_RGB_Rotate(false);
    LED_Strip_RGB_Update();
}

//-------------------------------------------------------------------------------------------------

static void led_SetIndication_RainbowCirculation(led_message_t * p_msg)
{
    double max = 0.0;

    gLeds.dst_color.dword = 0;
    gLeds.dst_color.r     = p_msg->dst_color.r;
    gLeds.dst_color.g     = p_msg->dst_color.g;
    gLeds.dst_color.b     = p_msg->dst_color.b;

    if (0 == gLeds.dst_color.dword)
    {
        /* Running Rainbow */
        max = 0.222;
        /* Set the default tick interval to 60 ms */
        gLeds.tick.interval = 6;
        gLeds.tick.counter  = gLeds.tick.interval;
        gLeds.fp_iterate    = led_IterateIndication_RainbowCirculation;
    }
    else
    {
        /* Static Rainbow */
        max = (gLeds.dst_color.r > gLeds.dst_color.g) ? gLeds.dst_color.r : gLeds.dst_color.g;
        max = (max > gLeds.dst_color.b) ? max : gLeds.dst_color.b;
        max /= 255.0;
        /* Disable iteration */
        gLeds.command       = LED_CMD_EMPTY;
        gLeds.tick.interval = 0;
        gLeds.tick.counter  = 0;
        gLeds.fp_iterate    = NULL;
    }

    /* Draw the Rainbow */
    gLeds.hsv.s = 1.0;
    gLeds.hsv.v = max;
    for (gLeds.led = 0; gLeds.led < (sizeof(gLeds.buffer) / 3); gLeds.led++)
    {
        gLeds.hsv.h = ((gLeds.led + 0.5) * 1.0 / (sizeof(gLeds.buffer) / 3));
        led_HSVtoRGB(&gLeds.hsv, &gLeds.dst_color);
        LED_Strip_RGB_SetPixelColor(gLeds.led, &gLeds.dst_color);
    }
    LED_Strip_RGB_Update();
}

//-------------------------------------------------------------------------------------------------

static void led_IterateIndication_Rainbow(void)
{
    led_color_t result  = {0};
    double      percent = (1.0 * gLeds.time.duration / gLeds.time.interval);

    if ((gLeds.dst_color.dword != gLeds.src_color.dword) &&
        (gLeds.time.duration < gLeds.time.interval))
    {
        led_RainbowColorTransition(&gLeds.src_color, &gLeds.dst_color, percent, &result);
        gLeds.time.duration += gLeds.time.delta;
    }
    else
    {
        result.dword          = gLeds.dst_color.dword;
        gLeds.src_color.dword = gLeds.dst_color.dword;
        gLeds.command         = LED_CMD_EMPTY;
    }

    LED_Strip_RGB_SetColor(&result);
    LED_Strip_RGB_Update();

    LED_LOGI("C(%d.%d.%d)-P:%d", result.r, result.g, result.b, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void led_SetIndication_Rainbow(led_message_t * p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    /* Store the SRC/DST colors */
    gLeds.dst_color.dword = p_msg->dst_color.dword;
    gLeds.src_color.dword = p_msg->src_color.dword;

    /* Set the default tick interval to 30 ms */
    gLeds.tick.interval = 3;
    gLeds.tick.counter  = gLeds.tick.interval;
    gLeds.time.delta    = (gLeds.tick.interval * LED_TASK_TICK_MS);

    /* Check the rainbow changing direction */
    if (0 == (p_msg->src_color.a ^ p_msg->dst_color.a))
    {
        /* The direction is set incorrectly - get the current color */
        LED_Strip_RGB_GetAverageColor(&gLeds.src_color);
        /* Set the default direction */
        gLeds.dst_color.a = 1;
        gLeds.src_color.a = 0;
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        gLeds.time.interval = p_msg->interval;
        gLeds.time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        gLeds.time.interval = MIN_TRANSITION_TIME_MS;
        gLeds.time.duration = 0;
    }
    gLeds.fp_iterate = led_IterateIndication_Rainbow;
    gLeds.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- Sine Color Indication -----------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void led_IterateIndication_Sine(void)
{
    led_color_t  result  = {0};
    double       percent = (1.0 * gLeds.time.duration / gLeds.time.interval);

    if ((gLeds.dst_color.dword != gLeds.src_color.dword) &&
        (gLeds.time.duration < gLeds.time.interval))
    {
        percent = sin(percent * gPi);
        led_SmoothColorTransition(&gLeds.src_color, &gLeds.dst_color, percent, &result);
        gLeds.time.duration += gLeds.time.delta;
    }
    else
    {
        result.dword          = gLeds.src_color.dword;
        gLeds.dst_color.dword = gLeds.src_color.dword;
        gLeds.command         = LED_CMD_EMPTY;
    }
    LED_Strip_RGB_SetColor(&result);
    LED_Strip_RGB_Update();

    LED_LOGI("C(%d.%d.%d)-P:%d", result.r, result.g, result.b, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void led_SetIndication_Sine(led_message_t * p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    gLeds.dst_color.dword = 0;
    gLeds.dst_color.r     = p_msg->dst_color.r;
    gLeds.dst_color.g     = p_msg->dst_color.g;
    gLeds.dst_color.b     = p_msg->dst_color.b;

    /* Set the default tick interval to 30 ms */
    gLeds.tick.interval = 3;
    gLeds.tick.counter  = gLeds.tick.interval;
    gLeds.time.delta    = (gLeds.tick.interval * LED_TASK_TICK_MS);

    /* Determine the SRC color */
    gLeds.src_color.dword = 0;
    if (0 == p_msg->src_color.a)
    {
        LED_Strip_RGB_GetAverageColor(&gLeds.src_color);
    }
    else
    {
        gLeds.src_color.r = p_msg->src_color.r;
        gLeds.src_color.g = p_msg->src_color.g;
        gLeds.src_color.b = p_msg->src_color.b;
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        gLeds.time.interval = p_msg->interval;
        gLeds.time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        gLeds.time.interval = MIN_TRANSITION_TIME_MS;
        gLeds.time.duration = 0;
    }
    gLeds.fp_iterate   = led_IterateIndication_Sine;
    gLeds.fp_iterate();
}

//-------------------------------------------------------------------------------------------------

static void led_ProcessMsg(led_message_t * p_msg)
{
    gLeds.command = p_msg->command;
    switch (gLeds.command)
    {
        case LED_CMD_INDICATE_COLOR:
            led_SetIndication_Color(p_msg);
            break;
        case LED_CMD_INDICATE_RGB_CIRCULATION:
            led_SetIndication_RgbCirculation(p_msg);
            break;
        case LED_CMD_INDICATE_FADE:
            led_SetIndication_Fade(p_msg);
            break;
        case LED_CMD_INDICATE_PINGPONG:
            led_SetIndication_PingPong(p_msg);
            break;
        case LED_CMD_INDICATE_RAINBOW_CIRCULATION:
            led_SetIndication_RainbowCirculation(p_msg);
            break;
        case LED_CMD_INDICATE_RAINBOW:
            led_SetIndication_Rainbow(p_msg);
            break;
        case LED_CMD_INDICATE_SINE:
            led_SetIndication_Sine(p_msg);
            break;
        default:
            gLeds.command       = LED_CMD_EMPTY;
            gLeds.fp_iterate    = NULL;
            gLeds.tick.interval = 0;
            gLeds.tick.counter  = 0;
            break;
    }
}

//-------------------------------------------------------------------------------------------------

static void led_Process(void)
{
    if (LED_CMD_EMPTY == gLeds.command) return;

    gLeds.tick.counter--;
    if (0 == gLeds.tick.counter)
    {
        if (NULL != gLeds.fp_iterate)
        {
            gLeds.fp_iterate();
        }
        gLeds.tick.counter = gLeds.tick.interval;
    }
}

//-------------------------------------------------------------------------------------------------

static void led_Task(void * pvParameters)
{
    enum
    {
        INIT_DELAY = 50,
    };
    BaseType_t    status = pdFAIL;
    led_message_t msg    = {0};

    LED_LOGI("LED Task started...");

    LED_Strip_RGB_Init(gLeds.buffer, sizeof(gLeds.buffer));
    vTaskDelay(pdMS_TO_TICKS(INIT_DELAY));
    LED_Strip_RGB_PowerOn();
    vTaskDelay(pdMS_TO_TICKS(INIT_DELAY));
    LED_Strip_RGB_Clear();
    LED_Strip_RGB_Update();
    vTaskDelay(pdMS_TO_TICKS(INIT_DELAY));
    LED_Strip_RGB_Clear();
    LED_Strip_RGB_Update();

    while (FW_TRUE)
    {
        status = xQueueReceive(gLedQueue, (void *)&msg, pdMS_TO_TICKS(LED_TASK_TICK_MS));

        if (pdTRUE == status)
        {
            led_ProcessMsg(&msg);
        }
        else
        {
            led_Process();
        }
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Task_Init(void)
{
    gLedQueue = xQueueCreate(20, sizeof(led_message_t));

    (void)xTaskCreate(led_Task, "LED_Task", 4096, NULL, 10, NULL);
}

//-------------------------------------------------------------------------------------------------

void LED_Task_SendMsg(led_message_t * p_msg)
{
    LED_LOGI
    (
        "Msg->C:%d-S(%d.%d.%d)-D(%d.%d.%d)-I:%d",
        p_msg->command,
        p_msg->src_color.r, p_msg->src_color.g, p_msg->src_color.b,
        p_msg->dst_color.r, p_msg->dst_color.g, p_msg->dst_color.b,
        (int)p_msg->interval
    );

    (void)xQueueSendToBack(gLedQueue, (void *)p_msg, (TickType_t)0);
}

//-------------------------------------------------------------------------------------------------

void LED_Task_DetermineColor(led_message_t * p_msg, led_color_t * p_color)
{
    double percent = (1.0 * p_msg->duration / p_msg->interval);

    p_color->dword = 0;

    if (p_msg->duration < p_msg->interval)
    {
        switch (p_msg->command)
        {
            case LED_CMD_INDICATE_RAINBOW:
                led_RainbowColorTransition(&p_msg->src_color, &p_msg->dst_color, percent, p_color);
                break;
            case LED_CMD_INDICATE_SINE:
                percent = sin(percent * gPi);
                led_SmoothColorTransition(&p_msg->src_color, &p_msg->dst_color, percent, p_color);
                break;
            default:
                led_SmoothColorTransition(&p_msg->src_color, &p_msg->dst_color, percent, p_color);
                break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Task_GetCurrentColor(led_color_t * p_color)
{
    /* This call is not thread safe but this is acceptable */
    LED_Strip_RGB_GetAverageColor(p_color);
}

//-------------------------------------------------------------------------------------------------
//--- Tests ---------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void led_Test_Color(void)
{
    led_message_t led_msg = {0};

    /* Default transition to DST color about 3000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst_color.r     = 255;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Transition to DST color about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_COLOR;
    /* To - Green */
    led_msg.dst_color.r     = 0;
    led_msg.dst_color.g     = 255;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 1;
    /* From - Ignored */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 0;
    led_msg.src_color.b     = 255;
    led_msg.src_color.a     = 0;
    led_msg.interval        = 8000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));

    /* Transition from SRC to DST color about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst_color.r     = 255;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 1;
    /* From - Blue */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 0;
    led_msg.src_color.b     = 255;
    led_msg.src_color.a     = 1;
    led_msg.interval        = 8000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));

    /* Transition from SRC to DST color about 4000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst_color.r     = 255;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 1;
    /* From - Blue with 50% */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 0;
    led_msg.src_color.b     = 255;
    led_msg.src_color.a     = 1;
    led_msg.interval        = 8000;
    led_msg.duration        = 4000;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void led_Test_RgbCirculation(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RGB_CIRCULATION;
    /* To - Ignored */
    led_msg.dst_color.r     = 0;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void led_Test_Fade(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_FADE;
    /* To - Some */
    led_msg.dst_color.r     = 160;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 130;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void led_Test_PingPong(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_PINGPONG;
    /* To - Some */
    led_msg.dst_color.r     = 160;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 130;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void led_Test_RainbowCirculation(void)
{
    led_message_t led_msg = {0};

    /* Static Rainbow */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RAINBOW_CIRCULATION;
    /* To - Some */
    led_msg.dst_color.r     = 90;
    led_msg.dst_color.g     = 90;
    led_msg.dst_color.b     = 90;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Rainbow Circulation */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RAINBOW_CIRCULATION;
    /* To - Some */
    led_msg.dst_color.r     = 0;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void led_Test_Rainbow(void)
{
    led_message_t led_msg = {0};

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RAINBOW;
    /* To - Green */
    led_msg.dst_color.r     = 0;
    led_msg.dst_color.g     = 255;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 0;
    led_msg.src_color.b     = 0;
    led_msg.src_color.a     = 0;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RAINBOW;
    /* To - Red */
    led_msg.dst_color.r     = 255;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 0;
    led_msg.src_color.b     = 0;
    led_msg.src_color.a     = 0;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RAINBOW;
    /* To - Green */
    led_msg.dst_color.r     = 0;
    led_msg.dst_color.g     = 255;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 0;
    led_msg.src_color.b     = 0;
    led_msg.src_color.a     = 0;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RAINBOW;
    /* To - Red */
    led_msg.dst_color.r     = 255;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 255;
    led_msg.src_color.b     = 0;
    led_msg.src_color.a     = 1;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_RAINBOW;
    /* To - Red */
    led_msg.dst_color.r     = 255;
    led_msg.dst_color.g     = 0;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 0;
    /* From - Ignored */
    led_msg.src_color.r     = 0;
    led_msg.src_color.g     = 255;
    led_msg.src_color.b     = 0;
    led_msg.src_color.a     = 1;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));
}

//-------------------------------------------------------------------------------------------------

static void led_Test_Sine(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_INDICATE_SINE;
    /* To - Some */
    led_msg.dst_color.r     = 0;
    led_msg.dst_color.g     = 255;
    led_msg.dst_color.b     = 0;
    led_msg.dst_color.a     = 1;
    /* From - Ignored */
    led_msg.src_color.r     = 255;
    led_msg.src_color.g     = 0;
    led_msg.src_color.b     = 0;
    led_msg.src_color.a     = 1;
    led_msg.interval        = 6000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(7000));
}

//-------------------------------------------------------------------------------------------------

static void led_Test_DayNight(void)
{
    #define RGBA(rv,gv,bv,av) {.r=rv,.g=gv,.b=bv,.a=av}

    typedef struct
    {
        uint32_t      start;
        uint32_t      end;
        led_color_t   from;
        led_color_t   to;
        led_command_t cmd;
    } time_point_t;

    led_message_t led_msg = {0};
    uint8_t       p       = 0;

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

    time_point_t points[] =
    {
        {   0,  429, RGBA(  0,   0,  32, 1), RGBA(  0,   0,  44, 1), LED_CMD_INDICATE_COLOR},
        { 429,  442, RGBA(  0,   0,  44, 0), RGBA( 64,   0,  56, 1), LED_CMD_INDICATE_RAINBOW},
        { 442,  505, RGBA( 64,   0,  56, 0), RGBA(220, 220,   0, 1), LED_CMD_INDICATE_RAINBOW},
        { 505, 1076, RGBA(220, 220,   0, 1), RGBA(255, 255, 255, 1), LED_CMD_INDICATE_SINE},
        {1076, 1140, RGBA(220, 220,   0, 1), RGBA( 64,   0,  56, 0), LED_CMD_INDICATE_RAINBOW},
        {1140, 1153, RGBA( 64,   0,  56, 1), RGBA(  0,   0,  44, 0), LED_CMD_INDICATE_RAINBOW},
        {1153, 1440, RGBA(  0,   0,  44, 1), RGBA(  0,   0,  32, 1), LED_CMD_INDICATE_COLOR},
    };

    for (p = 0; p < (sizeof(points)/sizeof(time_point_t)); p++)
    {
        uint32_t timeout = (points[p].end - points[p].start) * 100;

        memset(&led_msg, 0, sizeof(led_msg));
        led_msg.command         = points[p].cmd;
        led_msg.src_color.dword = points[p].from.dword;
        led_msg.dst_color.dword = points[p].to.dword;
        led_msg.interval        = timeout;
        led_msg.duration        = 0;
        LED_Task_SendMsg(&led_msg);
        vTaskDelay(pdMS_TO_TICKS(timeout));
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Task_Test(void)
{
    led_Test_Color();
    led_Test_RgbCirculation();
    led_Test_Fade();
    led_Test_PingPong();
    led_Test_RainbowCirculation();
    led_Test_Rainbow();
    led_Test_Sine();
    led_Test_DayNight();
}

//-------------------------------------------------------------------------------------------------
