#pragma once
#include <cstddef>
#include "esp_err.h"
typedef int spi_host_device_t;
typedef void* spi_device_handle_t;
enum { SPI2_HOST = 2, SPI_DMA_CH_AUTO = 1 };
struct spi_bus_config_t { int miso_io_num, mosi_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; };
struct spi_device_interface_config_t { int spics_io_num, clock_speed_hz, mode, queue_size; };
struct spi_transaction_t { int length; const void* tx_buffer; };
esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t*, int);
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t*);
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t*);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_free(spi_host_device_t);
