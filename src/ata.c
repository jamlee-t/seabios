// Low level ATA disk access
//
// Copyright (C) 2008,2009  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2002  MandrakeSoft S.A.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "types.h" // u8
#include "ioport.h" // inb
#include "util.h" // dprintf
#include "cmos.h" // inb_cmos
#include "pic.h" // enable_hwirq
#include "biosvar.h" // GET_EBDA
#include "pci.h" // foreachpci
#include "pci_ids.h" // PCI_CLASS_STORAGE_OTHER
#include "pci_regs.h" // PCI_INTERRUPT_LINE
#include "boot.h" // add_bcv_hd
#include "disk.h" // struct ata_s
#include "ata.h" // ATA_CB_STAT

#define IDE_TIMEOUT 32000 //32 seconds max for IDE ops

struct ata_channel_s ATA_channels[CONFIG_MAX_ATA_INTERFACES] VAR16VISIBLE;


/****************************************************************
 * Helper functions
 ****************************************************************/

// Wait for the specified ide state
static inline int
await_ide(u8 mask, u8 flags, u16 base, u16 timeout)
{
    u64 end = calc_future_tsc(timeout);
    for (;;) {
        u8 status = inb(base+ATA_CB_STAT);
        if ((status & mask) == flags)
            return status;
        if (check_time(end)) {
            dprintf(1, "IDE time out\n");
            return -1;
        }
        yield();
    }
}

// Wait for the device to be not-busy.
static int
await_not_bsy(u16 base)
{
    return await_ide(ATA_CB_STAT_BSY, 0, base, IDE_TIMEOUT);
}

// Wait for the device to be ready.
static int
await_rdy(u16 base)
{
    return await_ide(ATA_CB_STAT_RDY, ATA_CB_STAT_RDY, base, IDE_TIMEOUT);
}

// Wait for ide state - pauses for one ata cycle first.
static inline int
pause_await_not_bsy(u16 iobase1, u16 iobase2)
{
    // Wait one PIO transfer cycle.
    inb(iobase2 + ATA_CB_ASTAT);

    return await_not_bsy(iobase1);
}

// Wait for ide state - pause for 400ns first.
static inline int
ndelay_await_not_bsy(u16 iobase1)
{
    ndelay(400);
    return await_not_bsy(iobase1);
}

// Reset a drive
static void
ata_reset(struct drive_s *drive_g)
{
    u8 ataid = GET_GLOBAL(drive_g->cntl_id);
    u8 channel = ataid / 2;
    u8 slave = ataid % 2;
    u16 iobase1 = GET_GLOBAL(ATA_channels[channel].iobase1);
    u16 iobase2 = GET_GLOBAL(ATA_channels[channel].iobase2);

    dprintf(6, "ata_reset drive=%p\n", drive_g);
    // Pulse SRST
    outb(ATA_CB_DC_HD15 | ATA_CB_DC_NIEN | ATA_CB_DC_SRST, iobase2+ATA_CB_DC);
    udelay(5);
    outb(ATA_CB_DC_HD15 | ATA_CB_DC_NIEN, iobase2+ATA_CB_DC);
    msleep(2);

    // wait for device to become not busy.
    int status = await_not_bsy(iobase1);
    if (status < 0)
        goto done;
    if (slave) {
        // Change device.
        u64 end = calc_future_tsc(IDE_TIMEOUT);
        for (;;) {
            outb(ATA_CB_DH_DEV1, iobase1 + ATA_CB_DH);
            status = ndelay_await_not_bsy(iobase1);
            if (status < 0)
                goto done;
            if (inb(iobase1 + ATA_CB_DH) == ATA_CB_DH_DEV1)
                break;
            // Change drive request failed to take effect - retry.
            if (check_time(end)) {
                dprintf(1, "ata_reset slave time out\n");
                goto done;
            }
        }
    } else {
        // QEMU doesn't reset dh on reset, so set it explicitly.
        outb(ATA_CB_DH_DEV0, iobase1 + ATA_CB_DH);
    }

    // On a user-reset request, wait for RDY if it is an ATA device.
    u8 type=GET_GLOBAL(drive_g->type);
    if (type == DTYPE_ATA)
        status = await_rdy(iobase1);

done:
    // Enable interrupts
    outb(ATA_CB_DC_HD15, iobase2+ATA_CB_DC);

    dprintf(6, "ata_reset exit status=%x\n", status);
}

static int
isready(struct drive_s *drive_g)
{
    // Read the status from controller
    u8 ataid = GET_GLOBAL(drive_g->cntl_id);
    u8 channel = ataid / 2;
    u16 iobase1 = GET_GLOBAL(ATA_channels[channel].iobase1);
    u8 status = inb(iobase1 + ATA_CB_STAT);
    if ((status & (ATA_CB_STAT_BSY|ATA_CB_STAT_RDY)) == ATA_CB_STAT_RDY)
        return DISK_RET_SUCCESS;
    return DISK_RET_ENOTREADY;
}

static int
process_ata_misc_op(struct disk_op_s *op)
{
    if (!CONFIG_ATA)
        return 0;

    switch (op->command) {
    case CMD_RESET:
        ata_reset(op->drive_g);
        return DISK_RET_SUCCESS;
    case CMD_ISREADY:
        return isready(op->drive_g);
    case CMD_FORMAT:
    case CMD_VERIFY:
    case CMD_SEEK:
        return DISK_RET_SUCCESS;
    default:
        op->count = 0;
        return DISK_RET_EPARAM;
    }
}


/****************************************************************
 * ATA send command
 ****************************************************************/

struct ata_pio_command {
    u8 feature;
    u8 sector_count;
    u8 lba_low;
    u8 lba_mid;
    u8 lba_high;
    u8 device;
    u8 command;

    u8 sector_count2;
    u8 lba_low2;
    u8 lba_mid2;
    u8 lba_high2;
};

// Send an ata command to the drive.
static int
send_cmd(struct drive_s *drive_g, struct ata_pio_command *cmd)
{
    u8 ataid = GET_GLOBAL(drive_g->cntl_id);
    u8 channel = ataid / 2;
    u8 slave = ataid % 2;
    u16 iobase1 = GET_GLOBAL(ATA_channels[channel].iobase1);

    // Select device
    int status = await_not_bsy(iobase1);
    if (status < 0)
        return status;
    u8 newdh = ((cmd->device & ~ATA_CB_DH_DEV1)
                | (slave ? ATA_CB_DH_DEV1 : ATA_CB_DH_DEV0));
    u8 olddh = inb(iobase1 + ATA_CB_DH);
    outb(newdh, iobase1 + ATA_CB_DH);
    if ((olddh ^ newdh) & (1<<4)) {
        // Was a device change - wait for device to become not busy.
        status = ndelay_await_not_bsy(iobase1);
        if (status < 0)
            return status;
    }

    if (cmd->command & 0x04) {
        outb(0x00, iobase1 + ATA_CB_FR);
        outb(cmd->sector_count2, iobase1 + ATA_CB_SC);
        outb(cmd->lba_low2, iobase1 + ATA_CB_SN);
        outb(cmd->lba_mid2, iobase1 + ATA_CB_CL);
        outb(cmd->lba_high2, iobase1 + ATA_CB_CH);
    }
    outb(cmd->feature, iobase1 + ATA_CB_FR);
    outb(cmd->sector_count, iobase1 + ATA_CB_SC);
    outb(cmd->lba_low, iobase1 + ATA_CB_SN);
    outb(cmd->lba_mid, iobase1 + ATA_CB_CL);
    outb(cmd->lba_high, iobase1 + ATA_CB_CH);
    outb(cmd->command, iobase1 + ATA_CB_CMD);

    status = ndelay_await_not_bsy(iobase1);
    if (status < 0)
        return status;

    if (status & ATA_CB_STAT_ERR) {
        dprintf(6, "send_cmd : read error (status=%02x err=%02x)\n"
                , status, inb(iobase1 + ATA_CB_ERR));
        return -4;
    }
    if (!(status & ATA_CB_STAT_DRQ)) {
        dprintf(6, "send_cmd : DRQ not set (status %02x)\n", status);
        return -5;
    }

    return 0;
}


/****************************************************************
 * ATA transfers
 ****************************************************************/

// Transfer 'op->count' blocks (of 'blocksize' bytes) to/from drive
// 'op->drive_g'.
static int
ata_transfer(struct disk_op_s *op, int iswrite, int blocksize)
{
    dprintf(16, "ata_transfer id=%p write=%d count=%d bs=%d buf=%p\n"
            , op->drive_g, iswrite, op->count, blocksize, op->buf_fl);

    u8 ataid = GET_GLOBAL(op->drive_g->cntl_id);
    u8 channel = ataid / 2;
    u16 iobase1 = GET_GLOBAL(ATA_channels[channel].iobase1);
    u16 iobase2 = GET_GLOBAL(ATA_channels[channel].iobase2);
    int count = op->count;
    void *buf_fl = op->buf_fl;
    int status;
    for (;;) {
        if (iswrite) {
            // Write data to controller
            dprintf(16, "Write sector id=%p dest=%p\n", op->drive_g, buf_fl);
            if (CONFIG_ATA_PIO32)
                outsl_fl(iobase1, buf_fl, blocksize / 4);
            else
                outsw_fl(iobase1, buf_fl, blocksize / 2);
        } else {
            // Read data from controller
            dprintf(16, "Read sector id=%p dest=%p\n", op->drive_g, buf_fl);
            if (CONFIG_ATA_PIO32)
                insl_fl(iobase1, buf_fl, blocksize / 4);
            else
                insw_fl(iobase1, buf_fl, blocksize / 2);
        }
        buf_fl += blocksize;

        status = pause_await_not_bsy(iobase1, iobase2);
        if (status < 0) {
            // Error
            op->count -= count;
            return status;
        }

        count--;
        if (!count)
            break;
        status &= (ATA_CB_STAT_BSY | ATA_CB_STAT_DRQ | ATA_CB_STAT_ERR);
        if (status != ATA_CB_STAT_DRQ) {
            dprintf(6, "ata_transfer : more sectors left (status %02x)\n"
                    , status);
            op->count -= count;
            return -6;
        }
    }

    status &= (ATA_CB_STAT_BSY | ATA_CB_STAT_DF | ATA_CB_STAT_DRQ
               | ATA_CB_STAT_ERR);
    if (!iswrite)
        status &= ~ATA_CB_STAT_DF;
    if (status != 0) {
        dprintf(6, "ata_transfer : no sectors left (status %02x)\n", status);
        return -7;
    }

    return 0;
}


/****************************************************************
 * ATA hard drive functions
 ****************************************************************/

// Read/write count blocks from a harddrive.
static int
ata_cmd_data(struct disk_op_s *op, int iswrite, int command)
{
    u8 ataid = GET_GLOBAL(op->drive_g->cntl_id);
    u8 channel = ataid / 2;
    u16 iobase2 = GET_GLOBAL(ATA_channels[channel].iobase2);
    u64 lba = op->lba;

    struct ata_pio_command cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.command = command;
    if (op->count >= (1<<8) || lba + op->count >= (1<<28)) {
        cmd.sector_count2 = op->count >> 8;
        cmd.lba_low2 = lba >> 24;
        cmd.lba_mid2 = lba >> 32;
        cmd.lba_high2 = lba >> 40;

        cmd.command |= 0x04;
        lba &= 0xffffff;
    }

    cmd.feature = 0;
    cmd.sector_count = op->count;
    cmd.lba_low = lba;
    cmd.lba_mid = lba >> 8;
    cmd.lba_high = lba >> 16;
    cmd.device = ((lba >> 24) & 0xf) | ATA_CB_DH_LBA;

    // Disable interrupts
    outb(ATA_CB_DC_HD15 | ATA_CB_DC_NIEN, iobase2 + ATA_CB_DC);

    int ret = send_cmd(op->drive_g, &cmd);
    if (ret)
        goto fail;
    ret = ata_transfer(op, iswrite, DISK_SECTOR_SIZE);

fail:
    // Enable interrupts
    outb(ATA_CB_DC_HD15, iobase2+ATA_CB_DC);
    return ret;
}

int
process_ata_op(struct disk_op_s *op)
{
    if (!CONFIG_ATA)
        return 0;

    int ret;
    switch (op->command) {
    case CMD_READ:
        ret = ata_cmd_data(op, 0, ATA_CMD_READ_SECTORS);
        break;
    case CMD_WRITE:
        ret = ata_cmd_data(op, 1, ATA_CMD_WRITE_SECTORS);
        break;
    default:
        return process_ata_misc_op(op);
    }
    if (ret)
        return DISK_RET_EBADTRACK;
    return DISK_RET_SUCCESS;
}


/****************************************************************
 * ATAPI functions
 ****************************************************************/

// Low-level atapi command transmit function.
static int
atapi_cmd_data(struct disk_op_s *op, u8 *cmdbuf, u8 cmdlen, u16 blocksize)
{
    u8 ataid = GET_GLOBAL(op->drive_g->cntl_id);
    u8 channel = ataid / 2;
    u16 iobase1 = GET_GLOBAL(ATA_channels[channel].iobase1);
    u16 iobase2 = GET_GLOBAL(ATA_channels[channel].iobase2);

    struct ata_pio_command cmd;
    cmd.sector_count = 0;
    cmd.feature = 0;
    cmd.lba_low = 0;
    cmd.lba_mid = blocksize;
    cmd.lba_high = blocksize >> 8;
    cmd.device = 0;
    cmd.command = ATA_CMD_PACKET;

    // Disable interrupts
    outb(ATA_CB_DC_HD15 | ATA_CB_DC_NIEN, iobase2 + ATA_CB_DC);

    int ret = send_cmd(op->drive_g, &cmd);
    if (ret)
        goto fail;

    // Send command to device
    outsw_fl(iobase1, MAKE_FLATPTR(GET_SEG(SS), cmdbuf), cmdlen / 2);

    int status = pause_await_not_bsy(iobase1, iobase2);
    if (status < 0) {
        ret = status;
        goto fail;
    }

    if (status & ATA_CB_STAT_ERR) {
        u8 err = inb(iobase1 + ATA_CB_ERR);
        // skip "Not Ready"
        if (err != 0x20)
            dprintf(6, "send_atapi_cmd : read error (status=%02x err=%02x)\n"
                    , status, err);
        ret = -2;
        goto fail;
    }
    if (!(status & ATA_CB_STAT_DRQ)) {
        dprintf(6, "send_atapi_cmd : DRQ not set (status %02x)\n", status);
        ret = -3;
        goto fail;
    }

    ret = ata_transfer(op, 0, blocksize);

fail:
    // Enable interrupts
    outb(ATA_CB_DC_HD15, iobase2+ATA_CB_DC);
    return ret;
}

// Read sectors from the cdrom.
int
cdrom_read(struct disk_op_s *op)
{
    u8 atacmd[12];
    memset(atacmd, 0, sizeof(atacmd));
    atacmd[0]=0x28;                         // READ command
    atacmd[7]=(op->count & 0xff00) >> 8;    // Sectors
    atacmd[8]=(op->count & 0x00ff);
    atacmd[2]=(op->lba & 0xff000000) >> 24; // LBA
    atacmd[3]=(op->lba & 0x00ff0000) >> 16;
    atacmd[4]=(op->lba & 0x0000ff00) >> 8;
    atacmd[5]=(op->lba & 0x000000ff);

    return atapi_cmd_data(op, atacmd, sizeof(atacmd), CDROM_SECTOR_SIZE);
}

int
process_atapi_op(struct disk_op_s *op)
{
    int ret;
    switch (op->command) {
    case CMD_READ:
        ret = cdrom_read(op);
        break;
    case CMD_FORMAT:
    case CMD_WRITE:
        return DISK_RET_EWRITEPROTECT;
    default:
        return process_ata_misc_op(op);
    }
    if (ret)
        return DISK_RET_EBADTRACK;
    return DISK_RET_SUCCESS;
}

// Send a simple atapi command to a drive.
int
ata_cmd_packet(struct drive_s *drive_g, u8 *cmdbuf, u8 cmdlen
               , u32 length, void *buf_fl)
{
    struct disk_op_s dop;
    memset(&dop, 0, sizeof(dop));
    dop.drive_g = drive_g;
    dop.count = 1;
    dop.buf_fl = buf_fl;

    return atapi_cmd_data(&dop, cmdbuf, cmdlen, length);
}


/****************************************************************
 * ATA detect and init
 ****************************************************************/

// Extract the ATA/ATAPI version info.
static int
extract_version(u16 *buffer)
{
    // Extract ATA/ATAPI version.
    u16 ataversion = buffer[80];
    u8 version;
    for (version=15; version>0; version--)
        if (ataversion & (1<<version))
            break;
    return version;
}

// Extract common information from IDENTIFY commands.
static void
extract_identify(struct drive_s *drive_g, u16 *buffer)
{
    dprintf(3, "Identify w0=%x w2=%x\n", buffer[0], buffer[2]);

    // Read model name
    char *model = drive_g->model;
    int maxsize = ARRAY_SIZE(drive_g->model);
    int i;
    for (i=0; i<maxsize/2; i++) {
        u16 v = buffer[27+i];
        model[i*2] = v >> 8;
        model[i*2+1] = v & 0xff;
    }
    model[maxsize-1] = 0x00;

    // Trim trailing spaces from model name.
    for (i=maxsize-2; i>0 && model[i] == 0x20; i--)
        model[i] = 0x00;

    // Common flags.
    SET_GLOBAL(drive_g->removable, (buffer[0] & 0x80) ? 1 : 0);
    SET_GLOBAL(drive_g->cntl_info, extract_version(buffer));
}

void
describe_atapi(struct drive_s *drive_g)
{
    u8 ataid = drive_g->cntl_id;
    u8 channel = ataid / 2;
    u8 slave = ataid % 2;
    u8 version = drive_g->cntl_info;
    int iscd = drive_g->floppy_type;
    printf("ata%d-%d: %s ATAPI-%d %s", channel, slave
           , drive_g->model, version
           , (iscd ? "CD-Rom/DVD-Rom" : "Device"));
}

static struct drive_s *
init_drive_atapi(struct drive_s *dummy, u16 *buffer)
{
    // Send an IDENTIFY_DEVICE_PACKET command to device
    memset(buffer, 0, DISK_SECTOR_SIZE);
    struct disk_op_s dop;
    memset(&dop, 0, sizeof(dop));
    dop.drive_g = dummy;
    dop.count = 1;
    dop.lba = 1;
    dop.buf_fl = MAKE_FLATPTR(GET_SEG(SS), buffer);
    int ret = ata_cmd_data(&dop, 0, ATA_CMD_IDENTIFY_DEVICE_PACKET);
    if (ret)
        return NULL;

    // Success - setup as ATAPI.
    struct drive_s *drive_g = allocDrive();
    if (! drive_g)
        return NULL;
    SET_GLOBAL(drive_g->cntl_id, dummy->cntl_id);
    extract_identify(drive_g, buffer);
    SET_GLOBAL(drive_g->type, DTYPE_ATAPI);
    SET_GLOBAL(drive_g->blksize, CDROM_SECTOR_SIZE);
    SET_GLOBAL(drive_g->sectors, (u64)-1);
    u8 iscd = ((buffer[0] >> 8) & 0x1f) == 0x05;
    SET_GLOBAL(drive_g->floppy_type, iscd);

    // fill cdidmap
    if (iscd)
        map_cd_drive(drive_g);

    return drive_g;
}

void
describe_ata(struct drive_s *drive_g)
{
    u8 ataid = drive_g->cntl_id;
    u8 channel = ataid / 2;
    u8 slave = ataid % 2;
    u64 sectors = drive_g->sectors;
    u8 version = drive_g->cntl_info;
    char *model = drive_g->model;
    printf("ata%d-%d: %s ATA-%d Hard-Disk", channel, slave, model, version);
    u64 sizeinmb = sectors >> 11;
    if (sizeinmb < (1 << 16))
        printf(" (%u MiBytes)", (u32)sizeinmb);
    else
        printf(" (%u GiBytes)", (u32)(sizeinmb >> 10));
}

static struct drive_s *
init_drive_ata(struct drive_s *dummy, u16 *buffer)
{
    // Send an IDENTIFY_DEVICE command to device
    memset(buffer, 0, DISK_SECTOR_SIZE);
    struct disk_op_s dop;
    memset(&dop, 0, sizeof(dop));
    dop.drive_g = dummy;
    dop.count = 1;
    dop.lba = 1;
    dop.buf_fl = MAKE_FLATPTR(GET_SEG(SS), buffer);
    int ret = ata_cmd_data(&dop, 0, ATA_CMD_IDENTIFY_DEVICE);
    if (ret)
        return NULL;

    // Success - setup as ATA.
    struct drive_s *drive_g = allocDrive();
    if (! drive_g)
        return NULL;
    SET_GLOBAL(drive_g->cntl_id, dummy->cntl_id);
    extract_identify(drive_g, buffer);
    SET_GLOBAL(drive_g->type, DTYPE_ATA);
    SET_GLOBAL(drive_g->blksize, DISK_SECTOR_SIZE);

    SET_GLOBAL(drive_g->pchs.cylinders, buffer[1]);
    SET_GLOBAL(drive_g->pchs.heads, buffer[3]);
    SET_GLOBAL(drive_g->pchs.spt, buffer[6]);

    u64 sectors;
    if (buffer[83] & (1 << 10)) // word 83 - lba48 support
        sectors = *(u64*)&buffer[100]; // word 100-103
    else
        sectors = *(u32*)&buffer[60]; // word 60 and word 61
    SET_GLOBAL(drive_g->sectors, sectors);

    // Setup disk geometry translation.
    setup_translation(drive_g);

    // Register with bcv system.
    add_bcv_internal(drive_g);

    return drive_g;
}

static u64 SpinupEnd;

static int
powerup_await_non_bsy(u16 base)
{
    u8 orstatus = 0;
    u8 status;
    for (;;) {
        status = inb(base+ATA_CB_STAT);
        if (!(status & ATA_CB_STAT_BSY))
            break;
        orstatus |= status;
        if (orstatus == 0xff) {
            dprintf(1, "powerup IDE floating\n");
            return orstatus;
        }
        if (check_time(SpinupEnd)) {
            dprintf(1, "powerup IDE time out\n");
            return -1;
        }
        yield();
    }
    dprintf(6, "powerup iobase=%x st=%x\n", base, status);
    return status;
}

static void
ata_detect(void *data)
{
    struct ata_channel_s *atachannel = data;
    int startid = (atachannel - ATA_channels) * 2;
    struct drive_s dummy;
    memset(&dummy, 0, sizeof(dummy));
    // Device detection
    int ataid, last_reset_ataid=-1;
    for (ataid=startid; ataid<startid+2; ataid++) {
        u8 channel = ataid / 2;
        u8 slave = ataid % 2;

        u16 iobase1 = GET_GLOBAL(ATA_channels[channel].iobase1);
        if (!iobase1)
            break;

        // Wait for not-bsy.
        int status = powerup_await_non_bsy(iobase1);
        if (status < 0)
            continue;
        u8 newdh = slave ? ATA_CB_DH_DEV1 : ATA_CB_DH_DEV0;
        outb(newdh, iobase1+ATA_CB_DH);
        ndelay(400);
        status = powerup_await_non_bsy(iobase1);
        if (status < 0)
            continue;

        // Check if ioport registers look valid.
        outb(newdh, iobase1+ATA_CB_DH);
        u8 dh = inb(iobase1+ATA_CB_DH);
        outb(0x55, iobase1+ATA_CB_SC);
        outb(0xaa, iobase1+ATA_CB_SN);
        u8 sc = inb(iobase1+ATA_CB_SC);
        u8 sn = inb(iobase1+ATA_CB_SN);
        dprintf(6, "ata_detect ataid=%d sc=%x sn=%x dh=%x\n"
                , ataid, sc, sn, dh);
        if (sc != 0x55 || sn != 0xaa || dh != newdh)
            continue;

        // Prepare new drive.
        dummy.cntl_id = ataid;

        // reset the channel
        if (slave && ataid == last_reset_ataid + 1) {
            // The drive was just reset - no need to reset it again.
        } else {
            ata_reset(&dummy);
            last_reset_ataid = ataid;
        }

        // check for ATAPI
        u16 buffer[256];
        struct drive_s *drive_g = init_drive_atapi(&dummy, buffer);
        if (!drive_g) {
            // Didn't find an ATAPI drive - look for ATA drive.
            u8 st = inb(iobase1+ATA_CB_STAT);
            if (!st)
                // Status not set - can't be a valid drive.
                continue;

            // Wait for RDY.
            int ret = await_rdy(iobase1);
            if (ret < 0)
                continue;

            // check for ATA.
            drive_g = init_drive_ata(&dummy, buffer);
            if (!drive_g)
                // No ATA drive found
                continue;
        }

        u16 resetresult = buffer[93];
        dprintf(6, "ata_detect resetresult=%04x\n", resetresult);
        if (!slave && (resetresult & 0xdf61) == 0x4041)
            // resetresult looks valid and device 0 is responding to
            // device 1 requests - device 1 must not be present - skip
            // detection.
            ataid++;
    }
}

static void
init_controller(struct ata_channel_s *atachannel
                , int bdf, int irq, u32 port1, u32 port2)
{
    SET_GLOBAL(atachannel->irq, irq);
    SET_GLOBAL(atachannel->pci_bdf, bdf);
    SET_GLOBAL(atachannel->iobase1, port1);
    SET_GLOBAL(atachannel->iobase2, port2);
    dprintf(1, "ATA controller %d at %x/%x (irq %d dev %x)\n"
            , atachannel - ATA_channels, port1, port2, irq, bdf);
    run_thread(ata_detect, atachannel);
}

#define IRQ_ATA1 14
#define IRQ_ATA2 15

static void
ata_init()
{
    // Scan PCI bus for ATA adapters
    int count=0, pcicount=0;
    int bdf, max;
    foreachpci(bdf, max) {
        pcicount++;
        if (pci_config_readw(bdf, PCI_CLASS_DEVICE) != PCI_CLASS_STORAGE_IDE)
            continue;
        if (count >= ARRAY_SIZE(ATA_channels))
            break;

        u8 pciirq = pci_config_readb(bdf, PCI_INTERRUPT_LINE);
        u8 prog_if = pci_config_readb(bdf, PCI_CLASS_PROG);
        u32 port1, port2, irq;
        if (prog_if & 1) {
            port1 = pci_config_readl(bdf, PCI_BASE_ADDRESS_0) & ~3;
            port2 = pci_config_readl(bdf, PCI_BASE_ADDRESS_1) & ~3;
            irq = pciirq;
        } else {
            port1 = PORT_ATA1_CMD_BASE;
            port2 = PORT_ATA1_CTRL_BASE;
            irq = IRQ_ATA1;
        }
        init_controller(&ATA_channels[count], bdf, irq, port1, port2);
        count++;

        if (prog_if & 4) {
            port1 = pci_config_readl(bdf, PCI_BASE_ADDRESS_2) & ~3;
            port2 = pci_config_readl(bdf, PCI_BASE_ADDRESS_3) & ~3;
            irq = pciirq;
        } else {
            port1 = PORT_ATA2_CMD_BASE;
            port2 = PORT_ATA2_CTRL_BASE;
            irq = IRQ_ATA2;
        }
        init_controller(&ATA_channels[count], bdf, irq, port1, port2);
        count++;
    }

    if (!CONFIG_COREBOOT && !pcicount && ARRAY_SIZE(ATA_channels) >= 2) {
        // No PCI devices found - probably a QEMU "-M isapc" machine.
        // Try using ISA ports for ATA controllers.
        init_controller(&ATA_channels[0]
                        , -1, IRQ_ATA1, PORT_ATA1_CMD_BASE, PORT_ATA1_CTRL_BASE);
        init_controller(&ATA_channels[1]
                        , -1, IRQ_ATA2, PORT_ATA2_CMD_BASE, PORT_ATA2_CTRL_BASE);
    }
}

void
ata_setup()
{
    if (!CONFIG_ATA)
        return;

    dprintf(3, "init hard drives\n");

    SpinupEnd = calc_future_tsc(IDE_TIMEOUT);
    ata_init();

    SET_BDA(disk_control_byte, 0xc0);

    enable_hwirq(14, entry_76);
}
