// Standard VGA driver code
//
// Copyright (C) 2009-2024  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2001-2008 the LGPL VGABios developers Team
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "biosvar.h" // GET_GLOBAL
#include "farptr.h" // SET_FARVAR
#include "stdvga.h" // stdvga_setup
#include "string.h" // memset_far
#include "vgabios.h" // struct vgamode_s
#include "x86.h" // outb


/****************************************************************
 * Attribute control
 ****************************************************************/

// Emulate CGA background setting via VGA palette index registers
void
stdvga_set_cga_background_color(u8 color)
{
    // Set the background color (via palette index 0)
    u8 v1 = color & 0x0f;
    if (v1 & 0x08)
        v1 += 0x08;
    stdvga_attr_write(0x00, v1);

    // Dim/brighten foreground (see pal_cga[] in stdvgamodes.c)
    int i;
    for (i = 1; i < 4; i++)
        stdvga_attr_mask(i, 0x10, color & 0x10);
}

// Emulate CGA palette setting by altering VGA palette index registers
void
stdvga_set_cga_palette(u8 palid)
{
    // Switch foreground colors (see pal_cga[] in stdvgamodes.c)
    int i;
    for (i = 1; i < 4; i++)
        stdvga_attr_mask(i, 0x01, palid & 0x01);
}

// Set the VGA palette index register for the "overscan" area
void
stdvga_set_overscan_border_color(u8 color)
{
    stdvga_attr_write(0x11, color);
}

// Get the VGA palette index register for the "overscan" area
u8
stdvga_get_overscan_border_color(void)
{
    return stdvga_attr_read(0x11);
}

// Set the VGA palette index registers
void
stdvga_set_all_palette_reg(u16 seg, u8 *data_far)
{
    // Set palette indexes (offset into DAC colors)
    int i;
    for (i = 0; i < 0x10; i++) {
        stdvga_attr_write(i, GET_FARVAR(seg, *data_far));
        data_far++;
    }
    // Set "overscan" palette index (offset into DAC colors)
    stdvga_attr_write(0x11, GET_FARVAR(seg, *data_far));
}

// Get the VGA palette index registers
void
stdvga_get_all_palette_reg(u16 seg, u8 *data_far)
{
    int i;
    for (i = 0; i < 0x10; i++) {
        SET_FARVAR(seg, *data_far, stdvga_attr_read(i));
        data_far++;
    }
    SET_FARVAR(seg, *data_far, stdvga_attr_read(0x11));
}

// Set blinking mode (when enabled, palette index bit 0x08 indicates blinking)
void
stdvga_set_palette_blinking(u8 enable_blink)
{
    stdvga_attr_mask(0x10, 0x08, (enable_blink & 0x01) << 3);
}

// Select 4-bit or 6-bit palette indexes (for "page" switching of colors)
void
stdvga_set_palette_pagesize(u8 pal_pagesize)
{
    stdvga_attr_mask(0x10, 0x80, pal_pagesize << 7);
}

// Set palette index offset (enables color switching via "pages")
void
stdvga_set_palette_page(u8 pal_page)
{
    // Check if using 4-bit or 6-bit "palette index pages"
    u8 val = stdvga_attr_read(0x10);
    if (!(val & 0x80))
        pal_page <<= 2;
    // select page
    pal_page &= 0x0f;
    stdvga_attr_write(0x14, pal_page);
}

// Report current palette index pagesize and current page
void
stdvga_get_palette_page(u8 *pal_pagesize, u8 *pal_page)
{
    u8 val1 = stdvga_attr_read(0x10) >> 7;
    u8 val2 = stdvga_attr_read(0x14) & 0x0f;
    if (!(val1 & 0x01))
        val2 >>= 2;
    *pal_pagesize = val1;
    *pal_page = val2;
}


/****************************************************************
 * DAC control
 ****************************************************************/

// Store dac colors into memory in 3-byte rgb format
void
stdvga_dac_read_many(u16 seg, u8 *data_far, u8 start, int count)
{
    while (count) {
        struct vbe_palette_entry rgb = stdvga_dac_read(start);
        SET_FARVAR(seg, *data_far, rgb.red);
        data_far++;
        SET_FARVAR(seg, *data_far, rgb.green);
        data_far++;
        SET_FARVAR(seg, *data_far, rgb.blue);
        data_far++;
        start++;
        count--;
    }
}

// Load dac colors from memory in 3-byte rgb format
void
stdvga_dac_write_many(u16 seg, u8 *data_far, u8 start, int count)
{
    while (count) {
        u8 r = GET_FARVAR(seg, *data_far);
        data_far++;
        u8 g = GET_FARVAR(seg, *data_far);
        data_far++;
        u8 b = GET_FARVAR(seg, *data_far);
        data_far++;
        struct vbe_palette_entry rgb = { .red=r, .green=g, .blue=b };
        stdvga_dac_write(start, rgb);
        start++;
        count--;
    }
}

// Convert all loaded colors to shades of gray
void
stdvga_perform_gray_scale_summing(u16 start, u16 count)
{
    stdvga_attrindex_write(0x00);
    int i;
    for (i = start; i < start+count; i++) {
        struct vbe_palette_entry rgb = stdvga_dac_read(i);

        // intensity = ( 0.3 * Red ) + ( 0.59 * Green ) + ( 0.11 * Blue )
        u16 intensity = ((77 * rgb.red + 151 * rgb.green
                          + 28 * rgb.blue) + 0x80) >> 8;
        if (intensity > 0x3f)
            intensity = 0x3f;
        rgb.red = rgb.green = rgb.blue = intensity;

        stdvga_dac_write(i, rgb);
    }
    stdvga_attrindex_write(0x20);
}


/****************************************************************
 * Memory control
 ****************************************************************/

// Enable reads and writes to the given "plane" when in planar4 mode.
void
stdvga_planar4_plane(int plane)
{
    if (plane < 0) {
        // Return to default mode (read plane0, write all planes)
        stdvga_sequ_write(0x02, 0x0f);
        stdvga_grdc_write(0x04, 0);
    } else {
        stdvga_sequ_write(0x02, 1<<plane);
        stdvga_grdc_write(0x04, plane);
    }
}


/****************************************************************
 * Font loading
 ****************************************************************/

// Set the video memory location of the start of character fonts
void
stdvga_set_font_location(u8 spec)
{
    stdvga_sequ_write(0x03, spec);
}

static void
get_font_access(void)
{
    stdvga_sequ_write(0x00, 0x01);
    stdvga_sequ_write(0x02, 0x04);
    stdvga_sequ_write(0x04, 0x07);
    stdvga_sequ_write(0x00, 0x03);
    stdvga_grdc_write(0x04, 0x02);
    stdvga_grdc_write(0x05, 0x00);
    stdvga_grdc_write(0x06, 0x04);
}

static void
release_font_access(void)
{
    stdvga_sequ_write(0x00, 0x01);
    stdvga_sequ_write(0x02, 0x03);
    stdvga_sequ_write(0x04, 0x03);
    stdvga_sequ_write(0x00, 0x03);
    u16 v = (stdvga_misc_read() & 0x01) ? 0x0e : 0x0a;
    stdvga_grdc_write(0x06, v);
    stdvga_grdc_write(0x04, 0x00);
    stdvga_grdc_write(0x05, 0x10);
}

void
stdvga_load_font(u16 seg, void *src_far, u16 count
                 , u16 start, u8 destflags, u8 fontsize)
{
    get_font_access();
    u16 blockaddr = ((destflags & 0x03) << 14) + ((destflags & 0x04) << 11);
    void *dest_far = (void*)(blockaddr + start*32);
    u16 i;
    for (i = 0; i < count; i++)
        memcpy_far(SEG_GRAPH, dest_far + i*32
                   , seg, src_far + i*fontsize, fontsize);
    release_font_access();
}


/****************************************************************
 * CRTC registers
 ****************************************************************/

// Return the IO port used to access the CRTC register
u16
stdvga_get_crtc(void)
{
    if (stdvga_misc_read() & 1)
        return VGAREG_VGA_CRTC_ADDRESS;
    return VGAREG_MDA_CRTC_ADDRESS;
}

// Ratio between system visible framebuffer ram and the actual videoram used.
int
stdvga_vram_ratio(struct vgamode_s *vmode_g)
{
    switch (GET_GLOBAL(vmode_g->memmodel)) {
    case MM_TEXT:
        return 2;
    case MM_CGA:
        return 4 / GET_GLOBAL(vmode_g->depth);
    case MM_PLANAR:
        return 4;
    default:
        return 1;
    }
}

// Set cursor shape (when in text mode)
void
stdvga_set_cursor_shape(u16 cursor_type)
{
    u16 crtc_addr = stdvga_get_crtc();
    stdvga_crtc_write(crtc_addr, 0x0a, cursor_type >> 8);
    stdvga_crtc_write(crtc_addr, 0x0b, cursor_type);
}

// Set the position of the text cursor (as offset into system framebuffer)
void
stdvga_set_cursor_pos(int address)
{
    u16 crtc_addr = stdvga_get_crtc();
    address /= 2;  // Assume we're in text mode.
    stdvga_crtc_write(crtc_addr, 0x0e, address >> 8);
    stdvga_crtc_write(crtc_addr, 0x0f, address);
}

// Set the character height (when in text mode)
void
stdvga_set_character_height(u8 lines)
{
    stdvga_crtc_mask(stdvga_get_crtc(), 0x09, 0x1f, lines - 1);
}

// Get vertical screen size (number of horizontal lines in the display)
u16
stdvga_get_vertical_size(void)
{
    u16 crtc_addr = stdvga_get_crtc();
    u16 vde = stdvga_crtc_read(crtc_addr, 0x12);
    u8 ovl = stdvga_crtc_read(crtc_addr, 0x07);
    vde += ((ovl & 0x02) << 7) + ((ovl & 0x40) << 3);
    return vde + 1;
}

// Set vertical screen size (number of horizontal lines in the display)
void
stdvga_set_vertical_size(int lines)
{
    u16 crtc_addr = stdvga_get_crtc();
    u16 vde = lines - 1;
    stdvga_crtc_write(crtc_addr, 0x12, vde);
    u8 ovl = ((vde >> 7) & 0x02) + ((vde >> 3) & 0x40);
    stdvga_crtc_mask(crtc_addr, 0x07, 0x42, ovl);
}

// Get offset into framebuffer accessible from real-mode 64K segment
int
stdvga_get_window(struct vgamode_s *curmode_g, int window)
{
    return -1;
}

// Set offset into framebuffer that is accessible from real-mode 64K
// segment (in units of VBE_win_granularity windows)
int
stdvga_set_window(struct vgamode_s *curmode_g, int window, int val)
{
    // Stdvga does not support changing window offset
    return -1;
}

// Minimum framebuffer bytes between each vertical line for given mode
int
stdvga_minimum_linelength(struct vgamode_s *vmode_g)
{
    return DIV_ROUND_UP(GET_GLOBAL(vmode_g->width) * vga_bpp(vmode_g), 8);
}

// Return number of framebuffer bytes between start of each vertical line
int
stdvga_get_linelength(struct vgamode_s *curmode_g)
{
    u8 val = stdvga_crtc_read(stdvga_get_crtc(), 0x13);
    return val * 8 / stdvga_vram_ratio(curmode_g);
}

// Set number of framebuffer bytes between start of each vertical line
int
stdvga_set_linelength(struct vgamode_s *curmode_g, int val)
{
    val = DIV_ROUND_UP(val * stdvga_vram_ratio(curmode_g), 8);
    stdvga_crtc_write(stdvga_get_crtc(), 0x13, val);
    return 0;
}

// Return framebuffer offset of first byte of displayed content
int
stdvga_get_displaystart(struct vgamode_s *curmode_g)
{
    u16 crtc_addr = stdvga_get_crtc();
    int addr = (stdvga_crtc_read(crtc_addr, 0x0c) << 8
                | stdvga_crtc_read(crtc_addr, 0x0d));
    return addr * 4 / stdvga_vram_ratio(curmode_g);
}

// Set framebuffer offset of first byte of displayed content
int
stdvga_set_displaystart(struct vgamode_s *curmode_g, int val)
{
    u16 crtc_addr = stdvga_get_crtc();
    val = val * stdvga_vram_ratio(curmode_g) / 4;
    stdvga_crtc_write(crtc_addr, 0x0c, val >> 8);
    stdvga_crtc_write(crtc_addr, 0x0d, val);
    return 0;
}

// Report if using 8bit per rgb (24bit total) or 6bit per rgb (18bit total)
int
stdvga_get_dacformat(struct vgamode_s *curmode_g)
{
    return -1;
}

// Set 8bit per rgb (24bit total) or 6bit per rgb (18bit total)
int
stdvga_set_dacformat(struct vgamode_s *curmode_g, int val)
{
    // Stdvga only supports 6bits for each color channel
    return -1;
}


/****************************************************************
 * Save/Restore state
 ****************************************************************/

struct saveVideoHardware {
    u8 sequ_index;
    u8 crtc_index;
    u8 grdc_index;
    u8 actl_index;
    u8 feature;
    u8 sequ_regs[4];
    u8 sequ0;
    u8 crtc_regs[25];
    u8 actl_regs[20];
    u8 grdc_regs[9];
    u16 crtc_addr;
    u8 plane_latch[4];
} PACKED;

static void
stdvga_save_hw_state(u16 seg, struct saveVideoHardware *info)
{
    u16 crtc_addr = stdvga_get_crtc();
    SET_FARVAR(seg, info->sequ_index, inb(VGAREG_SEQU_ADDRESS));
    SET_FARVAR(seg, info->crtc_index, inb(crtc_addr));
    SET_FARVAR(seg, info->grdc_index, inb(VGAREG_GRDC_ADDRESS));
    SET_FARVAR(seg, info->actl_index, stdvga_attrindex_read());
    SET_FARVAR(seg, info->feature, inb(VGAREG_READ_FEATURE_CTL));

    int i;
    for (i=0; i<4; i++)
        SET_FARVAR(seg, info->sequ_regs[i], stdvga_sequ_read(i+1));
    SET_FARVAR(seg, info->sequ0, stdvga_sequ_read(0));

    for (i=0; i<25; i++)
        SET_FARVAR(seg, info->crtc_regs[i], stdvga_crtc_read(crtc_addr, i));

    for (i=0; i<20; i++)
        SET_FARVAR(seg, info->actl_regs[i], stdvga_attr_read(i));

    for (i=0; i<9; i++)
        SET_FARVAR(seg, info->grdc_regs[i], stdvga_grdc_read(i));

    SET_FARVAR(seg, info->crtc_addr, crtc_addr);

    /* XXX: read plane latches */
    for (i=0; i<4; i++)
        SET_FARVAR(seg, info->plane_latch[i], 0);
}

static void
stdvga_restore_hw_state(u16 seg, struct saveVideoHardware *info)
{
    int i;
    for (i=0; i<4; i++)
        stdvga_sequ_write(i+1, GET_FARVAR(seg, info->sequ_regs[i]));
    stdvga_sequ_write(0x00, GET_FARVAR(seg, info->sequ0));

    // Disable CRTC write protection
    u16 crtc_addr = GET_FARVAR(seg, info->crtc_addr);
    stdvga_crtc_write(crtc_addr, 0x11, 0x00);
    // Set CRTC regs
    for (i=0; i<25; i++)
        if (i != 0x11)
            stdvga_crtc_write(crtc_addr, i, GET_FARVAR(seg, info->crtc_regs[i]));
    // select crtc base address
    stdvga_misc_mask(0x01, crtc_addr == VGAREG_VGA_CRTC_ADDRESS ? 0x01 : 0x00);

    // enable write protection if needed
    stdvga_crtc_write(crtc_addr, 0x11, GET_FARVAR(seg, info->crtc_regs[0x11]));

    // Set Attribute Ctl
    for (i=0; i<20; i++)
        stdvga_attr_write(i, GET_FARVAR(seg, info->actl_regs[i]));
    stdvga_attrindex_write(GET_FARVAR(seg, info->actl_index));

    for (i=0; i<9; i++)
        stdvga_grdc_write(i, GET_FARVAR(seg, info->grdc_regs[i]));

    outb(GET_FARVAR(seg, info->sequ_index), VGAREG_SEQU_ADDRESS);
    outb(GET_FARVAR(seg, info->crtc_index), crtc_addr);
    outb(GET_FARVAR(seg, info->grdc_index), VGAREG_GRDC_ADDRESS);
    outb(GET_FARVAR(seg, info->feature), crtc_addr - 0x4 + 0xa);
}

struct saveDACcolors {
    u8 rwmode;
    u8 peladdr;
    u8 pelmask;
    u8 dac[768];
    u8 color_select;
} PACKED;

static void
stdvga_save_dac_state(u16 seg, struct saveDACcolors *info)
{
    /* XXX: check this */
    SET_FARVAR(seg, info->rwmode, inb(VGAREG_DAC_STATE));
    SET_FARVAR(seg, info->peladdr, inb(VGAREG_DAC_WRITE_ADDRESS));
    SET_FARVAR(seg, info->pelmask, stdvga_pelmask_read());
    stdvga_dac_read_many(seg, info->dac, 0, 256);
    SET_FARVAR(seg, info->color_select, 0);
}

static void
stdvga_restore_dac_state(u16 seg, struct saveDACcolors *info)
{
    stdvga_pelmask_write(GET_FARVAR(seg, info->pelmask));
    stdvga_dac_write_many(seg, info->dac, 0, 256);
    outb(GET_FARVAR(seg, info->peladdr), VGAREG_DAC_WRITE_ADDRESS);
}

int
stdvga_save_restore(int cmd, u16 seg, void *data)
{
    void *pos = data;
    if (cmd & SR_HARDWARE) {
        if (cmd & SR_SAVE)
            stdvga_save_hw_state(seg, pos);
        if (cmd & SR_RESTORE)
            stdvga_restore_hw_state(seg, pos);
        pos += sizeof(struct saveVideoHardware);
    }
    pos += bda_save_restore(cmd, seg, pos);
    if (cmd & SR_DAC) {
        if (cmd & SR_SAVE)
            stdvga_save_dac_state(seg, pos);
        if (cmd & SR_RESTORE)
            stdvga_restore_dac_state(seg, pos);
        pos += sizeof(struct saveDACcolors);
    }
    return pos - data;
}


/****************************************************************
 * Misc
 ****************************************************************/

// Enable/disable system access to the video memory
void
stdvga_enable_video_addressing(u8 disable)
{
    u8 v = (disable & 1) ? 0x00 : 0x02;
    stdvga_misc_mask(0x02, v);
}

int
stdvga_setup(void)
{
    // switch to color mode and enable CPU access 480 lines
    stdvga_misc_write(0xc3);
    // more than 64k 3C4/04
    stdvga_sequ_write(0x04, 0x02);

    return 0;
}
