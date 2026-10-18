/* Local p67 sensor bus models. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_SENSORS_H
#define MIBAND_SENSORS_H
#include "miband_peripherals.h"

typedef struct MiBandSensorChip {
    struct MiBandSensors *owner;
    unsigned kind, pointer, position;
    bool selected, command, read;
    uint8_t regs[256];
    uint8_t mreg[256][256];
    uint8_t phase[8][8];
    uint8_t int_status;
    bool irq;
    int64_t next_ns;
    uint64_t irq_events;
    uint64_t reads, writes, samples, fifo_reads;
    uint8_t fifo[16384];
    unsigned head, count;
    uint8_t last_fifo[48];
    unsigned last_fifo_next, last_fifo_count;
    uint64_t reg_reads[256];
    unsigned trace_count;
} MiBandSensorChip;

typedef struct MiBandSensors {
    MiBandGpio *gpio;
    MiBandSensorChip imu, optical, mag, als;
    double accel[3], gyro[3], magnetic[3], pd[2];
    double ambient, als_lux, bpm, amplitude;
    QEMUTimer *timer;
    unsigned ticks;
    unsigned enabled;
} MiBandSensors;

void miband_sensors_init(MiBandSensors *, MiBandGpio *, MiBandSpi[2], MiBandI2c *, unsigned);
void miband_sensors_shutdown(MiBandSensors *);
void miband_sensors_reset(MiBandSensors *);
void miband_sensors_append(MiBandSensors *, GString *);
bool miband_sensors_set(MiBandSensors *, const char *, double);
#endif
