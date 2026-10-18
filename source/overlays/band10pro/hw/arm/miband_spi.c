#include "qemu/osdep.h"
#include "miband_peripherals.h"

#define SPI_DATA    0x08
#define SPI_STATUS  0x0c
#define SPI_TX_ROOM 0x02
#define SPI_RX_DATA 0x04

void miband_spi_init(MiBandSpi spi[2])
{
    spi[0].base = 0x40007000;
    spi[1].base = 0x40068000;
}

bool miband_spi_read(MiBandSpi spi[2], hwaddr address, unsigned size,
                     uint64_t *result)
{
    unsigned i;

    if (size != sizeof(uint32_t)) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        if (address == spi[i].base + SPI_STATUS) {
            *result = SPI_TX_ROOM | (spi[i].rx_ready ? SPI_RX_DATA : 0);
            return true;
        }
        if (address == spi[i].base + SPI_DATA) {
            *result = spi[i].rx_data;
            spi[i].rx_ready = false;
            return true;
        }
        if (address == spi[i].base) {
            *result = spi[i].control;
            return true;
        }
        if (address == spi[i].base + 4) {
            *result = spi[i].enable;
            return true;
        }
    }
    return false;
}

bool miband_spi_write(MiBandSpi spi[2], hwaddr address, unsigned size,
                      uint64_t value)
{
    unsigned i;

    if (size != sizeof(uint32_t)) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        if (address == spi[i].base + SPI_DATA) {
            spi[i].rx_data = spi[i].transfer ?
                spi[i].transfer(spi[i].opaque, value & 0xff) : 0xff;
            spi[i].rx_ready = true;
            return true;
        }
        if (address == spi[i].base) {
            spi[i].control = value;
            return true;
        }
        if (address == spi[i].base + 4) {
            if ((spi[i].enable ^ value) & 2) {
                if (spi[i].select) spi[i].select(spi[i].opaque, !!(value & 2));
            }
            spi[i].enable = value;
            return true;
        }
    }
    return false;
}
