#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "miband_display_bridge.h"

struct MiBandDisplayBridge {
    QemuConsole *console;
    unsigned width, height;
    unsigned rotation, host_width, host_height;
    uint8_t *framebuffer;
    size_t framebuffer_size;
    bool frame_valid;
    bool valid;
};

static void display_invalidate(void *opaque) { }

static bool display_update(void *opaque)
{
    MiBandDisplayBridge *bridge = opaque;
    if (bridge->valid) {
        qemu_console_update(bridge->console, 0, 0, bridge->host_width, bridge->host_height);
    }
    return true;
}

static const GraphicHwOps display_ops = {
    .invalidate = display_invalidate,
    .gfx_update = display_update,
};

MiBandDisplayBridge *miband_display_bridge_new(unsigned width, unsigned height)
{
    MiBandDisplayBridge *bridge = g_new0(MiBandDisplayBridge, 1);
    bridge->width = width;
    bridge->height = height;
    bridge->host_width = width;
    bridge->host_height = height;
    bridge->console = qemu_graphic_console_create(NULL, 0, &display_ops, bridge);
    qemu_console_resize(bridge->console, width, height);
    return bridge;
}

void miband_display_bridge_set_rotation(MiBandDisplayBridge *bridge, unsigned rotation)
{
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        fprintf(stderr, "invalid display rotation %u\n", rotation);
        return;
    }
    bridge->rotation = rotation;
    bridge->host_width = (rotation == 90 || rotation == 270) ? bridge->height : bridge->width;
    bridge->host_height = (rotation == 90 || rotation == 270) ? bridge->width : bridge->height;
    bridge->valid = false;
    qemu_console_resize(bridge->console, bridge->host_width, bridge->host_height);
    DisplaySurface *surface = qemu_console_surface(bridge->console);
    if (!surface) {
        return;
    }
    if (bridge->frame_valid && bridge->framebuffer) {
        miband_display_bridge_present(bridge, bridge->framebuffer,
                                     bridge->width, bridge->height);
    } else {
        memset(surface_data(surface), 0,
               (size_t)surface_stride(surface) * bridge->host_height);
        qemu_console_update(bridge->console, 0, 0,
                       bridge->host_width, bridge->host_height);
    }
}

void miband_display_bridge_present(void *opaque, const uint8_t *pixels,
                                    unsigned width, unsigned height)
{
    MiBandDisplayBridge *bridge = opaque;
    DisplaySurface *surface = qemu_console_surface(bridge->console);
    if (!surface || width != bridge->width || height != bridge->height) {
        return;
    }
    if (!pixels) {
        bridge->valid = false;
        bridge->frame_valid = false;
        if (bridge->framebuffer) {
            memset(bridge->framebuffer, 0, bridge->framebuffer_size);
        }
        memset(surface_data(surface), 0,
               (size_t)surface_stride(surface) * bridge->host_height);
    } else {
        if (surface_bits_per_pixel(surface) != 32) {
            return;
        }
        size_t frame_size = (size_t)width * height * 4;
        if (bridge->framebuffer_size != frame_size) {
            bridge->framebuffer = g_realloc(bridge->framebuffer, frame_size);
            bridge->framebuffer_size = frame_size;
        }
        if (pixels != bridge->framebuffer) {
            memcpy(bridge->framebuffer, pixels, frame_size);
        }
        bridge->frame_valid = true;
        uint8_t *dst = surface_data(surface);
        unsigned dstride = surface_stride(surface);
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                unsigned dx = x, dy = y;
                if (bridge->rotation == 90) {
                    dx = height - 1 - y; dy = x;
                } else if (bridge->rotation == 180) {
                    dx = width - 1 - x; dy = height - 1 - y;
                } else if (bridge->rotation == 270) {
                    dx = y; dy = width - 1 - x;
                }
                memcpy(dst + (size_t)dy * dstride + (size_t)dx * 4,
                       pixels + (size_t)y * width * 4 + (size_t)x * 4, 4);
            }
        }
        bridge->valid = true;
    }
    qemu_console_update(bridge->console, 0, 0, bridge->host_width, bridge->host_height);
}
