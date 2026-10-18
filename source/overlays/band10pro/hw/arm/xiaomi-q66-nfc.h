/* Local I2C1 NFC transport observer and manual receive FIFO experiment.
 * No automatic NCI, RF, card, or secure-element responses.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
static void q66_nfc_trace(Q66MachineState *s, const char *direction,
                           const uint8_t *data, unsigned length, bool ack)
{
    Q66Nfc *n = &s->nfc;
    unsigned index = (n->trace_head + n->trace_count) % 96, i;
    char hex[2049];
    length = MIN(length, 1024);
    for (i = 0; i < length; i++) { snprintf(hex + 2 * i, 3, "%02X", data[i]); }
    hex[2 * length] = 0;
    if (n->trace_count == 96) { n->trace_head = (n->trace_head + 1) % 96; }
    else { n->trace_count++; }
    snprintf(n->trace[index], sizeof(n->trace[index]),
        "{\"seq\":%" PRIu64 ",\"direction\":\"%s\",\"hex\":\"%s\","
        "\"address\":%u,\"ack\":%s,\"pc\":%u}",
        ++n->sequence, direction, hex, n->regs[1] & 127,
        ack ? "true" : "false", s->arm.cpu->env.regs[15]);
}

static void q66_nfc_flush(Q66MachineState *s)
{
    Q66Nfc *n = &s->nfc;
    if (n->tx_count) {
        q66_nfc_trace(s, "TX", n->tx, n->tx_count, n->connected);
        n->tx_count = 0;
    }
}

static void q66_nfc_irq(Q66MachineState *s)
{
    Q66Nfc *n = &s->nfc;
    uint32_t raw = n->raw | 0x10;
    if (n->fifo_count > n->regs[0x38 / 4]) { raw |= 4; }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 29),
        n->connected && (n->regs[0x6c / 4] & 1) && (raw & n->regs[0x30 / 4]));
}

static void q66_nfc_reset(Q66MachineState *s)
{
    Q66Nfc *n = &s->nfc;
    q66_nfc_flush(s);
    memset(n->regs, 0, sizeof(n->regs));
    n->rx_head = n->rx_count = n->fifo_head = n->fifo_count = n->raw = 0;
    q66_nfc_irq(s);
}

static uint32_t q66_nfc_read(Q66MachineState *s, unsigned off)
{
    Q66Nfc *n = &s->nfc;
    uint32_t raw = n->raw | 0x10, value = 0;
    if (n->fifo_count > n->regs[0x38 / 4]) { raw |= 4; }
    switch (off) {
    case 0x10:
        if (n->fifo_count) {
            uint8_t byte = n->fifo[n->fifo_head];
            value = byte; n->fifo_head = (n->fifo_head + 1) % sizeof(n->fifo);
            n->fifo_count--; n->rx_bytes++;
            q66_nfc_trace(s, "RX", &byte, 1, true);
        } else { n->raw |= 1; }
        break;
    case 0x2c: return raw & n->regs[0x30 / 4];
    case 0x34: return raw;
    case 0x40: n->raw = 0; break;
    case 0x44: n->raw &= ~1u; break;
    case 0x54: n->raw &= ~0x40u; break;
    case 0x60: n->raw &= ~0x200u; break;
    case 0x70: return 6 | (n->fifo_count ? 8 : 0);
    case 0x74: return 0;
    case 0x78: return n->fifo_count;
    case 0x80: return (n->raw & 0x40) ? 1 : 0;
    case 0x9c: return n->regs[0x6c / 4] & 1;
    default: return off < sizeof(n->regs) ? n->regs[off / 4] : 0;
    }
    q66_nfc_irq(s);
    return value;
}

static void q66_nfc_write(Q66MachineState *s, unsigned off, uint32_t value)
{
    Q66Nfc *n = &s->nfc;
    if (off >= sizeof(n->regs)) { return; }
    if (off == 4 || (off == 0x6c && !(value & 1))) { q66_nfc_flush(s); }
    n->regs[off / 4] = value;
    if (off == 4) { n->fifo_head = n->fifo_count = 0; }
    if (off == 0x10 && (n->regs[0x6c / 4] & 1)) {
        if (value & 0x100) {
            n->read_requests++;
            if (n->connected && n->rx_count && n->fifo_count < sizeof(n->fifo)) {
                n->fifo[(n->fifo_head + n->fifo_count++) % sizeof(n->fifo)] = n->rx[n->rx_head];
                n->rx_head = (n->rx_head + 1) % sizeof(n->rx); n->rx_count--;
            } else if (n->connected) { n->raw |= 0x240; n->aborts++; }
        } else {
            n->tx[n->tx_count++] = value; n->tx_bytes++;
            if (n->tx_count == sizeof(n->tx)) { q66_nfc_flush(s); }
        }
        if (!n->connected) { n->aborts++; }
        if (value & 0x200) { n->raw |= 0x200; q66_nfc_flush(s); }
    }
    q66_nfc_irq(s);
}

static bool q66_nfc_get_connected(Object *obj, Error **errp)
{ return Q66_MACHINE(obj)->nfc.connected; }

static void q66_nfc_set_connected(Object *obj, bool value, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    q66_nfc_flush(s); s->nfc.connected = value;
    s->nfc.raw = s->nfc.fifo_head = s->nfc.fifo_count = 0;
    if (!value) { s->nfc.rx_head = s->nfc.rx_count = 0; }
    q66_nfc_irq(s);
}

static void q66_nfc_set_rx(Object *obj, const char *value, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    Q66Nfc *n = &s->nfc;
    unsigned i, length = strlen(value);
    uint8_t bytes[1024];
    if (!n->connected) { error_setg(errp, "Enable manual NFC transport first"); return; }
    if (!length || length > 2048 || (length & 1)) {
        error_setg(errp, "Expected 1..1024 bytes as compact HEX"); return;
    }
    for (i = 0; i < length; i++) {
        if (g_ascii_xdigit_value(value[i]) < 0) { error_setg(errp, "Invalid HEX"); return; }
    }
    if (n->rx_count + length / 2 > sizeof(n->rx)) { error_setg(errp, "NFC receive queue full"); return; }
    for (i = 0; i < length / 2; i++) {
        bytes[i] = (g_ascii_xdigit_value(value[2*i]) << 4) | g_ascii_xdigit_value(value[2*i+1]);
        n->rx[(n->rx_head + n->rx_count++) % sizeof(n->rx)] = bytes[i];
    }
    q66_nfc_trace(s, "INJECT", bytes, length / 2, true);
}

static void q66_nfc_properties(ObjectClass *klass)
{
    object_class_property_add_bool(klass, "nfc-connected", q66_nfc_get_connected, q66_nfc_set_connected);
    object_class_property_add_str(klass, "nfc-rx", NULL, q66_nfc_set_rx);
}

static void q66_nfc_state(Q66MachineState *s, GString *json)
{
    Q66Nfc *n = &s->nfc;
    unsigned i;
    g_string_append_printf(json, ",\"nfc\":{\"connected\":%s,\"bus\":1,\"address\":%u,"
        "\"tx_bytes\":%" PRIu64 ",\"rx_bytes\":%" PRIu64 ",\"read_requests\":%" PRIu64
        ",\"aborts\":%" PRIu64 ",\"queued\":%u,\"fifo_bytes\":%u,\"trace\":[",
        n->connected ? "true" : "false", n->regs[1] & 127, n->tx_bytes, n->rx_bytes,
        n->read_requests, n->aborts, n->rx_count, n->fifo_count);
    for (i = 0; i < n->trace_count; i++) {
        g_string_append_printf(json, "%s%s", i ? "," : "", n->trace[(n->trace_head + i) % 96]);
    }
    g_string_append(json, "]}");
}
