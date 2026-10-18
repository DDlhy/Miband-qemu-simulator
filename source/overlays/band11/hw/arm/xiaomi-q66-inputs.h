/* Local sensor-bus experiment. SPDX-License-Identifier: GPL-2.0-or-later */

static void q66_inputs_trace(Q66MachineState *s, unsigned bus, unsigned address,
                             const char *direction, unsigned reg, unsigned value)
{
    Q66Inputs *in = &s->inputs;
    unsigned index = (in->trace_head + in->trace_count) % 128;
    if (in->trace_count == 128) {
        in->trace_head = (in->trace_head + 1) % 128;
    } else {
        in->trace_count++;
    }
    snprintf(in->trace[index], sizeof(in->trace[index]),
             "{\"seq\":%" PRIu64 ",\"bus\":%u,\"address\":%u,"
             "\"direction\":\"%s\",\"register\":%u,\"value\":%u}",
             ++in->bus_sequence, bus, address, direction, reg, value);
    if (in->bus_logging) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "q66 sensor-bus t=%" PRId64 " bus=%u dev=%u %s page=%u reg=%02x value=%02x pc=%08x\n",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), bus, address, direction,
            in->ppg_page, reg, value, s->arm.cpu->env.regs[15]);
    }
    if (bus == 6 && address == 2 && !strcmp(direction, "TX")) {
        unsigned at = (in->optical_head + in->optical_count) % 128;
        if (in->optical_count == 128) { in->optical_head = (in->optical_head + 1) % 128; }
        else { in->optical_count++; }
        snprintf(in->optical_trace[at], sizeof(in->optical_trace[at]),
            "{\"seq\":%" PRIu64 ",\"page\":%u,\"selector\":%u,\"register\":%u,\"value\":%u}",
            in->bus_sequence, in->ppg_page & 1, in->ppg_page, reg, value);
    }
}

static void q66_inputs_reset(Q66MachineState *s)
{
    unsigned bus;
    memset(s->inputs.bus, 0, sizeof(s->inputs.bus));
    memset(s->inputs.imu_regs, 0, sizeof(s->inputs.imu_regs));
    memset(s->inputs.ppg_regs, 0, sizeof(s->inputs.ppg_regs));
    s->inputs.spi_device = s->inputs.spi_byte = 0;
    s->inputs.ppg_page = s->inputs.ppg_head = s->inputs.ppg_count = 0;
    s->inputs.imu_head = s->inputs.imu_count = 0;
    s->inputs.mag_head = s->inputs.mag_count = 0;
    s->inputs.mag_next = 0;
    s->inputs.ppg_next = s->inputs.imu_next[0] = s->inputs.imu_next[1] = 0;
    s->inputs.ppg_prf_until = 0;
    s->inputs.ppg_int_status = 0;
    memset(s->inputs.ppg_adc, 0, sizeof(s->inputs.ppg_adc));
    s->inputs.ppg_irq = s->inputs.imu_irq = false;
    s->inputs.ppg_read_word = s->inputs.ppg_read_bytes = s->inputs.ppg_last_mask = 0;
    s->inputs.vibrating = false;
    for (bus = 0; bus < 6; bus++) {
        s->inputs.bus[bus].address_byte = true;
    }
    s->inputs.bus[5].present[0x0e] = 1;
    s->inputs.bus[4].present[0x48] = 1;
    s->inputs.bus[4].device[0x48][0x3e] = 0xe5;
    s->inputs.bus[2].present[0x34] = 1;
    s->inputs.bus[2].device[0x34][1] = 0x25;
}

static void q66_inputs_sample(void *opaque);

static void q66_inputs_init(Q66MachineState *s)
{
    s->inputs.accel[2] = 9.80665;
    s->inputs.mag[0] = 30;
    s->inputs.mag[2] = 40;
    s->inputs.ambient = 100;
    s->inputs.als_lux = 100;
    s->inputs.pulse_bpm = 72;
    q66_inputs_reset(s);
    s->inputs.sample_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_inputs_sample, s);
    timer_mod(s->inputs.sample_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000000);
}

static uint32_t q66_inputs_raw(double value, double scale, unsigned bits)
{
    /* Wrap at the peripheral's ADC width; the physical input has no UI cap. */
    double modulus = ldexp(1.0, bits);
    return (uint32_t)(int64_t)round(fmod(value, modulus * scale) / scale) &
           ((1u << bits) - 1);
}

static uint8_t q66_inputs_spi_register(Q66MachineState *s, unsigned device,
                                      unsigned reg)
{
    Q66Inputs *in = &s->inputs;
    if (device == 1) {
        if (reg == 0x0f) { return 0x6c; }
        if (reg == 0x12) { return in->imu_regs[reg] & ~1u; }
        if (reg == 0x1e) { return 3; }
        if (reg == 0x3a) { return (in->imu_count / 7) & 255; }
        if (reg == 0x3b) { return ((in->imu_count / 7) >> 8) | (in->imu_irq ? 0x80 : 0); }
        if (reg >= 0x22 && reg < 0x2e) {
            unsigned off = reg - 0x22, axis = (off % 6) / 2;
            double scale;
            uint32_t raw;
            if (off < 6) {
                static const double sensitivity[] = {.00875, .0175, .035, .070};
                scale = in->imu_regs[0x11] & 2 ? .004375 :
                        sensitivity[(in->imu_regs[0x11] >> 2) & 3];
                raw = q66_inputs_raw(in->gyro[axis], scale, 16);
            } else {
                static const double sensitivity[] = {.000061, .000488, .000122, .000244};
                scale = sensitivity[(in->imu_regs[0x10] >> 2) & 3] * 9.80665;
                raw = q66_inputs_raw(in->accel[axis], scale, 16);
            }
            return raw >> (8 * (off & 1));
        }
        return in->imu_regs[reg];
    }
    if (!reg) { return in->ppg_page; }
    if (!(in->ppg_page & 1) && reg == 1) { return 0x69; }
    if (!(in->ppg_page & 1) && reg == 2) { return 0x19; }
    if (!(in->ppg_page & 1) && reg >= 4 && reg < 0x34) {
        unsigned off = reg - 4;
        unsigned slot = off / 6 + (off % 6 >= 3 ? 8 : 0);
        uint32_t raw = in->ppg_adc[slot];
        if (!(off % 3)) {
            in->ppg_direct_reads++;
            in->ppg_last_samples[slot] = (int32_t)(raw << 12) >> 12;
            in->ppg_last_mask |= 1u << slot;
        }
        in->ppg_reads++;
        return raw >> (8 * (off % 3));
    }
    if (!(in->ppg_page & 1) && (reg == 0x68 || reg == 0x69)) {
        return (in->ppg_count / 3) >> (8 * (reg - 0x68));
    }
    if (!(in->ppg_page & 1) && reg == 0x70) {
        /* The stock worker snapshots INT_STATUS before draining FIFO. Its
         * upper driver publishes samples only for reason 1 (FIFO), while
         * reason 4 drives AGC via direct ADC reads. Keep a short PRF pulse's
         * reason until the deferred worker can read it. */
        uint8_t status = in->ppg_int_status;
        in->ppg_int_status = 0;
        in->ppg_reads++;
        return status;
    }
    in->ppg_reads++;
    return in->ppg_regs[in->ppg_page & 1][reg & 127];
}

static void q66_inputs_sample(void *opaque)
{
    Q66MachineState *s = opaque;
    Q66Inputs *in = &s->inputs;
    unsigned slot, byte, kind;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned mask = in->ppg_regs[0][0x34] | (in->ppg_regs[0][0x35] << 8);
    unsigned prf = in->ppg_regs[0][0x5a] | (in->ppg_regs[0][0x5b] << 8) |
                   (in->ppg_regs[0][0x5c] << 16);
    if (now >= in->ppg_next && in->ppg_writes && !(in->ppg_regs[0][3] & 1)) {
        in->ppg_next = now + (prf ? (int64_t)prf * 1000000000 / 32192 : 40000000);
        for (slot = 0; slot < 16; slot++) {
            if (mask & (1u << slot)) {
                /* HX3697 slots carry signed 20-bit ADC data, then a 4-bit tag. */
                unsigned map_reg = slot < 8 ? 0x34 - slot / 2 : 0x39 - (slot - 8) / 2;
                unsigned pd_mask = (in->ppg_regs[1][map_reg] >> (4 * (slot & 1))) & 15;
                double sample = 0;
                unsigned pd;
                uint32_t raw;
                for (pd = 0; pd < 4; pd++) {
                    if (pd_mask & (1u << pd)) { sample += in->pd[pd]; }
                }
                if (pd_mask && in->ppg_regs[1][0x10 + 2 * (slot % 8)] &&
                    in->ppg_regs[1][0x11 + 2 * (slot % 8)]) {
                    /* Optional incident optical pulse, sampled by the same
                     * ADC path as manual PD input. No algorithm result is
                     * injected: stock AGC/wear/HR processing consumes it. */
                    double phase = fmod((double)now / 60000000000.0 *
                                        in->pulse_bpm, 1.0) * 2 * M_PI;
                    sample += in->pulse_amplitude *
                              (sin(phase) + .2 * sin(2 * phase) - .12 * sin(3 * phase));
                }
                raw = q66_inputs_raw(sample, 1, 20);
                in->ppg_adc[slot] = raw;
                raw |= slot << 20;
                if (in->ppg_count + 3 <= sizeof(in->ppg_fifo)) {
                    for (byte = 0; byte < 3; byte++) {
                        in->ppg_fifo[(in->ppg_head + in->ppg_count++) % sizeof(in->ppg_fifo)] = raw >> (8 * byte);
                    }
                }
                in->ppg_samples++;
            }
        }
        /* Stock AGC selects PRF (0x04), then FIFO (0x01) in INT_SEL.
         * PRF pulses once per completed conversion, independently of FIFO. */
        in->ppg_prf_until = now + 1000000;
    }
    {
        unsigned watermark = in->ppg_regs[1][0x0b] |
                             (in->ppg_regs[1][0x0c] << 8);
        unsigned select = in->ppg_regs[0][0x73];
        bool fifo = (select & 1) && watermark && in->ppg_count / 3 >= watermark;
        bool prf_irq = (select & 4) && now < in->ppg_prf_until;
        bool irq = (fifo || prf_irq) && mask &&
                   !(in->ppg_regs[0][3] & 1);
        if (irq) {
            in->ppg_int_status |= (fifo ? 1 : 0) | (prf_irq ? 4 : 0);
        }
        if (irq && !in->ppg_irq) { s->gpio_pending[0] |= 1u << 14; }
        in->ppg_irq = irq;
    }
    for (kind = 0; kind < 2; kind++) {
        unsigned odr = (in->imu_regs[9] >> (kind * 4)) & 15;
        if (odr && odr < 12 && (in->imu_regs[0x0a] & 7) && now >= in->imu_next[kind]) {
            static const double rates[] = {0, 12.5, 26, 52, 104, 208, 416, 833, 1666, 3332, 6664, 1.6};
            in->imu_next[kind] = now + (int64_t)(1000000000 / rates[odr]);
            if (in->imu_count + 7 <= sizeof(in->imu_fifo)) {
                uint8_t packet[7];
                packet[0] = (kind ? 1 : 2) << 3;
                for (byte = 0; byte < 6; byte++) {
                    packet[byte + 1] = q66_inputs_spi_register(s, 1, (kind ? 0x22 : 0x28) + byte);
                }
                for (byte = 0; byte < 7; byte++) {
                    in->imu_fifo[(in->imu_head + in->imu_count++) % sizeof(in->imu_fifo)] = packet[byte];
                }
                in->imu_samples++;
            }
        }
    }
    {
        unsigned watermark = in->imu_regs[7] | ((in->imu_regs[8] & 1) << 8);
        bool irq = watermark && (in->imu_regs[0x0d] | in->imu_regs[0x0e]) & 8 &&
                   in->imu_count / 7 >= watermark;
        if (irq && !in->imu_irq) { s->gpio_pending[2] |= 1u << 7; }
        in->imu_irq = irq;
    }
    {
        unsigned mode = in->bus[5].device[0x0e][0x31] & 0x1f;
        unsigned period = mode == 2 ? 100000 : mode == 4 ? 50000 :
                          mode == 6 ? 20000 : mode == 8 ? 10000 :
                          mode == 14 ? 200000 : mode == 1 ? 10000 : 0;
        if (period && now >= in->mag_next) {
            uint8_t *packet;
            in->mag_next = now + (int64_t)period * 1000;
            if (in->mag_count == 31) {
                in->mag_head = (in->mag_head + 1) % 32;
                in->mag_count--;
            }
            packet = in->mag_fifo[(in->mag_head + in->mag_count++) % 32];
            for (byte = 0; byte < 6; byte++) {
                /* The active AK09919C FIFO driver assembles MSB then LSB. */
                packet[byte] = q66_inputs_raw(in->mag[byte / 2], .15, 16) >> (8 * (1 - (byte & 1)));
            }
            packet[6] = packet[7] = 0; /* temperature / ST2: no overflow */
            if (mode == 1) { in->bus[5].device[0x0e][0x31] &= ~0x1f; }
        }
    }
    q66_gpio_update_irq(s);
    timer_mod(in->sample_timer, now + 1000000);
}

static uint32_t q66_inputs_spi(Q66MachineState *s, uint32_t frame, unsigned bits)
{
    Q66Inputs *in = &s->inputs;
    uint32_t pins = q66_reg(s, q66_gpio_bases[1]);
    uint32_t dirs = q66_reg(s, q66_gpio_bases[1] + 4);
    unsigned device = (dirs & (1u << 21)) && !(pins & (1u << 21)) ? 1 :
                      (dirs & (1u << 18)) && !(pins & (1u << 18)) ? 2 : 0;
    unsigned reg;
    uint8_t reply = 0;
    if (bits != 8 || !device) {
        return bits == 32 ? UINT32_MAX : (1u << bits) - 1;
    }
    if (in->spi_device != device) {
        in->spi_device = device;
        in->spi_byte = 0;
    }
    if (!in->spi_byte++) {
        in->spi_read = device == 1 ? !!(frame & 0x80) : !(frame & 0x80);
        in->spi_reg = frame & 0x7f;
        return 0;
    }
    reg = in->spi_reg & 255;
    if (!(device == 2 && !(in->ppg_page & 1) && reg == 0x6a && in->spi_read)) {
        in->spi_reg++;
    }
    if (in->spi_read) {
        if (device == 1 && ((reg >= 0x22 && reg <= 0x2d) || reg >= 0x78)) {
            in->imu_reads++;
        }
        if (device == 1 && reg >= 0x78 && reg <= 0x7e) {
            if (in->imu_count) {
                reply = in->imu_fifo[in->imu_head];
                in->imu_head = (in->imu_head + 1) % sizeof(in->imu_fifo);
                in->imu_count--;
                in->imu_last_packet[reg - 0x78] = reply;
                in->imu_fifo_reads++;
                if (in->imu_count / 7 < (in->imu_regs[7] | ((in->imu_regs[8] & 1) << 8))) {
                    in->imu_irq = false;
                }
            }
            if (reg == 0x7e) {
                uint8_t *last = (in->imu_last_packet[0] >> 3) == 1 ?
                                in->imu_last_gyro : in->imu_last_accel;
                memcpy(last, in->imu_last_packet, 7);
                in->spi_reg = 0x78;
            }
        } else if (device == 2 && !(in->ppg_page & 1) && reg == 0x6a) {
            if (in->ppg_count) {
                reply = in->ppg_fifo[in->ppg_head];
                in->ppg_head = (in->ppg_head + 1) % sizeof(in->ppg_fifo);
                in->ppg_count--;
                in->ppg_fifo_reads++;
                in->ppg_read_word |= (uint32_t)reply << (8 * in->ppg_read_bytes++);
                if (in->ppg_read_bytes == 3) {
                    unsigned slot = in->ppg_read_word >> 20;
                    in->ppg_last_samples[slot] = (int32_t)(in->ppg_read_word << 12) >> 12;
                    in->ppg_last_mask |= 1u << slot;
                    in->ppg_read_word = in->ppg_read_bytes = 0;
                }
                if (in->ppg_count / 3 < (in->ppg_regs[1][0x0b] |
                                       (in->ppg_regs[1][0x0c] << 8))) {
                    in->ppg_irq = false;
                }
            }
            in->ppg_reads++;
        } else {
            reply = q66_inputs_spi_register(s, device, reg);
        }
    } else if (device == 1) {
        if (reg == 0x12 && (frame & 1)) {
            memset(in->imu_regs, 0, sizeof(in->imu_regs));
        }
        in->imu_regs[reg] = frame & (reg == 0x12 ? 0xfe : 0xff);
        if (reg == 0x0a && !(frame & 7)) { in->imu_count = in->imu_head = 0; }
        in->imu_writes++;
    } else {
        if (!reg) {
            in->ppg_page = frame & 3;
        } else {
            /* Selector bit 0 selects the register bank. The stock driver
             * finishes a bank-0 update with selector 2 then toggles 0x4c. */
            in->ppg_regs[in->ppg_page & 1][reg & 127] = frame;
            if ((!(in->ppg_page & 1) && (reg == 0x4c || (reg == 3 && (frame & 1)))) ||
                ((in->ppg_page & 1) && reg == 8 && (frame & 0x80))) {
                in->ppg_head = in->ppg_count = 0;
                in->ppg_read_word = in->ppg_read_bytes = 0;
                in->ppg_irq = false;
                in->ppg_prf_until = 0;
                in->ppg_int_status = 0;
            }
            if (!(in->ppg_page & 1) && reg == 0x73) {
                in->ppg_int_status &= frame;
            }
        }
        in->ppg_writes++;
    }
    q66_inputs_trace(s, 6, device, in->spi_read ? "RX" : "TX", reg,
                     in->spi_read ? reply : frame & 255);
    return reply;
}

static uint8_t q66_inputs_register(Q66MachineState *s, unsigned bus,
                                    unsigned address, unsigned reg)
{
    Q66InputBus *b = &s->inputs.bus[bus];
    if (bus == 4 && address == 0x48) {
        Q66Inputs *in = &s->inputs;
        in->als_reads++;
        if (reg == 0x3e) return 0xe5;
        if (reg == 0x10) return b->device[address][reg] | 0x80;
        if (reg == 0x13) {
            unsigned gain = b->device[address][0x17] >> 4;
            double ratio = ldexp(1., gain >= 1 && gain <= 15 ? 18 - gain : 0);
            uint16_t adc = llround(fmax(0, fmin(65535, in->als_lux * 1000. / ratio)));
            b->device[address][0x13] = adc >> 8;
            b->device[address][0x14] = adc;
            b->device[address][0x15] = b->device[address][0x16] = 0;
            in->als_samples++;
        }
        return b->device[address][reg];
    }
    if (bus == 2 && address == 0x34) {
        if (reg == 1) { return 0x25; }
        if (reg == 4) { return s->inputs.vibrating ? 4 : 0; }
        if (reg == 0x11) { return s->inputs.vibrating ? 0 : 1; }
    }
    if (bus == 5 && address == 0x0e) {
        if (reg == 0) {
            return 0x48;
        }
        if (reg == 1) {
            return 0x0e;
        }
        if (reg == 0x10) {
            /* Stock FIFO driver extracts the sample count from ST1[6:2]. */
            return (s->inputs.mag_count << 2) | (s->inputs.mag_count ? 1 : 0);
        }
        if (reg >= 0x11 && reg <= 0x18) {
            Q66Inputs *in = &s->inputs;
            uint8_t data = in->mag_count ? in->mag_fifo[in->mag_head][reg - 0x11] : 0;
            if (reg == 0x11 && in->mag_count) { in->mag_reads++; }
            if (reg == 0x18 && in->mag_count) {
                in->mag_head = (in->mag_head + 1) % 32;
                in->mag_count--;
            }
            return data;
        }
    }
    return b->device[address][reg];
}

static void q66_inputs_i2c_irq(Q66MachineState *s, unsigned bus)
{
    Q66InputBus *b = &s->inputs.bus[bus];
    static const unsigned irqs[] = {14, 29, 64, 65, 53, 54};
    uint32_t raw = b->raw | 0x10;
    if (b->rx_count > b->regs[0x38 / 4]) {
        raw |= 4;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), irqs[bus]),
                 !!(raw & b->regs[0x30 / 4]) && (b->regs[0x6c / 4] & 1));
}

static uint32_t q66_inputs_i2c_read(Q66MachineState *s, unsigned bus, unsigned off)
{
    Q66InputBus *b = &s->inputs.bus[bus];
    uint32_t raw = b->raw | 0x10, value = 0;
    if (b->rx_count > b->regs[0x38 / 4]) {
        raw |= 4;
    }
    switch (off) {
    case 0x10:
        if (b->rx_count) {
            value = b->rx[b->rx_head];
            b->rx_head = (b->rx_head + 1) % sizeof(b->rx);
            b->rx_count--;
        } else {
            b->raw |= 1;
        }
        break;
    case 0x2c: return raw & b->regs[0x30 / 4];
    case 0x34: return raw;
    case 0x40: b->raw = 0; break;
    case 0x44: b->raw &= ~1u; break;
    case 0x48: b->raw &= ~2u; break;
    case 0x4c: b->raw &= ~8u; break;
    case 0x54: b->raw &= ~0x40u; break;
    case 0x60: b->raw &= ~0x200u; break;
    case 0x70: return 6 | (b->rx_count ? 8 : 0);
    case 0x74: return 0;
    case 0x78: return b->rx_count;
    case 0x80: return b->raw & 0x40 ? 1 : 0;
    case 0x9c: return b->regs[0x6c / 4] & 1;
    default: return off < sizeof(b->regs) ? b->regs[off / 4] : 0;
    }
    q66_inputs_i2c_irq(s, bus);
    return value;
}

static void q66_inputs_i2c_write(Q66MachineState *s, unsigned bus,
                                 unsigned off, uint32_t value)
{
    Q66InputBus *b = &s->inputs.bus[bus];
    unsigned address = b->regs[1] & 0x7f;
    if (off >= sizeof(b->regs)) {
        return;
    }
    b->regs[off / 4] = value;
    if (off == 4 || (off == 0x6c && !(value & 1))) {
        b->rx_head = b->rx_count = 0;
        b->address_byte = true;
    }
    if (off == 0x10 && (b->regs[0x6c / 4] & 1)) {
        if (!b->present[address]) {
            b->raw |= 0x240;
        } else if (value & 0x100) {
            uint8_t reg = b->pointer++;
            uint8_t data = q66_inputs_register(s, bus, address, reg);
            if (bus == 5 && address == 0x0e && reg == 0x18 &&
                (b->device[address][0x31] & 0x80)) {
                b->pointer = 0x11;
            }
            if (b->rx_count < sizeof(b->rx)) {
                b->rx[(b->rx_head + b->rx_count++) % sizeof(b->rx)] = data;
            }
            q66_inputs_trace(s, bus, address, "RX", reg, data);
        } else if (b->address_byte) {
            b->pointer = value;
            b->address_byte = false;
        } else {
            uint8_t reg = b->pointer++;
            b->device[address][reg] = value;
            if (bus == 4 && address == 0x48) {
                s->inputs.als_writes++;
                if (reg == 0x80 && value == 0x65) {
                    memset(b->device[address], 0, 256);
                    b->device[address][0x3e] = 0xe5;
                }
            }
            if (bus == 2 && address == 0x34 && reg == 0x12) {
                s->inputs.vibrating = value & 1;
                if (value & 1) { s->inputs.vibration_events++; }
            }
            if (bus == 5 && address == 0x0e && reg == 0x32 && (value & 1)) {
                memset(b->device[address], 0, 256);
                s->inputs.mag_head = s->inputs.mag_count = 0;
                s->inputs.mag_next = 0;
            }
            if (bus == 5 && address == 0x0e && reg == 0x31 && !(value & 0x1f)) {
                s->inputs.mag_head = s->inputs.mag_count = 0;
            }
            q66_inputs_trace(s, bus, address, "TX", reg, value & 255);
        }
        if (value & 0x200) {
            b->raw |= 0x200;
            b->address_byte = true;
        }
    }
    if ((off == 4 || off == 0x10) && s->inputs.traces++ < 96) {
        qemu_log_mask(LOG_UNIMP, "q66 sensor I2C%u addr=%02x off=%02x val=%03x pc=%08x\n",
                      bus, address, off, value, s->arm.cpu->env.regs[15]);
    }
    q66_inputs_i2c_irq(s, bus);
}

static void q66_inputs_get_axis(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    Q66Inputs *in = &Q66_MACHINE(obj)->inputs;
    unsigned index = GPOINTER_TO_UINT(opaque);
    double value = index < 3 ? in->accel[index] : index < 6 ? in->gyro[index - 3] :
                   index < 9 ? in->mag[index - 6] : index < 13 ? in->pd[index - 9] :
                   index == 13 ? in->als_lux : index == 14 ? in->pulse_bpm :
                   index == 15 ? in->pulse_amplitude : in->ambient;
    visit_type_number(v, name, &value, errp);
}

static void q66_inputs_set_axis(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    Q66Inputs *in = &Q66_MACHINE(obj)->inputs;
    unsigned index = GPOINTER_TO_UINT(opaque);
    double value;
    if (!visit_type_number(v, name, &value, errp)) {
        return;
    }
    if (!isfinite(value)) {
        error_setg(errp, "Input must be finite");
        return;
    }
    if (index < 3) {
        in->accel[index] = value;
    } else if (index < 6) {
        in->gyro[index - 3] = value;
    } else if (index < 9) {
        in->mag[index - 6] = value;
    } else if (index < 13) {
        in->pd[index - 9] = value;
    } else if (index == 13) {
        in->als_lux = value;
    } else if (index == 14) {
        in->pulse_bpm = value;
    } else if (index == 15) {
        in->pulse_amplitude = value;
    } else {
        in->ambient = value;
    }
}

static bool q66_inputs_get_trace(Object *obj, Error **errp)
{
    return Q66_MACHINE(obj)->inputs.bus_logging;
}

static void q66_inputs_set_trace(Object *obj, bool value, Error **errp)
{
    Q66_MACHINE(obj)->inputs.bus_logging = value;
}

static void q66_inputs_properties(ObjectClass *klass)
{
    static const char *names[] = {"accel-x", "accel-y", "accel-z", "gyro-x", "gyro-y", "gyro-z",
        "mag-x", "mag-y", "mag-z", "pd-0", "pd-1", "pd-2", "pd-3", "ambient-lux",
        "pulse-bpm", "pulse-amplitude", "optical-bias"};
    unsigned i;
    object_class_property_add_bool(klass, "sensor-bus-trace",
        q66_inputs_get_trace, q66_inputs_set_trace);
    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char *name = g_strdup_printf("sensor-%s", names[i]);
        object_class_property_add(klass, name, "number", q66_inputs_get_axis,
                                 q66_inputs_set_axis, NULL, GUINT_TO_POINTER(i));
    }
}

static void q66_inputs_state(Q66MachineState *s, GString *json)
{
    Q66Inputs *in = &s->inputs;
    unsigned i;
    g_string_append_printf(json, ",\"als\":{\"driver\":\"W1150\",\"bus\":4,\"address\":72,"
        "\"lux\":%.9g,\"reads\":%" PRIu64 ",\"writes\":%" PRIu64 ",\"samples\":%" PRIu64 "}",
        in->als_lux, in->als_reads, in->als_writes, in->als_samples);
    g_string_append_printf(json, ",\"virtual_ns\":%" PRId64
        ",\"cp\":{\"started\":%s,\"boots\":%" PRIu64
        ",\"pc\":%u,\"halted\":%s,\"to_ap\":%u,\"from_ap\":%u}",
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->cp_started ? "true" : "false",
        s->cp_boots, s->cp.cpu->env.regs[15],
        CPU(s->cp.cpu)->halted ? "true" : "false", s->cp_to_ap_irq, s->ap_to_cp_irq);
    g_string_append_printf(json,
        ",\"imu\":{\"samples\":%" PRIu64 ",\"fifo_reads\":%" PRIu64
        ",\"fifo_bytes\":%u,\"irq\":%s,\"registers\":[",
        in->imu_samples, in->imu_fifo_reads, in->imu_count,
        in->imu_irq ? "true" : "false");
    for (i = 0; i < 128; i++) {
        g_string_append_printf(json, "%s%u", i ? "," : "", in->imu_regs[i]);
    }
    g_string_append(json, "],\"last_packet\":[");
    for (i = 0; i < 7; i++) {
        g_string_append_printf(json, "%s%u", i ? "," : "", in->imu_last_packet[i]);
    }
    g_string_append(json, "],\"last_accel\":[");
    for (i = 0; i < 7; i++) {
        g_string_append_printf(json, "%s%u", i ? "," : "", in->imu_last_accel[i]);
    }
    g_string_append(json, "],\"last_gyro\":[");
    for (i = 0; i < 7; i++) {
        g_string_append_printf(json, "%s%u", i ? "," : "", in->imu_last_gyro[i]);
    }
    g_string_append_printf(json, "]},\"sensor_gpio\":[");
    for (i = 0; i < 3; i++) {
        uint32_t base = q66_gpio_bases[i];
        g_string_append_printf(json,
            "%s{\"bank\":%u,\"pending\":%u,\"enable\":%u,\"mask\":%u,"
            "\"edge\":%u,\"polarity\":%u,\"wake_enable\":%u}",
            i ? "," : "", i, s->gpio_pending[i], q66_reg(s, base + 0x30),
            q66_reg(s, base + 0x34), q66_reg(s, base + 0x38),
            q66_reg(s, base + 0x3c), q66_reg(s, 0x40085080 + 4 * i));
    }
    g_string_append_printf(json, "]");
    g_string_append_printf(json, ",\"motion\":{\"accel\":[%.9g,%.9g,%.9g],"
        "\"gyro\":[%.9g,%.9g,%.9g],\"mag\":[%.9g,%.9g,%.9g],\"mag_reads\":%" PRIu64 "},"
        "\"optical\":{\"pd\":[%.9g,%.9g,%.9g,%.9g],\"optical_bias\":%.9g",
        in->accel[0], in->accel[1], in->accel[2], in->gyro[0], in->gyro[1], in->gyro[2],
        in->mag[0], in->mag[1], in->mag[2], in->mag_reads,
        in->pd[0], in->pd[1], in->pd[2], in->pd[3], in->ambient);
    g_string_append_printf(json, ",\"pulse_bpm\":%.9g,\"pulse_amplitude\":%.9g",
                           in->pulse_bpm, in->pulse_amplitude);
    g_string_append_printf(json, ",\"reads\":%" PRIu64 ",\"writes\":%" PRIu64
        ",\"samples\":%" PRIu64 ",\"fifo_bytes\":%u,\"irq\":%s,\"registers\":[",
        in->ppg_reads, in->ppg_writes, in->ppg_samples, in->ppg_count,
        in->ppg_irq ? "true" : "false");
    for (i = 0; i < 256; i++) {
        g_string_append_printf(json, "%s%u", i ? "," : "", in->ppg_regs[i / 128][i % 128]);
    }
    g_string_append_printf(json, "],\"fifo_reads\":%" PRIu64
        ",\"direct_reads\":%" PRIu64 ",\"last_sample_mask\":%u,\"last_samples\":[",
        in->ppg_fifo_reads, in->ppg_direct_reads, in->ppg_last_mask);
    for (i = 0; i < 16; i++) { g_string_append_printf(json, "%s%d", i ? "," : "", in->ppg_last_samples[i]); }
    g_string_append(json, "],\"trace\":[");
    for (i = 0; i < in->optical_count; i++) {
        g_string_append_printf(json, "%s%s", i ? "," : "", in->optical_trace[(in->optical_head + i) % 128]);
    }
    g_string_append_printf(json, "]},\"vibration\":{\"active\":%s,\"events\":%" PRIu64
        ",\"gain\":%u,\"driver\":\"FS3001\"},\"imu_reads\":%" PRIu64 ",\"io_trace\":[",
        in->vibrating ? "true" : "false", in->vibration_events,
        in->bus[2].device[0x34][0x5f], in->imu_reads);
    for (i = 0; i < in->trace_count; i++) {
        g_string_append_printf(json, "%s%s", i ? "," : "", in->trace[(in->trace_head + i) % 128]);
    }
    g_string_append_c(json, ']');
    q66_audio_state(s, json);
}
