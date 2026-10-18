/* Adjustable physical GPADC inputs for the local Q66 experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * No guest RAM patching: the original AP reads the normal PMU SPI registers.
 */

static void q66_sensor_init(Object *obj)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    unsigned i;
    for (i = 0; i < 7; i++) {
        s->sensors.input[i] = i == 2 ? 11421 : i == 4 ? 3364 : 9180;
    }
    q66_inputs_init(s);
}

static void q66_sensor_get_adc(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    unsigned channel = GPOINTER_TO_UINT(opaque);
    uint64_t value = s->sensors.input[channel];
    visit_type_uint64(v, name, &value, errp);
}

static void q66_sensor_set_adc(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    unsigned channel = GPOINTER_TO_UINT(opaque);
    uint64_t value;
    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (value > UINT16_MAX) {
        error_setg(errp, "%s must be between 0 and 65535", name);
        return;
    }
    s->sensors.input[channel] = value;
}

static char *q66_sensor_get_state(Object *obj, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    GString *json = g_string_new("{\"version\":1,\"channels\":[");
    unsigned i;
    for (i = 0; i < 7; i++) {
        g_string_append_printf(json,
            "%s{\"channel\":%u,\"input_raw\":%u,\"sampled_raw\":%u,"
            "\"last_read_raw\":%u,\"samples\":%" PRIu64 ","
            "\"guest_reads\":%" PRIu64 "}",
            i ? "," : "", i, s->sensors.input[i],
            s->ispi.analog[3][0][0x56 + i], s->sensors.last_read[i],
            s->sensors.samples[i], s->sensors.reads[i]);
    }
    g_string_append_printf(json,
        "],\"enabled_mask\":%u,\"panel\":{\"power\":%u,"
        "\"brightness\":%u,\"frames\":%u},\"touch\":{"
        "\"received\":%u,\"acknowledged\":%u,\"dropped\":%u,"
        "\"queued\":%u},\"charger\":{\"connected\":%s,"
        "\"reads\":%u,\"writes\":%u,\"pmu_edge_enable\":%u,"
        "\"pmu_irq_mask\":%u,\"pmu_pending\":%u}}",
        s->ispi.analog[3][0][0x1d] & s->ispi.analog[3][0][0x20] & 0x7f,
        s->panel ? s->panel->power : 0,
        s->panel ? s->panel->brightness : 0,
        s->panel ? s->panel->commits : 0,
        s->touch.received, s->touch.acknowledged, s->touch.dropped,
        s->touch.report_count, s->charger.connected ? "true" : "false",
        s->charger.reads, s->charger.writes,
        s->ispi.analog[3][0][5], s->ispi.analog[3][0][0x40],
        s->charger.pending);
    g_string_truncate(json, json->len - 1);
    q66_inputs_state(s, json);
    q66_nfc_state(s, json);
    g_string_append_c(json, '}');
    return g_string_free(json, false);
}

static bool q66_sensor_get_charging(Object *obj, Error **errp)
{
    return Q66_MACHINE(obj)->charger.connected;
}

static void q66_sensor_set_charging(Object *obj, bool value, Error **errp)
{
    q66_charger_set_connected(Q66_MACHINE(obj), value);
}

static void q66_sensor_properties(ObjectClass *klass)
{
    unsigned i;
    q66_inputs_properties(klass);
    q66_nfc_properties(klass);
    for (i = 0; i < 7; i++) {
        g_autofree char *name = g_strdup_printf("sensor-adc-%u", i);
        /* Channel 4 determines the product board, not a user sensor. */
        object_class_property_add(klass, name, "uint64",
            q66_sensor_get_adc, i == 4 ? NULL : q66_sensor_set_adc,
            NULL, GUINT_TO_POINTER(i));
    }
    object_class_property_add_str(klass, "sensor-state",
                                  q66_sensor_get_state, NULL);
    object_class_property_add_bool(klass, "sensor-charging",
                                   q66_sensor_get_charging,
                                   q66_sensor_set_charging);
}
