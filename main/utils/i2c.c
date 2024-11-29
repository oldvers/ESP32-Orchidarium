#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"

#include "i2c.h"

//-------------------------------------------------------------------------------------------------

#define I2C_PORT_NUMBER  (-1)
#define I2C_TIMEOUT      (-1)

#define I2C_DEBUG  0

#if (1 == I2C_DEBUG)
#    define I2C_ERROR_CHECK(x) do                                       \
                               {                                        \
                                   esp_err_t r = (x);                   \
                                   while (r != ESP_OK)                  \
                                   {                                    \
                                       vTaskDelay(pdMS_TO_TICKS(1000)); \
                                   };                                   \
                               } while (0)
#else
#    define I2C_ERROR_CHECK(x) ESP_ERROR_CHECK(x)
#endif

//-------------------------------------------------------------------------------------------------

static i2c_bus_p gI2CBusHandle = NULL;

//-------------------------------------------------------------------------------------------------

void I2C_Init(void)
{
    if (NULL == gI2CBusHandle)
    {
        i2c_master_bus_config_t i2c_bus_config =
        {
            .clk_source                   = I2C_CLK_SRC_DEFAULT,
            .i2c_port                     = I2C_PORT_NUMBER,
            .scl_io_num                   = CONFIG_I2C_SCL_GPIO,
            .sda_io_num                   = CONFIG_I2C_SDA_GPIO,
            .glitch_ignore_cnt            = 7,
            .flags.enable_internal_pullup = 1,
        };

        I2C_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &gI2CBusHandle));
    }
}

//-------------------------------------------------------------------------------------------------

i2c_device_p I2C_AddDevice(i2c_device_config_t * p_config)
{
    i2c_device_p result = NULL;
    I2C_ERROR_CHECK(i2c_master_bus_add_device(gI2CBusHandle, p_config, &result));
    return result;
}

//-------------------------------------------------------------------------------------------------

void I2C_Tx(i2c_device_p p_dvc, uint8_t * p_tx, uint8_t tx_sz)
{
    I2C_ERROR_CHECK(i2c_master_transmit(p_dvc, p_tx, tx_sz, I2C_TIMEOUT));
}

//-------------------------------------------------------------------------------------------------

void I2C_Rx(i2c_device_p p_dvc, uint8_t * p_rx, uint8_t rx_sz)
{
    I2C_ERROR_CHECK(i2c_master_receive(p_dvc, p_rx, rx_sz, I2C_TIMEOUT));
}

//-------------------------------------------------------------------------------------------------

void I2C_TxRx(i2c_device_p p_dvc, uint8_t * p_tx, uint8_t tx_sz, uint8_t * p_rx, uint8_t rx_sz)
{
    I2C_ERROR_CHECK(i2c_master_transmit_receive(p_dvc, p_tx, tx_sz, p_rx, rx_sz, I2C_TIMEOUT));
}

//-------------------------------------------------------------------------------------------------

void I2C_Test(void)
{
    enum
    {
        DS1307_ADDR  = (0xD0 >> 1),
        I2C_SPEED_HZ = 100000,
        TX_SIZE      = 1,
        RX_SIZE      = 8,
        SECONDS_DIFF = 4,
    };
    const char * iTag = "I2C";
    i2c_device_config_t dvc_config =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS1307_ADDR,
        .scl_speed_hz    = I2C_SPEED_HZ,
        .scl_wait_us     = 0,
    };

    I2C_Init();

    i2c_device_p p_dvc = I2C_AddDevice(&dvc_config);

    uint8_t offs[TX_SIZE]         = {0};
    uint8_t rx[RX_SIZE]           = {0};
    uint8_t tx[TX_SIZE + RX_SIZE] = {0};

    I2C_TxRx(p_dvc, offs, sizeof(offs), rx, sizeof(rx));
    ESP_LOGI
    (
        iTag,
        "Rx %02X %02X %02X %02X %02X %02X %02X %02X",
        rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]
    );

    tx[0] = offs[0];
    tx[1] = 0x15; /* Seconds + Clock Halt flag cleared */
    tx[2] = 0x45; /* Minutes */
    tx[3] = 0x21; /* Hours */
    tx[4] = 0x01; /* Day */
    tx[5] = 0x30; /* Date */
    tx[6] = 0x11; /* Month */
    tx[7] = 0x24; /* Year */
    tx[8] = 0x00; /* Control */
    I2C_Tx(p_dvc, tx, sizeof(tx));

    vTaskDelay(pdMS_TO_TICKS(1000 * SECONDS_DIFF));

    I2C_TxRx(p_dvc, offs, sizeof(offs), rx, sizeof(rx));
    ESP_LOGI
    (
        iTag,
        "Rx %02X %02X %02X %02X %02X %02X %02X %02X",
        rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]
    );

    if (SECONDS_DIFF == (rx[0] - tx[1]))
    {
        ESP_LOGI(iTag, "PASS!");
    }
    else
    {
        ESP_LOGE(iTag, "FAIL!");
    }
}

//-------------------------------------------------------------------------------------------------
