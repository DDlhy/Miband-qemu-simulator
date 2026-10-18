/* Local SC7061 experiment: unchanged AP probes I2C0 address 0x07,
 * ID register 0x01=0x07; pin 83 is the PMU AC_ON virtual GPIO.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
static void q66_charger_irq(Q66MachineState *s)
{
    Q66Charger *c = &s->charger;
    uint32_t raw = c->raw | 0x10;
    if (c->rx_count > c->regs[0x38 / 4]) {
        raw |= 4;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 14),
                 !!(raw & c->regs[0x30 / 4]) && (c->regs[0x6c / 4] & 1));
}

static void q66_charger_set_connected(Q66MachineState *s, bool connected)
{
    if (s->charger.connected != connected) {
        s->charger.connected = connected;
        if (s->arm.cpu) {
            s->charger.pending |= connected ? 1 : 2;
            q66_gpadc_update_irq(s);
        }
    }
}

static uint8_t q66_charger_register(Q66MachineState *s)
{
    Q66Charger *c = &s->charger;
    uint8_t reg = c->pointer++;
    c->reads++;
    if (reg == 1) {
        return 7;
    }
    if (reg == 0x30) {
        /* AP checks bit 3 for VBUS power-good and bits 6:5 for charge
         * phase. 0x40 is fast charge; 0x60 is charge complete. */
        return c->connected ? 0x48 : 0;
    }
    return c->device[reg];
}

static uint32_t q66_charger_read(Q66MachineState *s, unsigned off)
{
    Q66Charger *c = &s->charger;
    uint32_t raw = c->raw | 0x10;
    uint32_t value = 0;
    if (c->rx_count > c->regs[0x38 / 4]) {
        raw |= 4;
    }
    switch (off) {
    case 0x10:
        if (c->rx_count) {
            value = c->rx[c->rx_head];
            c->rx_head = (c->rx_head + 1) % G_N_ELEMENTS(c->rx);
            c->rx_count--;
        } else {
            c->raw |= 1;
        }
        break;
    case 0x2c: return raw & c->regs[0x30 / 4];
    case 0x34: return raw;
    case 0x40: c->raw = 0; break;
    case 0x44: c->raw &= ~1u; break;
    case 0x48: c->raw &= ~2u; break;
    case 0x4c: c->raw &= ~8u; break;
    case 0x54: c->raw &= ~0x40u; break;
    case 0x60: c->raw &= ~0x200u; break;
    case 0x70: return 6 | (c->rx_count ? 8 : 0);
    case 0x74: return 0;
    case 0x78: return c->rx_count;
    case 0x80: return 0;
    case 0x9c: return c->regs[0x6c / 4] & 1;
    default: return off < sizeof(c->regs) ? c->regs[off / 4] : 0;
    }
    q66_charger_irq(s);
    return value;
}

static void q66_charger_write(Q66MachineState *s, unsigned off, uint32_t value)
{
    Q66Charger *c = &s->charger;
    if (off >= sizeof(c->regs)) {
        return;
    }
    c->regs[off / 4] = value;
    if (off == 0x6c && !(value & 1)) {
        c->rx_head = c->rx_count = 0;
        c->address_byte = true;
    }
    if (off == 0x10 && (c->regs[0x6c / 4] & 1)) {
        if (value & 0x100) {
            if (c->rx_count < G_N_ELEMENTS(c->rx)) {
                c->rx[(c->rx_head + c->rx_count) % G_N_ELEMENTS(c->rx)] =
                    q66_charger_register(s);
                c->rx_count++;
            } else {
                c->raw |= 2;
            }
        } else if (c->address_byte) {
            c->pointer = value;
            c->address_byte = false;
        } else {
            c->device[c->pointer++] = value;
            c->writes++;
        }
        if (value & 0x200) {
            c->raw |= 0x200;
            c->address_byte = true;
        }
    }
    q66_charger_irq(s);
}

static void q66_charger_reset(Q66MachineState *s)
{
    bool connected = s->charger.connected;
    memset(&s->charger, 0, sizeof(s->charger));
    s->charger.connected = connected;
    s->charger.address_byte = true;
    q66_charger_irq(s);
}
