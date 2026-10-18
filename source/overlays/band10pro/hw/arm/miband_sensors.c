/* p67: ICM42607P fallback, HX3695 and AK09919C peripheral subsets.
 * Registers/FIFOs are consumed by the unmodified AP drivers. No uORB, GUI,
 * algorithm result or guest-RAM substitution is performed here.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "miband_sensors.h"
#include <math.h>

static void chip_defaults(MiBandSensorChip *c)
{
    memset(c->regs, 0, sizeof(c->regs));
    memset(c->mreg, 0, sizeof(c->mreg));
    memset(c->phase, 0, sizeof(c->phase));
    c->head = c->count = 0;
    c->last_fifo_next = c->last_fifo_count = 0;
    c->int_status = 0;
    c->irq = false;
    c->next_ns = 0;
    if (c->kind == 0) {
        c->regs[0x75] = 0x60; /* ICM42607P WHO_AM_I */
        c->regs[0] = 8; /* MCLK_RDY bit 3, polled by the stock driver */
        c->regs[0x3a] = 0x10; /* reset done */
        c->int_status = 0x10;
    } else if (c->kind == 1) {
        c->regs[0] = 0x65; /* HX3695 chip ID, checked by stock driver */
    } else if (c->kind == 3) {
        c->regs[0x3e] = 0xe5; /* W1150 PID, p67 board I2C5 / 0x48. */
    } else {
        c->regs[0] = 0x48;
        c->regs[1] = 0x0e;
    }
}

static void chip_select(void *opaque, bool driven, bool high)
{
    MiBandSensorChip *c = opaque;
    bool selected = driven && !high;
    if (selected != c->selected) {
        c->command = true;
        c->position = 0;
    }
    c->selected = selected;
}

static void spi_select(void *opaque, bool enabled)
{
    MiBandSensorChip *c = opaque;
    if (c->kind == 0) chip_select(c, true, !enabled);
}

static int16_t signed_sample(double v)
{
    /* No UI ranges; real register conversion saturates at its signed width. */
    return (int16_t)llround(fmax(-32767, fmin(32767, v)));
}

static void fifo_push(MiBandSensorChip *c, uint8_t v)
{
    if (c->count < sizeof(c->fifo)) {
        c->fifo[(c->head + c->count++) % sizeof(c->fifo)] = v;
    }
}

static uint8_t fifo_pop(MiBandSensorChip *c)
{
    uint8_t v = 0;
    if (c->count) {
        v = c->fifo[c->head];
        c->head = (c->head + 1) % sizeof(c->fifo);
        c->count--;
        c->fifo_reads++;
        c->last_fifo[c->last_fifo_next] = v;
        c->last_fifo_next = (c->last_fifo_next + 1) % sizeof(c->last_fifo);
        c->last_fifo_count = MIN(c->last_fifo_count + 1, sizeof(c->last_fifo));
    }
    return v;
}

static uint8_t chip_read(MiBandSensorChip *c, unsigned reg)
{
    reg &= 0xff;
    c->reads++;
    c->reg_reads[reg]++;
    if (c->kind == 0) {
        unsigned count = (c->regs[0x35] & 0x40) ? c->count / 16 : c->count;
        bool be = c->regs[0x35] & 0x20;
        if (reg == 0x7e) return c->mreg[c->regs[0x7c]][c->regs[0x7d]++];
        if (reg == 0x3d) return be ? count >> 8 : count;
        if (reg == 0x3e) return be ? count : count >> 8;
        if (reg == 0x3f) return fifo_pop(c);
        if (reg == 0x3a) {
            uint8_t status = c->int_status;
            unsigned wm = c->regs[0x29] | ((c->regs[0x2a] & 15) << 8);
            /* Status is checked both by the IRQ path and its worker. A read
             * cannot remove the threshold condition while FIFO remains full
             * enough; otherwise gyro-only reconfiguration strands the queue. */
            if (wm && count >= wm) status |= 4;
            c->int_status = 0;
            c->irq = false;
            return status;
        }
    } else if (c->kind == 1) {
        if (reg == 0x20) return MIN(c->count / 3, 511);
        if (reg == 0x21) return MIN(c->count / 3, 511) >> 8;
        if (reg == 0x22) return fifo_pop(c);
        if (reg >= 0x44 && reg <= 0x4b) return c->phase[c->regs[0x43] & 7][reg-0x44];
    } else if (c->kind == 3) {
        /* The stock worker polls FLAG[7], then reads ALS0/ALS1 MSB first.
         * Keep the conversion latched over the complete four-byte burst.
         * This is a reference light source, not a factory lux calibration. */
        if (reg == 0x10) return c->regs[reg] | 0x80;
        if (reg == 0x13) {
            unsigned gain = c->regs[0x17] >> 4;
            double ratio = ldexp(1., gain >= 1 && gain <= 15 ? 18 - gain : 0);
            double raw = c->owner->als_lux * 1000. / ratio;
            uint16_t adc = (uint16_t)llround(fmax(0, fmin(65535, raw)));
            c->regs[0x13] = adc >> 8; c->regs[0x14] = adc;
            c->regs[0x15] = c->regs[0x16] = 0;
            c->samples++;
        }
    } else {
        if (reg == 0x10) return c->count ? 1 | (MIN(c->count / 6,31) << 2) : 0;
        if (reg >= 0x11 && reg <= 0x16)
            return c->count ? c->fifo[(c->head + reg - 0x11) % sizeof(c->fifo)] : 0;
        if (reg == 0x18 && c->count) {
            for (unsigned n = 0; n < 6; n++) fifo_pop(c);
            return 0;
        }
    }
    return c->regs[reg];
}

static void chip_write(MiBandSensorChip *c, unsigned reg, uint8_t value)
{
    reg &= 0xff;
    c->writes++;
    if (c->trace_count++ < 220) {
        qemu_log_mask(LOG_UNIMP, "p67 sensor%u W %02x=%02x\n", c->kind, reg, value);
    }
    if ((c->kind == 0 && reg == 0x01 && (value & 1)) ||
        (c->kind == 2 && reg == 0x32 && (value & 1)) ||
        (c->kind == 3 && reg == 0x80 && value == 0x65)) {
        chip_defaults(c);
        return;
    }
    if ((c->kind == 0 && reg == 0x75) || (c->kind == 1 && reg == 0) ||
        (c->kind == 2 && reg <= 1) || (c->kind == 3 && reg == 0x3e)) return;
    c->regs[reg] = value;
    if (c->kind == 0) {
        if (reg == 0x7b) c->mreg[c->regs[0x79]][c->regs[0x7a]++] = value;
        if (reg == 2) {
            if (value & 4) c->head = c->count = 0;
            c->regs[2] = 0; /* reset bits self-clear */
        }
    } else if (c->kind == 1) {
        if (reg >= 0x44 && reg <= 0x4b) c->phase[c->regs[0x43] & 7][reg - 0x44] = value;
        if (reg == 0x5c && (value & 2)) c->head = c->count = 0;
    } else if (c->kind == 2 && reg == 0x31) {
        c->head = c->count = 0;
        c->next_ns = 0;
    }
}

static uint8_t sensor_transfer(void *opaque, uint8_t tx)
{
    MiBandSensorChip *c = opaque;
    uint8_t rx = 0xff;
    if (!c->selected) return rx;
    if (c->command) {
        c->pointer = tx & 0x7f;
        c->read = c->kind == 1 ? !(tx & 0x80) : !!(tx & 0x80);
        c->command = false;
        c->position = 0;
    } else {
        if (c->read) rx = chip_read(c, c->pointer);
        else chip_write(c, c->pointer, tx);
        if (!((c->kind == 0 && c->pointer == 0x3f) ||
              (c->kind == 1 && c->read && c->pointer == 0x22)))
            c->pointer = (c->pointer + 1) & 0x7f;
        c->position++;
    }
    if (c->trace_count++ < 220)
        qemu_log_mask(LOG_UNIMP, "p67 sensor%u SPI tx=%02x rx=%02x ptr=%02x read=%u pos=%u\n",
                      c->kind, tx, rx, c->pointer, c->read, c->position);
    return rx;
}

static void mag_start(void *opaque, bool read)
{
    MiBandSensorChip *c = opaque;
    if (!read) c->command = true;
}
static void mag_write(void *opaque, uint8_t v)
{
    MiBandSensorChip *c = opaque;
    if (c->command) { c->pointer = v; c->command = false; }
    else chip_write(c, c->pointer++, v);
}
static uint8_t mag_read(void *opaque)
{
    MiBandSensorChip *c = opaque;
    uint8_t v = chip_read(c, c->pointer);
    c->pointer++;
    return v;
}
static void mag_stop(void *opaque) { }
static void mag_reset_bus(void *opaque)
{
    MiBandSensorChip *c = opaque;
    c->command = true;
}

static void push_word(MiBandSensorChip *c, int16_t value, bool be)
{
    fifo_push(c, be ? value >> 8 : value);
    fifo_push(c, be ? value : value >> 8);
}

static void sample_imu(MiBandSensors *s, int64_t now)
{
    MiBandSensorChip *c = &s->imu;
    unsigned afs = (c->regs[0x21] >> 5) & 3;
    unsigned gfs = (c->regs[0x20] >> 5) & 3;
    bool accel = (c->regs[0x1f] & 3) != 0;
    bool gyro = (c->regs[0x1f] & 0x0c) != 0;
    int16_t a[3], g[3];
    for (unsigned n = 0; n < 3; n++) {
        a[n] = signed_sample(s->accel[n] / 9.80665 * (2048 << afs));
        g[n] = signed_sample(s->gyro[n] * (16.384 * (1u << gfs)));
        if (c->regs[0x35] & 0x10) {
            stw_be_p(&c->regs[0x0b + n * 2], a[n]);
            stw_be_p(&c->regs[0x11 + n * 2], g[n]);
        } else {
            stw_le_p(&c->regs[0x0b + n * 2], a[n]);
            stw_le_p(&c->regs[0x11 + n * 2], g[n]);
        }
    }
    if (!(accel || gyro)) return;
    unsigned odr = (accel ? c->regs[0x21] : c->regs[0x20]) & 15;
    static const unsigned periods_us[16] = {10000,10000,10000,10000,10000,625,1250,2500,5000,10000,20000,40000,80000,160000,320000,640000};
    if (now < c->next_ns) return;
    c->next_ns = now + periods_us[odr] * 1000LL;
    c->samples++;
    c->int_status |= 1;
    /* FIFO_CONFIG5 is MREG1 bank 0, not the MREG bank 0x28 filters. */
    bool fa = accel && (c->mreg[0][1] & 1);
    bool fg = gyro && (c->mreg[0][1] & 2);
    if ((fa || fg) && (c->regs[0x28] & 3) != 1) {
        bool be = c->regs[0x35] & 0x10;
        unsigned packet = fa && fg ? 16 : 8;
        if (c->count + packet <= sizeof(c->fifo)) {
            fifo_push(c, (fa ? 0x40 : 0) | (fg ? 0x20 : 0) | (fa && fg ? 8 : 0));
            if (fa) for (unsigned n = 0; n < 3; n++) push_word(c, a[n], be);
            if (fg) for (unsigned n = 0; n < 3; n++) push_word(c, g[n], be);
            fifo_push(c, 0);
            if (fa && fg) push_word(c, now / 1000000, be);
        }
        unsigned count = c->regs[0x35] & 0x40 ? c->count / 16 : c->count;
        unsigned wm = c->regs[0x29] | ((c->regs[0x2a] & 15) << 8);
        if (wm && count >= wm) c->int_status |= 4;
    }
    bool irq = (c->int_status & c->regs[0x2b] & 7) != 0;
    miband_gpio_set_input(s->gpio,29,irq == !!(c->regs[6] & 1));
    if (irq && !c->irq) { miband_gpio_raise_edge(s->gpio,29); c->irq_events++; }
    c->irq = irq;
}

static void sample_mag(MiBandSensors *s, int64_t now)
{
    MiBandSensorChip *c = &s->mag;
    unsigned mode = c->regs[0x31] & 31;
    if (mode && now >= c->next_ns) {
        int period_ms = mode == 2 ? 100 : mode == 4 ? 50 : mode == 6 ? 20 : mode == 8 ? 10 : 5;
        c->next_ns = now + period_ms * 1000000LL;
        c->samples++;
        if (c->count >= 31 * 6) {
            c->head = (c->head + 6) % sizeof(c->fifo);
            c->count -= 6;
        }
        for (unsigned n = 0; n < 3; n++) push_word(c,signed_sample(s->magnetic[n] / 0.15),true);
        if (mode == 1) c->regs[0x31] &= ~31;
    }
}

static bool optical_active(MiBandSensorChip *c)
{
    /* 0x1b=0x90 is the stock measurement configuration; bit 7 controls
     * the interrupt path, not shutdown. Register 0x02 bit 0 powers down. */
    return !(c->regs[0x02] & 1) && c->regs[0x38];
}

static void sample_optical(MiBandSensors *s, int64_t now)
{
    MiBandSensorChip *c = &s->optical;
    if (!optical_active(c) || now < c->next_ns) return;
    c->next_ns = now + 40000000; /* Observed default 25 Hz; no algorithm-result injection. */
    double wave = s->amplitude * sin(fmod(now / 1e9 * s->bpm / 60,1) * 2 * G_PI);
    /* The stock driver deinterleaves eight signed 21-bit words by tag[7:5].
     * Four green words, red, IR and two dark PD words form one full frame. */
    bool room = c->count / 3 < 480;
    for (unsigned tag = 0; tag < 8; tag++) {
        double pd = s->pd[(tag == 1 || tag == 3 || tag == 7) ? 1 : 0];
        double value = s->ambient + (tag < 6 ? pd + wave : 0);
        uint32_t raw = (uint32_t)(int32_t)llround(fmod(value,2097152.)) & 0x1fffff;
        raw |= tag << 21;
        for (unsigned n = 0; n < 3; n++) {
            if (room) fifo_push(c,raw >> (n*8));
            c->regs[3 + tag*3+n] = raw >> (n*8);
        }
    }
    c->samples++;
    /* HX's GPIO interrupt is a pulse; bank masks remain owned by the AP. */
    /* p67 board config: IRQ at +4 is GPIO0; GPIO1 is a power output. */
    miband_gpio_set_input(s->gpio,0,true);
    miband_gpio_raise_edge(s->gpio,0);
    c->irq_events++;
}

static void sample_tick(void *opaque)
{
    MiBandSensors *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->enabled & 1) sample_imu(s,now);
    if (s->enabled & 2) sample_optical(s,now);
    if (s->enabled & 4) sample_mag(s,now);
    s->ticks++;
    timer_mod(s->timer, now + 5000000);
}

void miband_sensors_init(MiBandSensors *s, MiBandGpio *gpio, MiBandSpi spi[2], MiBandI2c *i2c, unsigned mask)
{
    s->gpio = gpio;
    s->enabled = mask;
    s->accel[2] = 9.80665;
    s->magnetic[0] = 30; s->magnetic[2] = 40;
    s->ambient = 100; s->als_lux = 100; s->bpm = 72;
    MiBandSensorChip *chips[] = { &s->imu, &s->optical, &s->mag, &s->als };
    for (unsigned n = 0; n < 4; n++) {
        chips[n]->owner = s; chips[n]->kind = n;
        chips[n]->command = true; chip_defaults(chips[n]);
    }
    miband_gpio_connect_output(gpio, 61, chip_select, &s->optical);
    for (unsigned n = 0; n < 2; n++) {
        if (!(mask & (1u << n))) continue;
        spi[n].opaque = chips[n]; spi[n].transfer = sensor_transfer; spi[n].select = spi_select;
    }
    if (mask & 4) i2c[3].sensor_endpoint = (MiBandI2cEndpoint){ .address = 0x0e, .opaque = &s->mag,
        .start = mag_start, .write = mag_write, .read = mag_read, .stop = mag_stop, .reset = mag_reset_bus };
    i2c[5].sensor_endpoint = (MiBandI2cEndpoint){ .address = 0x48, .opaque = &s->als,
        .start = mag_start, .write = mag_write, .read = mag_read, .stop = mag_stop, .reset = mag_reset_bus };
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sample_tick, s);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000000);
}
void miband_sensors_shutdown(MiBandSensors *s)
{
    if (s->timer) { timer_free(s->timer); s->timer = NULL; }
}
void miband_sensors_reset(MiBandSensors *s)
{
    chip_defaults(&s->imu); chip_defaults(&s->optical); chip_defaults(&s->mag);
    chip_defaults(&s->als);
}

bool miband_sensors_set(MiBandSensors *s, const char *name, double value)
{
    if (!isfinite(value)) return false;
    for (unsigned n = 0; n < 3; n++) {
        char key[32];
        snprintf(key, sizeof(key), "sensor-accel-%c", 'x' + n);
        if (!strcmp(key, name)) { s->accel[n] = value; return true; }
        snprintf(key, sizeof(key), "sensor-gyro-%c", 'x' + n);
        if (!strcmp(key, name)) { s->gyro[n] = value; return true; }
        snprintf(key, sizeof(key), "sensor-mag-%c", 'x' + n);
        if (!strcmp(key, name)) { s->magnetic[n] = value; return true; }
    }
    if (!strcmp(name, "sensor-pd-0")) s->pd[0] = value;
    else if (!strcmp(name, "sensor-pd-1")) s->pd[1] = value;
    else if (!strcmp(name, "sensor-ambient-lux")) s->als_lux = value;
    else if (!strcmp(name, "sensor-optical-bias")) s->ambient = value;
    else if (!strcmp(name, "sensor-pulse-bpm")) s->bpm = value;
    else if (!strcmp(name, "sensor-pulse-amplitude")) s->amplitude = value;
    else return false;
    return true;
}

static void append_chip(MiBandSensorChip *c, GString *j)
{
    g_string_append_printf(j, "\"reads\":%"PRIu64",\"writes\":%"PRIu64",\"samples\":%"PRIu64
        ",\"fifo_reads\":%"PRIu64",\"fifo_bytes\":%u,\"registers\":[", c->reads, c->writes, c->samples, c->fifo_reads, c->count);
    for (unsigned n = 0; n < 256; n++) g_string_append_printf(j, "%s%u", n ? "," : "", c->regs[n]);
    g_string_append(j, "],\"register_reads\":[");
    for (unsigned n = 0; n < 256; n++) g_string_append_printf(j, "%s%"PRIu64, n ? "," : "", c->reg_reads[n]);
    g_string_append_printf(j, "],\"irq_events\":%"PRIu64,c->irq_events);
    g_string_append(j, ",\"last_fifo_bytes\":[");
    for (unsigned n = 0; n < c->last_fifo_count; n++) {
        unsigned pos = (c->last_fifo_next + sizeof(c->last_fifo) - c->last_fifo_count + n) % sizeof(c->last_fifo);
        g_string_append_printf(j, "%s%u", n ? ",":"", c->last_fifo[pos]);
    }
    g_string_append(j, "]");
    if (c->kind == 0) {
        g_string_append(j, ",\"mreg1\":[");
        for (unsigned n = 0; n < 256; n++) g_string_append_printf(j,"%s%u",n ? ",":"",c->mreg[0][n]);
        g_string_append(j,"]");
    }
}
void miband_sensors_append(MiBandSensors *s, GString *j)
{
    g_string_append_printf(j, ",\"gpio\":{\"enable0\":%u,\"common0\":%u,\"mask0\":%u,\"pending0\":%u}",
        s->gpio->irq_enable[0],s->gpio->common_enable[0],s->gpio->irq_mask[0],s->gpio->status[0]);
    g_string_append_printf(j, ",\"motion\":{\"accel\":[%.9g,%.9g,%.9g],\"gyro\":[%.9g,%.9g,%.9g],"
        "\"mag\":[%.9g,%.9g,%.9g],\"mag_reads\":%"PRIu64"},\"imu\":{\"driver\":\"ICM42607P\",",
        s->accel[0],s->accel[1],s->accel[2],s->gyro[0],s->gyro[1],s->gyro[2],
        s->magnetic[0],s->magnetic[1],s->magnetic[2],s->mag.reads);
    append_chip(&s->imu,j);
    g_string_append_printf(j, "},\"optical\":{\"driver\":\"HX3695\",\"pd\":[%.9g,%.9g],\"optical_bias\":%.9g,"
        "\"pulse_bpm\":%.9g,\"pulse_amplitude\":%.9g,",s->pd[0],s->pd[1],s->ambient,s->bpm,s->amplitude);
    bool active = optical_active(&s->optical);
    g_string_append_printf(j,"\"active\":%s,\"leds\":[%s,%s,%s,%s],",
        active ? "true":"false",
        active && s->optical.phase[1][1] ? "true":"false",
        active && s->optical.phase[3][1] ? "true":"false",
        active && s->optical.phase[4][1] ? "true":"false",
        active && s->optical.phase[5][1] ? "true":"false");
    append_chip(&s->optical,j);
    g_string_append(j, "},\"compass\":{\"driver\":\"AK09919C\",");
    append_chip(&s->mag,j);
    g_string_append_printf(j, "},\"als\":{\"driver\":\"W1150\",\"lux\":%.9g,\"bus\":5,\"address\":72,", s->als_lux);
    append_chip(&s->als,j);
    g_string_append(j, "}");
}
