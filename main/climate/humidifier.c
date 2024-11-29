#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include "humidifier.h"
#include "i2c.h"

//-------------------------------------------------------------------------------------------------

#define HUMDFR_LOG  0

#if (1 == HUMDFR_LOG)
static const char * gTAG = "HUMIDIFIER";
#    define HUMDFR_LOGI(...)  ESP_LOGI(gTAG, __VA_ARGS__)
#    define HUMDFR_LOGE(...)  ESP_LOGE(gTAG, __VA_ARGS__)
#    define HUMDFR_LOGV(...)  ESP_LOGV(gTAG, __VA_ARGS__)
#else
#    define HUMDFR_LOGI(...)
#    define HUMDFR_LOGE(...)
#    define HUMDFR_LOGV(...)
#endif

//-------------------------------------------------------------------------------------------------

enum
{
    BME280_I2C_ADDRESS        = 0xEC,
    BME280_I2C_SPEED_HZ       = 100000,
    /* Specific values */
    BME280_CHIP_ID_VALUE      = 0x60,
    BME280_SOFT_RESET_VALUE   = 0xB6,
    /* Register offsets */
    BME280_REG_ADDR_DIG_T1    = 0x88,
    BME280_REG_ADDR_DIG_T2    = 0x8A,
    BME280_REG_ADDR_DIG_T3    = 0x8C,
    BME280_REG_ADDR_DIG_P1    = 0x8E,
    BME280_REG_ADDR_DIG_P2    = 0x90,
    BME280_REG_ADDR_DIG_P3    = 0x92,
    BME280_REG_ADDR_DIG_P4    = 0x94,
    BME280_REG_ADDR_DIG_P5    = 0x96,
    BME280_REG_ADDR_DIG_P6    = 0x98,
    BME280_REG_ADDR_DIG_P7    = 0x9A,
    BME280_REG_ADDR_DIG_P8    = 0x9C,
    BME280_REG_ADDR_DIG_P9    = 0x9E,
    BME280_REG_ADDR_DIG_H1    = 0xA1,
    BME280_REG_ADDR_DIG_H2    = 0xE1,
    BME280_REG_ADDR_DIG_H3    = 0xE3,
    BME280_REG_ADDR_DIG_H4    = 0xE4,
    BME280_REG_ADDR_DIG_H5    = 0xE5,
    BME280_REG_ADDR_DIG_H6    = 0xE7,
    BME280_REG_ADDR_CHIPID    = 0xD0,
    BME280_REG_ADDR_VERSION   = 0xD1,
    BME280_REG_ADDR_SOFTRESET = 0xE0,
    BME280_REG_ADDR_CTRL_HUM  = 0xF2,
    BME280_REG_ADDR_STATUS    = 0XF3,
    BME280_REG_ADDR_CTRL_MEAS = 0xF4,
    BME280_REG_ADDR_CONFIG    = 0xF5,
    BME280_REG_ADDR_PRESS     = 0xF7,
    BME280_REG_ADDR_TEMP      = 0xFA,
    BME280_REG_ADDR_HUM       = 0xFD,
};

typedef struct
{
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
    uint8_t  none;
    uint8_t  dig_H1;
} bme280_nvm_calibration_part_a_t;

typedef struct
{
    int16_t dig_H2;
    uint8_t dig_H3;
    int8_t  dig_H4_msb;
    struct
    {
        uint8_t dig_H4_lsb : 4;
        uint8_t dig_H5_lsb : 4;
    };
    int8_t  dig_H5_msb;
    int8_t  dig_H6;
    uint8_t none;
} bme280_nvm_calibration_part_b_t;

typedef struct
{
    uint16_t T1;
    int16_t  T2;
    int16_t  T3;
    uint16_t P1;
    int16_t  P2;
    int16_t  P3;
    int16_t  P4;
    int16_t  P5;
    int16_t  P6;
    int16_t  P7;
    int16_t  P8;
    int16_t  P9;
    uint16_t H1;
    int16_t  H2;
    uint16_t H3;
    int16_t  H4;
    int16_t  H5;
    int16_t  H6;
} bme280_calibration_t;

typedef struct
{
    uint8_t press_msb;
    uint8_t press_lsb;
    struct
    {
        uint8_t press_none : 4;
        uint8_t press_xlsb : 4;
    };
    uint8_t temp_msb;
    uint8_t temp_lsb;
    struct
    {
        uint8_t temp_none : 4;
        uint8_t temp_xlsb : 4;
    };
    uint8_t hum_msb;
    uint8_t hum_lsb;
} bme280_vm_measurement_t;

typedef struct
{
    uint32_t pressure;
    int16_t  temperature;
    uint16_t humidity;
} bme280_measurement_t;

typedef enum
{
    BME280_SAMPLING_NONE = 0b000,
    BME280_SAMPLING_X1   = 0b001,
    BME280_SAMPLING_X2   = 0b010,
    BME280_SAMPLING_X4   = 0b011,
    BME280_SAMPLING_X8   = 0b100,
    BME280_SAMPLING_X16  = 0b101,
} bme280_sampling_t;

typedef enum
{
    BME280_MODE_SLEEP  = 0b00,
    BME280_MODE_FORCED = 0b01,
    BME280_MODE_NORMAL = 0b11,
} bme280_mode_t;

typedef enum
{
    BME280_FILTER_OFF = 0b000,
    BME280_FILTER_X2  = 0b001,
    BME280_FILTER_X4  = 0b010,
    BME280_FILTER_X8  = 0b011,
    BME280_FILTER_X16 = 0b100,
} bme280_filter_t;

typedef enum
{
    BME280_STANDBY_MS_0_5  = 0b000,
    BME280_STANDBY_MS_10   = 0b110,
    BME280_STANDBY_MS_20   = 0b111,
    BME280_STANDBY_MS_62_5 = 0b001,
    BME280_STANDBY_MS_125  = 0b010,
    BME280_STANDBY_MS_250  = 0b011,
    BME280_STANDBY_MS_500  = 0b100,
    BME280_STANDBY_MS_1000 = 0b101,
} bme280_standby_duration_t;

/* The config register */
typedef union
{
    struct
    {
        uint8_t spi3w_en : 1;
        /* Unused - don't set */
        uint8_t none : 1;
        /* Filter settings */
        /* 000 = Filter off */
        /* 001 = 2x filter */
        /* 010 = 4x filter */
        /* 011 = 8x filter */
        /* 100 and above = 16x filter */
        uint8_t filter : 3;
        /* Inactive duration (standby time) in normal mode */
        /* 000 = 0.5 ms */
        /* 001 = 62.5 ms */
        /* 010 = 125 ms */
        /* 011 = 250 ms */
        /* 100 = 500 ms */
        /* 101 = 1000 ms */
        /* 110 = 10 ms */
        /* 111 = 20 ms */
        uint8_t t_sb : 3;
    };
    uint8_t raw;
} bme280_config_t;
        
/* The ctrl_meas register */
typedef union 
{
    struct
    {
        /* Device mode */
        /* 00       = Sleep */
        /* 01 or 10 = Forced */
        /* 11       = Normal */
        uint8_t mode : 2;
        /* Pressure oversampling */
        /* 000 = Skipped */
        /* 001 = x1 */
        /* 010 = x2 */
        /* 011 = x4 */
        /* 100 = x8 */
        /* 101 and above = x16 */
        uint8_t osrs_p : 3;
        /* Temperature oversampling */
        /* 000 = Skipped */
        /* 001 = x1 */
        /* 010 = x2 */
        /* 011 = x4 */
        /* 100 = x8 */
        /* 101 and above = x16 */
        uint8_t osrs_t : 3;
    };
    uint8_t raw;
} bme280_ctrl_meas_t;
        
/* The ctrl_hum register */
typedef union
{
    struct
    {
        /* Humidity oversampling */
        /* 000 = Skipped */
        /* 001 = x1 */
        /* 010 = x2 */
        /* 011 = x4 */
        /* 100 = x8 */
        /* 101 and above = x16 */
        uint8_t osrs_h : 3;
        /* Unused - don't set */
        uint8_t none : 5;
    };
    uint8_t raw;
} bme280_ctrl_hum_t;

/* The status register */
typedef struct
{
    /* Updating flag */
    uint8_t im_update : 1;
    /* Unused - don't set */
    uint8_t none_a : 2;
    /* Measuring flag */
    uint8_t measuring : 1;
    /* Unused - don't set */
    uint8_t none_b : 4;
} bme280_status_t;

enum
{
    SHT41_I2C_ADDRESS     = 0x88,
    SHT41_I2C_SPEED_HZ    = 100000,
    /* Specific values */
    SHT41_MEASURE         = 0xFD,
    SHT41_SOFT_RESET      = 0x94,
    SHT41_HEAT_200MW_0P1S = 0x32,
    SHT41_HEAT_110MW_0P1S = 0x24,
};

typedef struct
{
    uint8_t temp_msb;
    uint8_t temp_lsb;
    uint8_t temp_crc;
    uint8_t hum_msb;
    uint8_t hum_lsb;
    uint8_t hum_crc;
} sht41_vm_measurement_t;

typedef struct
{
    int16_t  temperature;
    uint16_t humidity;
} sht41_measurement_t;

//-------------------------------------------------------------------------------------------------

static i2c_device_p         gBme280             = NULL;
static bme280_calibration_t gBme280Calibrartion = {0};
static bme280_measurement_t gBme280Measurement  = {0};
static bool                 gOn                 = false;
static i2c_device_p         gSht41              = NULL;
static sht41_measurement_t  gSht41Measurement   = {0};

//-------------------------------------------------------------------------------------------------

static void bme280_Rd(uint8_t offset, uint8_t * p_buffer, uint8_t length)
{
    I2C_TxRx(gBme280, &offset, sizeof(offset), p_buffer, length);
}

//-------------------------------------------------------------------------------------------------

static void bme280_Wr(uint8_t offset, uint8_t value)
{
    uint8_t buffer[sizeof(offset) + sizeof(value)] = {offset, value};
    I2C_Tx(gBme280, buffer, sizeof(buffer));
}

//-------------------------------------------------------------------------------------------------

static bool bme280_IsReadingCalibration(void)
{
    bme280_status_t status = {0};
    bme280_Rd(BME280_REG_ADDR_STATUS, (uint8_t *)&status, sizeof(status));
    return (0 != status.im_update);
}

//-------------------------------------------------------------------------------------------------

static bool bme280_IsChipIdCorrect(void)
{
    uint8_t chip_id = 0;
    bme280_Rd(BME280_REG_ADDR_CHIPID, &chip_id, sizeof(chip_id));
    return (BME280_CHIP_ID_VALUE == chip_id);
}

//-------------------------------------------------------------------------------------------------

static void bme280_SoftReset(void)
{
    bme280_Wr(BME280_REG_ADDR_SOFTRESET, BME280_SOFT_RESET_VALUE);
}

//-------------------------------------------------------------------------------------------------

static void bme280_ReadCalibration(void)
{
    bme280_nvm_calibration_part_a_t part_a = {0};
    bme280_nvm_calibration_part_b_t part_b = {0};

    bme280_Rd(BME280_REG_ADDR_DIG_T1, (uint8_t *)&part_a, sizeof(part_a));
    bme280_Rd(BME280_REG_ADDR_DIG_H2, (uint8_t *)&part_b, sizeof(part_b));

    HUMDFR_LOGI("dig_T1 = %04X - %d", part_a.dig_T1, part_a.dig_T1);
    HUMDFR_LOGI("dig_T2 = %04X - %d", part_a.dig_T2, part_a.dig_T2);
    HUMDFR_LOGI("dig_T3 = %04X - %d", part_a.dig_T3, part_a.dig_T3);
    HUMDFR_LOGI("dig_P1 = %04X - %d", part_a.dig_P1, part_a.dig_P1);
    HUMDFR_LOGI("dig_P2 = %04X - %d", part_a.dig_P2, part_a.dig_P2);
    HUMDFR_LOGI("dig_P3 = %04X - %d", part_a.dig_P3, part_a.dig_P3);
    HUMDFR_LOGI("dig_P4 = %04X - %d", part_a.dig_P4, part_a.dig_P4);
    HUMDFR_LOGI("dig_P5 = %04X - %d", part_a.dig_P5, part_a.dig_P5);
    HUMDFR_LOGI("dig_P6 = %04X - %d", part_a.dig_P6, part_a.dig_P6);
    HUMDFR_LOGI("dig_P7 = %04X - %d", part_a.dig_P7, part_a.dig_P7);
    HUMDFR_LOGI("dig_P8 = %04X - %d", part_a.dig_P8, part_a.dig_P8);
    HUMDFR_LOGI("dig_P9 = %04X - %d", part_a.dig_P9, part_a.dig_P9);
    HUMDFR_LOGI("dig_H1 = %04X - %d", part_a.dig_H1, part_a.dig_H1);

    HUMDFR_LOGI("dig_H2     = %04X - %d", part_b.dig_H2, part_b.dig_H2);
    HUMDFR_LOGI("dig_H3     = %02X - %d", part_b.dig_H3, part_b.dig_H3);
    HUMDFR_LOGI("dig_H4_msb = %02X - %d", part_b.dig_H4_msb, part_b.dig_H4_msb);
    HUMDFR_LOGI("dig_H4_lsb = %02X - %d", part_b.dig_H4_lsb, part_b.dig_H4_lsb);
    HUMDFR_LOGI("dig_H5_lsb = %02X - %d", part_b.dig_H5_lsb, part_b.dig_H5_lsb);
    HUMDFR_LOGI("dig_H5_msb = %02X - %d", part_b.dig_H5_msb, part_b.dig_H5_msb);
    HUMDFR_LOGI("dig_H6     = %02X - %d", part_b.dig_H6, part_b.dig_H6);

    gBme280Calibrartion.T1 = part_a.dig_T1;
    gBme280Calibrartion.T2 = part_a.dig_T2;
    gBme280Calibrartion.T3 = part_a.dig_T3;
    gBme280Calibrartion.P1 = part_a.dig_P1;
    gBme280Calibrartion.P2 = part_a.dig_P2;
    gBme280Calibrartion.P3 = part_a.dig_P3;
    gBme280Calibrartion.P4 = part_a.dig_P4;
    gBme280Calibrartion.P5 = part_a.dig_P5;
    gBme280Calibrartion.P6 = part_a.dig_P6;
    gBme280Calibrartion.P7 = part_a.dig_P7;
    gBme280Calibrartion.P8 = part_a.dig_P8;
    gBme280Calibrartion.P9 = part_a.dig_P9;
    gBme280Calibrartion.H1 = part_a.dig_H1;
    gBme280Calibrartion.H2 = part_b.dig_H2;
    gBme280Calibrartion.H3 = part_b.dig_H3;
    gBme280Calibrartion.H4 = ((part_b.dig_H4_msb << 4) | part_b.dig_H4_lsb);
    gBme280Calibrartion.H5 = ((part_b.dig_H5_msb << 4) | part_b.dig_H5_lsb);
    gBme280Calibrartion.H6 = part_b.dig_H6;

    HUMDFR_LOGI("-----------------------------------");
    HUMDFR_LOGI("T1 = %04X: %d", gBme280Calibrartion.T1, gBme280Calibrartion.T1);
    HUMDFR_LOGI("T2 = %04X: %d", gBme280Calibrartion.T2, gBme280Calibrartion.T2);
    HUMDFR_LOGI("T3 = %04X: %d", gBme280Calibrartion.T3, gBme280Calibrartion.T3);
    HUMDFR_LOGI("P1 = %04X: %d", gBme280Calibrartion.P1, gBme280Calibrartion.P1);
    HUMDFR_LOGI("P2 = %04X: %d", gBme280Calibrartion.P2, gBme280Calibrartion.P2);
    HUMDFR_LOGI("P3 = %04X: %d", gBme280Calibrartion.P3, gBme280Calibrartion.P3);
    HUMDFR_LOGI("P4 = %04X: %d", gBme280Calibrartion.P4, gBme280Calibrartion.P4);
    HUMDFR_LOGI("P5 = %04X: %d", gBme280Calibrartion.P5, gBme280Calibrartion.P5);
    HUMDFR_LOGI("P6 = %04X: %d", gBme280Calibrartion.P6, gBme280Calibrartion.P6);
    HUMDFR_LOGI("P7 = %04X: %d", gBme280Calibrartion.P7, gBme280Calibrartion.P7);
    HUMDFR_LOGI("P8 = %04X: %d", gBme280Calibrartion.P8, gBme280Calibrartion.P8);
    HUMDFR_LOGI("P9 = %04X: %d", gBme280Calibrartion.P9, gBme280Calibrartion.P9);
    HUMDFR_LOGI("H1 = %04X: %d", gBme280Calibrartion.H1, gBme280Calibrartion.H1);
    HUMDFR_LOGI("H2 = %04X: %d", gBme280Calibrartion.H2, gBme280Calibrartion.H2);
    HUMDFR_LOGI("H3 = %04X: %d", gBme280Calibrartion.H3, gBme280Calibrartion.H3);
    HUMDFR_LOGI("H4 = %04X: %d", gBme280Calibrartion.H4, gBme280Calibrartion.H4);
    HUMDFR_LOGI("H5 = %04X: %d", gBme280Calibrartion.H5, gBme280Calibrartion.H5);
    HUMDFR_LOGI("H6 = %04X: %d", gBme280Calibrartion.H6, gBme280Calibrartion.H6);
}

//-------------------------------------------------------------------------------------------------

static void bme280_SetSampling(void)
{
    bme280_config_t    config    = {0};
    bme280_ctrl_meas_t ctrl_meas = {0};
    bme280_ctrl_hum_t  ctrl_hum  = {0};

    /* Changes to "ctrl_hum" register only become effective after a write operation to "ctrl_meas"
     * register, otherwise the values won't be applied (see DS 5.4.3) */
    ctrl_hum.osrs_h = BME280_SAMPLING_X16;
    bme280_Wr(BME280_REG_ADDR_CTRL_HUM, ctrl_hum.raw);

    /* Config */
    config.filter = BME280_FILTER_OFF;
    config.t_sb   = BME280_STANDBY_MS_10;
    bme280_Wr(BME280_REG_ADDR_CONFIG, config.raw);
   
    /* Ctrl_meas */
    ctrl_meas.mode   = BME280_MODE_NORMAL;
    ctrl_meas.osrs_p = BME280_SAMPLING_X16;
    ctrl_meas.osrs_t = BME280_SAMPLING_X16;
    bme280_Wr(BME280_REG_ADDR_CTRL_MEAS, ctrl_meas.raw);
}

//-------------------------------------------------------------------------------------------------

static void bme280_CompensateT(uint32_t temp_adc, int32_t * p_temp_fine)
{
    int32_t var1        = 0;
    int32_t var2        = 0;
    int32_t temperature = 0;

    var1 = (int32_t)((temp_adc >> 3) - ((int32_t)gBme280Calibrartion.T1 << 1));
    var1 = ((var1 * ((int32_t)gBme280Calibrartion.T2)) >> 11);
    var2 = (int32_t)((temp_adc >> 4) - ((int32_t)gBme280Calibrartion.T1));
    var2 = ((((var2 * var2) >> 12) * ((int32_t)gBme280Calibrartion.T3)) >> 14);
    *p_temp_fine = (var1 + var2);
    temperature = ((*p_temp_fine * 5 + 128) >> 8);

    if (-4000 > temperature)
    {
        temperature = -4000;
    }
    else if (8500 < temperature)
    {
        temperature = 8500;
    }

    gBme280Measurement.temperature = temperature;

    HUMDFR_LOGI("Temperature = %d", (int)temperature);
}

//-------------------------------------------------------------------------------------------------

static void bme280_CompensateP(uint32_t press_adc, int32_t temp_fine)
{
    int64_t  var1     = 0;
    int64_t  var2     = 0;
    int64_t  var3     = 0;
    int64_t  var4     = 0;
    uint32_t pressure = 0;

    var1 = (((int64_t)temp_fine) - 128000);
    var2 = (var1 * var1 * (int64_t)gBme280Calibrartion.P6);
    var2 = (var2 + ((var1 * (int64_t)gBme280Calibrartion.P5) << 17));
    var2 = (var2 + (((int64_t)gBme280Calibrartion.P4) << 35));
    var3 = ((var1 * var1 * (int64_t)gBme280Calibrartion.P3) >> 8);
    var1 = (var3 + ((var1 * ((int64_t)gBme280Calibrartion.P2) << 12)));
    var3 = (((int64_t)1) << 47);
    var1 = ((var3 + var1) * ((int64_t)gBme280Calibrartion.P1) >> 33);

    /* To avoid divide by zero exception */
    if (var1 != 0)
    {
        var4 = (1048576 - press_adc);
        var4 = ((((var4 << 31) - var2) * 3125) / var1);
        var1 = ((((int64_t)gBme280Calibrartion.P9) * (var4 >> 13) * (var4 >> 13)) >> 25);
        var2 = ((((int64_t)gBme280Calibrartion.P8) * var4) >> 19);
        var4 = ((var4 + var1 + var2) >> 8) + (((int64_t)gBme280Calibrartion.P7) << 4);
        pressure = (uint32_t)(((var4 >> 1) * 100) >> 7);

        if (3000000 > pressure)
        {
            pressure = 3000000;
        }
        else if (11000000 < pressure)
        {
            pressure = 11000000;
        }
    }
    else
    {
        pressure = 3000000;
    }
    pressure *= 10;

    gBme280Measurement.pressure = pressure;

    HUMDFR_LOGI("Pressure    = %d", (int)pressure);
}

//-------------------------------------------------------------------------------------------------

static void bme280_CompensateH(uint32_t hum_adc, int32_t temp_fine)
{
    int32_t  var1         = 0;
    int32_t  var2         = 0;
    int32_t  var3         = 0;
    int32_t  var4         = 0;
    int32_t  var5         = 0;
    uint32_t humidity     = 0;

    var1 = (temp_fine - ((int32_t)76800));
    var2 = (int32_t)(hum_adc << 14);
    var3 = (int32_t)(((int32_t)gBme280Calibrartion.H4) << 20);
    var4 = (((int32_t)gBme280Calibrartion.H5) * var1);
    var5 = ((((var2 - var3) - var4) + (int32_t)16384) >> 15);
    var2 = ((var1 * ((int32_t)gBme280Calibrartion.H6)) >> 10);
    var3 = ((var1 * ((int32_t)gBme280Calibrartion.H3)) >> 11);
    var4 = (((var2 * (var3 + (int32_t)32768)) / 1024) + (int32_t)2097152);
    var2 = (((var4 * ((int32_t)gBme280Calibrartion.H2)) + 8192) >> 14);
    var3 = (var5 * var2);
    var4 = (((var3 >> 15) * (var3 >> 15)) >> 7);
    var5 = (var3 - ((var4 * ((int32_t)gBme280Calibrartion.H1)) >> 4));
    var5 = (var5 < 0 ? 0 : var5);
    var5 = (var5 > 419430400 ? 419430400 : var5);
    humidity = (uint32_t)(var5 >> 12);

    if (102400 < humidity)
    {
        humidity = 102400;
    }

    humidity = (uint32_t)(100.0 * humidity / 1024);

    gBme280Measurement.humidity = humidity;

    HUMDFR_LOGI("Humidity    = %d", (int)humidity);
}

//-------------------------------------------------------------------------------------------------

static void bme280_Readout(void)
{
    enum
    {
        DISABLED_20BITS = 0x80000,
        DISABLED_16BITS = 0x8000,
    };
    bme280_vm_measurement_t meas      = {0};
    uint32_t                press_adc = 0;
    uint32_t                temp_adc  = 0;
    uint32_t                hum_adc   = 0;
    int32_t                 temp_fine = 0;

    /* Burst read */
    bme280_Rd(BME280_REG_ADDR_PRESS, (uint8_t *)&meas, sizeof(meas));

    press_adc = ((meas.press_msb << 12) | (meas.press_lsb << 4) | (meas.press_xlsb));
    temp_adc  = ((meas.temp_msb << 12) | (meas.temp_lsb << 4) | (meas.temp_xlsb));
    hum_adc   = ((meas.hum_msb << 8) | (meas.hum_lsb));

    HUMDFR_LOGI("P ADC = %08X", (int)press_adc);
    HUMDFR_LOGI("T ADC = %08X", (int)temp_adc);
    HUMDFR_LOGI("H ADC = %08X", (int)hum_adc);

    if (DISABLED_20BITS != temp_adc)
    {
        bme280_CompensateT(temp_adc, &temp_fine);

        if (DISABLED_20BITS != press_adc)
        {
            bme280_CompensateP(press_adc, temp_fine);
        }
        if (DISABLED_16BITS != hum_adc)
        {
            bme280_CompensateH(hum_adc, temp_fine);
        }
    }
}

//-------------------------------------------------------------------------------------------------

static void bme280_Init(void)
{
    enum
    {
        WAKE_UP_DELAY = 300,
        WAITING_DELAY = 50,
    };

    if (NULL == gBme280)
    {
        i2c_device_config_t bme280_dvc_config =
        {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = (BME280_I2C_ADDRESS >> 1),
            .scl_speed_hz    = BME280_I2C_SPEED_HZ,
            .scl_wait_us     = 0,
        };

        HUMDFR_LOGI("Init the BME280 I2C");
        I2C_Init();

        gBme280 = I2C_AddDevice(&bme280_dvc_config);

        /* Check if BME280 chip ID is correct */
        if (false == bme280_IsChipIdCorrect())
        {
            HUMDFR_LOGE("Incorrect BME280 chip ID!");
            ESP_ERROR_CHECK(ESP_FAIL);
        }

        /* Reset the BME280 using soft-reset. This makes sure the IIR is off, etc. */
        bme280_SoftReset();

        /* Wait for chip to wake up */
        vTaskDelay(pdMS_TO_TICKS(WAKE_UP_DELAY));

        /* If the chip is still reading calibration - delay */
        while (true == bme280_IsReadingCalibration())
        {
            vTaskDelay(pdMS_TO_TICKS(WAITING_DELAY));
        }

        /* Read trimming parameters, see DS 4.2.2 */
        bme280_ReadCalibration();
    }

    HUMDFR_LOGI("Init of BME280 is finished");
}

//-------------------------------------------------------------------------------------------------

static void bme280_StartMeasuring(void)
{
    enum
    {
        WAKE_UP_DELAY = 300,
        WAITING_DELAY = 50,
    };

    /* Check if BME280 chip ID is correct */
    if (false == bme280_IsChipIdCorrect())
    {
        HUMDFR_LOGE("Incorrect BME280 chip ID!");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    /* Reset the BME280 using soft-reset. This makes sure the IIR is off, etc. */
    bme280_SoftReset();
    /* Wait for chip to wake up */
    vTaskDelay(pdMS_TO_TICKS(WAKE_UP_DELAY));
    /* If the chip is still reading calibration - delay */
    while (true == bme280_IsReadingCalibration())
    {
        vTaskDelay(pdMS_TO_TICKS(WAITING_DELAY));
    }
    /* Start measuring */
    bme280_SetSampling();
    /* Wait for the first measurements are ready */
    vTaskDelay(pdMS_TO_TICKS(WAKE_UP_DELAY));
    /* Readuot the measurements */
    bme280_Readout();
}

//-------------------------------------------------------------------------------------------------

static void sht41_Rd(uint8_t * p_buffer, uint8_t length)
{
    I2C_Rx(gSht41, p_buffer, length);
}

//-------------------------------------------------------------------------------------------------

static void sht41_Wr(uint8_t value)
{
    uint8_t buffer[sizeof(value)] = {value};
    I2C_Tx(gSht41, buffer, sizeof(buffer));
}

//-------------------------------------------------------------------------------------------------

static void sht41_SoftReset(void)
{
    sht41_Wr(SHT41_SOFT_RESET);
}

//-------------------------------------------------------------------------------------------------

static void sht41_StartMeasuring(void)
{
    sht41_Wr(SHT41_MEASURE);
}

//-------------------------------------------------------------------------------------------------

static void sht41_Heat(void)
{
    sht41_Wr(SHT41_HEAT_110MW_0P1S);
}

//-------------------------------------------------------------------------------------------------

static void sht41_Readout(void)
{
    enum
    {
        HUMIDITY_MIN = 0,
        HUMIDITY_MAX = 10000,
    };
    sht41_vm_measurement_t meas = {0};
    int32_t                temp = 0;
    int32_t                hum  = 0;

    /* Burst read */
    sht41_Rd((uint8_t *)&meas, sizeof(meas));

    temp = (-4500 + 17500 * ((meas.temp_msb << 8) + meas.temp_lsb) / 65535);
    hum  = (-600 + 12500 * ((meas.hum_msb << 8) + meas.hum_lsb) / 65535);
    if (HUMIDITY_MIN > hum)
    {
        hum = HUMIDITY_MIN;
    }
    if (HUMIDITY_MAX < hum)
    {
        hum = HUMIDITY_MAX;
    }

    gSht41Measurement.temperature = temp;
    gSht41Measurement.humidity    = hum;

    HUMDFR_LOGI("T = %08X : %d", (int)temp, gSht41Measurement.temperature);
    HUMDFR_LOGI("H = %08X : %d", (int)hum, gSht41Measurement.humidity);
}

//-------------------------------------------------------------------------------------------------

static void sht41_Init(void)
{
    enum
    {
        WAITING_DELAY = 30,
    };

    if (NULL == gSht41)
    {
        i2c_device_config_t sht41_dvc_config =
        {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = (SHT41_I2C_ADDRESS >> 1),
            .scl_speed_hz    = SHT41_I2C_SPEED_HZ,
            .scl_wait_us     = 0,
        };

        HUMDFR_LOGI("Init the SHT41 I2C");
        I2C_Init();

        gSht41 = I2C_AddDevice(&sht41_dvc_config);

        HUMDFR_LOGI("SHT41 Start measuring");
        sht41_StartMeasuring();

        vTaskDelay(pdMS_TO_TICKS(WAITING_DELAY));

        HUMDFR_LOGI("SHT41 Readout");
        sht41_Readout();
    }

    HUMDFR_LOGI("Init of SHT41 is finished");
}

//-------------------------------------------------------------------------------------------------

void Humidifier_Init(void)
{
    enum
    {
        START_UP_DELAY = 100,
    };

    /* Init the power/button pins */
    gpio_config_t humidifier_gpio_config =
    {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 
        (
            (1ULL << CONFIG_HUMIDIFIER_POWER_GPIO) |
            (1ULL << CONFIG_HUMIDIFIER_BUTTON_GPIO)
        ),
    };
    ESP_ERROR_CHECK(gpio_config(&humidifier_gpio_config));
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 0);
    gpio_set_level(CONFIG_HUMIDIFIER_BUTTON_GPIO, 0);

    /* Power on the humidifier */
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 1);

    /* Wait some time for power stabilization */
    vTaskDelay(pdMS_TO_TICKS(START_UP_DELAY));

    /* Initialize the BME280 */
    bme280_Init();

    /* Initialize the SHT41 */
    sht41_Init();

    /* Power off the humidifier */
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 0);
}

//-------------------------------------------------------------------------------------------------

void Humidifier_PowerOn(void)
{
    enum
    {
        POWER_ON_DELAY = 900,
        HEAT_DELAY     = 200,
        STARTUP_DELAY  = 200,
    };
    /* Power on the humidifier */
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(POWER_ON_DELAY));
    sht41_SoftReset();
    vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY));
    sht41_Heat();
    vTaskDelay(pdMS_TO_TICKS(HEAT_DELAY));
    bme280_StartMeasuring();
    sht41_StartMeasuring();
    vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY));
}

//-------------------------------------------------------------------------------------------------

void Humidifier_PowerOff(void)
{
    enum
    {
        POWER_OFF_DELAY = 400,
    };
    /* Power off the humidifier */
    gpio_set_level(CONFIG_HUMIDIFIER_POWER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(POWER_OFF_DELAY));
    gOn = false;
}

//-------------------------------------------------------------------------------------------------

void Humidifier_OnOffButtonClick(void)
{
    enum
    {
        HOLD_DELAY    = 80,
        RELEASE_DELAY = 120,
    };
    /* Heat the sensor before humidification */
    sht41_Heat();
    /* Click the button */
    gpio_set_level(CONFIG_HUMIDIFIER_BUTTON_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(HOLD_DELAY));
    gpio_set_level(CONFIG_HUMIDIFIER_BUTTON_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(RELEASE_DELAY));
    gOn = true;
}

//-------------------------------------------------------------------------------------------------

bool Humidifier_IsPoweredOn(void)
{
    return gOn;
}

//-------------------------------------------------------------------------------------------------

void Humidifier_ReadSensors(void)
{
    enum
    {
        HUMIDITY_MAX = 10000,
    };
    bme280_Readout();
    sht41_Readout();
    if (HUMIDITY_MAX == gSht41Measurement.humidity)
    {
        sht41_Heat();
    }
    else
    {
        sht41_StartMeasuring();
    }
}

//-------------------------------------------------------------------------------------------------

int16_t Humidifier_GetTemperature(void)
{
    return ((gBme280Measurement.temperature + gSht41Measurement.temperature) / 2);
}

//-------------------------------------------------------------------------------------------------

uint32_t Humidifier_GetPressure(void)
{
    return gBme280Measurement.pressure;
}

//-------------------------------------------------------------------------------------------------

uint16_t Humidifier_GetHumidity(void)
{
    return gSht41Measurement.humidity;
}

//-------------------------------------------------------------------------------------------------

void Humidifier_Test(void)
{
    enum
    {
        COUNT_ON  = 10,
        COUNT_OFF = 100,
    };
    uint8_t cnt = 0;

    HUMDFR_LOGI("Init the Humidifier");
    Humidifier_Init();

    /* Test Sensors work during the Ultrasonic is on */
    HUMDFR_LOGI("Power On");
    Humidifier_PowerOn();
    HUMDFR_LOGI("Humidification");
    Humidifier_OnOffButtonClick();
    HUMDFR_LOGI("Read sensors");
    for (cnt = 0; cnt < COUNT_ON; cnt++)
    {
        HUMDFR_LOGI("---------------------------");
        Humidifier_ReadSensors();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    HUMDFR_LOGI("Power off");
    Humidifier_PowerOff();

    /* On */
    Humidifier_PowerOn();
    Humidifier_OnOffButtonClick();
    /* Humidify */
    vTaskDelay(pdMS_TO_TICKS(3000));
    /* Off */
    Humidifier_PowerOff();
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* On */
    Humidifier_PowerOn();
    /* Measure */
    for (cnt = 0; cnt < COUNT_OFF; cnt++)
    {
        HUMDFR_LOGI("---------------------------");
        Humidifier_ReadSensors();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* Off */
    Humidifier_PowerOff();
}

//-------------------------------------------------------------------------------------------------
