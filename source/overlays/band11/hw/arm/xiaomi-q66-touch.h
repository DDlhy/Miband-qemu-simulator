/* CST9217M on BEST1503 I2C3, for the local Q66 experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Wire protocol follows the unchanged AP's cst92xx driver. Firmware identity
 * matches the supplied HYN_TM_CST9217M_Q66.bin trailer, not a guest patch.
 */

static void q66_touch_i2c_irq(Q66MachineState *s)
{
    Q66Touch *t = &s->touch;
    uint32_t raw = t->raw | (1u << 4); /* synchronous TX FIFO is empty */
    if (t->rx_count > t->regs[0x38 / 4]) {
        raw |= 1u << 2;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 65),
                 !!(raw & t->regs[0x30 / 4]) && (t->regs[0x6c / 4] & 1));
}

static void q66_touch_reset_pin(Q66MachineState *s, bool low)
{
    Q66Touch *t = &s->touch;
    if (t->reset_low == low) {
        return;
    }
    t->reset_low = low;
    qemu_log_mask(LOG_UNIMP, "q66 CST reset gpio=28 low=%u sleep=%u pc=%08x\n",
                  low, t->sleep_mode, s->arm.cpu->env.regs[15]);
    if (!low) {
        return;
    }
    /* Reset only the external chip. The SoC I2C controller registers,
     * FIFOs, IRQ status and cumulative input diagnostics remain intact. */
    timer_del(t->timer);
    t->pointer = t->mem_address = t->read_offset = 0;
    t->mode = t->sleep_mode = 0;
    t->tap_down_ns = t->last_tap_ns = 0;
    t->tap_moved = false;
    t->wrist = t->palm = t->irq_low = false;
    t->report_head = t->report_count = 0;
    s->gpio_pending[0] &= ~(1u << 27);
    if (s->panel) {
        s->panel->touch_reported_down = false;
    }
    q66_gpio_update_irq(s);
}

static void q66_touch_raise(void *opaque)
{
    Q66MachineState *s = opaque;
    Q66Touch *t = &s->touch;
    if (!t->reset_low && t->report_count && !t->irq_low) {
        t->irq_low = true;
        s->gpio_pending[0] |= 1u << 27;
        q66_gpio_update_irq(s);
    }
}

static void q66_touch_ack(Q66MachineState *s)
{
    Q66Touch *t = &s->touch;
    if (t->report_count) {
        t->report_head = (t->report_head + 1) % G_N_ELEMENTS(t->reports);
        t->report_count--;
        t->acknowledged++;
    }
    t->irq_low = false;
    if (t->report_count) {
        /* Leave a high interval until the original GPIO ISR has acknowledged
         * its edge and the worker has re-enabled the interrupt. */
        timer_mod(t->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
    }
}

static void q66_touch_commit_write(Q66MachineState *s)
{
    Q66Touch *t = &s->touch;
    unsigned n = t->tx_count;
    if (!n) {
        return;
    }
    if (t->traces++ < 128) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 CST W n=%u bytes=%02x %02x %02x %02x pc=%08x\n",
                      n, t->tx[0], n > 1 ? t->tx[1] : 0,
                      n > 2 ? t->tx[2] : 0, n > 3 ? t->tx[3] : 0,
                      s->arm.cpu->env.regs[15]);
    }
    t->pointer = (t->tx[0] << 8) | (n > 1 ? t->tx[1] : 0);
    t->read_offset = 0;
    if (n == 2 && t->tx[0] == 0xd1 && t->tx[1] < 0x80) {
        t->mode = t->tx[1];
        if (t->mode == 4 || t->mode == 5 || t->mode == 7 || t->mode == 9) {
            /* Stock enter_sleep uses D104/D105/D107; normal mode is D109.
             * Configuration commands must not change the latched sleep mode. */
            t->sleep_mode = t->mode == 9 ? 0 : t->mode;
            t->tap_down_ns = t->last_tap_ns = 0;
        }
        switch (t->mode) {
        case 0x25: t->wrist = true; break;
        case 0x26: t->wrist = false; break;
        case 0x23: t->palm = true; break;
        case 0x24: t->palm = false; break;
        default: break;
        }
    } else if (n == 4 && t->pointer == 0xa00c) {
        t->mem_address = lduw_le_p(t->tx + 2);
    } else if (n == 3 && t->pointer == 0x0005 && !t->tx[2]) {
        q66_touch_ack(s);
    }
    t->tx_count = 0;
}

static uint8_t q66_touch_read_byte(Q66MachineState *s)
{
    Q66Touch *t = &s->touch;
    unsigned i = t->read_offset++;
    uint32_t word = 0;
    switch (t->pointer) {
    case 0x0002: return i == 1 ? t->mode : 0;
    case 0x0009: return t->mode;
    case 0xa002: return i == 0 ? 0x55 : 0xb0;
    case 0xa004: return 0; /* indirect memory read finished */
    case 0xa018:
        word = t->mem_address == 0x077c ? 0xcaca9217 :
               t->mem_address == 0x7fc0 ? 0x05660312 : 0;
        break;
    case 0xd1fc: word = 0x55aa55aa; break;
    case 0xd200: word = 0; break;
    case 0xd204: word = 0x92176213; break;
    case 0xd208: word = 0x01010008; break;
    case 0xd20c: word = 0xbcc68747; break;
    case 0xd21c: word = 0x55aa55aa + 0x92176213u + 0x01010008; break;
    case 0xd1f4: word = 0x000a0004; break;
    case 0xd048:
        /* ESD status: valid normal-mode value, wrist/palm settings echoed.
         * AP accepts 0x20..0x29 and checks these two configuration bits. */
        return i == 0 ? 0x29 : (t->wrist ? 2 : 0) | (t->palm ? 1 : 0);
    case 0xd000:
        if (i < 10 && t->report_count) {
            return t->reports[t->report_head][i];
        }
        return i == 6 ? 0xab : 0;
    default:
        qemu_log_mask(LOG_UNIMP, "q66 CST unknown read reg=%04x byte=%u\n",
                      t->pointer, i);
        break;
    }
    return i < 4 ? (word >> (i * 8)) & 0xff : 0;
}

static uint32_t q66_touch_i2c_read(Q66MachineState *s, unsigned off)
{
    Q66Touch *t = &s->touch;
    uint32_t raw = t->raw | 0x10;
    uint32_t value;
    if (t->rx_count > t->regs[0x38 / 4]) {
        raw |= 4;
    }
    switch (off) {
    case 0x10:
        if (!t->rx_count) {
            t->raw |= 1;
            value = 0;
        } else {
            value = t->rx[t->rx_head];
            t->rx_head = (t->rx_head + 1) % G_N_ELEMENTS(t->rx);
            t->rx_count--;
        }
        q66_touch_i2c_irq(s);
        return value;
    case 0x2c: return raw & t->regs[0x30 / 4];
    case 0x34: return raw;
    case 0x40: t->raw = t->abort_source = 0; break;
    case 0x44: t->raw &= ~1u; break;
    case 0x48: t->raw &= ~2u; break;
    case 0x4c: t->raw &= ~8u; break;
    case 0x54: t->raw &= ~0x40u; t->abort_source = 0; break;
    case 0x60: t->raw &= ~0x200u; break;
    case 0x70: return 6 | (t->rx_count ? 8 : 0);
    case 0x74: return 0;
    case 0x78: return t->rx_count;
    case 0x80: return t->abort_source;
    case 0x9c: return t->regs[0x6c / 4] & 1;
    case 0xfc: return 0x44570140;
    default: return off < sizeof(t->regs) ? t->regs[off / 4] : 0;
    }
    q66_touch_i2c_irq(s);
    return 0;
}

static void q66_touch_i2c_write(Q66MachineState *s, unsigned off,
                                 uint32_t value)
{
    Q66Touch *t = &s->touch;
    if (off >= sizeof(t->regs)) {
        return;
    }
    t->regs[off / 4] = value;
    if (off == 0x6c && !(value & 1)) {
        t->rx_head = t->rx_count = t->tx_count = 0;
        t->reading = false;
    }
    if (off == 0x10 && (t->regs[0x6c / 4] & 1)) {
        if (t->reset_low || (t->regs[0x04 / 4] & 0x3ff) != 0x5a) {
            t->raw |= 0x40;
            t->abort_source = 1; /* 7-bit address NAK */
        } else if (value & 0x100) {
            if (!t->reading) {
                q66_touch_commit_write(s);
                t->reading = true;
            }
            if (t->rx_count < G_N_ELEMENTS(t->rx)) {
                t->rx[(t->rx_head + t->rx_count) % G_N_ELEMENTS(t->rx)] =
                    q66_touch_read_byte(s);
                t->rx_count++;
            } else {
                t->raw |= 2;
            }
        } else {
            if (t->reading) {
                t->reading = false;
                t->read_offset = 0;
            }
            if (t->tx_count < sizeof(t->tx)) {
                t->tx[t->tx_count++] = value;
            } else {
                t->raw |= 8;
            }
        }
        if (value & 0x200) {
            q66_touch_commit_write(s);
            t->reading = false;
            t->raw |= 0x200;
        }
    }
    q66_touch_i2c_irq(s);
}

static void q66_touch_input_event(DeviceState *dev, QemuConsole *src,
                                  QemuInputEvent *event)
{
    Q66PanelState *p = Q66_PANEL(dev);
    if (event->type == INPUT_EVENT_KIND_ABS) {
        int *axis = event->abs.axis == INPUT_AXIS_X ? &p->touch_x : &p->touch_y;
        int extent = event->abs.axis == INPUT_AXIS_X ? WIDTH : HEIGHT;
        int position = (CLAMP(event->abs.value, 0, 0x7fff) * (extent - 1) +
                        0x3fff) / 0x7fff;
        if (*axis != position) {
            *axis = position;
            p->touch_dirty = true;
        }
    } else if (event->type == INPUT_EVENT_KIND_BTN &&
               event->btn.button == INPUT_BUTTON_LEFT) {
        if (p->touch_down != event->btn.down) {
            p->touch_down = event->btn.down;
            p->touch_dirty = true;
        }
    }
}

static void q66_touch_input_sync(DeviceState *dev)
{
    Q66PanelState *p = Q66_PANEL(dev);
    Q66Touch *t;
    uint8_t *r;
    unsigned sw;
    if (!p->machine || !p->touch_dirty) {
        return;
    }
    p->touch_dirty = false;
    if (!p->touch_down && !p->touch_reported_down) {
        return; /* hovering is not a touch */
    }
    t = &p->machine->touch;
    if (t->reset_low) {
        p->touch_reported_down = false;
        return;
    }
    if (t->sleep_mode) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        bool down = p->touch_down && !p->touch_reported_down;
        bool up = !p->touch_down && p->touch_reported_down;
        p->touch_reported_down = p->touch_down;
        if (t->sleep_mode != 4) {
            return; /* Deep/idle sleep does not accept wake gestures. */
        }
        if (down) {
            t->tap_down_ns = now;
            t->tap_x = p->touch_x;
            t->tap_y = p->touch_y;
            t->tap_moved = false;
        }
        if (abs(p->touch_x - t->tap_x) > 12 ||
            abs(p->touch_y - t->tap_y) > 12) {
            t->tap_moved = true;
        }
        if (!up || !t->tap_down_ns) {
            return;
        }
        if (t->tap_moved || now - t->tap_down_ns > 500 * SCALE_MS) {
            t->tap_down_ns = t->last_tap_ns = 0;
            return;
        }
        t->tap_down_ns = 0;
        if (!t->last_tap_ns || now - t->last_tap_ns > 350 * SCALE_MS ||
            abs(p->touch_x - t->last_tap_x) > 24 ||
            abs(p->touch_y - t->last_tap_y) > 24) {
            t->last_tap_ns = now;
            t->last_tap_x = p->touch_x;
            t->last_tap_y = p->touch_y;
            return;
        }
        t->last_tap_ns = 0;
        if (t->report_count == G_N_ELEMENTS(t->reports)) {
            t->dropped++;
            return;
        }
        r = t->reports[(t->report_head + t->report_count) % G_N_ELEMENTS(t->reports)];
        memset(r, 0, 10);
        /* Original hynitron_read_point extracts gesture ID from byte 4's
         * high nibble. ID 2 reaches its double-click wake-report branch. */
        r[4] = 0x20;
        r[6] = 0xab;
        t->report_count++;
        t->received++;
        qemu_log_mask(LOG_UNIMP, "q66 CST double-click wake gesture queued=%u\n",
                      t->report_count);
        if (!t->irq_low && !timer_pending(t->timer)) {
            q66_touch_raise(p->machine);
        }
        return;
    }
    if (t->report_count == G_N_ELEMENTS(t->reports)) {
        /* Coalesce motion before sacrificing either contact transition. */
        unsigned tail = (t->report_head + t->report_count - 1) %
                        G_N_ELEMENTS(t->reports);
        if (t->reports[tail][0] == 7) {
            t->report_count--;
        } else {
            t->dropped++;
            return;
        }
    }
    sw = !p->touch_down ? 0 : p->touch_reported_down ? 7 : 6;
    r = t->reports[(t->report_head + t->report_count) % G_N_ELEMENTS(t->reports)];
    memset(r, 0, 10);
    r[0] = sw;
    r[1] = p->touch_x >> 4;
    r[2] = p->touch_y >> 4;
    r[3] = ((p->touch_x & 15) << 4) | (p->touch_y & 15);
    /* The CST driver requires one record even for the UP transition. */
    r[5] = 1;
    r[6] = 0xab;
    t->report_count++;
    t->received++;
    p->touch_reported_down = p->touch_down;
    qemu_log_mask(LOG_UNIMP, "q66 CST input sw=%u x=%d y=%d queued=%u\n",
                  sw, p->touch_x, p->touch_y, t->report_count);
    if (!t->irq_low && !timer_pending(t->timer)) {
        q66_touch_raise(p->machine);
    }
}

static const QemuInputHandler q66_touch_input = {
    .name = "Q66 CST9217M touchscreen",
    .mask = INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = q66_touch_input_event,
    .sync = q66_touch_input_sync,
};

static void q66_touch_reset(Q66MachineState *s)
{
    QEMUTimer *timer = s->touch.timer;
    timer_del(timer);
    memset(&s->touch, 0, sizeof(s->touch));
    s->touch.timer = timer;
    q66_touch_i2c_irq(s);
    if (s->panel) {
        s->panel->touch_down = s->panel->touch_reported_down = false;
        s->panel->touch_dirty = false;
    }
}
