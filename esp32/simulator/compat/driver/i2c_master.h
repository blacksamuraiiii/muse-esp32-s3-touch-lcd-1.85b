/*
 * Desktop stand-in for ESP-IDF's driver/i2c_master.h.
 *
 * The production headers (muse_board.h) name the master-bus handle, but the
 * simulator has no I2C: none of the calls exist here, only the types the shared
 * headers need to compile.
 */
#pragma once

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;
