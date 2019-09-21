/*
 * video driver for framebuffer device
 * copyright (C) 2003 Joey Parrish <joey@nicewarrior.org>
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include <libswscale/swscale.h>

#include "drm_common.h"

#include "common/msg.h"
#include "osdep/timer.h"
#include "sub/osd.h"
#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "vo.h"

struct priv
{
    char *fb_dev_name; // such as /dev/fb0
    int fb_dev_fd; // handle for fb_dev_name
    uint8_t *frame_buffer; // mmap'd access to fbdev
    uint8_t *center; // where to begin writing our image (centered?)
    struct fb_fix_screeninfo fb_finfo; // fixed info
    struct fb_var_screeninfo fb_vinfo; // variable info
    struct fb_var_screeninfo fb_orig_vinfo; // variable info to restore later
    unsigned short fb_ored[256], fb_ogreen[256], fb_oblue[256];
    struct fb_cmap fb_oldcmap;
    int fb_cmap_changed; //  to restore map
    int fb_pixel_size; // 32:  4  24:  3  16:  2  15:  2
    size_t fb_size; // size of frame_buffer
    int fb_line_len; // length of one line in bytes
    uint8_t *next_frame; // for double buffering
    int in_width;
    int in_height;

    // Moved from the inside of fb_preinit function. TODO: remove this ugly shit
    int fb_preinit_done;
    int fb_err;

    struct mp_sws_context *sws;
};

static void set_bpp(struct fb_var_screeninfo *p, int bpp)
{
    p->bits_per_pixel = (bpp + 1) & ~1;
    p->red.msb_right = p->green.msb_right = p->blue.msb_right = p->transp.msb_right = 0;
    p->transp.offset = p->transp.length = 0;
    p->blue.offset = 0;
    switch (bpp) {
    case 32:
        p->transp.offset = 24;
        p->transp.length = 8;
        /* Fallthrough, rest matches 24 bit */
    case 24:
        p->red.offset = 16;
        p->red.length = 8;
        p->green.offset = 8;
        p->green.length = 8;
        p->blue.length = 8;
        break;
    case 16:
        p->red.offset = 11;
        p->green.length = 6;
        p->red.length = 5;
        p->green.offset = 5;
        p->blue.length = 5;
        break;
    case 15:
        p->red.offset = 10;
        p->green.length = 5;
        p->red.length = 5;
        p->green.offset = 5;
        p->blue.length = 5;
        break;
    case 12:
        p->red.offset   = 8;
        p->green.length = 4;
        p->red.length   = 4;
        p->green.offset = 4;
        p->blue.length  = 4;
        break;
    }
}

static struct fb_cmap *make_directcolor_cmap(struct vo *vo, struct fb_var_screeninfo *var)
{
//    struct priv *p = vo->priv;

    int i, cols, rcols, gcols, bcols;
    uint16_t *red, *green, *blue;
    struct fb_cmap *cmap;

    rcols = 1 << var->red.length;
    gcols = 1 << var->green.length;
    bcols = 1 << var->blue.length;

    /* Make our palette the length of the deepest color */
    cols = FFMAX3(rcols, gcols, bcols);

    red = malloc(3 * cols * sizeof(red[0]));
    if(!red) {
        MP_ERR(vo, "Can't allocate red palette with %d entries.\n", cols);
        return NULL;
    }
    green = red   + cols;
    blue  = green + cols;
    for (i = 0; i < cols; i++) {
        red[i]   = (65535/(rcols-1)) * i;
        green[i] = (65535/(gcols-1)) * i;
        blue[i]  = (65535/(bcols-1)) * i;
    }

    cmap = malloc(sizeof(struct fb_cmap));
    if(!cmap) {
        MP_ERR(vo, "Can't allocate color map\n");
        free(red);
        return NULL;
    }
    cmap->start = 0;
    cmap->transp = 0;
    cmap->len = cols;
    cmap->red = red;
    cmap->blue = blue;
    cmap->green = green;
    cmap->transp = NULL;

    return cmap;
}

static int fb_preinit(struct vo *vo, int reset)
{
    struct priv *p = vo->priv;

    if (reset) {
        p->fb_preinit_done = 0;
        return 0;
    }

    if (p->fb_preinit_done)
        return p->fb_err;
    p->fb_preinit_done = 1;

    if (!p->fb_dev_name && !(p->fb_dev_name = getenv("FRAMEBUFFER"))) {
        p->fb_dev_name = talloc_strdup(p, "/dev/fb0");
    }

    MP_ERR(vo, "Using device %s\n", p->fb_dev_name);

    if ((p->fb_dev_fd = open(p->fb_dev_name, O_RDWR)) == -1) {
        MP_ERR(vo, "Can't open %s: %s\n", p->fb_dev_name, mp_strerror(errno));
        goto err_out;
    }
    if (ioctl(p->fb_dev_fd, FBIOGET_VSCREENINFO, &p->fb_vinfo)) {
        MP_ERR(vo, "Can't get VSCREENINFO: %s\n", mp_strerror(errno));
        goto err_out;
    }
    p->fb_orig_vinfo = p->fb_vinfo;

    p->fb_err = 0;
    return 0;

err_out:
    if (p->fb_dev_fd >= 0) close(p->fb_dev_fd);
    p->fb_dev_fd = -1;
    p->fb_err = -1;
    return -1;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->sws = mp_sws_alloc(vo);

    p->fb_oldcmap = (struct fb_cmap){ 0, 256, p->fb_ored, p->fb_ogreen, p->fb_oblue };

    p->fb_err = -1;
    p->fb_preinit_done = 0;

    p->fb_dev_name = "/dev/fb0"; // TODO: have option. don't hardcode
    return fb_preinit(vo, 0);
}

/* static int config(uint32_t width, uint32_t height, uint32_t d_width, */
/*                   uint32_t d_height, uint32_t flags, char *title, */
/*                   uint32_t format) */
static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;

    vo->dwidth = p->fb_vinfo.xres;
    vo->dheight = p->fb.vinfo.yres;

    /********************************************************************************/

    // Oldness compatibility.
    // TODO: do something that actually works. This has obv. changed. What we
    //       have now is like being passed d_width and d_height only;
    uint32_t d_width = params->w;
    uint32_t d_height = params->h;
    uint32_t flags = params->hw_flags;
    uint32_t format = params->imgfmt; // TODO: don't do this ugly cast

    struct fb_cmap *cmap;
    int fs = flags & VOFLAG_FULLSCREEN;
    int x_offset = vo_dx + (d_width  - width ) / 2;
    int y_offset = vo_dy + (d_height - height) / 2;
    x_offset = av_clip(x_offset, 0, p->fb_vinfo.xres - width);
    y_offset = av_clip(y_offset, 0, p->fb_vinfo.yres - height);

    p->in_width = width;
    p->in_height = height;

    if (p->fb_vinfo.xres < in_width || p->fb_vinfo.yres < in_height) {
        MP_ERR(vo, "Screensize is smaller than video size (%dx%d < %dx%d)\n",
               p->fb_vinfo.xres, p->fb_vinfo.yres, p->in_width, p->in_height);
        return 1;
    }

    /* draw_alpha_p = vo_get_draw_alpha(format); */
    p->fb_pixel_size = pixel_stride(format);

    if (vo_config_count == 0) {
        if (ioctl(p->fb_dev_fd, FBIOGET_FSCREENINFO, &p->fb_finfo)) {
            MP_ERR(vo, "Can't get FSCREENINFO: %s\n", mp_strerror(errno));
            return 1;
        }

        if (p->fb_finfo.type != FB_TYPE_PACKED_PIXELS) {
            MP_ERR(vo, "type %d not supported\n", p->fb_finfo.type);
            return 1;
        }

        switch (p->fb_finfo.visual) {
        case FB_VISUAL_TRUECOLOR:
            break;
        case FB_VISUAL_DIRECTCOLOR:
            MP_ERR(vo, "creating cmap for directcolor\n");
            if (ioctl(p->fb_dev_fd, FBIOGETCMAP, &p->fb_oldcmap)) {
                MP_ERR(vo, "can't get cmap: %s\n", mp_strerror(errno));
                return 1;
            }
            if (!(cmap = make_directcolor_cmap(vo, &p->fb_vinfo)))
                return 1;
            if (ioctl(p->fb_dev_fd, FBIOPUTCMAP, cmap)) {
                MP_ERR(vo, "can't put cmap: %s\n", mp_strerror(errno));
                free(cmap->red);
                free(cmap);
                return 1;
            }
            p->fb_cmap_changed = 1;
            free(cmap->red);
            free(cmap);
            break;
        default:
            MP_ERR(vo, "visual: %d not yet supported\n", p->fb_finfo.visual);
            break;
        }

        p->fb_size = p->fb_finfo.smem_len;
        p->fb_line_len = p->fb_finfo.line_length;
        if ((p->frame_buffer = (uint8_t *) mmap(0, p->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, p->fb_dev_fd, 0)) == (uint8_t *) -1) {
            MP_ERR(vo, "Can't mmap %s: %s\n", p->fb_dev_name, mp_strerror(errno));
            return 1;
        }
    }

    p->center = p->frame_buffer +
        x_offset * p->fb_pixel_size +
        y_offset * p->fb_line_len;

#ifndef USE_CONVERT2FB
    if (!(p->next_frame = realloc(p->next_frame, p->in_width * p->in_height * p->fb_pixel_size))) {
        MP_ERR(vo, "Can't malloc next_frame: %s\n", mp_strerror(errno));
        return 1;
    }
#endif
    if (fs) {
        int len = p->fb_line_len * p->fb_vinfo.yres;
        int i;
        switch (format) {
        case IMGFMT_YUY2:
            for (i = 0; i < len - 3;) {
                p->frame_buffer[i++] = 0;
                p->frame_buffer[i++] = 0x80;
                p->frame_buffer[i++] = 0;
                p->frame_buffer[i++] = 0x80;
            }
            break;
        case IMGFMT_UYVY:
            for (i = 0; i < len - 3;) {
                p->frame_buffer[i++] = 0x80;
                p->frame_buffer[i++] = 0;
                p->frame_buffer[i++] = 0x80;
                p->frame_buffer[i++] = 0;
            }
            break;
        default:
            memset(p->frame_buffer, 0, len);
        }
    }

    return 0;
}

static int query_format(struct vo *vo, uint32_t format)
{
    // open the device, etc.
    if (fb_preinit(vo, 0)) return 0;
    if (IMGFMT_IS_BGR(format)) {
        int bpp;
        int fb_target_bpp = format & 0xff;
        set_bpp(&p->fb_vinfo, fb_target_bpp);
        p->fb_vinfo.xres_virtual = p->fb_vinfo.xres;
        p->fb_vinfo.yres_virtual = p->fb_vinfo.yres;
        p->fb_vinfo.nonstd = 0;
        if (ioctl(p->fb_dev_fd, FBIOPUT_VSCREENINFO, &fb_vinfo))
            // Needed for Intel framebuffer with 32 bpp
            p->fb_vinfo.transp.length = p->fb_vinfo.transp.offset = 0;
        if (ioctl(p->fb_dev_fd, FBIOPUT_VSCREENINFO, &p->fb_vinfo)) {
            MP_ERR(vo, "Can't put VSCREENINFO: %s\n", mp_strerror(errno));
            return 0;
        }
        bpp = p->fb_vinfo.bits_per_pixel;
        if (bpp == 16)
            bpp = p->fb_vinfo.red.length + p->fb_vinfo.green.length + p->fb_vinfo.blue.length;
        if (bpp == fb_target_bpp)
            return VFCAP_CSP_SUPPORTED|VFCAP_CSP_SUPPORTED_BY_HW|VFCAP_ACCEPT_STRIDE;
    }
    return 0;
}

#if 0
static void draw_alpha(int x0, int y0, int w, int h, unsigned char *src,
                       unsigned char *srca, int stride)
{
    unsigned char *dst;
    int dstride;

#ifdef USE_CONVERT2FB
    dst = p->center + (p->fb_line_len * y0) + (x0 * p->fb_pixel_size);
    dstride = p->fb_line_len;
#else
    dst = p->next_frame + (p->in_width * y0 + x0) * p->fb_pixel_size;
    dstride = p->in_width * p->fb_pixel_size;
#endif
    (*draw_alpha_p)(w, h, src, srca, stride, dst, dstride);
}

static void draw_osd(void)
{
    vo_draw_text(in_width, in_height, draw_alpha);
}

static int draw_slice(uint8_t *src[], int stride[], int w, int h, int x, int y)
{
    uint8_t *in = src[0];
#ifdef USE_CONVERT2FB
    uint8_t *dest = p->center + (p->fb_line_len * y) + (x * p->fb_pixel_size);
    int next = p->fb_line_len;
#else
    uint8_t *dest = p->next_frame + (p->in_width * y + x) * p->fb_pixel_size;
    int next = in_width * fb_pixel_size;
#endif

    memcpy_pic(dest, in, w * p->fb_pixel_size, h, next, stride[0]);
    return 0;
}
#endif

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;

#ifndef USE_CONVERT2FB
    int out_offset = 0, in_offset = 0;

    memcpy_pic(p->center + out_offset, p->next_frame + in_offset,
               p->in_width * fb_pixel_size, p->in_height,
               p->fb_line_len, p->in_width * p->fb_pixel_size);
#endif
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (p->fb_cmap_changed) {
        if (ioctl(p->fb_dev_fd, FBIOPUTCMAP, &p->fb_oldcmap))
            MP_ERR(vo, "Can't restore original cmap\n");
        p->fb_cmap_changed = 0;
    }
    free(p->next_frame);
    if (p->fb_dev_fd >= 0) {
        if (ioctl(p->fb_dev_fd, FBIOPUT_VSCREENINFO, &p->fb_orig_vinfo))
            MP_ERR("Can't reset original fb_var_screeninfo: %s\n", mp_strerror(errno));
        close(p->fb_dev_fd);
        fb_dev_fd = -1;
    }
    if (p->frame_buffer) {
        munmap(p->frame_buffer, p->fb_size);
    }
    p->next_frame = p->frame_buffer = NULL;
    fb_preinit(vo, 1); // so that later calls to preinit don't fail
}

#if 0
static int control(uint32_t request, void *data)
{
    switch (request) {
    case VOCTRL_QUERY_FORMAT:
        return query_format(*((uint32_t*)data));
    case VOCTRL_UPDATE_SCREENINFO:
        vo_screenwidth  = fb_vinfo.xres;
        vo_screenheight = fb_vinfo.yres;
        aspect_save_screenres(vo_screenwidth, vo_screenheight);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}
#endif

const struct vo_driver video_out_fbdev = {
    .name = "fbdev",
    .description = "fbdev",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    /* .control = control, */
    /* .draw_frame = draw_frame, */
    .draw_image = draw_image,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
