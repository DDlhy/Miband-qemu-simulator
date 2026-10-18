/* Optional external H4 controller transport for the local Q66 experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
static char *q66_hci_get_chardev(Object *obj, Error **errp)
{
    return g_strdup(Q66_MACHINE(obj)->hci_chardev);
}

static void q66_hci_set_chardev(Object *obj, const char *value, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    g_free(s->hci_chardev);
    s->hci_chardev = g_strdup(value);
}

static int q66_hci_can_read(void *opaque)
{
    Q66MachineState *s = opaque;
    if (s->hci_event_count == G_N_ELEMENTS(s->hci_events)) {
        return 0;
    }
    return sizeof(s->hci_stream) - s->hci_stream_len;
}

static void q66_hci_read(void *opaque, const uint8_t *bytes, int count)
{
    Q66MachineState *s = opaque;
    unsigned offset = 0;
    if (count < 0 || (unsigned)count >
        sizeof(s->hci_stream) - s->hci_stream_len) {
        error_report("q66 HCI receive buffer overflow");
        return;
    }
    if (count) {
        memcpy(s->hci_stream + s->hci_stream_len, bytes, count);
        s->hci_stream_len += count;
    }
    while (offset < s->hci_stream_len &&
           s->hci_event_count < G_N_ELEMENTS(s->hci_events)) {
        uint8_t *p = s->hci_stream + offset;
        unsigned available = s->hci_stream_len - offset;
        unsigned size, slot;
        if (p[0] == 4) {
            if (available < 3) { break; }
            size = 3 + p[2];
        } else if (p[0] == 2) {
            if (available < 5) { break; }
            size = 5 + lduw_le_p(p + 3);
        } else {
            error_report("q66 HCI backend sent unsupported H4 type %u", p[0]);
            qemu_chr_fe_disconnect(&s->hci_chr);
            s->hci_stream_len = 0;
            return;
        }
        if (size > sizeof(s->hci_events[0])) {
            error_report("q66 HCI backend packet too large: %u", size);
            qemu_chr_fe_disconnect(&s->hci_chr);
            s->hci_stream_len = 0;
            return;
        }
        if (available < size) { break; }
        slot = (s->hci_event_head + s->hci_event_count++) %
               G_N_ELEMENTS(s->hci_events);
        memcpy(s->hci_events[slot], p, size);
        s->hci_event_len[slot] = size;
        offset += size;
    }
    memmove(s->hci_stream, s->hci_stream + offset, s->hci_stream_len - offset);
    s->hci_stream_len -= offset;
    if (s->hci_event_count && !s->intersys_rx_pending &&
        !timer_pending(s->intersys_rx_timer)) {
        timer_mod(s->intersys_rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

static void q66_hci_chr_event(void *opaque, QEMUChrEvent event)
{
    Q66MachineState *s = opaque;
    s->hci_stream_len = 0;
    qemu_log_mask(LOG_UNIMP, "q66 HCI bridge event=%d\n", event);
}
