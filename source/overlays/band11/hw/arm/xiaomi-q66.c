/*
 * Xiaomi Smart Band 11 NFC (q66nfc) local bring-up model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is an experimental board, not a complete BES2700imp implementation.
 * Memory aliases and the AP entry point come from the supplied OTA binary.
 * The panel transport also exposes a diagnostic interface at 0x60000000.
 * Stock AP frames reach it through the modeled BES LCDC at 0x40200000.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "elf.h"
#include "exec/tb-flush.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "target/arm/cpu.h"
#include "target/arm/arm-powerctl.h"
#include "target/arm/internals.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/physmem.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "chardev/char-fe.h"

#define WIDTH 212
#define HEIGHT 520
#define FLASH_SIZE (16 * MiB)
#define PSRAM_SIZE (32 * MiB)
#define NAND_SIZE (256 * MiB)
#define NAND_PAGE_SIZE 2048
#define NAND_SPARE_SIZE 128
#define NAND_PAGES (NAND_SIZE / NAND_PAGE_SIZE)
#define NAND_DATA_SIZE (128 * MiB)
#define NAND_DATA_FIRST_PAGE (NAND_DATA_SIZE / NAND_PAGE_SIZE)
#define NAND_DATA_OOB_SIZE (NAND_DATA_FIRST_PAGE * NAND_SPARE_SIZE)
#define TYPE_Q66_MACHINE MACHINE_TYPE_NAME("xiaomi-q66nfc")
#define TYPE_Q66_PANEL "q66-diagnostic-panel"
OBJECT_DECLARE_SIMPLE_TYPE(Q66MachineState, Q66_MACHINE)
OBJECT_DECLARE_SIMPLE_TYPE(Q66PanelState, Q66_PANEL)

typedef struct Q66MmioWindow {
    MemoryRegion region;
    Q66MachineState *machine;
    uint32_t base;
} Q66MmioWindow;

typedef struct Q66Ispi {
    MemoryRegion region;
    Q66MachineState *machine;
    uint32_t regs[35];
    uint32_t tx[8], rx[8];
    unsigned tx_head, tx_count, rx_head, rx_count;
    uint8_t page[16];
    uint16_t analog[16][5][256];
    bool overrun;
    /* AON global reset resets the AP, but the analog PMU bank remains
     * powered on real BEST1503 silicon.  Keep that bank across the one
     * reset requested by pmu_reboot(); cold machine reset still clears it. */
    bool retain_analog_on_reset;
    bool external;
    unsigned external_traces;
} Q66Ispi;

typedef struct Q66Nor {
    MemoryRegion region;
    Q66MachineState *machine;
    uint32_t regs[256];
    /* The physical TX FIFO is 16 bytes, but the AP can refill it many times
     * before issuing REG_000.  Keep the FIFO occupancy separate from the
     * transaction staging buffer so long writes do not overwrite the first
     * bytes while the emulated flash drains the hardware FIFO. */
    uint8_t tx[8192], rx[8192];
    unsigned tx_count, tx_slots, rx_head, rx_count;
    uint32_t nor_program_control;
    unsigned nor_program_length;
    uint8_t status[3];
    bool write_enable;
    BlockBackend *nand_data;
    BlockBackend *nand_resource;
    uint8_t nand_failure;
    uint8_t nand_features[256];
    uint8_t nand_cache[NAND_PAGE_SIZE + NAND_SPARE_SIZE];
    uint8_t *nand_spare;
    uint32_t nand_page;
    uint8_t nand_pending_command;
    uint32_t nand_pending_address;
    unsigned nand_pending_length;
    unsigned nand_traces[256];
    unsigned nand_xip_write_traces;
} Q66Nor;

typedef struct Q66Dma {
    MemoryRegion region;
    Q66MachineState *machine;
    uint32_t regs[256];
    uint32_t tc_status, error_status;
    QEMUTimer *timer;
    unsigned traces;
    bool audio;
} Q66Dma;

typedef struct Q66Audio {
    int16_t pcm[32000];
    unsigned head, count;
    uint64_t injected, consumed, underflows, dma_blocks;
    int64_t next_sample;
} Q66Audio;

typedef struct Q66Psram {
    MemoryRegion region;
    Q66MachineState *machine;
    uint32_t regs[0x9000 / 4];
    uint32_t tx[8], rx[8];
    unsigned tx_count, rx_head, rx_count;
    unsigned transfer_traces;
    uint8_t mode[256];
} Q66Psram;

typedef struct Q66Touch {
    uint32_t regs[64];
    uint8_t tx[64], rx[16];
    unsigned tx_count, rx_head, rx_count;
    uint32_t raw, abort_source;
    uint16_t pointer, mem_address;
    unsigned read_offset, traces;
    uint8_t mode, sleep_mode;
    int64_t tap_down_ns, last_tap_ns;
    int tap_x, tap_y, last_tap_x, last_tap_y;
    bool tap_moved;
    bool reading, irq_low, wrist, palm, reset_low;
    uint8_t reports[32][10];
    unsigned report_head, report_count;
    uint32_t received, acknowledged, dropped;
    QEMUTimer *timer;
} Q66Touch;

typedef struct Q66Sensors {
    uint16_t input[7], last_read[7];
    uint64_t samples[7], reads[7];
} Q66Sensors;

typedef struct Q66Charger {
    uint32_t regs[64], raw;
    uint8_t device[256], pointer, rx[16];
    unsigned rx_head, rx_count, writes, reads;
    bool address_byte, connected;
    uint8_t pending;
} Q66Charger;

typedef struct Q66InputBus {
    uint32_t regs[64], raw;
    uint8_t device[128][256], present[128], pointer, rx[256];
    unsigned rx_head, rx_count;
    bool address_byte;
} Q66InputBus;

typedef struct Q66Inputs {
    Q66InputBus bus[6];
    double accel[3], gyro[3], mag[3], pd[4], ambient, als_lux;
    uint64_t als_reads, als_writes, als_samples;
    double pulse_bpm, pulse_amplitude;
    uint64_t mag_reads, imu_reads, ppg_reads, bus_sequence;
    unsigned spi_byte, spi_reg, spi_device;
    bool spi_read, bus_logging;
    uint64_t ppg_writes, imu_writes;
    char trace[128][192];
    char optical_trace[128][192];
    unsigned optical_head, optical_count;
    unsigned trace_head, trace_count, traces;
    uint8_t imu_regs[256], ppg_regs[4][128], ppg_page;
    uint8_t ppg_fifo[1536];
    unsigned ppg_head, ppg_count;
    uint64_t ppg_samples, vibration_events;
    uint64_t ppg_fifo_reads, ppg_direct_reads;
    uint32_t ppg_adc[16];
    uint32_t ppg_read_word, ppg_read_bytes, ppg_last_mask;
    int32_t ppg_last_samples[16];
    bool vibrating;
    QEMUTimer *sample_timer;
    uint8_t imu_fifo[3584];
    unsigned imu_head, imu_count;
    uint64_t imu_samples, imu_fifo_reads;
    uint8_t imu_last_packet[7];
    uint8_t imu_last_accel[7], imu_last_gyro[7];
    uint8_t mag_fifo[32][8];
    unsigned mag_head, mag_count;
    int64_t mag_next;
    int64_t ppg_next, ppg_prf_until, imu_next[2];
    uint8_t ppg_int_status;
    bool ppg_irq, imu_irq;
} Q66Inputs;

typedef struct Q66Nfc {
    bool connected;
    uint8_t tx[1024], rx[4096], fifo[256];
    unsigned tx_count, rx_head, rx_count, fifo_head, fifo_count;
    uint32_t regs[64], raw;
    uint64_t tx_bytes, rx_bytes, read_requests, aborts, sequence;
    char trace[96][2304];
    unsigned trace_head, trace_count;
} Q66Nfc;

struct Q66PanelState {
    SysBusDevice parent_obj;
    MemoryRegion control;
    MemoryRegion vram;
    QemuConsole *console;
    uint32_t power;
    uint32_t commits;
    uint32_t brightness;
    Q66MachineState *machine;
    QemuInputHandlerState *input;
    int touch_x, touch_y;
    bool touch_down, touch_reported_down, touch_dirty;
};

struct Q66MachineState {
    MachineState parent_obj;
    ARMv7MState arm, cp;
    MemoryRegion cp_memory;
    bool cp_started;
    uint32_t cp_to_ap_irq, ap_to_cp_irq;
    uint64_t cp_boots;
    MemoryRegion vectors, sram, psram, bt_ram, flash, resource;
    BlockBackend *flash_backend;
    MemoryRegion aliases[9];
    Q66MmioWindow peripherals, bt_subsys, bt_aux;
    Q66MmioWindow core_cache[2];
    Q66Ispi ispi;
    Q66Ispi spi;
    Q66Nor nor;
    Q66Nor nor1;
    Q66Dma dma;
    Q66Dma audma;
    Q66Audio audio;
    Q66Psram psram_ctrl;
    Q66Touch touch;
    Q66Sensors sensors;
    Q66Charger charger;
    Q66Inputs inputs;
    Q66Nfc nfc;
    QEMUTimer *psc_timer;
    bool psc_power_down;
    unsigned psc_wake_count;
    GHashTable *registers;
    uint32_t last_read, last_write;
    unsigned trace_count;
    unsigned display_trace_count;
    uint8_t lcdc_read_reg;
    uint8_t lcdc_write_command;
    uint32_t lcdc_framebuffer;
    uint32_t lcdc_irq_status;
    QEMUTimer *lcdc_frame_timer;
    QEMUTimer *panel_te_timer;
    bool panel_te_enabled;
    uint32_t gpio_pending[3];
    unsigned gpio_trace_count;
    unsigned panel_te_count;
    Q66PanelState *panel;
    uint8_t memsc_locked;
    uint8_t memsc_aon_locked;
    /* The physical power key is held only long enough to wake the band.  The
     * stock boot debounce samples it ten times, then requires a release
     * before the normal application/rendering path can run. */
    uint8_t pwrkey_startup_reads;
    bool resource_flash1_mapped;
    /* The AP raises CMU_MCU2BT_DATA1_IND through ISIRQ_SET[3] when it
     * wakes the BT core.  There is no second CPU in this bring-up model, so
     * complete the controller's acknowledge handshake after the AP's
     * readback of the posted write. */
    uint8_t bt_wakeup_ack_reads;
    uint32_t intersys_control;
    uint8_t intersys_tx_pending;
    uint8_t intersys_rx_pending;
    QEMUTimer *intersys_tx_timer;
    QEMUTimer *intersys_rx_timer;
    uint8_t hci_events[32][4096];
    uint16_t hci_event_len[32];
    unsigned hci_event_head, hci_event_count;
    char *hci_chardev;
    char *ota_filename;
    char *recovery_filename;
    uint32_t boot_entry;
    uint32_t initial_bootmode;
    CharFrontend hci_chr;
    uint8_t hci_stream[8192];
    unsigned hci_stream_len;
    unsigned gpu_trace_count;
    uint32_t gpu_states[0x1000];
    uint32_t gpu_irq_status;
    QEMUTimer *gpu_timer;
    uint32_t gpu_submits, gpu_draws, gpu_errors, gpu_pixels;
    uint32_t gpu_command_address, gpu_command_bytes;
    QEMUTimer *gpadc_timer;
    unsigned gpadc_trace_count;
    uint8_t hci_address[6];
    uint8_t hci_le_address[6];
    uint8_t hci_sleep_settings[8];
    uint8_t hci_event_mask[8], hci_le_event_mask[8];
};

static void q66_dma_schedule(Q66MachineState *s);
static void q66_charger_set_connected(Q66MachineState *s, bool connected);
static uint32_t q66_reg(Q66MachineState *s, uint32_t address);
static void q66_gpadc_update_irq(Q66MachineState *s);
static void q66_inputs_init(Q66MachineState *s);
static void q66_inputs_properties(ObjectClass *klass);
static void q66_inputs_state(Q66MachineState *s, GString *json);
static void q66_audio_run(Q66Dma *dma);
static void q66_audio_properties(ObjectClass *klass);
static void q66_audio_state(Q66MachineState *s, GString *json);
static uint32_t q66_audio_fifo(Q66MachineState *s, unsigned width);
static void q66_nfc_properties(ObjectClass *klass);
static void q66_nfc_state(Q66MachineState *s, GString *json);

#include "xiaomi-q66-sensors.h"

static void q66_gpadc_update_irq(Q66MachineState *s)
{
    uint16_t *pmu = s->ispi.analog[3][0];
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 42),
                 !!(pmu[0x50] & pmu[0x1f] & pmu[0x20] & 0x1ff) ||
                 (s->charger.pending && (pmu[5] & 0x1800) &&
                  (pmu[0x40] & 0x1000)));
}

static void q66_gpadc_sample(void *opaque)
{
    Q66MachineState *s = opaque;
    uint16_t *pmu = s->ispi.analog[3][0];
    unsigned channels = pmu[0x1d] & pmu[0x20] & 0x7f;
    unsigned i;
    for (i = 0; i < 7; i++) {
        if (channels & (1u << i)) {
            /* AP calibration: 15968 -> 800 mV, 20816 -> 1050 mV.
             * The supplied AP's soft_gauge uses a VBAT/7 divider.
             * Channel 4 is the board-version divider. About 150 mV selects
             * board 1 (Q66 NFC) in the AP's 101..200 mV interval table.
             * The NTC divider inputs represent about 450 mV. */
            pmu[0x56 + i] = s->sensors.input[i];
            s->sensors.samples[i]++;
        }
    }
    pmu[0x50] |= channels | 0x100;
    if (s->gpadc_trace_count++ < 64) {
        qemu_log_mask(LOG_UNIMP, "q66 GPADC sample channels=%02x raw=%04x "
                      "mask=%04x enable=%04x\n", channels, pmu[0x50],
                      pmu[0x1f], pmu[0x20]);
    }
    q66_gpadc_update_irq(s);
    if (channels && (pmu[0x18] & 0x1000) && (pmu[0x4f] & 0x20)) {
        timer_mod(s->gpadc_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MAX(1, pmu[0x1c]) * 1024);
    }
}

/* The stock AP uses three SET/CLR GPIO banks, with a shared IRQ 44.
 * Its RM690 TE input is P8_1 (bank 2, bit 1). */
static const uint32_t q66_gpio_bases[] = {
    0x40081000, 0x40089000, 0x4008a000,
};

static uint32_t q66_reg(Q66MachineState *s, uint32_t address)
{
    return GPOINTER_TO_UINT(g_hash_table_lookup(s->registers,
                                               GUINT_TO_POINTER(address)));
}

static void q66_cp_update_irq(Q66MachineState *s)
{
    unsigned i;
    for (i = 0; i < 4; i++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 36 + i),
                     !!(s->cp_to_ap_irq & (0x10u << i)));
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 48 + i),
                     !!(s->cp_to_ap_irq & (1u << i)));
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cp), 36 + i),
                     !!(s->ap_to_cp_irq & (0x10u << i)));
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cp), 48 + i),
                     !!(s->ap_to_cp_irq & (1u << i)));
    }
}

static void q66_cp_start_on_cpu(CPUState *cs, run_on_cpu_data data)
{
    Q66MachineState *s = data.host_ptr;
    ARMCPU *cpu = s->cp.cpu;
    uint32_t vector = q66_reg(s, 0x40080110) & ~127u;
    if (!s->cp_started) { return; }
    cpu->init_svtor = vector;
    device_cold_reset(DEVICE(&s->cp.nvic));
    cpu_reset(cs);
    arm_set_cpu_power_state(cpu, PSCI_ON);
    cs->halted = false;
    s->cp_boots++;
    q66_cp_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
        "q66 CP boot vector=%08x pc=%08x sp=%08x\n",
        vector, cpu->env.regs[15], cpu->env.regs[13]);
    qemu_cpu_kick(cs);
}

static void q66_psc_power_cycle_on_cpu(CPUState *cs, run_on_cpu_data data)
{
    Q66MachineState *s = data.host_ptr;
    ARMCPU *cpu = s->arm.cpu;
    uint32_t vector = q66_reg(s, 0x40080078) & 0xffffff80;
    uint32_t initial_svtor;
    bool levels[128];
    unsigned i;

    /* CPU work runs with the BQL held, outside cpu_exec(). A timer's BQL
     * does not prevent MTTCG from executing or returning from a TB. Resetting
     * there can clear icount_decr after TCG has requested a TB exit. */
    g_assert(qemu_cpu_is_self(cs));
    g_assert(!qatomic_read(&cs->running));
    if (!s->psc_power_down) {
        return;
    }
    /* The AON controller wakes the powered-down core on an enabled pending
     * interrupt, even while the saved CPU context has PRIMASK asserted. */
    if (!(cs->interrupt_request & CPU_INTERRUPT_HARD)) {
        cs->halted = true;
        timer_mod(s->psc_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
        return;
    }
    for (i = 0; i < G_N_ELEMENTS(levels); i++) {
        levels[i] = s->arm.nvic.vectors[NVIC_INTERNAL_VECTORS + i].level;
    }
    s->psc_power_down = false;
    initial_svtor = cpu->init_svtor;
    cpu->init_svtor = vector;
    /* Only the core/NVIC reset domain loses state. Retain SRAM, peripheral
     * state and AON GPIO latches. The AP programmed MCU_VTOR and its reset
     * slot to hal_sleep_core_power_up; that stock code restores its own
     * saved stack and registers. No firmware address is hard-coded here. */
    device_cold_reset(DEVICE(&s->arm.nvic));
    cpu_reset(cs);
    cs->halted = false;
    cpu->init_svtor = initial_svtor;
    for (i = 0; i < G_N_ELEMENTS(levels); i++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), i), levels[i]);
    }
    s->psc_wake_count++;
    qemu_log_mask(LOG_UNIMP,
                  "q66 PSC core wake vector=%08x pc=%08x count=%u "
                  "cpu-thread=%d running=%d\n",
                  vector, cpu->env.regs[15], s->psc_wake_count,
                  qemu_cpu_is_self(cs), qatomic_read(&cs->running));
    qemu_cpu_kick(cs);
}

static void q66_psc_power_cycle(void *opaque)
{
    Q66MachineState *s = opaque;

    /* Queue every CPU state transition, including the no-IRQ halt path.
     * Pending work is harmless after a global reset clears psc_power_down. */
    async_run_on_cpu(CPU(s->arm.cpu), q66_psc_power_cycle_on_cpu,
                     RUN_ON_CPU_HOST_PTR(s));
}

#include "xiaomi-q66-gpu.h"

static uint32_t q66_intersys_status(Q66MachineState *s)
{
    uint32_t value = s->intersys_control;
    unsigned id;
    for (id = 0; id < 2; id++) {
        if (s->intersys_rx_pending & (1u << id)) {
            value |= 1u << (17 + id);
            if (value & (1u << (13 + id))) {
                value |= 1u << (21 + id);
            }
        }
        if (s->intersys_tx_pending & (1u << id)) {
            value |= 1u << (19 + id);
            if (value & (1u << (15 + id))) {
                value |= 1u << (23 + id);
            }
        }
    }
    return value;
}

static void q66_intersys_update_irq(Q66MachineState *s)
{
    uint32_t status = q66_intersys_status(s);
    unsigned id;
    for (id = 0; id < 2; id++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 32 + id),
                     !!(status & (1u << (23 + id))));
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 34 + id),
                     !!(status & (1u << (21 + id))));
    }
}

#include "xiaomi-q66-hci.h"

static void q66_hci_command(Q66MachineState *s, const uint8_t *data, unsigned len)
{
    uint16_t opcode;
    uint8_t *event;
    uint8_t parameters[65] = { 0 };
    unsigned slot, parameter_len = 1;
    if (len < 4 || data[0] != 1 || len != 4 + data[3]) {
        return;
    }
    opcode = lduw_le_p(data + 1);
    switch (opcode) {
    case 0x0c03: /* Reset */
        memset(s->hci_sleep_settings, 0, sizeof(s->hci_sleep_settings));
        break;
    case 0xfc77: /* BES sleep settings */
        if (data[3] != 8) {
            parameters[0] = 0x12;
        } else {
            memcpy(s->hci_sleep_settings, data + 4, 8);
        }
        break;
    case 0xfc32: /* BES controller address */
        if (data[3] != 6) {
            parameters[0] = 0x12;
        } else {
            memcpy(s->hci_address, data + 4, 6);
        }
        break;
    case 0xfc72: /* BES LE address: address type followed by six octets */
        if (data[3] != 7) {
            parameters[0] = 0x12;
        } else {
            memcpy(s->hci_le_address, data + 5, 6);
        }
        break;
    case 0x1001: /* Read Local Version: experimental controller, BT 4.2 */
        parameters[1] = parameters[4] = 8;
        stw_le_p(parameters + 2, 1);
        stw_le_p(parameters + 5, 0xffff);
        stw_le_p(parameters + 7, 1);
        parameter_len = 9;
        break;
    case 0x1002: /* Only advertise the small initialization command set. */
        parameters[6] = 0xc0;   /* Set Event Mask / Reset */
        parameters[15] = 0xa8;  /* version / features / buffer size */
        parameters[16] = 2;     /* BD_ADDR */
        parameters[26] = 7;     /* LE event mask / buffer size / features */
        parameter_len = 65;
        break;
    case 0x1003: /* BR/EDR unsupported, LE supported */
        parameters[5] = 0x60;
        parameter_len = 9;
        break;
    case 0x1009:
        memcpy(parameters + 1, s->hci_address, 6);
        parameter_len = 7;
        break;
    case 0x1005: /* Read Buffer Size */
        stw_le_p(parameters + 1, 1021);
        stw_le_p(parameters + 4, 4);
        parameter_len = 8;
        break;
    case 0x2002: /* LE Read Buffer Size */
        stw_le_p(parameters + 1, 251);
        parameters[3] = 4;
        parameter_len = 4;
        break;
    case 0x2003: /* LE features: no optional radio/link procedures */
        parameter_len = 9;
        break;
    case 0x0c01:
    case 0x2001:
        if (data[3] != 8) {
            parameters[0] = 0x12;
        } else {
            memcpy(opcode == 0x0c01 ? s->hci_event_mask : s->hci_le_event_mask,
                   data + 4, 8);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "q66 HCI command unmodeled opcode=%04x\n", opcode);
        return;
    }
    if (s->hci_event_count == G_N_ELEMENTS(s->hci_events)) {
        qemu_log_mask(LOG_GUEST_ERROR, "q66 HCI event queue full\n");
        return;
    }
    slot = (s->hci_event_head + s->hci_event_count++) %
           G_N_ELEMENTS(s->hci_events);
    event = s->hci_events[slot];
    event[0] = 4;
    event[1] = 0x0e;
    event[2] = 3 + parameter_len;
    event[3] = 1;
    stw_le_p(event + 4, opcode);
    memcpy(event + 6, parameters, parameter_len);
    s->hci_event_len[slot] = 6 + parameter_len;
    if (!s->intersys_rx_pending && !timer_pending(s->intersys_rx_timer)) {
        timer_mod(s->intersys_rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

static void q66_intersys_tx_done(void *opaque)
{
    Q66MachineState *s = opaque;
    uint8_t *bt = memory_region_get_ram_ptr(&s->bt_ram);
    uint8_t word[4], msg[16], data[4096];
    uint32_t pointer = ldl_le_p(bt + 0x4004), head = 0;
    unsigned records = 0;
    if (pointer && address_space_read(&address_space_memory, pointer,
            MEMTXATTRS_UNSPECIFIED, word, 4) == MEMTX_OK) {
        head = ldl_le_p(word);
    }
    while (head && records++ < 16 && address_space_read(&address_space_memory, head,
            MEMTXATTRS_UNSPECIFIED, msg, sizeof(msg)) == MEMTX_OK) {
        uint32_t len = ldl_le_p(msg + 8);
        uint32_t address = ldl_le_p(msg + 12);
        unsigned i, count = MIN(len, sizeof(data));
        if (address_space_read(&address_space_memory, address,
                MEMTXATTRS_UNSPECIFIED, data, count) == MEMTX_OK) {
            qemu_log_mask(LOG_UNIMP, "q66 HCI TX head=%08x type=%u len=%u data=",
                          head, msg[4], len);
            for (i = 0; i < MIN(count, 32); i++) {
                qemu_log_mask(LOG_UNIMP, "%02x", data[i]);
            }
            qemu_log_mask(LOG_UNIMP, "\n");
            if (msg[4] == 0 && count == len) {
                if (s->hci_chardev) {
                    int written = qemu_chr_fe_write_all(&s->hci_chr, data, len);
                    if (written != len) {
                        error_report("q66 HCI bridge write failed (%d/%u)",
                                     written, len);
                        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_ERROR);
                    }
                } else {
                    q66_hci_command(s, data, len);
                }
            }
        }
        head = ldl_le_p(msg);
    }
    s->intersys_control &= ~(1u << 2);
    s->intersys_tx_pending |= 1;
    q66_intersys_update_irq(s);
}

static void q66_intersys_rx_event(void *opaque)
{
    Q66MachineState *s = opaque;
    uint8_t *bt = memory_region_get_ram_ptr(&s->bt_ram);
    /* Controller-owned intersys descriptor and H4 packet in BT RAM.  The AP
     * consumes the descriptor through the shared head at BT_RAM+0x4000. */
    uint8_t *msg = bt + 0xdf00, *data = bt + 0xe000;
    unsigned len;
    if (!s->hci_event_count || s->intersys_rx_pending) {
        return;
    }
    len = s->hci_event_len[s->hci_event_head];
    memset(msg, 0, 16);
    stl_le_p(msg + 8, len);
    stl_le_p(msg + 12, 0xc000e000);
    memcpy(data, s->hci_events[s->hci_event_head], len);
    s->hci_event_head = (s->hci_event_head + 1) % G_N_ELEMENTS(s->hci_events);
    s->hci_event_count--;
    stl_le_p(bt + 0x4000, 0xc000df00);
    s->intersys_rx_pending |= 1;
    q66_intersys_update_irq(s);
    qemu_log_mask(LOG_UNIMP, "q66 HCI RX type=%u len=%u\n", data[0], len);
    if (s->hci_chardev) {
        /* A full queue may have left complete packets in the stream. */
        q66_hci_read(s, NULL, 0);
        qemu_chr_fe_accept_input(&s->hci_chr);
    }
}

static int q66_gpio_bank(uint32_t address)
{
    unsigned i;
    for (i = 0; i < G_N_ELEMENTS(q66_gpio_bases); i++) {
        if (address >= q66_gpio_bases[i] &&
            address < q66_gpio_bases[i] + 0x64) {
            return i;
        }
    }
    return -1;
}

static void q66_gpio_update_irq(Q66MachineState *s)
{
    uint32_t pending = 0;
    unsigned i;
    for (i = 0; i < G_N_ELEMENTS(q66_gpio_bases); i++) {
        uint32_t base = q66_gpio_bases[i];
        pending |= s->gpio_pending[i] & q66_reg(s, base + 0x30) &
                   ~q66_reg(s, base + 0x34) &
                   q66_reg(s, 0x40085080 + 4 * i);
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 44), !!pending);
}

#include "xiaomi-q66-touch.h"
#include "xiaomi-q66-charger.h"
#include "xiaomi-q66-nfc.h"
#include "xiaomi-q66-inputs.h"

static void q66_panel_te(void *opaque)
{
    Q66MachineState *s = opaque;
    uint32_t base = q66_gpio_bases[2];
    if (!s->panel_te_enabled || !s->panel || !s->panel->power) {
        return;
    }
    if ((q66_reg(s, base + 0x30) & ~q66_reg(s, base + 0x34)) & 2) {
        s->gpio_pending[2] |= 2;
        q66_gpio_update_irq(s);
        if (s->panel_te_count++ < 16) {
            qemu_log_mask(LOG_UNIMP, "q66 panel TE gpio=65 irq=44\n");
        }
    }
    timer_mod(s->panel_te_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667);
}

static void q66_gpio_write(Q66MachineState *s, unsigned bank,
                           uint32_t offset, uint32_t value)
{
    uint32_t base = q66_gpio_bases[bank];
    uint32_t target = offset;
    bool clear = false;
    if (s->inputs.bus_logging && bank == 0 && (value & (1u << 14))) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "q66 sensor-gpio t=%" PRId64 " bank=%u off=%02x value=%08x ppg_irq=%d pc=%08x\n",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), bank, offset, value,
            s->inputs.ppg_irq, s->arm.cpu->env.regs[15]);
    }
    switch (offset) {
    case 0x0c: case 0x10: case 0x14:
        target = offset - 0x0c;
        clear = true;
        break;
    case 0x18: case 0x1c: case 0x20: case 0x24:
        target = offset + 0x18;
        clear = true;
        break;
    case 0x28:
        target = 0x48;
        clear = true;
        break;
    case 0x4c:
        s->gpio_pending[bank] &= ~value;
        q66_gpio_update_irq(s);
        return;
    case 0x40: case 0x44: case 0x50:
        return;
    default:
        break;
    }
    if (target != 0x60) {
        uint32_t old = q66_reg(s, base + target);
        value = clear ? old & ~value : old | value;
    }
    g_hash_table_insert(s->registers, GUINT_TO_POINTER(base + target),
                        GUINT_TO_POINTER(value));
    if (bank == 1 && target == 0 &&
        ((s->inputs.spi_device == 1 && (value & (1u << 21))) ||
         (s->inputs.spi_device == 2 && (value & (1u << 18))))) {
        s->inputs.spi_device = s->inputs.spi_byte = 0;
    }
    if (bank == 0 && (target == 0 || target == 4)) {
        /* Stock touch_nreset selects the "reset" pinutils entry: P3_4.
         * Only an output driven low asserts the chip's active-low reset. */
        bool reset_low = (q66_reg(s, base + 4) & (1u << 28)) &&
                         !(q66_reg(s, base) & (1u << 28));
        q66_touch_reset_pin(s, reset_low);
    }
    q66_gpio_update_irq(s);
}

static void q66_lcdc_update_irq(Q66MachineState *s)
{
    uint32_t enable = GPOINTER_TO_UINT(g_hash_table_lookup(
        s->registers, GUINT_TO_POINTER(0x402001c0)));
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 67),
                 !!(s->lcdc_irq_status & enable));
}

static void q66_lcdc_frame_done(void *opaque)
{
    Q66MachineState *s = opaque;
    /* Smart-panel transfer and its graphics layer both complete.  The AP
     * enables TXC bit 21, then checks GRA_FRAME_IRQ0 bit 27 before releasing
     * its queued framebuffer and starting the next animation frame. */
    s->lcdc_irq_status |= (1u << 21) | (1u << 27);
    q66_lcdc_update_irq(s);
    qemu_log_mask(LOG_UNIMP, "q66 LCDC frame done irq=%08x\n",
                  s->lcdc_irq_status);
}

static void q66_lcdc_commit(Q66MachineState *s)
{
    const size_t frame_size = WIDTH * HEIGHT * 2;
    uint8_t *dst;
    const uint8_t *src = NULL;
    uint32_t addr = s->lcdc_framebuffer;

    if (!s->panel || !addr) {
        return;
    }
    if (addr >= 0x20000000u &&
        addr - 0x20000000u <= memory_region_size(&s->sram) - frame_size) {
        src = memory_region_get_ram_ptr(&s->sram) + (addr - 0x20000000u);
    } else if (addr >= 0x3c000000u &&
               addr - 0x3c000000u <= memory_region_size(&s->psram) - frame_size) {
        src = memory_region_get_ram_ptr(&s->psram) + (addr - 0x3c000000u);
    } else if (addr >= 0x1c000000u &&
               addr - 0x1c000000u <= memory_region_size(&s->psram) - frame_size) {
        src = memory_region_get_ram_ptr(&s->psram) + (addr - 0x1c000000u);
    }
    if (!src) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 LCDC frame address %08x is outside modeled RAM\n",
                      addr);
        return;
    }
    dst = memory_region_get_ram_ptr(&s->panel->vram);
    memcpy(dst, src, frame_size);
    s->panel->commits++;
    timer_mod(s->lcdc_frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16 * SCALE_MS);
    qemu_log_mask(LOG_UNIMP,
                  "q66 LCDC frame commit=%u fb=%08x\n",
                  s->panel->commits, addr);
}

static bool panel_update(void *opaque)
{
    Q66PanelState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->console);
    uint16_t *input = memory_region_get_ram_ptr(&s->vram);
    uint8_t *output = surface_data(surface);
    int stride = surface_stride(surface);
    int x, y;

    for (y = 0; y < HEIGHT; y++) {
        uint32_t *row = (uint32_t *)(output + y * stride);
        for (x = 0; x < WIDTH; x++) {
            uint16_t c = s->power && s->brightness ?
                         le16_to_cpu(input[y * WIDTH + x]) : 0;
            uint8_t r = ((c >> 11) & 31) * s->brightness / 31;
            uint8_t g = ((c >> 5) & 63) * s->brightness / 63;
            uint8_t b = (c & 31) * s->brightness / 31;
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
    qemu_console_update_full(s->console);
    return true;
}

static const GraphicHwOps panel_graphics = {
    .gfx_update = panel_update,
};

static uint64_t panel_read(void *opaque, hwaddr address, unsigned size)
{
    Q66PanelState *s = opaque;
    switch (address) {
    case 0: return s->power;
    case 4: return WIDTH;
    case 8: return HEIGHT;
    case 12: return s->commits;
    case 16: return s->brightness;
    case 20: return s->machine ? s->machine->touch.received : 0;
    case 24: return s->machine ? s->machine->touch.acknowledged : 0;
    case 28: return s->machine ? s->machine->touch.dropped : 0;
    case 32: return s->machine ? s->machine->touch.report_count : 0;
    default: return 0;
    }
}

static void panel_write(void *opaque, hwaddr address, uint64_t value,
                        unsigned size)
{
    Q66PanelState *s = opaque;
    if (address == 0) {
        s->power = value & 1;
    } else if (address == 12) {
        s->commits++;
    }
}

static const MemoryRegionOps panel_ops = {
    .read = panel_read,
    .write = panel_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void panel_realize(DeviceState *dev, Error **errp)
{
    Q66PanelState *s = Q66_PANEL(dev);
    s->brightness = 255;
    memory_region_init_io(&s->control, OBJECT(dev), &panel_ops, s,
                          "q66.diagnostic-control", 0x1000);
    memory_region_init_ram(&s->vram, OBJECT(dev), "q66.diagnostic-vram",
                           WIDTH * HEIGHT * 2, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->control);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vram);
    s->console = qemu_graphic_console_create(dev, 0, &panel_graphics, s);
    qemu_console_resize(s->console, WIDTH, HEIGHT);
    s->input = qemu_input_handler_register(dev, &q66_touch_input);
    qemu_input_handler_activate(s->input);
}

static void panel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = panel_realize;
    dc->desc = "q66 212x520 RGB565 display transport diagnostic";
    /* No migration until the real controller and its state are implemented. */
    dc->user_creatable = false;
}

static const TypeInfo panel_type = {
    .name = TYPE_Q66_PANEL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Q66PanelState),
    .class_init = panel_class_init,
};

static void ispi_reset(void *opaque)
{
    Q66Ispi *spi = opaque;

    memset(spi->regs, 0, sizeof(spi->regs));
    if (!spi->retain_analog_on_reset) {
        memset(spi->analog, 0, sizeof(spi->analog));
    }
    memset(spi->page, 0, sizeof(spi->page));
    spi->tx_head = spi->tx_count = spi->rx_head = spi->rx_count = 0;
    spi->overrun = false;
    spi->external_traces = 0;
    /* Documented best1503 chip-id field, revision 0; cold power-key boot. */
    spi->analog[3][0][0] = 0x1530;
    spi->analog[3][0][0x5f] = 2;
    /* PMU 0x304F is the power-off control register.  Bit 0 is
     * SOFT_POWER_OFF (the SDK names it explicitly); asserting it at reset
     * makes the stock AP immediately enter pmu_shutdown().  A cold boot has
     * no pending software power-off request, so leave this register clear. */
    if (!spi->retain_analog_on_reset) {
        spi->analog[3][0][0x4f] = 0;
    }
    spi->retain_analog_on_reset = false;
}

static uint32_t ispi_transaction(Q66Ispi *spi, uint32_t frame)
{
    unsigned bits = ((spi->regs[0] >> 16) & 31) + 1;
    unsigned padding = bits > 25 ? bits - 25 : 0;
    unsigned cs = (spi->regs[1] >> 7) & 15;
    uint32_t command = frame >> padding;
    unsigned reg = (command >> 16) & 255;
    unsigned page = spi->page[cs];
    uint16_t value = command;
    bool read = command & (1 << 24);

    if (spi->regs[1] & 1) {
        return frame; /* SPI loopback mode. */
    }
    if (spi->external) {
        /* An unattached SPI target leaves MISO high.  Clock a real FIFO
         * response for every transmitted frame so the stock driver can
         * complete its probe and report the missing peripheral. */
        uint32_t reply = q66_inputs_spi(spi->machine, frame, bits);
        if (spi->external_traces++ < 256) {
            qemu_log_mask(LOG_UNIMP,
                          "q66 SPI cs=%u tx=%08x rx=%08x bits=%u "
                          "pc=%08x lr=%08x sp=%08x gpio=%08x/%08x/%08x\n", cs, frame, reply,
                          bits, spi->machine->arm.cpu->env.regs[15],
                          spi->machine->arm.cpu->env.regs[14],
                          spi->machine->arm.cpu->env.regs[13],
                          q66_reg(spi->machine, q66_gpio_bases[0]),
                          q66_reg(spi->machine, q66_gpio_bases[1]),
                          q66_reg(spi->machine, q66_gpio_bases[2]));
        }
        return reply;
    }
    if (read) {
        value = spi->analog[cs][page][reg];
        if (!spi->external && cs == 3 && page == 0 &&
            reg >= 0x56 && reg <= 0x5c) {
            unsigned channel = reg - 0x56;
            spi->machine->sensors.reads[channel]++;
            spi->machine->sensors.last_read[channel] = value;
        }
        if (cs == 3 && page == 0 && reg == 0x51) {
            value = spi->analog[3][0][0x50] & spi->analog[3][0][0x1f] &
                    spi->analog[3][0][0x20];
        }
        if (cs == 3 && page == 0 && reg == 0x5e) {
            unsigned pending = spi->machine->charger.pending;
            value = (spi->machine->charger.connected ? 4 : 0) | pending;
            if (spi->analog[3][0][5] & 0x1800) {
                value |= pending << 3;
            }
        }
    } else if (reg == 0 && (value & 0xff0f) == 0xa000 &&
               ((value >> 4) & 15) < 5) {
        spi->page[cs] = (value >> 4) & 15;
    } else if (!(cs == 3 && page == 0 && reg == 0)) {
        /* Keep the chip-id read-only; PMU reset unlock writes do not turn
         * its identification register into ordinary writable storage. */
        spi->analog[cs][page][reg] = value;
    }
    if (cs == 3 && page == 0 && !read && !spi->external) {
        if (reg == 0x5e) {
            spi->machine->charger.pending &= ~((value | (value >> 3)) & 3);
            q66_gpadc_update_irq(spi->machine);
        } else if (reg == 5 || reg == 0x40 || reg == 0x41) {
            q66_gpadc_update_irq(spi->machine);
        } else if (reg == 0x51) {
            spi->analog[3][0][0x50] &= ~value;
            q66_gpadc_update_irq(spi->machine);
        } else if (reg == 0x1f || reg == 0x20) {
            q66_gpadc_update_irq(spi->machine);
        } else if (reg == 0x4f) {
            if (value & 0x20) {
                timer_mod(spi->machine->gpadc_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            } else {
                timer_del(spi->machine->gpadc_timer);
            }
            if (value & 1) {
                qemu_log_mask(LOG_UNIMP, "q66 PMU software power-off\n");
                if (spi->machine->panel) {
                    spi->machine->panel->power = 0;
                }
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            }
        }
    }
    if (cs == 3 && page == 0 && reg == 0x48 && !read) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 PMU reg48 write val=%04x pc=%08x lr=%08x sp=%08x\n",
                      value, spi->machine->arm.cpu->env.regs[15],
                      spi->machine->arm.cpu->env.regs[14],
                      spi->machine->arm.cpu->env.regs[13]);
    }
    if (spi->machine->trace_count++ < 20000) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 ISPI %c cs=%u page=%u reg=%02x val=%04x "
                      "frame=%08x bits=%u pc=%08x (register model)\n",
                      read ? 'R' : 'W', cs, page, reg, value, frame, bits,
                      spi->machine->arm.cpu->env.regs[15]);
    }
    return read ? (uint32_t)value << padding : 0;
}

static void ispi_clock_fifo(Q66Ispi *spi)
{
    while ((spi->regs[1] & 2) && spi->tx_count) {
        uint32_t frame = spi->tx[spi->tx_head];
        unsigned bits = ((spi->regs[0] >> 16) & 31) + 1;
        uint32_t reply;

        spi->tx_head = (spi->tx_head + 1) & 7;
        spi->tx_count--;
        if (bits < 32) {
            frame &= (1u << bits) - 1;
        }
        reply = ispi_transaction(spi, frame);
        /* Synchronous transfers finish immediately. RX overflow discards
         * the incoming word but does not stop a transmit-only transaction. */
        if (spi->rx_count < 8) {
            spi->rx[(spi->rx_head + spi->rx_count) & 7] = reply;
            spi->rx_count++;
        } else {
            spi->overrun = true;
        }
    }
}

static uint64_t ispi_read(void *opaque, hwaddr offset, unsigned size)
{
    Q66Ispi *spi = opaque;
    uint32_t status = (!spi->tx_count ? 1 : 0) |
                      (spi->tx_count < 8 ? 2 : 0) |
                      (spi->rx_count ? 4 : 0) |
                      (spi->rx_count == 8 ? 8 : 0) |
                      ((spi->regs[1] & 2) && spi->tx_count ? 16 : 0);
    uint32_t irq = (spi->overrun ? 1 : 0) |
                   (spi->rx_count >= 4 ? 4 : 0) |
                   (spi->tx_count <= 4 ? 8 : 0);
    uint32_t value;

    switch (offset) {
    case 8:
        if (!spi->rx_count) {
            return 0;
        }
        value = spi->rx[spi->rx_head];
        spi->rx_head = (spi->rx_head + 1) & 7;
        spi->rx_count--;
        return value;
    case 12: return status;
    case 24: return irq;
    case 28: return irq & spi->regs[5];
    default:
        return offset / 4 < G_N_ELEMENTS(spi->regs) ?
               spi->regs[offset / 4] : 0;
    }
}

static void ispi_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    Q66Ispi *spi = opaque;

    if (offset == 8) {
        if (spi->tx_count < 8) {
            spi->tx[(spi->tx_head + spi->tx_count) & 7] = value;
            spi->tx_count++;
        }
        ispi_clock_fifo(spi);
    } else if (offset == 32) {
        if (value & 1) {
            spi->overrun = false;
        }
    } else if (offset / 4 < G_N_ELEMENTS(spi->regs)) {
        spi->regs[offset / 4] = value;
        if (offset == 4) {
            ispi_clock_fifo(spi);
        }
    }
}

static const MemoryRegionOps ispi_ops = {
    .read = ispi_read,
    .write = ispi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void nor_reset(void *opaque)
{
    Q66Nor *nor = opaque;
    Q66MachineState *s = nor->machine;

    /* XIP is enabled by controller setup, and must disappear on AP reset.
     * Otherwise the next boot's flash probing writes into the NAND window. */
    if (nor == &s->nor1 && s->resource_flash1_mapped) {
        memory_region_del_subregion(get_system_memory(), &s->aliases[7]);
        memory_region_del_subregion(get_system_memory(), &s->aliases[8]);
        s->resource_flash1_mapped = false;
    }

    memset(nor->regs, 0, sizeof(nor->regs));
    memset(nor->status, 0, sizeof(nor->status));
    nor->tx_count = nor->tx_slots = nor->rx_head = nor->rx_count = 0;
    nor->nor_program_control = nor->nor_program_length = 0;
    nor->write_enable = false;
    memset(nor->nand_features, 0, sizeof(nor->nand_features));
    nor->nand_features[0xb0] = 0x10; /* Power-on ECC enable. */
    memset(nor->nand_cache, 0xff, sizeof(nor->nand_cache));
    nor->nand_page = 0;
    nor->nand_pending_command = 0;
    nor->nand_failure = 0;
}

static bool q66_nand_sync(Q66Nor *nor, unsigned page, unsigned pages)
{
    uint8_t *main = memory_region_get_ram_ptr(&nor->machine->resource);
    unsigned first = page < NAND_DATA_FIRST_PAGE ? 0 : NAND_DATA_FIRST_PAGE;
    BlockBackend *blk = first ? nor->nand_data : nor->nand_resource;
    int ret;
    if (!blk) {
        return true;
    }
    ret = blk_pwrite(blk,
                     (page - first) * NAND_PAGE_SIZE,
                     pages * NAND_PAGE_SIZE, main + page * NAND_PAGE_SIZE, 0);
    if (ret >= 0) {
        ret = blk_pwrite(blk, NAND_DATA_SIZE +
                         (page - first) * NAND_SPARE_SIZE,
                         pages * NAND_SPARE_SIZE,
                         nor->nand_spare + page * NAND_SPARE_SIZE, 0);
    }
    if (ret < 0) {
        error_report("q66 NAND data write failed: %s", strerror(-ret));
        return false;
    }
    return true;
}

static bool q66_nand_erase_block(Q66Nor *nor, unsigned page)
{
    uint8_t *main = memory_region_get_ram_ptr(&nor->machine->resource);

    assert(page < NAND_PAGES && !(page & 63));
    memset(main + page * NAND_PAGE_SIZE, 0xff, 64 * NAND_PAGE_SIZE);
    memset(nor->nand_spare + page * NAND_SPARE_SIZE,
           0xff, 64 * NAND_SPARE_SIZE);
    return q66_nand_sync(nor, page, 64);
}

static void q66_nor_sync(Q66MachineState *s, unsigned address, unsigned bytes)
{
    uint8_t *flash = memory_region_get_ram_ptr(&s->flash);
    unsigned offset = address & (FLASH_SIZE - 1);
    unsigned length = MIN(bytes, FLASH_SIZE - offset);
    int ret = 0;

    /* Notify TCG of programmed executable RAM, including its XIP aliases. */
    physical_memory_write(0x2c000000 + offset, flash + offset, length);
    if (s->flash_backend) {
        ret = blk_pwrite(s->flash_backend, offset, length, flash + offset, 0);
    }
    if (ret < 0) {
        error_report("q66 NOR write failed: %s", strerror(-ret));
        qemu_system_vmstop_request(RUN_STATE_IO_ERROR);
    }
    if (bytes > length) {
        q66_nor_sync(s, 0, bytes - length);
    }
}

static void nand_command(Q66Nor *nor, uint8_t cmd, bool ext,
                         unsigned address, unsigned length)
{
    /* The AP's nandflash device table supports C8 82 C8, with 2 KiB pages,
     * 128-byte spare areas and 256 MiB capacity.  Its resource partition
     * occupies the first 128 MiB; the remainder is initially erased /data.
     * The supplied OTA contains logical main-area bytes, without OOB. */
    static const uint8_t id[] = { 0xc8, 0x82, 0xc8 };
    uint8_t *main = memory_region_get_ram_ptr(&nor->machine->resource);
    unsigned column, i;

    length = MIN(length, sizeof(nor->rx));
    nor->nand_pending_command = 0;
    nor->rx_head = nor->rx_count = 0;
    switch (cmd) {
    case 0x9f:
        for (i = 0; i < length; i++) {
            nor->rx[i] = id[i % sizeof(id)];
        }
        nor->rx_count = length;
        break;
    case 0xff:
        nor->write_enable = false;
        nor->nand_failure = 0;
        memset(nor->nand_features, 0, sizeof(nor->nand_features));
        nor->nand_features[0xb0] = 0x10;
        break;
    case 0x06:
        nor->write_enable = true;
        break;
    case 0x04:
        nor->write_enable = false;
        break;
    case 0x0f:
        address = nor->tx_count ? nor->tx[0] : address & 0xff;
        memset(nor->rx, address == 0xc0 ?
               ((nor->write_enable ? 2 : 0) | nor->nand_failure) :
               nor->nand_features[address],
               length);
        nor->rx_count = length;
        break;
    case 0x1f:
        if (nor->tx_count >= 2) {
            nor->nand_features[nor->tx[0]] = nor->tx[1];
        }
        break;
    case 0x13: /* Page read to cache. */
        nor->nand_page = address % NAND_PAGES;
        memcpy(nor->nand_cache, main + nor->nand_page * NAND_PAGE_SIZE,
               NAND_PAGE_SIZE);
        memcpy(nor->nand_cache + NAND_PAGE_SIZE,
               nor->nand_spare + nor->nand_page * NAND_SPARE_SIZE,
               NAND_SPARE_SIZE);
        break;
    case 0x03: case 0x0b: case 0x3b: case 0x6b: case 0xbb: case 0xeb:
        column = ext && nor->tx_count >= 2 ?
                 (nor->tx[0] << 8) | nor->tx[1] : address;
        column &= 0xfff; /* Exclude the chip's plane-select address bit. */
        for (i = 0; i < length; i++) {
            nor->rx[i] = column + i < sizeof(nor->nand_cache) ?
                         nor->nand_cache[column + i] : 0xff;
        }
        nor->rx_count = length;
        break;
    case 0x02: case 0x32: case 0x84: case 0x34:
        if (!ext || nor->tx_count >= 2) {
            unsigned first = ext ? 2 : 0;
            column = ext ? (nor->tx[0] << 8) | nor->tx[1] : address;
            column &= 0xfff;
            if (cmd == 0x02 || cmd == 0x32) {
                memset(nor->nand_cache, 0xff, sizeof(nor->nand_cache));
            }
            for (i = first; i < nor->tx_count &&
                            column + i - first < sizeof(nor->nand_cache); i++) {
                nor->nand_cache[column + i - first] = nor->tx[i];
            }
        }
        break;
    case 0x10:
        if (nor->write_enable) {
            nor->nand_page = address % NAND_PAGES;
            for (i = 0; i < NAND_PAGE_SIZE; i++) {
                main[nor->nand_page * NAND_PAGE_SIZE + i] &= nor->nand_cache[i];
            }
            for (i = 0; i < NAND_SPARE_SIZE; i++) {
                nor->nand_spare[nor->nand_page * NAND_SPARE_SIZE + i] &=
                    nor->nand_cache[NAND_PAGE_SIZE + i];
            }
            nor->write_enable = false;
            nor->nand_failure = q66_nand_sync(nor, nor->nand_page, 1) ? 0 : 8;
        }
        break;
    case 0xd8:
        if (nor->write_enable) {
            unsigned page = (address % NAND_PAGES) & ~63u;
            nor->write_enable = false;
            nor->nand_failure = q66_nand_erase_block(nor, page) ? 0 : 4;
        }
        break;
    case 0xb9: case 0xab:
        break;
    default:
        memset(nor->rx, 0xff, length);
        nor->rx_count = length;
        break;
    }
    if (nor->nand_traces[cmd]++ < 8) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 NAND cmd=%02x ext=%u addr=%06x tx=%u rx=%u "
                      "page=%06x tx0=%02x tx1=%02x pc=%08x\n",
                      cmd, ext, address, nor->tx_count, nor->rx_count,
                      nor->nand_page, nor->tx[0], nor->tx[1],
                      nor->machine->arm.cpu->env.regs[15]);
    }
    nor->tx_count = nor->tx_slots = 0;
    q66_dma_schedule(nor->machine);
}

static uint64_t nand_cache_read(void *opaque, hwaddr offset, unsigned size)
{
    Q66Nor *nor = opaque;
    uint64_t value = 0;
    unsigned column = offset & 0xfff;
    unsigned i;

    /* NAND XIP is a view of the selected page's cache, not the entire die.
     * The stock driver issues PAGE_READ and reads its main/OOB columns here. */
    for (i = 0; i < size; i++) {
        uint8_t byte = column + i < sizeof(nor->nand_cache) ?
                       nor->nand_cache[column + i] : 0xff;
        value |= (uint64_t)byte << (i * 8);
    }
    return value;
}

static void nand_cache_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Q66Nor *nor = opaque;
    /* This is a read window. NAND programming uses PROGRAM_LOAD/EXECUTE;
     * speculative or boot-time stores here must not change NAND contents. */
    if (nor->nand_xip_write_traces++ < 8) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 NAND XIP ignored W%u offset=%08x value=%08x pc=%08x\n",
                      size, (unsigned)offset, (uint32_t)value,
                      nor->machine->arm.cpu->env.regs[15]);
    }
}

static const MemoryRegionOps nand_cache_ops = {
    .read = nand_cache_read,
    .write = nand_cache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void nor_drain_fifo(Q66Nor *nor)
{
    /* A status read is separated from the AP's next TX write by the same
     * polling loop used in hal_norflaship_v2.c.  Let a byte leave the real
     * 16-byte FIFO on each poll while preserving the staged transaction. */
    if (nor->tx_slots) {
        nor->tx_slots--;
    }
}

static void nor_command(Q66Nor *nor, uint32_t value)
{
    /* BES flash-controller v7 command/FIFO layout. C8 60 18 occurs in the
     * supplied AP's device table. This is a modeled part, not a physical ID
     * recovered from this band's flash. */
    static const uint8_t jedec[] = { 0xc8, 0x60, 0x18 };
    uint8_t cmd = value;
    bool ext = nor->regs[1] & 1;
    unsigned length = (nor->regs[1] >> 12) & 0x1fff;
    unsigned address = value >> 8;
    uint8_t *flash = memory_region_get_ram_ptr(&nor->machine->flash);
    unsigned i;

    if (ext) {
        length = value & 0x100 ? (value >> 13) & 0x1fff : 0;
        address = nor->tx_count >= 3 ?
                  (nor->tx[0] << 16) | (nor->tx[1] << 8) | nor->tx[2] : 0;
    }
    if (nor == &nor->machine->nor1 && nor->nand_spare) {
        if (!ext && (cmd == 0x02 || cmd == 0x32 ||
                     cmd == 0x84 || cmd == 0x34) && length > nor->tx_count) {
            /* The command starts clocking before DMA has filled its FIFO.
             * Keep it active until all requested payload bytes arrive. */
            nor->nand_pending_command = cmd;
            nor->nand_pending_address = address;
            nor->nand_pending_length = length;
            q66_dma_schedule(nor->machine);
            return;
        }
        nand_command(nor, cmd, ext, address, length);
        return;
    }
    if (!ext && nor->write_enable && (cmd == 0x02 || cmd == 0x32) &&
        length > nor->tx_count) {
        /* The AP preloads sixteen bytes, starts PAGE PROGRAM, then fills
         * the rest through the same FIFO. Commit only the complete frame. */
        nor->nor_program_control = value;
        nor->nor_program_length = length;
        q66_dma_schedule(nor->machine);
        return;
    }
    nor->nor_program_length = 0;
    nor->rx_head = nor->rx_count = 0;
    if (cmd == 0x05 || cmd == 0x35 || cmd == 0x15) {
        length = MAX(length, 1);
    }
    switch (cmd) {
    case 0x9f:
        for (i = 0; i < length; i++) {
            if (nor->regs[0x204 / 4] & (1 << 19)) {
                uint8_t id = jedec[(i / 2) % sizeof(jedec)];
                /* Two quad devices supply one nibble each per RX byte. */
                nor->rx[i] = i & 1 ? (id & 15) * 0x11 : (id >> 4) * 0x11;
            } else {
                nor->rx[i] = jedec[i % sizeof(jedec)];
            }
        }
        nor->rx_count = length;
        break;
    case 0x05: case 0x35: case 0x15:
        for (i = 0; i < length; i++) {
            uint8_t status = nor->status[cmd == 0x05 ? 0 : cmd == 0x35 ? 1 : 2] |
                             (cmd == 0x05 && nor->write_enable ? 2 : 0);
            nor->rx[i] = nor->regs[0x204 / 4] & (1 << 19) ?
                         (i & 1 ? (status & 15) * 0x11 :
                          (status >> 4) * 0x11) : status;
        }
        nor->rx_count = length;
        break;
    case 0x06: case 0x50:
        nor->write_enable = true;
        break;
    case 0x04:
        nor->write_enable = false;
        break;
    case 0x01: case 0x31: case 0x11:
        if (nor->write_enable) {
            unsigned first = cmd == 0x01 ? 0 : cmd == 0x31 ? 1 : 2;
            for (i = 0; i < nor->tx_count && first + i < 3; i++) {
                nor->status[first + i] = nor->tx[i];
            }
            nor->status[0] &= ~3u;
            nor->write_enable = false;
        }
        break;
    case 0x03: case 0x0b: case 0x3b: case 0x6b: case 0xbb: case 0xeb:
        for (i = 0; i < length; i++) {
            nor->rx[i] = flash[(address + i) & (FLASH_SIZE - 1)];
        }
        nor->rx_count = length;
        break;
    case 0x02: case 0x32:
        if (nor->write_enable) {
            unsigned first = ext ? 3 : 0;
            for (i = first; i < nor->tx_count; i++) {
                flash[(address + i - first) & (FLASH_SIZE - 1)] &= nor->tx[i];
            }
            q66_nor_sync(nor->machine, address, nor->tx_count - first);
            nor->write_enable = false;
        }
        break;
    case 0x20: case 0x52: case 0xd8:
        if (nor->write_enable) {
            unsigned block = cmd == 0x20 ? 4096 : cmd == 0x52 ? 32768 : 65536;
            /* Parallel quad chips double the logical erase span. REG_000
             * already carries the logical XIP address (as do 512-byte
             * dual-mode page programs), not a per-chip wire address. */
            if (nor->regs[0x204 / 4] & (1u << 19)) {
                block *= 2;
            }
            memset(flash + (address & (FLASH_SIZE - 1) & ~(block - 1)),
                   0xff, block);
            q66_nor_sync(nor->machine,
                         address & (FLASH_SIZE - 1) & ~(block - 1), block);
            nor->write_enable = false;
        }
        break;
    case 0x66: case 0x99: case 0xab: case 0xb9: case 0xff: case 0xe9:
        break;
    default:
        /* No SFDP/security/factory data is present in the supplied AP. */
        memset(nor->rx, 0xff, length);
        nor->rx_count = length;
        qemu_log_mask(LOG_UNIMP, "q66 NOR unsupported cmd=%02x\n", cmd);
        break;
    }
    if (nor->machine->trace_count++ < 20000) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 NOR cmd=%02x ext=%u addr=%06x tx=%u rx=%u "
                      "pc=%08x\n", cmd, ext, address, nor->tx_count,
                      nor->rx_count, nor->machine->arm.cpu->env.regs[15]);
    }
    if (nor->nand_traces[cmd]++ < 32) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 NOR transaction cmd=%02x ext=%u addr=%06x "
                      "len=%u tx=%u rx=%u dual=%u data=%02x%02x%02x%02x "
                      "pc=%08x\n", cmd, ext, address, length,
                      nor->tx_count, nor->rx_count,
                      !!(nor->regs[0x204 / 4] & (1u << 19)),
                      nor->tx[0], nor->tx[1], nor->tx[2], nor->tx[3],
                      nor->machine->arm.cpu->env.regs[15]);
    }
    /* WREN has no payload: software can stage the following program's
     * bytes before sending it. Do not discard those bytes with the latch. */
    if (cmd != 0x06 && cmd != 0x50) {
        nor->tx_count = nor->tx_slots = 0;
    }
}

static uint64_t nor_read(void *opaque, hwaddr offset, unsigned size)
{
    Q66Nor *nor = opaque;

    if (offset == 0x0c) {
        nor_drain_fifo(nor);
        return ((16 - nor->tx_slots) << 9) |
               (MIN(nor->rx_count, 16) << 4) |
               (!nor->rx_count ? 8 : 0) |
               (nor->tx_slots == 16 ? 4 : 0) |
               (!nor->tx_slots ? 2 : 0) |
               (nor->nand_pending_command || nor->nor_program_length ? 1 : 0);
    }
    if (offset == 0x10) {
        /* CPU RDR reads expose one serial byte even on a 32-bit bus access. */
        if (!nor->rx_count) {
            return 0;
        }
        nor->rx_count--;
        return nor->rx[nor->rx_head++];
    }
    if (offset == 0x08) {
        uint64_t value = 0;
        unsigned i;
        if (!nor->rx_count) {
            return 0;
        }
        for (i = 0; i < size && nor->rx_count; i++) {
            value |= (uint64_t)nor->rx[nor->rx_head++] << (i * 8);
            nor->rx_count--;
        }
        return value;
    }
    return offset / 4 < G_N_ELEMENTS(nor->regs) ?
           nor->regs[offset / 4] >> ((offset & 3) * 8) : 0;
}

static void nor_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    Q66Nor *nor = opaque;
    unsigned i;

    if (nor == &nor->machine->nor1 && nor->machine->trace_count++ < 20000) {
        qemu_log_mask(LOG_UNIMP,
                      "q66 NOR1 W%u @%03x = %08x pc=%08x\n",
                      size, (unsigned)offset, (uint32_t)value,
                      nor->machine->arm.cpu ?
                      nor->machine->arm.cpu->env.regs[15] : 0);
    }

    if (nor == &nor->machine->nor1 && !nor->machine->resource_flash1_mapped) {
        memory_region_add_subregion(get_system_memory(), 0x34000000,
                                    &nor->machine->aliases[7]);
        memory_region_add_subregion(get_system_memory(), 0x30000000,
                                    &nor->machine->aliases[8]);
        nor->machine->resource_flash1_mapped = true;
        qemu_log_mask(LOG_UNIMP, "q66 resource FLASH1 XIP aliases enabled\n");
    }

    if (offset == 8) {
        if (nor->tx_slots < 16) {
            for (i = 0; i < size; i++) {
                if (nor->tx_count < sizeof(nor->tx)) {
                    nor->tx[nor->tx_count++] = value >> (i * 8);
                }
            }
            nor->tx_slots++;
            if (nor->nand_pending_command &&
                nor->tx_count >= nor->nand_pending_length) {
                nand_command(nor, nor->nand_pending_command, false,
                             nor->nand_pending_address,
                             nor->nand_pending_length);
            } else if (nor->nor_program_length &&
                       nor->tx_count >= nor->nor_program_length) {
                nor_command(nor, nor->nor_program_control);
            }
        }
    } else if (offset == 0x18) {
        if (value & 1) {
            nor->rx_head = nor->rx_count = 0;
        }
        if (value & 2) {
            nor->tx_count = nor->tx_slots = 0;
        }
    } else if (offset / 4 < G_N_ELEMENTS(nor->regs)) {
        nor->regs[offset / 4] = value;
        if (offset == 0) {
            nor_command(nor, value);
        }
    }
}

static const MemoryRegionOps nor_ops = {
    .read = nor_read,
    .write = nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static uint32_t q66_dma_pending(Q66Dma *dma, bool error)
{
    uint32_t status = error ? dma->error_status : dma->tc_status;
    uint32_t pending = 0;
    unsigned ch;

    for (ch = 0; ch < 8; ch++) {
        uint32_t config = dma->regs[(0x110 + ch * 0x20) / 4];
        if (config & (1u << (error ? 14 : 15))) {
            pending |= status & (1u << ch);
        }
    }
    return pending;
}

static void q66_dma_update_irq(Q66Dma *dma)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&dma->machine->arm), dma->audio ? 3 : 4),
                 !!(q66_dma_pending(dma, false) | q66_dma_pending(dma, true)));
}

static Q66Nor *q66_dma_fifo(Q66Dma *dma, uint32_t address)
{
    if (address == 0x40500008 || address == 0x40500010) {
        return &dma->machine->nor1;
    }
    if (address == 0x40140008 || address == 0x40140010) {
        return &dma->machine->nor;
    }
    return NULL;
}

static void q66_dma_run(void *opaque)
{
    Q66Dma *dma = opaque;
    unsigned ch;

    if (dma->audio) {
        q66_audio_run(dma);
        return;
    }

    if (!(dma->regs[0x30 / 4] & 1)) {
        return;
    }
    for (ch = 0; ch < 8; ch++) {
        uint32_t *reg = &dma->regs[(0x100 + ch * 0x20) / 4];
        unsigned chain = 0;

        while ((reg[4] & 1) && !(reg[4] & (1u << 18)) && chain++ < 64) {
            unsigned sw = (reg[3] >> 18) & 7;
            unsigned dw = (reg[3] >> 21) & 7;
            unsigned count = reg[3] & 0xfff;
            uint32_t source = reg[0], destination = reg[1];
            Q66Nor *rx = q66_dma_fifo(dma, source);
            Q66Nor *tx = q66_dma_fifo(dma, destination);
            uint8_t bytes[16384];
            unsigned length, i, width;
            bool failed = false;

            if (sw > 2 || dw > 2) {
                dma->error_status |= 1u << ch;
                reg[4] &= ~1u;
                break;
            }
            length = count << sw;
            if (rx && rx->rx_count < length) {
                /* Peripheral requests arrive after READ_CACHE, which the
                 * AP issues after arming the DMA channel. */
                break;
            }
            width = 1u << sw;
            for (i = 0; i < length; i += width) {
                if (address_space_read(&address_space_memory, source,
                                       MEMTXATTRS_UNSPECIFIED, bytes + i,
                                       width) != MEMTX_OK) {
                    failed = true;
                    break;
                }
                if (reg[3] & (1u << 26)) {
                    source += width;
                }
            }
            width = 1u << dw;
            for (i = 0; !failed && i < length; i += width) {
                if (tx) {
                    unsigned j;
                    for (j = 0; j < MIN(width, length - i); j++) {
                        nor_write(tx, 8, bytes[i + j], 1);
                        nor_drain_fifo(tx);
                    }
                } else if (address_space_write(&address_space_memory,
                                              destination,
                                              MEMTXATTRS_UNSPECIFIED,
                                              bytes + i,
                                              MIN(width, length - i)) !=
                           MEMTX_OK) {
                    failed = true;
                }
                if (reg[3] & (1u << 27)) {
                    destination += width;
                }
            }
            if (dma->traces++ < 64) {
                qemu_log_mask(LOG_UNIMP,
                              "q66 DMA ch=%u src=%08x dst=%08x bytes=%u "
                              "control=%08x config=%08x failed=%u\n",
                              ch, reg[0], reg[1], length, reg[3], reg[4],
                              failed);
            }
            reg[0] = source;
            reg[1] = destination;
            reg[3] &= ~0xfffu;
            if (failed) {
                dma->error_status |= 1u << ch;
                reg[4] &= ~1u;
                break;
            }
            if (reg[3] & (1u << 31)) {
                dma->tc_status |= 1u << ch;
            }
            if (reg[2]) {
                uint8_t descriptor[16];
                if (address_space_read(&address_space_memory, reg[2] & ~3u,
                                       MEMTXATTRS_UNSPECIFIED, descriptor,
                                       sizeof(descriptor)) != MEMTX_OK) {
                    dma->error_status |= 1u << ch;
                    reg[4] &= ~1u;
                    break;
                }
                for (i = 0; i < 4; i++) {
                    reg[i] = ldl_le_p(descriptor + i * 4);
                }
            } else {
                reg[4] &= ~1u;
            }
        }
    }
    q66_dma_update_irq(dma);
}

static void q66_dma_schedule(Q66MachineState *s)
{
    if (s->dma.timer) {
        timer_mod(s->dma.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
    }
}

static uint64_t q66_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    Q66Dma *dma = opaque;
    uint32_t value;
    unsigned ch;

    switch (offset) {
    case 0x00:
        return q66_dma_pending(dma, false) | q66_dma_pending(dma, true);
    case 0x04:
        return q66_dma_pending(dma, false);
    case 0x0c:
        return q66_dma_pending(dma, true);
    case 0x14:
        return dma->tc_status;
    case 0x18:
        return dma->error_status;
    case 0x1c:
        value = 0;
        for (ch = 0; ch < 8; ch++) {
            value |= (dma->regs[(0x110 + ch * 0x20) / 4] & 1) << ch;
        }
        return value;
    default:
        return dma->regs[offset / 4];
    }
}

static void q66_dma_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    Q66Dma *dma = opaque;
    if (offset == 0x08) {
        dma->tc_status &= ~value;
    } else if (offset == 0x10) {
        dma->error_status &= ~value;
    } else {
        dma->regs[offset / 4] = value;
        if (dma->audio) {
            timer_mod(dma->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
        } else {
            q66_dma_schedule(dma->machine);
        }
    }
    q66_dma_update_irq(dma);
}

static void q66_dma_reset(void *opaque)
{
    Q66Dma *dma = opaque;
    memset(dma->regs, 0, sizeof(dma->regs));
    dma->tc_status = dma->error_status = 0;
    timer_del(dma->timer);
    q66_dma_update_irq(dma);
}

static const MemoryRegionOps q66_dma_ops = {
    .read = q66_dma_read,
    .write = q66_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

#include "xiaomi-q66-audio.h"

static void psram_reset(void *opaque)
{
    Q66Psram *mc = opaque;

    memset(mc->regs, 0, sizeof(mc->regs));
    memset(mc->mode, 0, sizeof(mc->mode));
    /* AP's identification branch masks MR1 with 0x1f and expects AP Memory
     * vendor 0x0d before selecting its normal OPI configuration. */
    mc->mode[1] = 0x0d;
    mc->tx_count = mc->rx_head = mc->rx_count = 0;
    mc->transfer_traces = 0;
}

static uint64_t psram_read(void *opaque, hwaddr offset, unsigned size)
{
    Q66Psram *mc = opaque;

    if (offset == 0x8058) {
        /* Runtime matches reg_psram_phy_v2.h, not the v4 PHY. Ideal digital
         * DLL locks when powered, released from reset and clock-ready.
         * Analog calibration/timing margins are not simulated. */
        uint32_t dll = mc->regs[0x8050 / 4];
        return (dll & 0x600001) == 0x600001 ? 0x40 | 16 : 0;
    }
    if (offset == 0x404) {
        return (!mc->rx_count ? 2 : 0) | (mc->rx_count << 2) |
               (mc->tx_count == 8 ? 64 : 0) | ((8 - mc->tx_count) << 7);
    }
    if (offset == 0x18) {
        if (!mc->rx_count) {
            return 0;
        }
        mc->rx_count--;
        return mc->rx[mc->rx_head++ & 7];
    }
    return mc->regs[offset / 4];
}

static void psram_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    Q66Psram *mc = opaque;
    unsigned i;

    if (offset == 0x14) {
        if (mc->tx_count < 8) {
            mc->tx[mc->tx_count++] = value;
        }
    } else if (offset == 0x1c) {
        if (value & 1) {
            mc->tx_count = 0;
        }
        if (value & 2) {
            mc->rx_head = mc->rx_count = 0;
        }
    } else {
        mc->regs[offset / 4] = value;
        if (offset == 4) {
            unsigned address = mc->regs[2];
            unsigned words = MIN(MAX(mc->regs[3], 1), 8);
            uint8_t *ram = memory_region_get_ram_ptr(&mc->machine->psram);
            mc->rx_head = mc->rx_count = 0;
            switch (value & 255) {
            case 5: /* MEMIF_MRS */
                if (mc->tx_count) {
                    mc->mode[address & 255] = mc->tx[0];
                }
                break;
            case 6: /* MEMIF_MRR */
                mc->rx[0] = mc->mode[address & 255] * 0x01010101u;
                mc->rx_count = 1;
                break;
            case 2: /* MEMIF_READ */
                for (i = 0; i < words; i++) {
                    mc->rx[i] = ldl_le_p(ram + ((address + i * 4) &
                                        (PSRAM_SIZE - 4)));
                }
                mc->rx_count = words;
                break;
            case 1: /* MEMIF_WRITE */
                for (i = 0; i < mc->tx_count; i++) {
                    stl_le_p(ram + ((address + i * 4) & (PSRAM_SIZE - 4)),
                             mc->tx[i]);
                }
                break;
            case 0xff:
                memset(mc->mode, 0, sizeof(mc->mode));
                mc->mode[1] = 0x0d;
                break;
            default:
                break; /* Clock, refresh and no-op commands have no RAM effect. */
            }
            if (((value != 1 && value != 2) || mc->transfer_traces++ < 64) &&
                mc->machine->trace_count++ < 20000) {
                qemu_log_mask(LOG_UNIMP,
                              "q66 PSRAM cmd=%02x addr=%08x tx=%u rx=%u "
                              "pc=%08x\n", (unsigned)value & 255, address,
                              mc->tx_count, mc->rx_count,
                              mc->machine->arm.cpu->env.regs[15]);
            }
            mc->tx_count = 0;
        }
    }
}

static const MemoryRegionOps psram_ops = {
    .read = psram_read,
    .write = psram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t peripheral_read(void *opaque, hwaddr offset, unsigned size)
{
    Q66MmioWindow *window = opaque;
    Q66MachineState *s = window->machine;
    uint32_t address = offset + window->base;
    uint32_t aligned = address & ~3u;
    uint32_t value = GPOINTER_TO_UINT(g_hash_table_lookup(s->registers,
                                                       GUINT_TO_POINTER(aligned)));
    int gpio_bank = q66_gpio_bank(aligned);

    if (aligned >= 0x40006000 && aligned < 0x40007000 &&
        s->nfc.connected) {
        return q66_nfc_read(s, aligned & 0xfff);
    }
    if (aligned >= 0x40061000 && aligned < 0x40062000) {
        return q66_touch_i2c_read(s, aligned - 0x40061000);
    }
    if (aligned == 0x4030001c) { return q66_audio_fifo(s, size); }
    if (aligned == 0x40000098 || aligned == 0x4000009c) { return s->cp_to_ap_irq; }
    if (aligned == 0x400000b8 || aligned == 0x400000bc) { return s->ap_to_cp_irq; }
    if (aligned >= 0x40062000 && aligned < 0x40063000) {
        return q66_inputs_i2c_read(s, 4, aligned & 0xfff);
    }
    if (aligned >= 0x40063000 && aligned < 0x40064000) {
        return q66_inputs_i2c_read(s, 5, aligned & 0xfff);
    }
    if (aligned >= 0x40060000 && aligned < 0x40061000) {
        return q66_inputs_i2c_read(s, 2, aligned & 0xfff);
    }
    if (aligned >= 0x40005000 && aligned < 0x40006000 &&
        (q66_reg(s, 0x40005004) & 0x3ff) == 7) {
        return q66_charger_read(s, aligned - 0x40005000);
    }

    if (aligned == 0x40220004) {
        /* FE/PE/AXI are idle when the queued command buffer has finished. */
        value = timer_pending(s->gpu_timer) ? 0 : 0x0b05;
    }
    /* The supplied AP's vg_lite_init compares these exact silicon IDs
     * (0c92002e..04a); the older display SDK's GC355 is a different GPU. */
    if (aligned == 0x40220020) {
        value = 0x265;
    } else if (aligned == 0x40220024) {
        value = 0x2002;
    } else if (aligned == 0x40220030) {
        value = 0x424;
    }
    if (aligned == 0x40220010) {
        value = s->gpu_irq_status;
        s->gpu_irq_status = 0; /* GC265 interrupt acknowledge is read-to-clear. */
        q66_gpu_update_irq(s);
    }
    /* Read-only counters in an unused part of the experimental GPU aperture. */
    switch (aligned) {
    case 0x40223f00: value = s->gpu_submits; break;
    case 0x40223f04: value = s->gpu_draws; break;
    case 0x40223f08: value = s->gpu_errors; break;
    case 0x40223f0c: value = s->gpu_pixels; break;
    }
    if (aligned >= 0x40220000 && aligned < 0x40224000 &&
        s->gpu_trace_count++ < 512) {
        qemu_log_mask(LOG_UNIMP, "q66 GPU R @%08x=%08x pc=%08x\n",
                      aligned, value, s->arm.cpu->env.regs[15]);
    }

    if (aligned >= 0x4008508c && aligned <= 0x40085094) {
        /* The AON PSC snapshots all three banks before the AP GPIO ISR
         * acknowledges their EOI registers. */
        unsigned bank = (aligned - 0x4008508c) / 4;
        value = s->gpio_pending[bank] &
                q66_reg(s, 0x40085080 + 4 * bank);
    }
    if (gpio_bank >= 0) {
        uint32_t base = q66_gpio_bases[gpio_bank];
        switch (aligned - base) {
        case 0x40:
            value = s->gpio_pending[gpio_bank] & q66_reg(s, base + 0x30) &
                    ~q66_reg(s, base + 0x34);
            break;
        case 0x44:
            value = s->gpio_pending[gpio_bank];
            break;
        case 0x50:
            value = q66_reg(s, base) & q66_reg(s, base + 4);
            if (gpio_bank == 0 && !(q66_reg(s, base + 4) & (1u << 27)) &&
                !s->touch.irq_low) {
                value |= 1u << 27;
            }
            /* HX IRQ is configured by the AP as a falling-edge input. */
            if (gpio_bank == 0 && !(q66_reg(s, base + 4) & (1u << 14)) &&
                !s->inputs.ppg_irq) { value |= 1u << 14; }
            if (gpio_bank == 2 && s->inputs.imu_irq) { value |= 1u << 7; }
            break;
        default:
            break;
        }
    }
    /* The BES I2C blocks use the Synopsys DW_apb_i2c register layout.  The
     * bring-up image initializes four buses before it starts its scheduler;
     * returning the hardware's idle FIFO state here is enough for that
     * initialization to complete even when no external sensor is attached.
     * Keep the generic register store for all other offsets so later probes
     * still see their writes. */
    if ((address >= 0x40005000u && address < 0x40007000u) ||
        (address >= 0x40060000u && address < 0x40064000u)) {
        uint32_t i2c_off = address & 0xfffu;
        switch (i2c_off) {
        case 0x2c: /* INTR_STAT: complete an absent-device transaction */
        case 0x34: /* RAW_INTR_STAT */
            /* TX_ABRT plus RX_UNDER is the controller's normal no-device
             * completion path.  The stock driver turns this into an error
             * code and advances its task queue rather than waiting for a
             * sensor that is not present in the emulator. */
            value |= 0x241u;
            break;
        case 0x70: /* STATUS: RX available, TX empty/not-full, bus idle */
            value |= 0x16u;
            value &= ~0x1u;
            break;
        case 0x74: /* TXFLR */
            value = 0;
            break;
        case 0x78: /* RXFLR */
            /* A sensor transaction can complete without a physical sensor
             * in this board model.  Advertise a small filled RX FIFO so the
             * stock polling path consumes its requested bytes instead of
             * waiting forever for an interrupt-driven peripheral. */
            value = 8;
            break;
        case 0x80: /* TX_ABRT_SOURCE */
            value = 1;
            break;
        case 0x9c: /* ENABLE_STATUS */
            value |= (GPOINTER_TO_UINT(g_hash_table_lookup(
                s->registers, GUINT_TO_POINTER(aligned - 0x30))) & 1);
            break;
        default:
            break;
        }
    }
    if (aligned == 0x40080000) {
        /* AON CHIP_ID[31:16] is the digital silicon revision, independent
         * of the PMU's analog revision.  The supplied AP's power-on HCI
         * table at 0x0c929e70 supports revision 3..5 and rejects 0..2. */
        value = 0x00030000;
    }
    /* The stock AP's LCDC command path polls SPU_IRQ_ISR bit 18 (CMD/SMPN
     * transfer complete) after every panel command.  The generic register
     * aperture retains writes, but without the controller's completion edge
     * the original firmware spins forever at 0x0c934a96. */
    if (aligned == 0x402001c4) {
        value = s->lcdc_irq_status | 0x00040000u;
    }
    /* First byte of the RM690C0/O66-TM panel identification returned by the
     * smart-panel read FIFO.  The stock probe reads RDID1 (0xDA) first and
     * expects the manufacturer byte 0x46 before requesting the remaining ID
     * bytes. */
    if (aligned == 0x40200140) {
        value = s->lcdc_read_reg == 0xDA ? 0x46u :
                s->lcdc_read_reg == 0xDB ? 0x02u :
                s->lcdc_read_reg == 0xDC ? 0x20u : 0u;
    }
    if ((address >= 0x40160000 && address < 0x40180000) ||
        (address >= 0x40200000 && address < 0x40210000)) {
        if (s->display_trace_count++ < 20000) {
            qemu_log_mask(LOG_UNIMP,
                          "q66 DISPLAY R%u @%08x = %08x pc=%08x\n",
                          size, address, value,
                          s->arm.cpu->env.regs[15]);
        }
    }
    if (address == 0x4000008c) {
        /* LPU auto-switch completes once the selected clock is stable. */
        value |= (value & 0x3000) << 2;
    }
    /* BEST1503 AON IOMUX REG_040 exposes the power-key input at bit 31.
     * The physical band is held in the asserted power-on state while the
     * boot-time key checker samples it.  Returning the asserted level here
     * lets the stock app_key_open(true) path finish its 10-sample startup
     * debounce instead of treating the emulator as a power-key dither and
     * immediately entering pmu_shutdown(). */
    if (address == 0x40086040 || address == 0x40086044) {
        if (s->pwrkey_startup_reads < 10) {
            value |= (1u << 31);
            s->pwrkey_startup_reads++;
        } else {
            value &= ~(1u << 31);
        }
    }
    if (aligned == 0x400000a0 || aligned == 0x400000a4) {
        value = q66_intersys_status(s);
    }
    if (address == 0x400000a0 && s->bt_wakeup_ack_reads) {
        if (s->bt_wakeup_ack_reads == 1) {
            /* The first read observes the posted ISIRQ_SET write.  The
             * following poll sees the BT-side clear, matching hardware. */
            s->bt_wakeup_ack_reads = 0;
            s->intersys_control &= ~(1u << 3);
            value &= ~(1u << 3);
            g_hash_table_insert(s->registers, GUINT_TO_POINTER(aligned),
                                GUINT_TO_POINTER(value));
        } else {
            s->bt_wakeup_ack_reads--;
        }
    }
    if (address >= 0x400000c0 && address < 0x400000d0) {
        unsigned bit = 1u << ((address - 0x400000c0) / 4);
        value = !(s->memsc_locked & bit);
        s->memsc_locked |= bit; /* MEMSC read-to-acquire, write-one-to-release. */
    } else if (address == 0x400000d0) {
        value = (~s->memsc_locked) & 15;
    } else if (address >= 0x40080024 && address < 0x40080034) {
        unsigned bit = 1u << ((address - 0x40080024) / 4);
        value = !(s->memsc_aon_locked & bit);
        s->memsc_aon_locked |= bit;
    } else if (address == 0x40080034) {
        value = (~s->memsc_aon_locked) & 15;
    }
    value = (value >> ((address & 3) * 8)) & (size == 4 ? UINT32_MAX :
                                            (1u << (size * 8)) - 1);
    if (s->last_read != address && s->trace_count++ < 20000) {
        qemu_log_mask(LOG_UNIMP, "q66 R%u @%08x = %08x pc=%08x (stub)\n",
                      size, address, value, s->arm.cpu->env.regs[15]);
    }
    s->last_read = address;
    return value;
}

static void peripheral_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Q66MmioWindow *window = opaque;
    Q66MachineState *s = window->machine;
    uint32_t address = offset + window->base;
    uint32_t aligned = address & ~3u;
    uint32_t old = GPOINTER_TO_UINT(g_hash_table_lookup(s->registers,
                                                      GUINT_TO_POINTER(aligned)));
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    unsigned shift = (address & 3) * 8;
    uint32_t merged = (old & ~(mask << shift)) | ((value & mask) << shift);
    int gpio_bank = q66_gpio_bank(aligned);

    if (aligned >= 0x40006000 && aligned < 0x40007000) {
        q66_nfc_write(s, aligned & 0xfff, merged);
        if (s->nfc.connected) { return; }
    }
    if (aligned >= 0x40061000 && aligned < 0x40062000) {
        q66_touch_i2c_write(s, aligned - 0x40061000, merged);
        return;
    }
    if (aligned >= 0x40062000 && aligned < 0x40063000) {
        q66_inputs_i2c_write(s, 4, aligned & 0xfff, merged);
        return;
    }
    if (aligned >= 0x40063000 && aligned < 0x40064000) {
        q66_inputs_i2c_write(s, 5, aligned & 0xfff, merged);
        return;
    }
    if (aligned >= 0x40060000 && aligned < 0x40061000) {
        q66_inputs_i2c_write(s, 2, aligned & 0xfff, merged);
        return;
    }
    if (aligned >= 0x40005000 && aligned < 0x40006000) {
        g_hash_table_insert(s->registers, GUINT_TO_POINTER(aligned),
                            GUINT_TO_POINTER(merged));
        q66_charger_write(s, aligned - 0x40005000, merged);
        if ((q66_reg(s, 0x40005004) & 0x3ff) == 7) {
            return;
        }
    }

    if (aligned == 0x40085010 && (merged & 0xffff0001) == 0xcafe0001) {
        /* BEST1503 PSC POWERDN_START. Defer the reset domain transition
         * until the current MMIO access and translated block have exited. */
        s->psc_power_down = true;
        timer_mod(s->psc_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        qemu_log_mask(LOG_UNIMP, "q66 PSC core power-down requested\n");
    }

    if (aligned >= 0x40220000 && aligned < 0x40224000 &&
        s->gpu_trace_count++ < 512) {
        qemu_log_mask(LOG_UNIMP, "q66 GPU W @%08x=%08x pc=%08x\n",
                      aligned, merged, s->arm.cpu->env.regs[15]);
    }
    if (aligned >= 0x40220000 && aligned < 0x40224000) {
        g_hash_table_insert(s->registers, GUINT_TO_POINTER(aligned),
                            GUINT_TO_POINTER(merged));
        if (aligned == 0x40220000 && (merged & 0x1000)) {
            q66_gpu_reset(s);
        } else if (aligned == 0x40220014) {
            q66_gpu_update_irq(s);
        } else if (aligned == 0x40220504 && merged) {
            if (timer_pending(s->gpu_timer) || merged > MiB / 8) {
                s->gpu_errors++;
                s->gpu_irq_status |= 0x80000000;
                q66_gpu_update_irq(s);
            } else {
                s->gpu_command_address = q66_reg(s, 0x40220500);
                s->gpu_command_bytes = merged * 8;
                s->gpu_submits++;
                timer_mod(s->gpu_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
        }
        return;
    }

    if (aligned == 0xd0330054) {
        s->intersys_rx_pending &= ~(((value & mask) << shift) >> 2 & 3);
        q66_intersys_update_irq(s);
        if (value & 4) {
            uint8_t *bt = memory_region_get_ram_ptr(&s->bt_ram);
            stl_le_p(bt + 0x4000, 0);
            if (s->hci_event_count) {
                timer_mod(s->intersys_rx_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
            }
        }
        return;
    }
    if (aligned == 0xd0330050) {
        uint32_t bits = (value & mask) << shift;
        s->intersys_rx_pending |= (bits >> 2) & 3;
        s->intersys_tx_pending |= bits & 3;
        q66_intersys_update_irq(s);
        return;
    }

    if (aligned == 0x400000a0 || aligned == 0x400000a4) {
        uint32_t bits = (value & mask) << shift;
        if (aligned == 0x400000a0) {
            s->intersys_control |= bits & 0x1ffff;
            if (bits & (1u << 3)) {
                s->bt_wakeup_ack_reads = 2;
            }
            if (bits & (1u << 2)) {
                timer_mod(s->intersys_tx_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
        } else {
            s->intersys_control &= ~(bits & 0x1ffff);
            s->intersys_tx_pending &= ~(bits & 3);
            if (bits & (1u << 3)) {
                s->bt_wakeup_ack_reads = 0;
            }
        }
        q66_intersys_update_irq(s);
        qemu_log_mask(LOG_UNIMP,
                      "q66 intersys W @%08x=%08x status=%08x pc=%08x\n",
                      aligned, bits, q66_intersys_status(s),
                      s->arm.cpu->env.regs[15]);
        return;
    }

    if (gpio_bank >= 0) {
        q66_gpio_write(s, gpio_bank, aligned - q66_gpio_bases[gpio_bank],
                       (value & mask) << shift);
        if (s->gpio_trace_count++ < 512) {
            qemu_log_mask(LOG_UNIMP,
                          "q66 GPIO W%u @%08x = %08x pc=%08x\n",
                          size, address, (uint32_t)value,
                          s->arm.cpu->env.regs[15]);
        }
        return;
    }

    if (address == 0x402001c4) {
        /* The AP acknowledges frame/command status by writing zero. */
        s->lcdc_irq_status &= value;
        q66_lcdc_update_irq(s);
    }

    if (address == 0x40200284 && (value & 0xFF000000u) == 0x03000000u) {
        /* Smart-panel read command word: command byte occupies bits 15:8. */
        s->lcdc_read_reg = (value >> 8) & 0xff;
    }
    if (address == 0x40200284 &&
        (value & 0xFF000000u) == 0x02000000u && s->panel) {
        uint8_t command = (value >> 8) & 0xff;
        s->lcdc_write_command = command;
        /* SMPN DCS command words use 0x02 in the high byte.  The stock
         * RM690C0 sequence turns the panel on with DISPON (0x29). */
        if (command == 0x29) {
            s->panel->power = 1;
            qemu_log_mask(LOG_UNIMP, "q66 panel DCS DISPON\n");
        } else if (command == 0x28) {
            s->panel->power = 0;
        } else if (command == 0x35) {
            s->panel_te_enabled = true;
        } else if (command == 0x34) {
            s->panel_te_enabled = false;
        }
        if (s->panel->power && s->panel_te_enabled &&
            !timer_pending(s->panel_te_timer)) {
            timer_mod(s->panel_te_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667);
        }
    }
    if (address == 0x40200288 && s->lcdc_write_command == 0x51 && s->panel) {
        s->panel->brightness = (value >> 24) & 255;
        s->lcdc_write_command = 0;
        qemu_log_mask(LOG_UNIMP, "q66 panel brightness=%u\n",
                      s->panel->brightness);
    }
    if (address == 0x402000f4) {
        s->lcdc_framebuffer = value;
    }
    if (address == 0x40200224 && (value & 1)) {
        q66_lcdc_commit(s);
    } else if (address == 0x40200210 && (value & 0x20000000u)) {
        q66_lcdc_commit(s);
    }

    if ((address >= 0x40160000 && address < 0x40180000) ||
        (address >= 0x40200000 && address < 0x40210000)) {
        if (s->display_trace_count++ < 20000) {
            qemu_log_mask(LOG_UNIMP,
                          "q66 DISPLAY W%u @%08x = %08x pc=%08x\n",
                          size, address, (uint32_t)value,
                          s->arm.cpu->env.regs[15]);
        }
    }

    /* The AP image's secure CMU veneer passes HAL_CMU_AON_GLOBAL as 0xA6.
     * On this firmware build the veneer maps that ID to GBL_RESET_SET[7]
     * (0x80), rather than the bit-8 value in the newer SDK header.  A real
     * BEST1503 resets the AP immediately after this write.  Keep the
     * boot-mode cause visible across the QEMU reset so the ROM handoff can
     * distinguish this from a cold power-key boot. */
    if (address == 0x400800c8 && (value & 0x80)) {
        uint8_t stack_words[8] = { 0 };
        address_space_read(&address_space_memory, s->arm.cpu->env.regs[13],
                           MEMTXATTRS_UNSPECIFIED, stack_words,
                           sizeof(stack_words));
        s->ispi.retain_analog_on_reset = true;
        uint32_t bootmode = GPOINTER_TO_UINT(g_hash_table_lookup(
            s->registers, GUINT_TO_POINTER(0x40080038)));
        g_hash_table_insert(s->registers, GUINT_TO_POINTER(0x40080038),
                            GUINT_TO_POINTER(bootmode | 0x2));
        qemu_log_mask(LOG_UNIMP,
                      "q66 AON global reset request pc=%08x lr=%08x sp=%08x "
                      "stack0=%08x stack1=%08x bootmode=%08x\n",
                      s->arm.cpu->env.regs[15], s->arm.cpu->env.regs[14],
                      s->arm.cpu->env.regs[13],
                      ldl_le_p(stack_words), ldl_le_p(stack_words + 4),
                      bootmode | 0x2);
        /* A physical BEST1503 enters the ROM hand-off path here.  Request a
         * real QEMU reset so the AP observes the retained global-boot cause
         * on its next start, instead of falling into pmu_reboot()'s timeout
         * fallback when the reset write has no effect. */
        /* Defer reset to QEMU's main loop; resetting synchronously inside
         * a TCG MMIO callback invalidates the executing CPU state. */
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
    if (address >= 0x400000c0 && address < 0x400000d0 && (value & 1)) {
        s->memsc_locked &= ~(1u << ((address - 0x400000c0) / 4));
    } else if (address >= 0x40080024 && address < 0x40080034 && (value & 1)) {
        s->memsc_aon_locked &= ~(1u << ((address - 0x40080024) / 4));
    }
    g_hash_table_insert(s->registers, GUINT_TO_POINTER(aligned),
                         GUINT_TO_POINTER(merged));
    if (aligned == 0x40000098) { s->cp_to_ap_irq |= value; q66_cp_update_irq(s); }
    if (aligned == 0x4000009c) { s->cp_to_ap_irq &= ~value; q66_cp_update_irq(s); }
    if (aligned == 0x400000b8) { s->ap_to_cp_irq |= value; q66_cp_update_irq(s); }
    if (aligned == 0x400000bc) { s->ap_to_cp_irq &= ~value; q66_cp_update_irq(s); }
    if (aligned == 0x40000038 && (value & (1u << 23)) && !s->cp_started) {
        s->cp_started = true;
        async_run_on_cpu(CPU(s->cp.cpu), q66_cp_start_on_cpu, RUN_ON_CPU_HOST_PTR(s));
    }
    if (aligned == 0x40000034 && (value & (1u << 23))) {
        s->cp_started = false;
        arm_set_cpu_off(arm_cpu_mp_affinity(s->cp.cpu));
    }
    if (address == 0x402001c0) {
        q66_lcdc_update_irq(s);
    }
    if (aligned >= 0x40085080 && aligned <= 0x40085088) {
        q66_gpio_update_irq(s);
    }
    if (s->last_write != address && s->trace_count++ < 20000) {
        qemu_log_mask(LOG_UNIMP, "q66 W%u @%08x = %08x pc=%08x (stub)\n",
                      size, address, (uint32_t)value,
                      s->arm.cpu->env.regs[15]);
    }
    s->last_write = address;
}

static const MemoryRegionOps peripheral_ops = {
    .read = peripheral_read,
    .write = peripheral_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void add_mmio_window(Q66MachineState *s, Q66MmioWindow *window,
                            const char *name, uint32_t base, uint64_t size)
{
    window->machine = s;
    window->base = base;
    memory_region_init_io(&window->region, OBJECT(s), &peripheral_ops,
                          window, name, size);
    memory_region_add_subregion(get_system_memory(), base, &window->region);
}

static void add_alias(Q66MachineState *s, unsigned slot, const char *name,
                      hwaddr address, MemoryRegion *original, uint64_t size)
{
    memory_region_init_alias(&s->aliases[slot], OBJECT(s), name, original, 0,
                             size);
    memory_region_add_subregion(get_system_memory(), address, &s->aliases[slot]);
}

static void add_boot_timer(Q66MachineState *s, const char *name,
                           hwaddr address, uint32_t frequency,
                           unsigned irq0, unsigned irq1)
{
    DeviceState *timer = qdev_new("sp804");

    /* The AP writes control=0x82 and polls Value at offset 4. This matches
     * the SP804 free-running 32-bit downcounter, not a stored register.
     * Interrupt routing is not yet known; boot uses interrupt-free polling. */
    object_property_add_child(OBJECT(s), name, OBJECT(timer));
    qdev_prop_set_uint32(timer, "freq0", frequency);
    qdev_prop_set_uint32(timer, "freq1", frequency);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(timer), 0, address, 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 0,
                       qdev_get_gpio_in(DEVICE(&s->arm), irq0));
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 1,
                       qdev_get_gpio_in(DEVICE(&s->arm), irq1));
}

static void add_uart(Q66MachineState *s, unsigned index, unsigned irq)
{
    DeviceState *uart = qdev_new("pl011");
    g_autofree char *name = g_strdup_printf("uart%u", index);
    g_autofree char *clock_name = g_strdup_printf("uart%u-clock", index);
    Clock *clock = clock_new(OBJECT(s), clock_name);

    clock_set_hz(clock, 24000000);
    object_property_add_child(OBJECT(s), name, OBJECT(uart));
    qdev_prop_set_chr(uart, "chardev", serial_hd(index));
    qdev_connect_clock_in(uart, "clk", clock);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(uart), 0, 0x4000b000 + index * 0x1000, 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                       qdev_get_gpio_in(DEVICE(&s->arm), irq));
}

/* Reset AP-side display/GPIO state while the powered panel retains its
 * pixels. The CPU's normal reset path supplies the initial security state. */
static void q66_board_reset(void *opaque)
{
    Q66MachineState *s = opaque;
    unsigned bank, offset;

    timer_del(s->lcdc_frame_timer);
    timer_del(s->psc_timer);
    s->psc_power_down = false;
    s->psc_wake_count = 0;
    timer_del(s->intersys_tx_timer);
    timer_del(s->intersys_rx_timer);
    s->intersys_control = 0;
    s->intersys_tx_pending = s->intersys_rx_pending = 0;
    s->hci_event_head = s->hci_event_count = 0;
    s->hci_stream_len = 0;
    s->bt_wakeup_ack_reads = 0;
    q66_touch_reset(s);
    q66_charger_reset(s);
    q66_inputs_reset(s);
    q66_nfc_reset(s);
    s->audio.head = s->audio.count = 0;
    s->audio.next_sample = 0;
    s->cp_started = false;
    s->cp_to_ap_irq = s->ap_to_cp_irq = 0;
    q66_cp_update_irq(s);
    q66_intersys_update_irq(s);
    q66_gpu_reset(s);
    timer_del(s->gpadc_timer);
    q66_gpadc_update_irq(s);
    s->lcdc_irq_status = 0;
    s->lcdc_framebuffer = 0;
    s->lcdc_write_command = 0;
    for (bank = 0; bank < G_N_ELEMENTS(q66_gpio_bases); bank++) {
        s->gpio_pending[bank] = 0;
        for (offset = 0; offset < 0x64; offset += 4) {
            g_hash_table_remove(s->registers,
                GUINT_TO_POINTER(q66_gpio_bases[bank] + offset));
        }
        g_hash_table_remove(s->registers,
                            GUINT_TO_POINTER(0x40085080 + 4 * bank));
    }
    g_hash_table_remove(s->registers, GUINT_TO_POINTER(0x402001c0));
    g_hash_table_remove(s->registers, GUINT_TO_POINTER(0x40200224));
    q66_lcdc_update_irq(s);
    q66_gpio_update_irq(s);
}

static char *q66_get_ota_image(Object *obj, Error **errp)
{
    return g_strdup(Q66_MACHINE(obj)->ota_filename);
}

static void q66_set_ota_image(Object *obj, const char *value, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    g_free(s->ota_filename);
    s->ota_filename = g_strdup(value);
}

static uint64_t q66_elf_load_address(void *opaque, uint64_t address)
{
    /* Preserve the ELF's execute-alias entry while loading segments at LMA. */
    return address;
}

static char *q66_get_recovery_image(Object *obj, Error **errp)
{
    return g_strdup(Q66_MACHINE(obj)->recovery_filename);
}

static void q66_set_recovery_image(Object *obj, const char *value, Error **errp)
{
    Q66MachineState *s = Q66_MACHINE(obj);
    g_free(s->recovery_filename);
    s->recovery_filename = g_strdup(value);
}

static char *q66_get_initial_bootmode(Object *obj, Error **errp)
{
    return g_strdup_printf("0x%08x", Q66_MACHINE(obj)->initial_bootmode);
}

static void q66_set_initial_bootmode(Object *obj, const char *value, Error **errp)
{
    uint64_t mode;
    if (qemu_strtou64(value, NULL, 0, &mode) < 0 || mode > UINT32_MAX) {
        error_setg(errp, "initial-bootmode must be a 32-bit integer");
        return;
    }
    Q66_MACHINE(obj)->initial_bootmode = mode;
}

static void q66_copy_raw_image(const char *path, uint8_t *destination,
                               size_t maximum)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    size_t length;

    if (!g_file_get_contents(path, &contents, &length, &error) ||
        length > maximum) {
        error_report("q66 cannot read image '%s' within its partition", path);
        exit(1);
    }
    memcpy(destination, contents, length);
}

static BlockBackend *q66_open_backing(const char *name, int64_t size)
{
    BlockBackend *blk = blk_by_name(name);
    if (blk && (blk_getlength(blk) != size ||
                blk_set_perm(blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                             BLK_PERM_CONSISTENT_READ, &error_fatal) < 0)) {
        error_report("q66 %s must be a writable raw image of %" PRId64
                     " bytes", name, size);
        exit(1);
    }
    return blk;
}

static void q66_read_backing(BlockBackend *blk, int64_t offset,
                             int64_t bytes, void *destination)
{
    int ret = blk_pread(blk, offset, bytes, destination, 0);
    if (ret < 0) {
        error_report("q66 flash image read failed: %s", strerror(-ret));
        exit(1);
    }
}

static void q66_flush_backing(BlockBackend *blk)
{
    int ret;

    if (blk && (ret = blk_flush(blk)) < 0) {
        error_report("q66 flash image flush failed: %s", strerror(-ret));
        exit(1);
    }
}

static void q66_boot_factory_reset(Q66MachineState *s)
{
    unsigned page;

    /* The supplied AP requests factory reset with SW_BOOTMODE bit 20.
     * Its BES boot ROM/BL2 is absent from the OTA distribution. Model that
     * boot service using the NAND erase path, then let the unchanged AP
     * initialize an empty YAFFS filesystem. This is a modeled boot service,
     * not execution of an original recovery binary. NOR identity/firmware
     * and the resource half of the NAND are outside this erase range. */
    if (!s->nor1.nand_spare) {
        error_report("q66 factory reset requires a NAND device");
        exit(1);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "q66 modeled bootloader factory reset: erase userdata\n");
    for (page = NAND_DATA_FIRST_PAGE; page < NAND_PAGES; page += 64) {
        if (!q66_nand_erase_block(&s->nor1, page)) {
            error_report("q66 factory reset failed at NAND page %u", page);
            exit(1);
        }
    }
    q66_flush_backing(s->nor1.nand_data);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "q66 modeled bootloader factory reset: userdata erased\n");
}

static void q66_boot_reset(void *opaque)
{
    Q66MachineState *s = opaque;
    MachineState *machine = MACHINE(s);
    uint32_t bootmode = q66_reg(s, 0x40080038);
    uint32_t entry = machine->kernel_filename ? 0x0c0c0011 : 0x1001;
    uint8_t *vectors = memory_region_get_ram_ptr(&s->vectors);

    q66_flush_backing(s->flash_backend);
    q66_flush_backing(s->nor1.nand_resource);
    q66_flush_backing(s->nor1.nand_data);

    const char *image = NULL;
    uint32_t expected_entry = 0;
    if (bootmode & ((1u << 23) | (1u << 28))) {
        image = s->ota_filename;
        expected_entry = 0x1cd00011;
    } else if (bootmode & (1u << 20)) {
        if (s->recovery_filename) {
            /* Explicit recovery-image is only for recovery diagnostics. */
            image = s->recovery_filename;
            expected_entry = 0x1c000011;
        } else {
            q66_boot_factory_reset(s);
            bootmode &= ~(1u << 20);
            g_hash_table_insert(s->registers, GUINT_TO_POINTER(0x40080038),
                                GUINT_TO_POINTER(bootmode));
        }
    }
    if ((bootmode & ((1u << 23) | (1u << 28))) && !image) {
        error_report("q66 OTA boot requested without ota-image");
        exit(1);
    }
    if (image) {
        uint64_t elf_entry;
        uint8_t reboot_parameter[0x40];
        uint8_t *retained = memory_region_get_ram_ptr(&s->sram) + 0x6017c;
        /* .reboot_param is NOLOAD retained SRAM, not ordinary ELF BSS. */
        memcpy(reboot_parameter, retained, sizeof(reboot_parameter));
        qemu_log_mask(LOG_UNIMP, "q66 retained reboot param=%08x %08x "
                      "%08x %08x\n", ldl_le_p(retained),
                      ldl_le_p(retained + 4), ldl_le_p(retained + 8),
                      ldl_le_p(retained + 12));
        ssize_t size = load_elf_ram_sym(image, NULL,
                                      q66_elf_load_address, NULL,
                                      &elf_entry, NULL, NULL, NULL,
                                      ELFDATA2LSB, EM_ARM, 1, 0,
                                      &address_space_memory, false, NULL);
        if (size < 0 || elf_entry != expected_entry) {
            error_report("q66 could not load the original recovery ELF");
            exit(1);
        }
        memcpy(retained, reboot_parameter, sizeof(reboot_parameter));
        entry = elf_entry;
    }
    s->boot_entry = entry;
    stl_le_p(vectors, 0x2015fec0);
    stl_le_p(vectors + 4, entry);
    /* Recovery replaces the executable SRAM at the same aliases used by AP.
     * A CPU reset alone does not discard previously translated AP blocks. */
    queue_tb_flush(CPU(s->arm.cpu));
    qemu_log_mask(LOG_UNIMP, "q66 boot ROM entry=%08x bootmode=%08x\n",
                  entry, bootmode);
}

static void q66_init(MachineState *machine)
{
    Q66MachineState *s = Q66_MACHINE(machine);
    MemoryRegion *memory = get_system_memory();
    DeviceState *panel;
    Clock *clock;
    uint32_t vectors[128];
    unsigned i;


    /* OTA image is not a vector-table image: the boot ROM normally jumps to
     * FLASHX_BASE + OTA_CODE_OFFSET + 0x10. Supply that small ROM contract. */
    for (i = 0; i < G_N_ELEMENTS(vectors); i++) {
        vectors[i] = cpu_to_le32(0x0c0c0085);
    }
    vectors[0] = cpu_to_le32(0x2015fec0);
    vectors[1] = cpu_to_le32(machine->kernel_filename ? 0x0c0c0011 : 0x1001);
    memory_region_init_ram_flags_nomigrate(&s->vectors, OBJECT(s), "q66.bootrom", 64 * KiB, 0,
                           &error_fatal);
    memory_region_add_subregion(memory, 0, &s->vectors);
    memcpy(memory_region_get_ram_ptr(&s->vectors), vectors, sizeof(vectors));

    memory_region_init_ram_flags_nomigrate(&s->sram, OBJECT(s), "q66.sram", 0x160000, 0,
                           &error_fatal);
    memory_region_add_subregion(memory, 0x20000000, &s->sram);
    /* The stock PMU startup callback records an asserted wake/power source in
     * this BSS byte before it decides whether to fall into pmu_shutdown().
     * Model a cold power-key assertion for the initial boot. */
    stl_le_p(memory_region_get_ram_ptr(&s->sram) + 0x78f7c, 1);
    add_alias(s, 0, "q66.sram-execute", 0x00200000, &s->sram, 0x160000);

    memory_region_init_ram_flags_nomigrate(&s->psram, OBJECT(s), "q66.psram", PSRAM_SIZE, 0,
                           &error_fatal);
    memory_region_add_subregion(memory, 0x3c000000, &s->psram);
    add_alias(s, 1, "q66.psram-execute", 0x1c000000, &s->psram, PSRAM_SIZE);
    add_alias(s, 2, "q66.psram-uncached", 0x38000000, &s->psram, PSRAM_SIZE);
    add_alias(s, 3, "q66.psram-uncached-execute", 0x18000000, &s->psram,
              PSRAM_SIZE);

    /* BEST1503 exposes a small Bluetooth controller RAM window at
     * BT_RAM_BASE.  The AP firmware copies controller tables into this
     * device-memory region during the timer-driven system bring-up; leaving
     * it unmapped raises a precise BusFault before the display stack starts.
     * The SDK's plat_addr_map_best1503.h gives the authoritative 64 KiB
     * window size. */
    memory_region_init_ram_flags_nomigrate(&s->bt_ram, OBJECT(s), "q66.bt-ram",
                                           0x00010000, 0, &error_fatal);
    memory_region_add_subregion(memory, 0xc0000000, &s->bt_ram);

    memory_region_init_ram_flags_nomigrate(&s->flash, OBJECT(s), "q66.nor", FLASH_SIZE, 0,
                           &error_fatal);
    memory_region_add_subregion(memory, 0x2c000000, &s->flash);
    add_alias(s, 4, "q66.flash-execute", 0x0c000000, &s->flash, FLASH_SIZE);
    add_alias(s, 5, "q66.flash-uncached", 0x28000000, &s->flash, FLASH_SIZE);
    add_alias(s, 6, "q66.flash-uncached-execute", 0x08000000, &s->flash,
              FLASH_SIZE);
    memset(memory_region_get_ram_ptr(&s->flash), 0xff, FLASH_SIZE);
    s->flash_backend = q66_open_backing("nor-flash", FLASH_SIZE);
    if (s->flash_backend) {
        q66_read_backing(s->flash_backend, 0, FLASH_SIZE,
                         memory_region_get_ram_ptr(&s->flash));
    } else if (machine->kernel_filename) {
        q66_copy_raw_image(machine->kernel_filename,
                          memory_region_get_ram_ptr(&s->flash) + 0xc0000,
                          FLASH_SIZE - 0xc0000);
    }

    s->registers = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(s->registers, GUINT_TO_POINTER(0x40080038),
                        GUINT_TO_POINTER(s->initial_bootmode));
    s->last_read = s->last_write = UINT32_MAX;
    add_mmio_window(s, &s->peripherals, "q66.unimplemented-peripherals",
                    0x40000000, 0x20000000);
    /* BEST1503's BT subsystem register aperture is separate from the AP
     * peripheral window.  The stock AP programs the controller reset/clock
     * block around 0xA01FFE00 while bringing up the Bluetooth shared RAM. */
    add_mmio_window(s, &s->bt_subsys, "q66.bt-subsystem",
                    0xa0000000, 0x00200000);
    /* BT exchange/CMU/CPU-dump blocks occupy the 0xD0xxxxxx aperture.  They
     * are touched while the AP publishes the controller image; retain writes
     * and reads through the common register model until individual blocks are
     * needed by a later execution path. */
    add_mmio_window(s, &s->bt_aux, "q66.bt-auxiliary",
                    0xd0000000, 0x01000000);
    /* AP cache enable routine at 0x0c0c1800 uses BES cache registers.
     * TCG is coherent; retain control writes without inventing cache timing. */
    add_mmio_window(s, &s->core_cache[0], "q66.core-cache0", 0x07ffa000, 0x1000);
    add_mmio_window(s, &s->core_cache[1], "q66.core-cache1", 0x07ffc000, 0x1000);
    s->ispi.machine = s;
    ispi_reset(&s->ispi);
    memory_region_init_io(&s->ispi.region, OBJECT(s), &ispi_ops, &s->ispi,
                          "q66.bes-ispi", 0x1000);
    memory_region_add_subregion_overlap(memory, 0x40009000,
                                        &s->ispi.region, 1);
    qemu_register_reset(ispi_reset, &s->ispi);
    s->spi.machine = s;
    s->spi.external = true;
    ispi_reset(&s->spi);
    memory_region_init_io(&s->spi.region, OBJECT(s), &ispi_ops, &s->spi,
                          "q66.bes-spi", 0x1000);
    memory_region_add_subregion_overlap(memory, 0x40007000,
                                        &s->spi.region, 1);
    qemu_register_reset(ispi_reset, &s->spi);
    s->nor.machine = s;
    nor_reset(&s->nor);
    memory_region_init_io(&s->nor.region, OBJECT(s), &nor_ops, &s->nor,
                          "q66.bes-nor-controller", 0x1000);
    memory_region_add_subregion_overlap(memory, 0x40140000,
                                        &s->nor.region, 1);
    qemu_register_reset(nor_reset, &s->nor);
    /* The second serial-flash controller hosts the resource/data SPI NAND. */
    s->nor1.machine = s;
    nor_reset(&s->nor1);
    memory_region_init_io(&s->nor1.region, OBJECT(s), &nor_ops, &s->nor1,
                          "q66.bes-nor1-controller", 0x1000);
    memory_region_add_subregion_overlap(memory, 0x40500000,
                                        &s->nor1.region, 1);
    qemu_register_reset(nor_reset, &s->nor1);
    s->psram_ctrl.machine = s;
    psram_reset(&s->psram_ctrl);
    memory_region_init_io(&s->psram_ctrl.region, OBJECT(s), &psram_ops,
                          &s->psram_ctrl, "q66.bes-psram-controller", 0x9000);
    memory_region_add_subregion_overlap(memory, 0x40160000,
                                        &s->psram_ctrl.region, 1);
    qemu_register_reset(psram_reset, &s->psram_ctrl);

    if (machine->initrd_filename) {
        memory_region_init_ram_flags_nomigrate(&s->resource, OBJECT(s),
                               "q66.nand-main", NAND_SIZE, 0, &error_fatal);
        memory_region_add_subregion(memory, 0x70000000, &s->resource);
        memset(memory_region_get_ram_ptr(&s->resource), 0xff, NAND_SIZE);
        s->nor1.nand_spare = g_malloc(NAND_PAGES * NAND_SPARE_SIZE);
        memset(s->nor1.nand_spare, 0xff, NAND_PAGES * NAND_SPARE_SIZE);
        memory_region_init_io(&s->aliases[7], OBJECT(s), &nand_cache_ops,
                              &s->nor1, "q66.nand-cache-cached", 64 * MiB);
        memory_region_init_io(&s->aliases[8], OBJECT(s), &nand_cache_ops,
                              &s->nor1, "q66.nand-cache-uncached", 64 * MiB);
        s->nor1.nand_resource = q66_open_backing("nand-resource",
                                     NAND_DATA_SIZE + NAND_DATA_OOB_SIZE);
        s->nor1.nand_data = q66_open_backing("nand-data",
                                     NAND_DATA_SIZE + NAND_DATA_OOB_SIZE);
        if (!s->nor1.nand_resource) {
            q66_copy_raw_image(machine->initrd_filename,
                              memory_region_get_ram_ptr(&s->resource),
                              NAND_DATA_SIZE);
        }
        for (i = 0; i < 2; i++) {
            BlockBackend *blk = i ? s->nor1.nand_data : s->nor1.nand_resource;
            if (blk) {
                q66_read_backing(blk, 0, NAND_DATA_SIZE,
                          memory_region_get_ram_ptr(&s->resource) +
                          i * NAND_DATA_SIZE);
                q66_read_backing(blk, NAND_DATA_SIZE, NAND_DATA_OOB_SIZE,
                          s->nor1.nand_spare + i * NAND_DATA_OOB_SIZE);
            }
        }
    }

    object_initialize_child(OBJECT(machine), "arm", &s->arm, TYPE_ARMV7M);
    qdev_prop_set_string(DEVICE(&s->arm), "cpu-type", machine->cpu_type);
    qdev_prop_set_uint32(DEVICE(&s->arm), "num-irq", 128);
    object_property_set_link(OBJECT(&s->arm), "memory", OBJECT(memory),
                             &error_fatal);
    clock = clock_new(OBJECT(machine), "core-clock");
    clock_set_hz(clock, 208000000);
    qdev_connect_clock_in(DEVICE(&s->arm), "cpuclk", clock);
    sysbus_realize(SYS_BUS_DEVICE(&s->arm), &error_fatal);
    object_initialize_child(OBJECT(machine), "cp", &s->cp, TYPE_ARMV7M);
    qdev_prop_set_string(DEVICE(&s->cp), "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
    qdev_prop_set_uint32(DEVICE(&s->cp), "num-irq", 128);
    qdev_prop_set_bit(DEVICE(&s->cp), "start-powered-off", true);
    memory_region_init_alias(&s->cp_memory, OBJECT(machine), "q66.cp-memory",
                             memory, 0, memory_region_size(memory));
    object_property_set_link(OBJECT(&s->cp), "memory", OBJECT(&s->cp_memory),
                             &error_fatal);
    qdev_connect_clock_in(DEVICE(&s->cp), "cpuclk", clock);
    sysbus_realize(SYS_BUS_DEVICE(&s->cp), &error_fatal);
    /* BES hal_get_cpu_id() reads implementation-defined AFR0[1:0].
     * Stock CP code installs its private NVIC handlers only for core 1. */
    SET_IDREG(&s->cp.cpu->isar, ID_AFR0, 1);
    s->lcdc_frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      q66_lcdc_frame_done, s);
    s->panel_te_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_panel_te, s);
    s->touch.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_touch_raise, s);
    s->psc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_psc_power_cycle, s);
    s->intersys_tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      q66_intersys_tx_done, s);
    s->intersys_rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      q66_intersys_rx_event, s);
    s->gpu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_gpu_run, s);
    s->gpadc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_gpadc_sample, s);
    s->dma.machine = s;
    s->dma.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_dma_run, &s->dma);
    memory_region_init_io(&s->dma.region, OBJECT(s), &q66_dma_ops, &s->dma,
                          "q66.gpdma", 0x400);
    memory_region_add_subregion_overlap(memory, 0x40130000, &s->dma.region, 1);
    qemu_register_reset(q66_dma_reset, &s->dma);
    s->audma.machine = s;
    s->audma.audio = true;
    s->audma.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q66_dma_run, &s->audma);
    memory_region_init_io(&s->audma.region, OBJECT(s), &q66_dma_ops, &s->audma,
                          "q66.audma", 0x400);
    memory_region_add_subregion_overlap(memory, 0x40120000, &s->audma.region, 1);
    qemu_register_reset(q66_dma_reset, &s->audma);
    add_uart(s, 0, 17);
    add_uart(s, 1, 18);
    add_uart(s, 2, 26);
    /* BEST1503 exposes several SP804-compatible dual timers.  The AP's
     * TIMER0/TIMER1/TIMER2 aliases select these blocks at build time, while
     * the raw addresses are also used by early boot and the RTOS tick. */
    add_boot_timer(s, "mcu-timer0", 0x40002000, 6000000, 12, 13);
    /* Copied AP clock variable is 24 MHz; fast delay divides it by four. */
    add_boot_timer(s, "mcu-timer1", 0x40003000, 6000000, 27, 28);
    add_boot_timer(s, "mcu-timer2", 0x40004000, 6000000, 62, 62);
    add_boot_timer(s, "aon-timer0", 0x40084000, 16000, 45, 46);
    /* In this AP's active address map 0x40089000 is GPIO bank 1,
     * as confirmed by its GPIO bank table and ISR, not another timer. */
    qemu_register_reset(q66_boot_reset, s);
    /* Flash is seeded once at cold boot. ROM blobs would overwrite the
     * original updater's AP/resource writes on each software reset. */
    armv7m_load_kernel(s->arm.cpu, NULL,
                       0x2c0c0000, FLASH_SIZE - 0xc0000);
    qemu_register_reset(q66_board_reset, s);

    panel = qdev_new(TYPE_Q66_PANEL);
    object_property_add_child(OBJECT(machine), "diagnostic-panel", OBJECT(panel));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(panel), &error_fatal);
    s->panel = Q66_PANEL(panel);
    s->panel->machine = s;
    sysbus_mmio_map(SYS_BUS_DEVICE(panel), 0, 0x60000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(panel), 1, 0x61000000);
    if (s->hci_chardev) {
        Chardev *chr = qemu_chr_find(s->hci_chardev);
        if (!chr) {
            error_report("q66 HCI chardev '%s' does not exist", s->hci_chardev);
            exit(1);
        }
        qemu_chr_fe_init(&s->hci_chr, chr, &error_fatal);
        qemu_chr_fe_set_handlers(&s->hci_chr, q66_hci_can_read,
                                q66_hci_read, q66_hci_chr_event,
                                NULL, s, NULL, true);
    }
}

static void q66_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);
    mc->desc = "Xiaomi Smart Band 11 NFC q66nfc (experimental bring-up)";
    mc->init = q66_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_ram_size = PSRAM_SIZE;
    object_class_property_add_str(klass, "hci-chardev",
                                  q66_hci_get_chardev, q66_hci_set_chardev);
    object_class_property_add_str(klass, "ota-image",
                                  q66_get_ota_image, q66_set_ota_image);
    object_class_property_add_str(klass, "recovery-image",
                                  q66_get_recovery_image, q66_set_recovery_image);
    object_class_property_add_str(klass, "initial-bootmode",
                                  q66_get_initial_bootmode, q66_set_initial_bootmode);
    q66_sensor_properties(klass);
    q66_audio_properties(klass);
}

static const TypeInfo q66_type = {
    .name = TYPE_Q66_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Q66MachineState),
    .instance_init = q66_sensor_init,
    .class_init = q66_class_init,
};

static void q66_register_types(void)
{
    type_register_static(&panel_type);
    type_register_static(&q66_type);
}
type_init(q66_register_types);
