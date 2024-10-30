#ifndef __I2C_H__
#define __I2C_H__

#include <stdint.h>

#include "driver/i2c_master.h"

typedef i2c_master_bus_handle_t i2c_bus_p;
typedef i2c_master_dev_handle_t i2c_device_p;

void         I2C_Init(void);
i2c_device_p I2C_AddDevice(i2c_device_config_t * p_config);

void I2C_Tx(i2c_device_p p_dvc, uint8_t * p_tx, uint8_t tx_sz);
void I2C_Rx(i2c_device_p p_dvc, uint8_t * p_rx, uint8_t rx_sz);
void I2C_TxRx(i2c_device_p p_dvc, uint8_t * p_tx, uint8_t tx_sz, uint8_t * p_rx, uint8_t rx_sz);
void I2C_Test(void);

#endif /* __I2C_H__ */
