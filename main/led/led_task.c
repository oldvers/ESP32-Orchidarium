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

#define LED_RGB_STRIP_PIXELS_COUNT (18)

//-------------------------------------------------------------------------------------------------

typedef void (* iterate_fp_t)(void);
typedef uint8_t (* get_fp_t)(void);
typedef void (* set_fp_t)(uint8_t value);

typedef struct
{
    double h;
    double s;
    double v;
} hsv_t, * hsv_p;

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
    uint8_t       buffer[LED_RGB_STRIP_PIXELS_COUNT * 3];
} leds_rgb_t;

typedef struct
{
    led_command_t command;
    uint8_t       dst;
    uint8_t       src;
    led_tick_t    tick;
    led_time_t    time;
    iterate_fp_t  fp_iterate;
    get_fp_t      fp_get;
    set_fp_t      fp_set;
} leds_t, * leds_p;

typedef struct
{
    leds_t u;
    leds_t w;
    leds_t f;
} leds_uwf_t;

//-------------------------------------------------------------------------------------------------

static const double  gPi           = 3.1415926;

static QueueHandle_t gLedsRgbQueue = {0};
static leds_rgb_t    gLedsRgb      = {0};
static QueueHandle_t gLedsUwfQueue = {0};
static leds_uwf_t    gLedsUwf      = {0};

//-------------------------------------------------------------------------------------------------

static void rgb_RGBtoHSV(led_color_p p_color, hsv_p p_hsv)
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

static void rgb_HSVtoRGB(hsv_t * p_hsv, led_color_p p_color)
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
static void rgb_SmoothColorTransition
(
    led_color_p p_a,
    led_color_p p_b,
    double prgs,
    led_color_p p_r
)
{
    /* Clamp progress value between 0 and 1 */
    if (prgs < 0) prgs = 0;
    if (prgs > 1) prgs = 1;

    /* Interpolate each RGB component separately */
    p_r->r = (uint8_t)led_LinearInterpolation(p_a->r, p_b->r, prgs);
    p_r->g = (uint8_t)led_LinearInterpolation(p_a->g, p_b->g, prgs);
    p_r->b = (uint8_t)led_LinearInterpolation(p_a->b, p_b->b, prgs);
}

//-------------------------------------------------------------------------------------------------

/* Calculates rainbow color transition between two RGB colors */
static void rgb_RainbowColorTransition
(
    led_color_p p_a,
    led_color_p p_b,
    double prgs,
    led_color_p p_r
)
{
    hsv_t       src_hsv = {0};
    hsv_t       dst_hsv = {0};
    hsv_t       hsv     = {0};

    /* Determine the SRC/DST HSVs */
    rgb_RGBtoHSV(p_a, &src_hsv);
    rgb_RGBtoHSV(p_b, &dst_hsv);

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
    rgb_HSVtoRGB(&hsv, p_r);
}

//-------------------------------------------------------------------------------------------------
//--- Simple Color Indication ---------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void rgb_IterateIndication_Color(void)
{
    led_color_t result  = {0};
    double      percent = (1.0 * gLedsRgb.time.duration / gLedsRgb.time.interval);

    if ((gLedsRgb.dst_color.dword != gLedsRgb.src_color.dword) &&
        (gLedsRgb.time.duration < gLedsRgb.time.interval))
    {
        rgb_SmoothColorTransition(&gLedsRgb.src_color, &gLedsRgb.dst_color, percent, &result);
        gLedsRgb.time.duration += gLedsRgb.time.delta;
    }
    else
    {
        result.dword             = gLedsRgb.dst_color.dword;
        gLedsRgb.src_color.dword = gLedsRgb.dst_color.dword;
        gLedsRgb.command         = LED_CMD_EMPTY;
    }
    LED_Strip_RGB_SetColor(&result);
    LED_Strip_RGB_Update();

    LED_LOGI("C(%d.%d.%d)-P:%d", result.r, result.g, result.b, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void rgb_SetIndication_Color(led_message_p p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    gLedsRgb.dst_color.dword = 0;
    gLedsRgb.dst_color.r     = p_msg->dst.color.r;
    gLedsRgb.dst_color.g     = p_msg->dst.color.g;
    gLedsRgb.dst_color.b     = p_msg->dst.color.b;

    /* Set the default tick interval to 30 ms */
    gLedsRgb.tick.interval = 3;
    gLedsRgb.tick.counter  = gLedsRgb.tick.interval;
    gLedsRgb.time.delta    = (gLedsRgb.tick.interval * LED_TASK_TICK_MS);

    /* Determine the SRC color */
    gLedsRgb.src_color.dword = 0;
    if (0 == p_msg->src.color.a)
    {
        LED_Strip_RGB_GetAverageColor(&gLedsRgb.src_color);
    }
    else
    {
        gLedsRgb.src_color.r = p_msg->src.color.r;
        gLedsRgb.src_color.g = p_msg->src.color.g;
        gLedsRgb.src_color.b = p_msg->src.color.b;
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        gLedsRgb.time.interval = p_msg->interval;
        gLedsRgb.time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        gLedsRgb.time.interval = MIN_TRANSITION_TIME_MS;
        gLedsRgb.time.duration = 0;
    }
    gLedsRgb.fp_iterate = rgb_IterateIndication_Color;
    gLedsRgb.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- RGB Circulation LED Indication --------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void rgb_IterateIndication_RgbCirculation(void)
{
    LED_Strip_RGB_Update();
    LED_Strip_RGB_Rotate(false);
    if (UINT16_MAX != gLedsRgb.led)
    {
        gLedsRgb.led = ((gLedsRgb.led + 1) % (sizeof(gLedsRgb.buffer) / 3));
        /* Switch the color R -> G -> B */
        if (0 == gLedsRgb.led)
        {
            gLedsRgb.dst_color.bytes[gLedsRgb.offset++] = 0;
            gLedsRgb.offset %= 3;
            gLedsRgb.dst_color.bytes[gLedsRgb.offset] = UINT8_MAX;
            LED_Strip_RGB_SetPixelColor(gLedsRgb.led, &gLedsRgb.dst_color);
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void rgb_SetIndication_RgbCirculation(led_message_p p_msg)
{
    LED_Strip_RGB_Clear();

    gLedsRgb.dst_color.dword = 0;
    gLedsRgb.dst_color.r     = p_msg->dst.color.r;
    gLedsRgb.dst_color.g     = p_msg->dst.color.g;
    gLedsRgb.dst_color.b     = p_msg->dst.color.b;

    /* Set the color depending on color settings */
    if (0 == gLedsRgb.dst_color.dword)
    {
        gLedsRgb.offset      = 0;
        gLedsRgb.led         = 0;
        gLedsRgb.dst_color.r = UINT8_MAX;
        LED_Strip_RGB_SetPixelColor(gLedsRgb.led, &gLedsRgb.dst_color);
    }
    else
    {
        gLedsRgb.offset = UINT8_MAX;
        gLedsRgb.led    = UINT16_MAX;
        LED_Strip_RGB_SetPixelColor(0, &gLedsRgb.dst_color);
    }
    /* Set the default tick interval to 40 ms */
    gLedsRgb.tick.interval = 4;
    gLedsRgb.tick.counter  = gLedsRgb.tick.interval;
    gLedsRgb.fp_iterate    = rgb_IterateIndication_RgbCirculation;
    gLedsRgb.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- Fade LED Indication -------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void rgb_IterateIndication_Fade(void)
{
    enum
    {
        MAX_FADE_LEVEL = 30,
    };

    rgb_HSVtoRGB(&gLedsRgb.hsv, &gLedsRgb.dst_color);
    LED_Strip_RGB_SetColor(&gLedsRgb.dst_color);
    LED_Strip_RGB_Update();

    gLedsRgb.led++;
    if (MAX_FADE_LEVEL == gLedsRgb.led)
    {
        gLedsRgb.led    = 0;
        gLedsRgb.offset = ((gLedsRgb.offset + 1) % 2);
    }
    if (0 == gLedsRgb.offset)
    {
        gLedsRgb.hsv.v = (gLedsRgb.led * 0.02);
    }
    else
    {
        gLedsRgb.hsv.v = ((MAX_FADE_LEVEL - gLedsRgb.led - 1) * 0.02);
    }
}

//-------------------------------------------------------------------------------------------------

static void rgb_SetIndication_Fade(led_message_p p_msg)
{
    LED_Strip_RGB_Clear();

    gLedsRgb.dst_color.dword = 0;
    gLedsRgb.dst_color.r     = p_msg->dst.color.r;
    gLedsRgb.dst_color.g     = p_msg->dst.color.g;
    gLedsRgb.dst_color.b     = p_msg->dst.color.b;

    rgb_RGBtoHSV(&gLedsRgb.dst_color, &gLedsRgb.hsv);
    gLedsRgb.hsv.v  = 0.0;
    gLedsRgb.offset = 0;
    gLedsRgb.led    = 0;
    /* Set the default tick interval to 30 ms */
    gLedsRgb.tick.interval = 3;
    gLedsRgb.tick.counter  = gLedsRgb.tick.interval;
    gLedsRgb.fp_iterate    = rgb_IterateIndication_Fade;
    gLedsRgb.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- PingPong LED Indication ---------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void rgb_IterateIndication_PingPong(void)
{
    LED_Strip_RGB_Update();
    LED_Strip_RGB_Rotate(0 == gLedsRgb.offset);

    gLedsRgb.led++;
    if ((sizeof(gLedsRgb.buffer) / 3) == gLedsRgb.led)
    {
        gLedsRgb.led    = 0;
        gLedsRgb.offset = ((gLedsRgb.offset + 1) % 2);
    }
}

//-------------------------------------------------------------------------------------------------

static void rgb_SetIndication_PingPong(led_message_p p_msg)
{
    LED_Strip_RGB_Clear();

    gLedsRgb.dst_color.dword = 0;
    gLedsRgb.dst_color.r     = p_msg->dst.color.r;
    gLedsRgb.dst_color.g     = p_msg->dst.color.g;
    gLedsRgb.dst_color.b     = p_msg->dst.color.b;
    gLedsRgb.offset          = 0;
    gLedsRgb.led             = 0;
    LED_Strip_RGB_SetPixelColor(gLedsRgb.led, &gLedsRgb.dst_color);
    /* Set the default tick interval to 40 ms */
    gLedsRgb.tick.interval = 4;
    gLedsRgb.tick.counter  = gLedsRgb.tick.interval;
    gLedsRgb.fp_iterate    = rgb_IterateIndication_PingPong;
    gLedsRgb.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- Rainbow LED Indication ---------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void rgb_IterateIndication_RainbowCirculation(void)
{
    LED_Strip_RGB_Rotate(false);
    LED_Strip_RGB_Update();
}

//-------------------------------------------------------------------------------------------------

static void rgb_SetIndication_RainbowCirculation(led_message_p p_msg)
{
    double max = 0.0;

    gLedsRgb.dst_color.dword = 0;
    gLedsRgb.dst_color.r     = p_msg->dst.color.r;
    gLedsRgb.dst_color.g     = p_msg->dst.color.g;
    gLedsRgb.dst_color.b     = p_msg->dst.color.b;

    if (0 == gLedsRgb.dst_color.dword)
    {
        /* Running Rainbow */
        max = 0.222;
        /* Set the default tick interval to 60 ms */
        gLedsRgb.tick.interval = 6;
        gLedsRgb.tick.counter  = gLedsRgb.tick.interval;
        gLedsRgb.fp_iterate    = rgb_IterateIndication_RainbowCirculation;
    }
    else
    {
        /* Static Rainbow */
        if (gLedsRgb.dst_color.r > gLedsRgb.dst_color.g)
        {
            max = gLedsRgb.dst_color.r;
        }
        else
        {
            max = gLedsRgb.dst_color.g;
        }
        if (max < gLedsRgb.dst_color.b)
        {
            max = gLedsRgb.dst_color.b;
        }
        max /= 255.0;
        /* Disable iteration */
        gLedsRgb.command       = LED_CMD_EMPTY;
        gLedsRgb.tick.interval = 0;
        gLedsRgb.tick.counter  = 0;
        gLedsRgb.fp_iterate    = NULL;
    }

    /* Draw the Rainbow */
    gLedsRgb.hsv.s = 1.0;
    gLedsRgb.hsv.v = max;
    for (gLedsRgb.led = 0; gLedsRgb.led < (sizeof(gLedsRgb.buffer) / 3); gLedsRgb.led++)
    {
        gLedsRgb.hsv.h = ((gLedsRgb.led + 0.5) * 1.0 / (sizeof(gLedsRgb.buffer) / 3));
        rgb_HSVtoRGB(&gLedsRgb.hsv, &gLedsRgb.dst_color);
        LED_Strip_RGB_SetPixelColor(gLedsRgb.led, &gLedsRgb.dst_color);
    }
    LED_Strip_RGB_Update();
}

//-------------------------------------------------------------------------------------------------

static void rgb_IterateIndication_Rainbow(void)
{
    led_color_t result  = {0};
    double      percent = (1.0 * gLedsRgb.time.duration / gLedsRgb.time.interval);

    if ((gLedsRgb.dst_color.dword != gLedsRgb.src_color.dword) &&
        (gLedsRgb.time.duration < gLedsRgb.time.interval))
    {
        rgb_RainbowColorTransition(&gLedsRgb.src_color, &gLedsRgb.dst_color, percent, &result);
        gLedsRgb.time.duration += gLedsRgb.time.delta;
    }
    else
    {
        result.dword             = gLedsRgb.dst_color.dword;
        gLedsRgb.src_color.dword = gLedsRgb.dst_color.dword;
        gLedsRgb.command         = LED_CMD_EMPTY;
    }

    LED_Strip_RGB_SetColor(&result);
    LED_Strip_RGB_Update();

    LED_LOGI("C(%d.%d.%d)-P:%d", result.r, result.g, result.b, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void rgb_SetIndication_Rainbow(led_message_p p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    /* Store the SRC/DST colors */
    gLedsRgb.dst_color.dword = p_msg->dst.color.dword;
    gLedsRgb.src_color.dword = p_msg->src.color.dword;

    /* Set the default tick interval to 30 ms */
    gLedsRgb.tick.interval = 3;
    gLedsRgb.tick.counter  = gLedsRgb.tick.interval;
    gLedsRgb.time.delta    = (gLedsRgb.tick.interval * LED_TASK_TICK_MS);

    /* Check the rainbow changing direction */
    if (0 == (p_msg->src.color.a ^ p_msg->dst.color.a))
    {
        /* The direction is set incorrectly - get the current color */
        LED_Strip_RGB_GetAverageColor(&gLedsRgb.src_color);
        /* Set the default direction */
        gLedsRgb.dst_color.a = 1;
        gLedsRgb.src_color.a = 0;
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        gLedsRgb.time.interval = p_msg->interval;
        gLedsRgb.time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        gLedsRgb.time.interval = MIN_TRANSITION_TIME_MS;
        gLedsRgb.time.duration = 0;
    }
    gLedsRgb.fp_iterate = rgb_IterateIndication_Rainbow;
    gLedsRgb.fp_iterate();
}

//-------------------------------------------------------------------------------------------------
//--- Sine Color Indication -----------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void rgb_IterateIndication_Sine(void)
{
    led_color_t  result  = {0};
    double       percent = (1.0 * gLedsRgb.time.duration / gLedsRgb.time.interval);

    if ((gLedsRgb.dst_color.dword != gLedsRgb.src_color.dword) &&
        (gLedsRgb.time.duration < gLedsRgb.time.interval))
    {
        percent = sin(percent * gPi);
        rgb_SmoothColorTransition(&gLedsRgb.src_color, &gLedsRgb.dst_color, percent, &result);
        gLedsRgb.time.duration += gLedsRgb.time.delta;
    }
    else
    {
        result.dword             = gLedsRgb.src_color.dword;
        gLedsRgb.dst_color.dword = gLedsRgb.src_color.dword;
        gLedsRgb.command         = LED_CMD_EMPTY;
    }
    LED_Strip_RGB_SetColor(&result);
    LED_Strip_RGB_Update();

    LED_LOGI("C(%d.%d.%d)-P:%d", result.r, result.g, result.b, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void rgb_SetIndication_Sine(led_message_p p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    gLedsRgb.dst_color.dword = 0;
    gLedsRgb.dst_color.r     = p_msg->dst.color.r;
    gLedsRgb.dst_color.g     = p_msg->dst.color.g;
    gLedsRgb.dst_color.b     = p_msg->dst.color.b;

    /* Set the default tick interval to 30 ms */
    gLedsRgb.tick.interval = 3;
    gLedsRgb.tick.counter  = gLedsRgb.tick.interval;
    gLedsRgb.time.delta    = (gLedsRgb.tick.interval * LED_TASK_TICK_MS);

    /* Determine the SRC color */
    gLedsRgb.src_color.dword = 0;
    if (0 == p_msg->src.color.a)
    {
        LED_Strip_RGB_GetAverageColor(&gLedsRgb.src_color);
    }
    else
    {
        gLedsRgb.src_color.r = p_msg->src.color.r;
        gLedsRgb.src_color.g = p_msg->src.color.g;
        gLedsRgb.src_color.b = p_msg->src.color.b;
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        gLedsRgb.time.interval = p_msg->interval;
        gLedsRgb.time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        gLedsRgb.time.interval = MIN_TRANSITION_TIME_MS;
        gLedsRgb.time.duration = 0;
    }
    gLedsRgb.fp_iterate   = rgb_IterateIndication_Sine;
    gLedsRgb.fp_iterate();
}

//-------------------------------------------------------------------------------------------------

static void rgb_ProcessMsg(led_message_p p_msg)
{
    gLedsRgb.command = p_msg->command;
    switch (gLedsRgb.command)
    {
        case LED_CMD_RGB_INDICATE_COLOR:
            rgb_SetIndication_Color(p_msg);
            break;
        case LED_CMD_RGB_INDICATE_RGB_CIRCULATION:
            rgb_SetIndication_RgbCirculation(p_msg);
            break;
        case LED_CMD_RGB_INDICATE_FADE:
            rgb_SetIndication_Fade(p_msg);
            break;
        case LED_CMD_RGB_INDICATE_PINGPONG:
            rgb_SetIndication_PingPong(p_msg);
            break;
        case LED_CMD_RGB_INDICATE_RAINBOW_CIRCULATION:
            rgb_SetIndication_RainbowCirculation(p_msg);
            break;
        case LED_CMD_RGB_INDICATE_RAINBOW:
            rgb_SetIndication_Rainbow(p_msg);
            break;
        case LED_CMD_RGB_INDICATE_SINE:
            rgb_SetIndication_Sine(p_msg);
            break;
        default:
            gLedsRgb.command       = LED_CMD_EMPTY;
            gLedsRgb.fp_iterate    = NULL;
            gLedsRgb.tick.interval = 0;
            gLedsRgb.tick.counter  = 0;
            break;
    }
}

//-------------------------------------------------------------------------------------------------

static void rgb_Process(void)
{
    if (LED_CMD_EMPTY == gLedsRgb.command) return;

    gLedsRgb.tick.counter--;
    if (0 == gLedsRgb.tick.counter)
    {
        if (NULL != gLedsRgb.fp_iterate)
        {
            gLedsRgb.fp_iterate();
        }
        gLedsRgb.tick.counter = gLedsRgb.tick.interval;
    }
}

//-------------------------------------------------------------------------------------------------

static void vRGB_Task(void * pvParameters)
{
    enum
    {
        INIT_DELAY = 50,
    };
    BaseType_t    status = pdFAIL;
    led_message_t msg    = {0};

    LED_LOGI("LED RGB Task started...");

    LED_Strip_RGB_Init(gLedsRgb.buffer, sizeof(gLedsRgb.buffer));
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
        status = xQueueReceive(gLedsRgbQueue, (void *)&msg, pdMS_TO_TICKS(LED_TASK_TICK_MS));

        if (pdTRUE == status)
        {
            rgb_ProcessMsg(&msg);
        }
        else
        {
            rgb_Process();
        }
    }
}

//-------------------------------------------------------------------------------------------------

/* Calculates smooth brightness transition between two values */
static uint8_t uwf_SmoothBrightnessTransition(uint8_t a, uint8_t b, double prgs)
{
    /* Clamp progress value between 0 and 1 */
    if (prgs < 0) prgs = 0;
    if (prgs > 1) prgs = 1;

    /* Interpolate brightness */
    return (uint8_t)led_LinearInterpolation(a, b, prgs);
}

//-------------------------------------------------------------------------------------------------
//--- Simple Brightness Indication ----------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void uwf_IterateIndication_Brightness(leds_p p_leds)
{
    uint8_t value   = 0;
    double  percent = (1.0 * p_leds->time.duration / p_leds->time.interval);

    if ((p_leds->dst != p_leds->src) && (p_leds->time.duration < p_leds->time.interval))
    {
        value = uwf_SmoothBrightnessTransition(p_leds->src, p_leds->dst, percent);
        p_leds->time.duration += p_leds->time.delta;
    }
    else
    {
        value           = p_leds->dst;
        p_leds->src     = p_leds->dst;
        p_leds->command = LED_CMD_EMPTY;
    }
    p_leds->fp_set(value);

    LED_LOGI("Br:V(%d)-P:%d", value, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void uwf_SetIndication_Brightness(leds_p p_leds, led_message_p p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    p_leds->command = p_msg->command;
    p_leds->dst     = p_msg->dst.brightness.v;

    /* Set the default tick interval to 30 ms */
    p_leds->tick.interval = 3;
    p_leds->tick.counter  = p_leds->tick.interval;
    p_leds->time.delta    = (p_leds->tick.interval * LED_TASK_TICK_MS);

    /* Determine the SRC brightness */
    p_leds->src = p_msg->src.brightness.v;
    if (0 == p_msg->src.brightness.a)
    {
        p_leds->src = p_leds->fp_get();
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        p_leds->time.interval = p_msg->interval;
        p_leds->time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        p_leds->time.interval = MIN_TRANSITION_TIME_MS;
        p_leds->time.duration = 0;
    }
    p_leds->fp_iterate();
}

//-------------------------------------------------------------------------------------------------

static void ultraviolet_IterateIndication_Brightness(void)
{
    uwf_IterateIndication_Brightness(&gLedsUwf.u);
}

//-------------------------------------------------------------------------------------------------

static void white_IterateIndication_Brightness(void)
{
    uwf_IterateIndication_Brightness(&gLedsUwf.w);
}

//-------------------------------------------------------------------------------------------------

static void fito_IterateIndication_Brightness(void)
{
    uwf_IterateIndication_Brightness(&gLedsUwf.f);
}

//-------------------------------------------------------------------------------------------------
//--- Sine Brightness Indication ------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void uwf_IterateIndication_Sine(leds_p p_leds)
{
    uint8_t value  = 0;
    double  percent = (1.0 * p_leds->time.duration / p_leds->time.interval);

    if ((p_leds->dst != p_leds->src) && (p_leds->time.duration < p_leds->time.interval))
    {
        percent = sin(percent * gPi);
        value   = uwf_SmoothBrightnessTransition(p_leds->src, p_leds->dst, percent);
        p_leds->time.duration += p_leds->time.delta;
    }
    else
    {
        value           = p_leds->src;
        p_leds->dst     = p_leds->src;
        p_leds->command = LED_CMD_EMPTY;
    }
    p_leds->fp_set(value);

    LED_LOGI("Sn:V(%d)-P:%d", value, (int)(100 * percent));
}

//-------------------------------------------------------------------------------------------------

static void uwf_SetIndication_Sine(leds_p p_leds, led_message_p p_msg)
{
    enum
    {
        MIN_TRANSITION_TIME_MS = 1000,
    };

    p_leds->command = p_msg->command;
    p_leds->dst     = p_msg->dst.brightness.v;

    /* Set the default tick interval to 30 ms */
    p_leds->tick.interval = 3;
    p_leds->tick.counter  = p_leds->tick.interval;
    p_leds->time.delta    = (p_leds->tick.interval * LED_TASK_TICK_MS);

    /* Determine the SRC brightness */
    p_leds->src = p_msg->src.brightness.v;
    if (0 == p_msg->src.brightness.a)
    {
        p_leds->src = p_leds->fp_get();
    }

    /* Calculate the timer parameters */
    if ((MIN_TRANSITION_TIME_MS < p_msg->interval) && (p_msg->duration < p_msg->interval))
    {
        /* Use timings from the request */
        p_leds->time.interval = p_msg->interval;
        p_leds->time.duration = p_msg->duration;
    }
    else
    {
        /* Use default timings */
        p_leds->time.interval = MIN_TRANSITION_TIME_MS;
        p_leds->time.duration = 0;
    }
    p_leds->fp_iterate();
}

//-------------------------------------------------------------------------------------------------

static void ultraviolet_IterateIndication_Sine(void)
{
    uwf_IterateIndication_Sine(&gLedsUwf.u);
}

//-------------------------------------------------------------------------------------------------

static void white_IterateIndication_Sine(void)
{
    uwf_IterateIndication_Sine(&gLedsUwf.w);
}

//-------------------------------------------------------------------------------------------------

static void fito_IterateIndication_Sine(void)
{
    uwf_IterateIndication_Sine(&gLedsUwf.f);
}

//-------------------------------------------------------------------------------------------------

static void uwf_ProcessMsg(led_message_p p_msg)
{
    switch (p_msg->command)
    {
        case LED_CMD_UV_INDICATE_BRIGHTNESS:
            gLedsUwf.u.fp_iterate = ultraviolet_IterateIndication_Brightness;
            uwf_SetIndication_Brightness(&gLedsUwf.u, p_msg);
            break;
        case LED_CMD_UV_INDICATE_SINE:
            gLedsUwf.u.fp_iterate = ultraviolet_IterateIndication_Sine;
            uwf_SetIndication_Sine(&gLedsUwf.u, p_msg);
            break;
        case LED_CMD_W_INDICATE_BRIGHTNESS:
            gLedsUwf.w.fp_iterate = white_IterateIndication_Brightness;
            uwf_SetIndication_Brightness(&gLedsUwf.w, p_msg);
            break;
        case LED_CMD_W_INDICATE_SINE:
            gLedsUwf.w.fp_iterate = white_IterateIndication_Sine;
            uwf_SetIndication_Sine(&gLedsUwf.w, p_msg);
            break;
        case LED_CMD_F_INDICATE_BRIGHTNESS:
            gLedsUwf.f.fp_iterate = fito_IterateIndication_Brightness;
            uwf_SetIndication_Brightness(&gLedsUwf.f, p_msg);
            break;
        case LED_CMD_F_INDICATE_SINE:
            gLedsUwf.f.fp_iterate = fito_IterateIndication_Sine;
            uwf_SetIndication_Sine(&gLedsUwf.f, p_msg);
            break;
        default:
            break;
    }
}

//-------------------------------------------------------------------------------------------------

static void uwf_ProcessLeds(leds_p p_leds)
{
    if (LED_CMD_EMPTY != p_leds->command)
    {
        p_leds->tick.counter--;
        if (0 == p_leds->tick.counter)
        {
            if (NULL != p_leds->fp_iterate)
            {
                p_leds->fp_iterate();
            }
            p_leds->tick.counter = p_leds->tick.interval;
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void uwf_Process(void)
{
    uwf_ProcessLeds(&gLedsUwf.u);
    uwf_ProcessLeds(&gLedsUwf.w);
    uwf_ProcessLeds(&gLedsUwf.f);
}

//-------------------------------------------------------------------------------------------------

static void vUWF_Task(void * pvParameters)
{
    enum
    {
        INIT_DELAY = 50,
    };
    BaseType_t    status = pdFAIL;
    led_message_t msg    = {0};

    LED_LOGI("LED UWF Task started...");

    LED_Strip_UWF_Init();
    vTaskDelay(pdMS_TO_TICKS(INIT_DELAY));

    gLedsUwf.u.fp_get = LED_Strip_U_GetBrightness;
    gLedsUwf.u.fp_set = LED_Strip_U_SetBrightness;
    gLedsUwf.w.fp_get = LED_Strip_W_GetBrightness;
    gLedsUwf.w.fp_set = LED_Strip_W_SetBrightness;
    gLedsUwf.f.fp_get = LED_Strip_F_GetBrightness;
    gLedsUwf.f.fp_set = LED_Strip_F_SetBrightness;

    while (FW_TRUE)
    {
        status = xQueueReceive(gLedsUwfQueue, (void *)&msg, pdMS_TO_TICKS(LED_TASK_TICK_MS));

        if (pdTRUE == status)
        {
            uwf_ProcessMsg(&msg);
        }
        else
        {
            uwf_Process();
        }
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Task_Init(void)
{
    gLedsRgbQueue = xQueueCreate(20, sizeof(led_message_t));
    gLedsUwfQueue = xQueueCreate(20, sizeof(led_message_t));

    (void)xTaskCreatePinnedToCore(vRGB_Task, "LED RGB", 4096, NULL, 10, NULL, CORE1);
    (void)xTaskCreatePinnedToCore(vUWF_Task, "LED UWF", 4096, NULL, 10, NULL, CORE1);
}

//-------------------------------------------------------------------------------------------------

void LED_Task_SendMsg(led_message_p p_msg)
{
    LED_LOGI
    (
        "Msg->C:%d-S(%d.%d.%d.%d)-D(%d.%d.%d.%d)-I:%d",
        p_msg->command,
        p_msg->src_color.r, p_msg->src_color.g, p_msg->src_color.b, p_msg->src_color.a,
        p_msg->dst_color.r, p_msg->dst_color.g, p_msg->dst_color.b, p_msg->dst_color.a,
        (int)p_msg->interval
    );

    if ((LED_CMD_RGB_INDICATE_COLOR <= p_msg->command) && 
        (LED_CMD_RGB_SWITCH_OFF >= p_msg->command))
    {
        (void)xQueueSendToBack(gLedsRgbQueue, (void *)p_msg, (TickType_t)0);
    }
    else
    {
        (void)xQueueSendToBack(gLedsUwfQueue, (void *)p_msg, (TickType_t)0);
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Task_DetermineColor(led_message_p p_msg, led_color_p p_color)
{
    double percent = (1.0 * p_msg->duration / p_msg->interval);

    p_color->dword = 0;

    if (p_msg->duration < p_msg->interval)
    {
        switch (p_msg->command)
        {
            case LED_CMD_RGB_INDICATE_RAINBOW:
                rgb_RainbowColorTransition(&p_msg->src.color, &p_msg->dst.color, percent, p_color);
                break;
            case LED_CMD_RGB_INDICATE_SINE:
                percent = sin(percent * gPi);
                rgb_SmoothColorTransition(&p_msg->src.color, &p_msg->dst.color, percent, p_color);
                break;
            case LED_CMD_UV_INDICATE_SINE:
                /* No break */
            case LED_CMD_W_INDICATE_SINE:
                /* No break */
            case LED_CMD_F_INDICATE_SINE:
                percent = sin(percent * gPi);
                p_color->a = uwf_SmoothBrightnessTransition
                             (p_msg->src.brightness.v, p_msg->dst.brightness.v, percent);
                break;
            case LED_CMD_UV_INDICATE_BRIGHTNESS:
                /* No break */
            case LED_CMD_W_INDICATE_BRIGHTNESS:
                /* No break */
            case LED_CMD_F_INDICATE_BRIGHTNESS:
                p_color->a = uwf_SmoothBrightnessTransition
                             (p_msg->src.brightness.v, p_msg->dst.brightness.v, percent);
                break;
            default:
                rgb_SmoothColorTransition(&p_msg->src.color, &p_msg->dst.color, percent, p_color);
                break;
        }
    }
}

//-------------------------------------------------------------------------------------------------

void LED_Task_GetCurrentColor(led_color_p p_color)
{
    /* This call is not thread safe but this is acceptable */
    LED_Strip_RGB_GetAverageColor(p_color);
}

//-------------------------------------------------------------------------------------------------

uint8_t LED_Task_GetCurrentUltraViolet(void)
{
    /* This call is not thread safe but this is acceptable */
    return LED_Strip_U_GetBrightness();
}

//-------------------------------------------------------------------------------------------------

uint8_t LED_Task_GetCurrentWhite(void)
{
    /* This call is not thread safe but this is acceptable */
    return LED_Strip_W_GetBrightness();
}

//-------------------------------------------------------------------------------------------------

uint8_t LED_Task_GetCurrentFito(void)
{
    /* This call is not thread safe but this is acceptable */
    return LED_Strip_F_GetBrightness();
}

//-------------------------------------------------------------------------------------------------
//--- Tests ---------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

static void rgb_Test_Color(void)
{
    led_message_t led_msg = {0};

    /* Default transition to DST color about 3000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst.color.r     = 255;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Transition to DST color about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
    /* To - Green */
    led_msg.dst.color.r     = 0;
    led_msg.dst.color.g     = 255;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 1;
    /* From - Ignored */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 0;
    led_msg.src.color.b     = 255;
    led_msg.src.color.a     = 0;
    led_msg.interval        = 8000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));

    /* Transition from SRC to DST color about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst.color.r     = 255;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 1;
    /* From - Blue */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 0;
    led_msg.src.color.b     = 255;
    led_msg.src.color.a     = 1;
    led_msg.interval        = 8000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));

    /* Transition from SRC to DST color about 4000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_COLOR;
    /* To - Red */
    led_msg.dst.color.r     = 255;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 1;
    /* From - Blue with 50% */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 0;
    led_msg.src.color.b     = 255;
    led_msg.src.color.a     = 1;
    led_msg.interval        = 8000;
    led_msg.duration        = 4000;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void rgb_Test_RgbCirculation(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RGB_CIRCULATION;
    /* To - Ignored */
    led_msg.dst.color.r     = 0;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void rgb_Test_Fade(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_FADE;
    /* To - Some */
    led_msg.dst.color.r     = 160;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 130;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void rgb_Test_PingPong(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_PINGPONG;
    /* To - Some */
    led_msg.dst.color.r     = 160;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 130;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void rgb_Test_RainbowCirculation(void)
{
    led_message_t led_msg = {0};

    /* Static Rainbow */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RAINBOW_CIRCULATION;
    /* To - Some */
    led_msg.dst.color.r     = 90;
    led_msg.dst.color.g     = 90;
    led_msg.dst.color.b     = 90;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Rainbow Circulation */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RAINBOW_CIRCULATION;
    /* To - Some */
    led_msg.dst.color.r     = 0;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.dword = 0;
    led_msg.interval        = 0;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(5000));
}

//-------------------------------------------------------------------------------------------------

static void rgb_Test_Rainbow(void)
{
    led_message_t led_msg = {0};

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RAINBOW;
    /* To - Green */
    led_msg.dst.color.r     = 0;
    led_msg.dst.color.g     = 255;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 0;
    led_msg.src.color.b     = 0;
    led_msg.src.color.a     = 0;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RAINBOW;
    /* To - Red */
    led_msg.dst.color.r     = 255;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 0;
    led_msg.src.color.b     = 0;
    led_msg.src.color.a     = 0;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RAINBOW;
    /* To - Green */
    led_msg.dst.color.r     = 0;
    led_msg.dst.color.g     = 255;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 0;
    led_msg.src.color.b     = 0;
    led_msg.src.color.a     = 0;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RAINBOW;
    /* To - Red */
    led_msg.dst.color.r     = 255;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 255;
    led_msg.src.color.b     = 0;
    led_msg.src.color.a     = 1;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));

    /* Transition from SRC to DST color about 15000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_RAINBOW;
    /* To - Red */
    led_msg.dst.color.r     = 255;
    led_msg.dst.color.g     = 0;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 0;
    /* From - Ignored */
    led_msg.src.color.r     = 0;
    led_msg.src.color.g     = 255;
    led_msg.src.color.b     = 0;
    led_msg.src.color.a     = 1;
    led_msg.interval        = 15000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(16000));
}

//-------------------------------------------------------------------------------------------------

static void rgb_Test_Sine(void)
{
    led_message_t led_msg = {0};

    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command         = LED_CMD_RGB_INDICATE_SINE;
    /* To - Some */
    led_msg.dst.color.r     = 0;
    led_msg.dst.color.g     = 255;
    led_msg.dst.color.b     = 0;
    led_msg.dst.color.a     = 1;
    /* From - Ignored */
    led_msg.src.color.r     = 255;
    led_msg.src.color.g     = 0;
    led_msg.src.color.b     = 0;
    led_msg.src.color.a     = 1;
    led_msg.interval        = 6000;
    led_msg.duration        = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(7000));
}

//-------------------------------------------------------------------------------------------------

static void uwf_Test_Brightness(void)
{
    led_message_t led_msg = {0};

    /* Transition to DST UV brightness about 3000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command          = LED_CMD_UV_INDICATE_BRIGHTNESS;
    led_msg.dst.brightness.v = 100;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 1;
    led_msg.interval         = 3000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Transition to DST UV brightness about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command          = LED_CMD_UV_INDICATE_BRIGHTNESS;
    led_msg.dst.brightness.v = 0;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 8000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));

    /* Transition to DST W brightness about 3000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command          = LED_CMD_W_INDICATE_BRIGHTNESS;
    led_msg.dst.brightness.v = 100;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 3000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Transition to DST W brightness about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command          = LED_CMD_W_INDICATE_BRIGHTNESS;
    led_msg.dst.brightness.v = 0;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 8000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));

    /* Transition to DST F brightness about 3000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command          = LED_CMD_F_INDICATE_BRIGHTNESS;
    led_msg.dst.brightness.v = 100;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 3000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Transition to DST F brightness about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command          = LED_CMD_F_INDICATE_BRIGHTNESS;
    led_msg.dst.brightness.v = 0;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 8000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));

    /* Transition to DST UV/W/F brightness about 3000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.dst.brightness.v = 100;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 3000;
    led_msg.duration         = 0;
    led_msg.command          = LED_CMD_UV_INDICATE_BRIGHTNESS;
    LED_Task_SendMsg(&led_msg);
    led_msg.command          = LED_CMD_W_INDICATE_BRIGHTNESS;
    LED_Task_SendMsg(&led_msg);
    led_msg.command          = LED_CMD_F_INDICATE_BRIGHTNESS;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Transition to DST UV/W/F brightness about 8000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.dst.brightness.v = 0;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 8000;
    led_msg.duration         = 0;
    led_msg.command          = LED_CMD_UV_INDICATE_BRIGHTNESS;
    LED_Task_SendMsg(&led_msg);
    led_msg.command          = LED_CMD_W_INDICATE_BRIGHTNESS;
    LED_Task_SendMsg(&led_msg);
    led_msg.command          = LED_CMD_F_INDICATE_BRIGHTNESS;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(9000));
}

//-------------------------------------------------------------------------------------------------

static void uwf_Test_Sine(void)
{
    led_message_t led_msg = {0};

    /* Transition to DST UV brightness about 5000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command     = LED_CMD_UV_INDICATE_SINE;
    led_msg.dst.brightness.v = 200;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 5000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(6000));

    /* Transition to DST W brightness about 5000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command     = LED_CMD_W_INDICATE_SINE;
    led_msg.dst.brightness.v = 100;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 5000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(6000));

    /* Transition to DST F brightness about 5000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.command          = LED_CMD_F_INDICATE_SINE;
    led_msg.dst.brightness.v = 150;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 5000;
    led_msg.duration         = 0;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(6000));

    /* Transition to DST UV/W/F brightness about 3000 ms */
    memset(&led_msg, 0, sizeof(led_msg));
    led_msg.dst.brightness.v = 100;
    led_msg.dst.brightness.a = 1;
    led_msg.src.brightness.v = 0;
    led_msg.src.brightness.a = 0;
    led_msg.interval         = 3000;
    led_msg.duration         = 0;
    led_msg.command          = LED_CMD_UV_INDICATE_SINE;
    LED_Task_SendMsg(&led_msg);
    led_msg.command          = LED_CMD_W_INDICATE_SINE;
    LED_Task_SendMsg(&led_msg);
    led_msg.command          = LED_CMD_F_INDICATE_SINE;
    LED_Task_SendMsg(&led_msg);
    vTaskDelay(pdMS_TO_TICKS(4000));
}

//-------------------------------------------------------------------------------------------------

void LED_Task_Test(void)
{
    rgb_Test_Color();
    rgb_Test_RgbCirculation();
    rgb_Test_Fade();
    rgb_Test_PingPong();
    rgb_Test_RainbowCirculation();
    rgb_Test_Rainbow();
    rgb_Test_Sine();
    uwf_Test_Brightness();
    uwf_Test_Sine();
}

//-------------------------------------------------------------------------------------------------
