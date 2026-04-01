/*
 * SonicMesa - ssX XAA Implementation
 * Direct-to-metal 2D acceleration for 5800X3D
 * 
 * Copyright 2026 Collin Beyer, AzuriteShift, and ssX Contributors
 * SPDX-License-Identifier: ssX
 */

#include "ssx_xaa_bridge.h"
#include "ssx_xaa_io_uring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Global ring context - initialized once
 */
static struct ssx_xaa_ring_ctx *g_ring_ctx = NULL;
static uint32_t g_seq_num = 0;

/*
 * XAA Init - Initialize the ssX XAA bridge
 */
void ssx_xaa_init(struct ssx_xaa_info *info, int ring_fd)
{
    if (!info) return;
    
    memset(info, 0, sizeof(*info));
    
    info->ring_fd = ring_fd;
    
    /* Initialize acceleration vectors with direct GPU pushes */
    info->SetupForSolidFill = (void (*)(int, int, uint32_t))ssx_xaa_setup_solid_fill;
    info->SubsequentSolidFillRect = ssx_xaa_subsequent_solid_fill_rect;
    info->SetupForScreenToScreenCopy = (void (*)(int, int, int, uint32_t))ssx_xaa_setup_copy;
    info->SubsequentScreenToScreenCopy = ssx_xaa_subsequent_copy;
    info->SetupForCPUToScreen = (void (*)(int, int, int, uint32_t, int))ssx_xaa_setup_cpu_to_screen;
    info->SubsequentCPUToScreen = ssx_xaa_subsequent_cpu_to_screen;
    info->SetupForLine = (void (*)(int, int, uint32_t))ssx_xaa_setup_line;
    info->SubsequentBresenhamLine = ssx_xaa_subsequent_bresenham_line;
    info->SetupForTriangle = (void (*)(int, uint32_t))ssx_xaa_setup_triangle;
    info->SubsequentTriangle = ssx_xaa_subsequent_triangle;
    info->Sync = ssx_xaa_sync;
    info->Flush = ssx_xaa_flush;
    info->CPUFallback = ssx_xaa_cpu_fallback;
    
    /* Set default flags */
    info->acceleration_flags = 0x7FFFFFFF; /* All acceleration enabled */
    
    /* Performance counters */
    info->ops_queued = 0;
    info->ops_completed = 0;
    info->cache_hits = 0;
    info->cache_misses = 0;
}

/*
 * XAA Destroy - Cleanup
 */
void ssx_xaa_destroy(struct ssx_xaa_info *info)
{
    if (!info) return;
    
    /* Sync before destroy */
    ssx_xaa_sync(info);
    
    memset(info, 0, sizeof(*info));
}

/*
 * Initialize global io_uring ring
 */
int ssx_xaa_ring_init_global(void)
{
    if (g_ring_ctx) return 0;
    
    g_ring_ctx = ssx_xaa_ring_init(SSX_XAA_RING_SIZE, SSX_XAA_CMD_QUEUE_SZ);
    if (!g_ring_ctx) {
        fprintf(stderr, "ssX XAA: Failed to initialize io_uring ring: %d\n", errno);
        return -ENOMEM;
    }
    
    return 0;
}

/*
 * Shutdown global io_uring ring
 */
void ssx_xaa_ring_shutdown_global(void)
{
    if (g_ring_ctx) {
        ssx_xaa_ring_shutdown(g_ring_ctx);
        g_ring_ctx = NULL;
    }
}

/*
 * Get global ring context
 */
struct ssx_xaa_ring_ctx *ssx_xaa_get_ring_ctx(void)
{
    return g_ring_ctx;
}

/*
 * Default command submission via io_uring
 */
int ssx_xaa_submit_command(struct ssx_xaa_info *info, union ssx_xaa_command *cmd)
{
    if (!cmd || !g_ring_ctx) return -EINVAL;
    
    /* Set header fields */
    cmd->header.magic = 0x58414100; /* "XAA\0" */
    cmd->header.seq_num = __sync_fetch_and_add(&g_seq_num, 1);
    cmd->header.timestamp = 0; /* Will be set by ring */
    
    /* Submit via io_uring */
    int ret = ssx_xaa_ring_submit(g_ring_ctx, cmd, cmd->header.cmd_size, 0);
    
    if (ret == 0) {
        info->ops_queued++;
    }
    
    return ret;
}

/*
 * Sync - Wait for all pending operations
 */
int ssx_xaa_sync(struct ssx_xaa_info *info)
{
    if (!g_ring_ctx) return 0;
    
    int ret = ssx_xaa_ring_drain(g_ring_ctx);
    
    if (ret == 0) {
        info->ops_completed = info->ops_queued;
    }
    
    return ret;
}

/*
 * Flush - Flush pending commands
 */
void ssx_xaa_flush(struct ssx_xaa_info *info)
{
    if (!g_ring_ctx) return;
    
    /* Drain with timeout */
    ssx_xaa_ring_drain(g_ring_ctx);
}

/*
 * Setup for solid fill - Direct GPU state setup
 */
void ssx_xaa_setup_solid_fill(struct ssx_xaa_info *info, int color, int rop, uint32_t planemask)
{
    if (!info) return;
    
    info->current_rop = (uint32_t)rop;
    info->current_fg_color = (uint32_t)color;
    info->current_planemask = planemask;
}

/*
 * Subsequent solid fill rect - Push directly to GPU command buffer
 */
void ssx_xaa_subsequent_solid_fill_rect(struct ssx_xaa_info *info, int x, int y, int w, int h)
{
    union ssx_xaa_command cmd = {0};
    
    cmd.header.magic = 0x58414100;
    cmd.header.cmd_type = SSX_XAA_CMD_SUBSEQUENT_SOLID_FILL_RECT;
    cmd.header.cmd_size = sizeof(cmd.fill_rect) + sizeof(cmd.header);
    cmd.fill_rect.x = (int16_t)x;
    cmd.fill_rect.y = (int16_t)y;
    cmd.fill_rect.width = (uint16_t)w;
    cmd.fill_rect.height = (uint16_t)h;
    cmd.fill_rect.color = info->current_fg_color;
    
    ssx_xaa_submit_command(info, &cmd);
}

/*
 * Setup for screen-to-screen copy
 */
void ssx_xaa_setup_copy(struct ssx_xaa_info *info, int xdir, int ydir, int rop, uint32_t planemask)
{
    if (!info) return;
    
    info->current_rop = (uint32_t)rop;
    info->current_planemask = planemask;
}

/*
 * Subsequent screen-to-screen copy
 */
void ssx_xaa_subsequent_copy(struct ssx_xaa_info *info, int srcX, int srcY, int dstX, int dstY, int w, int h)
{
    union ssx_xaa_command cmd = {0};
    
    cmd.header.magic = 0x58414100;
    cmd.header.cmd_type = SSX_XAA_CMD_SUBSEQUENT_SCREEN_TO_SCREEN_COPY;
    cmd.header.cmd_size = sizeof(cmd.copy) + sizeof(cmd.header);
    cmd.copy.src_x = srcX;
    cmd.copy.src_y = srcY;
    cmd.copy.dst_x = dstX;
    cmd.copy.dst_y = dstY;
    cmd.copy.width = w;
    cmd.copy.height = h;
    
    /* Determine direction for overlap handling */
    if (dstX < srcX) cmd.copy.direction |= SSX_XAA_COPY_RIGHT;
    else if (dstX > srcX) cmd.copy.direction |= SSX_XAA_COPY_LEFT;
    if (dstY < srcY) cmd.copy.direction |= SSX_XAA_COPY_DOWN;
    else if (dstY > srcY) cmd.copy.direction |= SSX_XAA_COPY_UP;
    
    ssx_xaa_submit_command(info, &cmd);
}

/*
 * Setup for CPU-to-screen upload
 */
void ssx_xaa_setup_cpu_to_screen(struct ssx_xaa_info *info, int x, int y, int rop, uint32_t planemask, int bpp)
{
    if (!info) return;
    
    info->current_rop = (uint32_t)rop;
    info->current_planemask = planemask;
    info->scratch_pixel_depth = bpp;
}

/*
 * Subsequent CPU-to-screen upload
 */
void ssx_xaa_subsequent_cpu_to_screen(struct ssx_xaa_info *info, int x, int y, int w, int h, int pitch, int bpp, const uint8_t *pixels)
{
    union ssx_xaa_command cmd = {0};
    
    cmd.header.magic = 0x58414100;
    cmd.header.cmd_type = SSX_XAA_CMD_SUBSEQUENT_CPU_TO_SCREEN;
    cmd.header.cmd_size = sizeof(cmd.upload) + sizeof(cmd.header);
    cmd.upload.dst_x = x;
    cmd.upload.dst_y = y;
    cmd.upload.width = w;
    cmd.upload.height = h;
    cmd.upload.pitch = pitch;
    cmd.upload.bpp = bpp;
    cmd.upload.pixels = pixels;
    
    ssx_xaa_submit_command(info, &cmd);
}

/*
 * Setup for line drawing
 */
void ssx_xaa_setup_line(struct ssx_xaa_info *info, int color, int rop, uint32_t planemask)
{
    if (!info) return;
    
    info->current_rop = (uint32_t)rop;
    info->current_fg_color = (uint32_t)color;
    info->current_planemask = planemask;
}

/*
 * Subsequent Bresenham line
 */
void ssx_xaa_subsequent_bresenham_line(struct ssx_xaa_info *info, int x1, int y1, int e, int dx, int dy, int octant)
{
    union ssx_xaa_command cmd = {0};
    
    cmd.header.magic = 0x58414100;
    cmd.header.cmd_type = SSX_XAA_CMD_SUBSEQUENT_LINE;
    cmd.header.cmd_size = sizeof(cmd.line) + sizeof(cmd.header);
    cmd.line.color = info->current_fg_color;
    cmd.line.x1 = (int16_t)x1;
    cmd.line.y1 = (int16_t)y1;
    cmd.line.x2 = (int16_t)(x1 + dx);
    cmd.line.y2 = (int16_t)(y1 + dy);
    
    ssx_xaa_submit_command(info, &cmd);
}

/*
 * Setup for triangle
 */
void ssx_xaa_setup_triangle(struct ssx_xaa_info *info, int rop, uint32_t planemask)
{
    if (!info) return;
    
    info->current_rop = (uint32_t)rop;
    info->current_planemask = planemask;
}

/*
 * Subsequent triangle
 */
void ssx_xaa_subsequent_triangle(struct ssx_xaa_info *info, int x1, int y1, int x2, int y2, int x3, int y3)
{
    union ssx_xaa_command cmd = {0};
    
    cmd.header.magic = 0x58414100;
    cmd.header.cmd_type = SSX_XAA_CMD_SUBSEQUENT_TRIANGLE;
    cmd.header.cmd_size = sizeof(cmd.triangle) + sizeof(cmd.header);
    cmd.triangle.color = info->current_fg_color;
    cmd.triangle.tri.x1 = (int16_t)x1;
    cmd.triangle.tri.y1 = (int16_t)y1;
    cmd.triangle.tri.x2 = (int16_t)x2;
    cmd.triangle.tri.y2 = (int16_t)y2;
    cmd.triangle.tri.x3 = (int16_t)x3;
    cmd.triangle.tri.y3 = (int16_t)y3;
    
    ssx_xaa_submit_command(info, &cmd);
}

/*
 * CPU Fallback - Handled entirely in L3 cache
 */
void ssx_xaa_cpu_fallback(struct ssx_xaa_info *info, int cmd, void *data, int x, int y, int w, int h)
{
    if (!info || !data) return;
    
    info->cache_misses++;
    
    switch (cmd) {
    case SSX_XAA_CMD_SUBSEQUENT_SOLID_FILL_RECT:
        ssx_xaa_cpu_fallback_solid_fill(info, x, y, w, h, info->current_fg_color);
        break;
    case SSX_XAA_CMD_SUBSEQUENT_SCREEN_TO_SCREEN_COPY:
        ssx_xaa_cpu_fallback_copy(info, x, y, x, y, w, h);
        break;
    default:
        fprintf(stderr, "ssX XAA: Unknown fallback cmd %d\n", cmd);
        break;
    }
    
    info->ops_completed++;
}

/*
 * CPU fallback solid fill - Optimized for L3 cache
 */
void ssx_xaa_cpu_fallback_solid_fill(struct ssx_xaa_info *info, int x, int y, int w, int h, int color)
{
    /* TODO: Implement optimized CPU fill using SIMD
     * This should use SSE/AVX for 16-32 pixels at a time
     * keeping all data in L3 cache for 5800X3D */
}

/*
 * CPU fallback copy
 */
void ssx_xaa_cpu_fallback_copy(struct ssx_xaa_info *info, int src_x, int src_y, int dst_x, int dst_y, int w, int h)
{
    /* TODO: Implement optimized CPU copy using SIMD */
}

/*
 * CPU fallback blit
 */
void ssx_xaa_cpu_fallback_blit(struct ssx_xaa_info *info, const ssx_xaa_bitmap *bmp, int x, int y)
{
    /* TODO: Implement CPU bitmap blit */
}
