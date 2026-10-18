/* Local CODEC/AUDMA capture experiment. SPDX-License-Identifier: GPL-2.0-or-later */
static bool q66_audio_active(Q66MachineState *s)
{
    /* IF_EN, ADC_ENABLE and ADC_ENABLE_CH0. The active stock mono
     * recorder enables 0x107 on this silicon/firmware revision. */
    return (q66_reg(s, 0x40300000) & 7) == 7;
}

static uint32_t q66_audio_fifo(Q66MachineState *s, unsigned width)
{
    int32_t sample = 0;
    if (s->audio.count) {
        sample = s->audio.pcm[s->audio.head];
        s->audio.head = (s->audio.head + 1) % G_N_ELEMENTS(s->audio.pcm);
        s->audio.count--;
        s->audio.consumed++;
    } else {
        s->audio.underflows++;
    }
    return width == 4 ? sample * 256 : (uint16_t)sample;
}

static void q66_audio_run(Q66Dma *dma)
{
    Q66MachineState *s = dma->machine;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned ch;
    bool running = false;
    if (!(dma->regs[0x30 / 4] & 1)) { return; }
    for (ch = 0; ch < 8; ch++) {
        uint32_t *reg = &dma->regs[(0x100 + ch * 0x20) / 4];
        unsigned budget = 64;
        if (!(reg[4] & 1) || (reg[4] & (1u << 18))) { continue; }
        running = true;
        if (!q66_audio_active(s) || reg[0] != 0x4030001c) { continue; }
        if (s->audio.next_sample < now - 10000000) { s->audio.next_sample = now; }
        while (budget-- && (reg[4] & 1) && s->audio.next_sample <= now) {
            unsigned count = reg[3] & 0xfff;
            unsigned width = 1u << ((reg[3] >> 21) & 7);
            uint8_t data[4];
            if (!count || width > 4 || width < 2) {
                dma->error_status |= 1u << ch;
                reg[4] &= ~1u;
                break;
            }
            stl_le_p(data, q66_audio_fifo(s, width));
            if (address_space_write(&address_space_memory, reg[1],
                                    MEMTXATTRS_UNSPECIFIED, data, width) != MEMTX_OK) {
                dma->error_status |= 1u << ch;
                reg[4] &= ~1u;
                break;
            }
            s->audio.next_sample += 62500; /* Native recorder PCM: 16 kHz mono. */
            if (reg[3] & (1u << 27)) { reg[1] += width; }
            reg[3] = (reg[3] & ~0xfffu) | (--count);
            if (!count) {
                s->audio.dma_blocks++;
                if (reg[3] & (1u << 31)) { dma->tc_status |= 1u << ch; }
                if (reg[2]) {
                    uint8_t descriptor[16];
                    unsigned i;
                    if (address_space_read(&address_space_memory, reg[2] & ~3u,
                            MEMTXATTRS_UNSPECIFIED, descriptor, 16) != MEMTX_OK) {
                        dma->error_status |= 1u << ch;
                        reg[4] &= ~1u;
                        break;
                    }
                    for (i = 0; i < 4; i++) { reg[i] = ldl_le_p(descriptor + 4 * i); }
                } else { reg[4] &= ~1u; }
            }
        }
    }
    q66_dma_update_irq(dma);
    if (running) { timer_mod(dma->timer, now + 1000000); }
}

static void q66_audio_set_pcm(Object *obj, const char *value, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    size_t i, length = strlen(value);
    if (length % 4 || length > 32768) {
        error_setg(errp, "PCM must be hexadecimal little-endian signed 16-bit mono");
        return;
    }
    for (i = 0; i < length; i++) {
        if (g_ascii_xdigit_value(value[i]) < 0) {
            error_setg(errp, "Invalid PCM hexadecimal input"); return;
        }
    }
    if (!q66_audio_active(s)) { return; }
    for (i = 0; i < length; i += 4) {
        unsigned lo = (g_ascii_xdigit_value(value[i]) << 4) | g_ascii_xdigit_value(value[i + 1]);
        unsigned hi = (g_ascii_xdigit_value(value[i + 2]) << 4) | g_ascii_xdigit_value(value[i + 3]);
        if (s->audio.count == G_N_ELEMENTS(s->audio.pcm)) {
            s->audio.head = (s->audio.head + 1) % G_N_ELEMENTS(s->audio.pcm);
            s->audio.count--;
        }
        s->audio.pcm[(s->audio.head + s->audio.count++) % G_N_ELEMENTS(s->audio.pcm)] = lo | (hi << 8);
        s->audio.injected++;
    }
}

static void q66_audio_properties(ObjectClass *klass)
{
    object_class_property_add_str(klass, "audio-pcm", NULL, q66_audio_set_pcm);
}

static void q66_audio_state(Q66MachineState *s, GString *json)
{
    unsigned ch;
    g_string_append_printf(json, ",\"audio\":{\"capture_active\":%s,\"rate\":16000,"
        "\"queued\":%u,\"injected\":%" PRIu64 ",\"consumed\":%" PRIu64
        ",\"underflows\":%" PRIu64 ",\"dma_blocks\":%" PRIu64 ",\"codec_control\":%u,\"dma\":[",
        q66_audio_active(s) ? "true" : "false", s->audio.count, s->audio.injected,
        s->audio.consumed, s->audio.underflows, s->audio.dma_blocks, q66_reg(s, 0x40300000));
    for (ch = 0; ch < 8; ch++) {
        uint32_t *r = &s->audma.regs[(0x100 + ch * 0x20) / 4];
        g_string_append_printf(json, "%s{\"channel\":%u,\"source\":%u,\"destination\":%u,"
            "\"control\":%u,\"config\":%u}", ch ? "," : "", ch, r[0], r[1], r[3], r[4]);
    }
    g_string_append(json, "]}");
}
