#include <drivers/usb/usb_msc.h>

#include <drivers/usb/usb_core.h>

#include <drivers/block/bdev.h>

#include <hal/lock.h>

#include <lib/idr.h>

#include <mm/heap.h>

#include <lib/string.h>


#define USB_CLASS_MASS_STORAGE 0x08u

#define USB_MSC_SUBCLASS_SCSI_TRANSPARENT 0x06u
#define USB_MSC_PROTOCOL_BULK_ONLY 0x50u

#define USB_MSC_REQ_BULK_ONLY_RESET 0xFFu
#define USB_MSC_REQ_GET_MAX_LUN 0xFEu

#define USB_REQ_SET_FEATURE 3u

#define USB_FEATURE_ENDPOINT_HALT 0u


#define MSC_CBW_SIGNATURE 0x43425355u
#define MSC_CSW_SIGNATURE 0x53425355u

#define MSC_CSW_STATUS_OK 0u
#define MSC_CSW_STATUS_FAIL 1u
#define MSC_CSW_STATUS_PHASE_ERROR 2u


#define SCSI_CMD_TEST_UNIT_READY 0x00u
#define SCSI_CMD_REQUEST_SENSE 0x03u
#define SCSI_CMD_INQUIRY 0x12u
#define SCSI_CMD_START_STOP_UNIT 0x1Bu
#define SCSI_CMD_READ_CAPACITY_10 0x25u
#define SCSI_CMD_READ_10 0x28u
#define SCSI_CMD_WRITE_10 0x2Au


typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;
    uint8_t bCBWLUN;
    uint8_t bCBWCBLength;
    uint8_t CBWCB[16];
} msc_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t bCSWStatus;
} msc_csw_t;


typedef struct {
    usb_device_t* dev;

    uint8_t iface;

    uint8_t ep_in;
    uint16_t ep_in_mps;

    uint8_t ep_out;
    uint16_t ep_out_mps;

    uint8_t toggle_in;
    uint8_t toggle_out;

    uint8_t max_lun;

    mutex_t io_lock;

    uint8_t dead;

    block_device_t* bdev;
    char* bdev_name;
    int disk_id;
} usb_msc_dev_t;


static idr_t g_usb_msc_idr;
static int g_usb_msc_idr_init = 0;


static uint32_t be32_read(const uint8_t* p) {
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | (uint32_t)p[3];
}

static void be32_write(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v >> 0);
}

static void be16_write(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v >> 0);
}


static int usb_msc_control_class(
    usb_device_t* dev,
    uint8_t req,
    uint8_t dir_in,
    uint16_t value,
    uint16_t index,
    void* data,
    uint16_t length,
    uint32_t timeout_us
) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = (dir_in ? USB_REQ_TYPE_DIR_IN : USB_REQ_TYPE_DIR_OUT)
        | USB_REQ_TYPE_CLASS
        | USB_REQ_RECIP_INTERFACE;

    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = length;

    const int r = usb_device_control_xfer(dev, &setup, data, length, timeout_us);
    if (dir_in) {
        return r == (int)length;
    }

    return r >= 0;
}

static int usb_msc_clear_halt(usb_device_t* dev, uint8_t ep_addr) {
    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));

    setup.bmRequestType = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_ENDPOINT;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wValue = USB_FEATURE_ENDPOINT_HALT;
    setup.wIndex = ep_addr;
    setup.wLength = 0;

    return usb_device_control_xfer(dev, &setup, 0, 0, 1000000u) >= 0;
}

static int usb_msc_bulk_only_reset(usb_msc_dev_t* d) {
    if (!d || !d->dev) {
        return 0;
    }

    const int ok = usb_msc_control_class(
        d->dev,
        USB_MSC_REQ_BULK_ONLY_RESET,
        0,
        0,
        d->iface,
        0,
        0,
        1000000u
    );

    (void)usb_msc_clear_halt(d->dev, d->ep_in | USB_EP_DIR_IN);
    (void)usb_msc_clear_halt(d->dev, d->ep_out);

    d->toggle_in = 0;
    d->toggle_out = 0;

    return ok;
}

static uint8_t usb_msc_get_max_lun(usb_msc_dev_t* d) {
    if (!d || !d->dev) {
        return 0;
    }

    uint8_t max_lun = 0;

    const int ok = usb_msc_control_class(
        d->dev,
        USB_MSC_REQ_GET_MAX_LUN,
        1,
        0,
        d->iface,
        &max_lun,
        sizeof(max_lun),
        1000000u
    );

    if (!ok) {
        return 0;
    }

    return max_lun;
}


static int usb_msc_bulk_xfer(
    usb_msc_dev_t* d,
    uint8_t dir_in,
    void* data,
    uint32_t length,
    uint32_t timeout_us
) {
    if (!d || !d->dev) {
        return -1;
    }

    const uint8_t ep_num = dir_in ? d->ep_in : d->ep_out;
    const uint16_t mps = dir_in ? d->ep_in_mps : d->ep_out_mps;

    uint8_t* toggle = dir_in ? &d->toggle_in : &d->toggle_out;

    return usb_device_bulk_xfer(
        d->dev,
        ep_num,
        dir_in,
        mps,
        data,
        length,
        timeout_us,
        toggle
    );
}

static int usb_msc_bot_transport(
    usb_msc_dev_t* d,
    uint8_t lun,
    const uint8_t* cdb,
    uint8_t cdb_len,
    uint8_t dir_in,
    void* data,
    uint32_t data_len,
    uint8_t* out_scsi_status
) {
    if (!d || !cdb || cdb_len == 0 || cdb_len > 16) {
        return 0;
    }

    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));

    static uint32_t tag_seed = 1u;
    const uint32_t tag = __atomic_fetch_add(&tag_seed, 1u, __ATOMIC_RELAXED);

    cbw.dCBWSignature = MSC_CBW_SIGNATURE;
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir_in ? 0x80u : 0x00u;
    cbw.bCBWLUN = lun;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    const int r_cbw = usb_msc_bulk_xfer(d, 0, &cbw, sizeof(cbw), 2000000u);
    if (r_cbw != (int)sizeof(cbw)) {
        return 0;
    }

    int phase_ok = 1;

    if (data_len) {
        const int r_data = usb_msc_bulk_xfer(d, dir_in, data, data_len, 5000000u);
        if (r_data != (int)data_len) {
            phase_ok = 0;
        }
    }

    msc_csw_t csw;
    memset(&csw, 0, sizeof(csw));

    const int r_csw = usb_msc_bulk_xfer(d, 1, &csw, sizeof(csw), 2000000u);
    if (r_csw != (int)sizeof(csw)) {
        return 0;
    }

    if (csw.dCSWSignature != MSC_CSW_SIGNATURE || csw.dCSWTag != tag) {
        (void)usb_msc_bulk_only_reset(d);
        return 0;
    }

    if (!phase_ok) {
        (void)usb_msc_bulk_only_reset(d);
        return 0;
    }

    if (out_scsi_status) {
        *out_scsi_status = csw.bCSWStatus;
    }

    return csw.bCSWStatus == MSC_CSW_STATUS_OK;
}


static int usb_msc_scsi_request_sense(usb_msc_dev_t* d, uint8_t lun, uint8_t* out, uint32_t out_len) {
    if (!out || out_len < 18) {
        return 0;
    }

    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));

    cdb[0] = SCSI_CMD_REQUEST_SENSE;
    cdb[4] = (uint8_t)out_len;

    uint8_t st = 0;
    const int ok = usb_msc_bot_transport(d, lun, cdb, sizeof(cdb), 1, out, out_len, &st);

    return ok != 0;
}

static int usb_msc_scsi_test_unit_ready(usb_msc_dev_t* d, uint8_t lun) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));

    cdb[0] = SCSI_CMD_TEST_UNIT_READY;

    uint8_t st = 0;
    const int ok = usb_msc_bot_transport(d, lun, cdb, sizeof(cdb), 1, 0, 0, &st);

    if (ok) {
        return 1;
    }

    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));

    (void)usb_msc_scsi_request_sense(d, lun, sense, sizeof(sense));

    return 0;
}

static int usb_msc_scsi_inquiry(usb_msc_dev_t* d, uint8_t lun) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));

    cdb[0] = SCSI_CMD_INQUIRY;
    cdb[4] = 36;

    uint8_t buf[36];
    memset(buf, 0, sizeof(buf));

    uint8_t st = 0;
    const int ok = usb_msc_bot_transport(d, lun, cdb, sizeof(cdb), 1, buf, sizeof(buf), &st);

    return ok != 0;
}

static int usb_msc_scsi_read_capacity_10(usb_msc_dev_t* d, uint8_t lun, uint32_t* out_block_size, uint64_t* out_block_count) {
    if (!out_block_size || !out_block_count) {
        return 0;
    }

    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));

    cdb[0] = SCSI_CMD_READ_CAPACITY_10;

    uint8_t buf[8];
    memset(buf, 0, sizeof(buf));

    uint8_t st = 0;
    const int ok = usb_msc_bot_transport(d, lun, cdb, sizeof(cdb), 1, buf, sizeof(buf), &st);
    if (!ok) {
        return 0;
    }

    const uint32_t last_lba = be32_read(&buf[0]);
    const uint32_t blen = be32_read(&buf[4]);

    if (blen == 0u) {
        return 0;
    }

    *out_block_size = blen;
    *out_block_count = (uint64_t)last_lba + 1u;

    return 1;
}

static int usb_msc_scsi_rw_10(
    usb_msc_dev_t* d,
    uint8_t lun,
    uint8_t is_write,
    uint32_t lba,
    uint16_t blocks,
    void* data,
    uint32_t data_len
) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));

    cdb[0] = is_write ? SCSI_CMD_WRITE_10 : SCSI_CMD_READ_10;
    be32_write(&cdb[2], lba);
    be16_write(&cdb[7], blocks);

    uint8_t st = 0;
    const int ok = usb_msc_bot_transport(d, lun, cdb, sizeof(cdb), is_write ? 0u : 1u, data, data_len, &st);
    if (ok) {
        return 1;
    }

    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));

    (void)usb_msc_scsi_request_sense(d, lun, sense, sizeof(sense));

    return 0;
}


static int usb_msc_bdev_read_sectors(block_device_t* bdev, uint64_t lba, uint32_t count, void* buf) {
    if (!bdev || !buf || count == 0) {
        return 0;
    }

    usb_msc_dev_t* d = (usb_msc_dev_t*)bdev->private_data;
    if (!d) {
        return 0;
    }

    mutex_lock(&d->io_lock);

    if (d->dead) {
        mutex_unlock(&d->io_lock);
        return 0;
    }

    const uint32_t block_size = bdev->sector_size;

    if (block_size == 0u) {
        mutex_unlock(&d->io_lock);
        return 0;
    }

    uint32_t done = 0;

    while (done < count) {
        uint32_t n = count - done;
        if (n > 0xFFFFu) {
            n = 0xFFFFu;
        }

        const uint32_t chunk_lba = (uint32_t)(lba + (uint64_t)done);
        const uint32_t chunk_bytes = n * block_size;

        uint8_t* p = (uint8_t*)buf + (done * block_size);

        if (!usb_msc_scsi_rw_10(d, 0, 0, chunk_lba, (uint16_t)n, p, chunk_bytes)) {
            mutex_unlock(&d->io_lock);
            return 0;
        }

        done += n;
    }

    mutex_unlock(&d->io_lock);
    return 1;
}

static int usb_msc_bdev_write_sectors(block_device_t* bdev, uint64_t lba, uint32_t count, const void* buf) {
    if (!bdev || !buf || count == 0) {
        return 0;
    }

    usb_msc_dev_t* d = (usb_msc_dev_t*)bdev->private_data;
    if (!d) {
        return 0;
    }

    mutex_lock(&d->io_lock);

    if (d->dead) {
        mutex_unlock(&d->io_lock);
        return 0;
    }

    const uint32_t block_size = bdev->sector_size;

    if (block_size == 0u) {
        mutex_unlock(&d->io_lock);
        return 0;
    }

    uint32_t done = 0;

    while (done < count) {
        uint32_t n = count - done;
        if (n > 0xFFFFu) {
            n = 0xFFFFu;
        }

        const uint32_t chunk_lba = (uint32_t)(lba + (uint64_t)done);
        const uint32_t chunk_bytes = n * block_size;

        const uint8_t* p = (const uint8_t*)buf + (done * block_size);

        if (!usb_msc_scsi_rw_10(d, 0, 1, chunk_lba, (uint16_t)n, (void*)p, chunk_bytes)) {
            mutex_unlock(&d->io_lock);
            return 0;
        }

        done += n;
    }

    mutex_unlock(&d->io_lock);
    return 1;
}

static int usb_msc_bdev_flush(block_device_t* bdev) {
    (void)bdev;
    return 1;
}

static const block_ops_t g_usb_msc_bdev_ops = {
    .read_sectors = usb_msc_bdev_read_sectors,
    .write_sectors = usb_msc_bdev_write_sectors,

    .flush = usb_msc_bdev_flush,
};


static char* usb_msc_alloc_disk_name(uint32_t index) {
    char tmp[16];
    uint32_t v = index;
    uint32_t n = 0;

    tmp[n++] = 'u';
    tmp[n++] = 'd';

    if (v == 0u) {
        tmp[n++] = '0';
    } else {
        char digits[10];
        uint32_t d = 0;

        while (v != 0u && d < (uint32_t)sizeof(digits)) {
            digits[d++] = (char)('0' + (v % 10u));
            v /= 10u;
        }

        while (d > 0u) {
            tmp[n++] = digits[--d];
        }
    }

    tmp[n++] = '\0';

    char* out = (char*)kmalloc((size_t)n);
    if (!out) {
        return 0;
    }

    memcpy(out, tmp, (size_t)n);
    return out;
}

static int usb_msc_create_and_register_bdev(usb_msc_dev_t* d, uint32_t block_size, uint64_t block_count) {
    if (!d || d->bdev || d->bdev_name) {
        return 0;
    }

    if (block_size == 0u || block_count == 0u) {
        return 0;
    }

    block_device_t* bdev = (block_device_t*)kzalloc(sizeof(*bdev));
    if (!bdev) {
        return 0;
    }

    const int disk_id = idr_alloc(&g_usb_msc_idr, d);
    if (disk_id < 0) {
        kfree(bdev);
        return 0;
    }

    d->disk_id = disk_id;

    char* name = usb_msc_alloc_disk_name((uint32_t)disk_id);
    if (!name) {
        idr_remove(&g_usb_msc_idr, disk_id);
        kfree(bdev);
        return 0;
    }

    bdev->name = name;
    bdev->sector_size = block_size;
    bdev->sector_count = block_count;
    bdev->ops = &g_usb_msc_bdev_ops;
    bdev->private_data = d;

    if (bdev_register(bdev) != 0) {
        kfree(name);
        idr_remove(&g_usb_msc_idr, disk_id);
        kfree(bdev);
        return 0;
    }

    d->bdev = bdev;
    d->bdev_name = name;

    return 1;
}

static void usb_msc_unregister_and_destroy_bdev(usb_msc_dev_t* d) {
    if (!d) {
        return;
    }

    if (d->bdev_name) {
        (void)bdev_unregister(d->bdev_name);
    }

    if (d->disk_id > 0) {
        idr_remove(&g_usb_msc_idr, d->disk_id);
        d->disk_id = 0;
    }

    if (d->bdev_name) {
        kfree(d->bdev_name);
        d->bdev_name = 0;
    }

    d->bdev = 0;
}


static int usb_msc_parse_cfg(
    const uint8_t* cfg,
    uint16_t cfg_len,
    uint8_t* out_iface,
    uint8_t* out_ep_in,
    uint16_t* out_ep_in_mps,
    uint8_t* out_ep_out,
    uint16_t* out_ep_out_mps
) {
    if (!cfg || cfg_len < sizeof(usb_config_descriptor_t)) {
        return 0;
    }

    const usb_config_descriptor_t* cd = (const usb_config_descriptor_t*)cfg;
    if (cd->bLength < 9 || cd->bDescriptorType != USB_DESC_CONFIGURATION) {
        return 0;
    }

    int in_msc = 0;

    uint8_t iface = 0;

    uint8_t ep_in = 0;
    uint16_t ep_in_mps = 0;

    uint8_t ep_out = 0;
    uint16_t ep_out_mps = 0;

    uint16_t i = 0;
    while (i + 2 <= cfg_len) {
        const uint8_t blen = cfg[i + 0];
        const uint8_t dtype = cfg[i + 1];

        if (blen < 2) {
            break;
        }

        if ((uint32_t)i + (uint32_t)blen > cfg_len) {
            break;
        }

        if (dtype == USB_DESC_INTERFACE && blen >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t* id = (const usb_interface_descriptor_t*)&cfg[i];

            if (id->bInterfaceClass == USB_CLASS_MASS_STORAGE
                && id->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI_TRANSPARENT
                && id->bInterfaceProtocol == USB_MSC_PROTOCOL_BULK_ONLY) {
                in_msc = 1;
                iface = id->bInterfaceNumber;

                ep_in = 0;
                ep_in_mps = 0;

                ep_out = 0;
                ep_out_mps = 0;
            } else {
                in_msc = 0;
            }
        } else if (dtype == USB_DESC_ENDPOINT && blen >= sizeof(usb_endpoint_descriptor_t)) {
            if (in_msc) {
                const usb_endpoint_descriptor_t* ed = (const usb_endpoint_descriptor_t*)&cfg[i];

                const uint8_t ep_addr = ed->bEndpointAddress;
                const uint8_t ep_attr = ed->bmAttributes & USB_EP_XFER_MASK;

                if (ep_attr == USB_EP_XFER_BULK) {
                    const uint16_t mps = (uint16_t)(usb_le16_read(&ed->wMaxPacketSize) & 0x07FFu);

                    if ((ep_addr & USB_EP_DIR_IN) && ep_in == 0) {
                        ep_in = (uint8_t)(ep_addr & USB_EP_NUM_MASK);
                        ep_in_mps = mps ? mps : 64;
                    } else if (!(ep_addr & USB_EP_DIR_IN) && ep_out == 0) {
                        ep_out = (uint8_t)(ep_addr & USB_EP_NUM_MASK);
                        ep_out_mps = mps ? mps : 64;
                    }

                    if (ep_in && ep_out) {
                        *out_iface = iface;

                        *out_ep_in = ep_in;
                        *out_ep_in_mps = ep_in_mps;

                        *out_ep_out = ep_out;
                        *out_ep_out_mps = ep_out_mps;

                        return 1;
                    }
                }
            }
        }

        i = (uint16_t)(i + blen);
    }

    return 0;
}


static int usb_msc_probe(usb_device_t* dev, const uint8_t* cfg, uint16_t cfg_len) {
    if (!dev || !cfg || cfg_len == 0) {
        return 0;
    }

    uint8_t iface = 0;

    uint8_t ep_in = 0;
    uint16_t ep_in_mps = 0;

    uint8_t ep_out = 0;
    uint16_t ep_out_mps = 0;

    if (!usb_msc_parse_cfg(cfg, cfg_len, &iface, &ep_in, &ep_in_mps, &ep_out, &ep_out_mps)) {
        return 0;
    }

    usb_msc_dev_t* d = (usb_msc_dev_t*)kzalloc(sizeof(*d));
    if (!d) {
        return 0;
    }

    d->dev = dev;

    d->iface = iface;

    d->ep_in = ep_in;
    d->ep_in_mps = ep_in_mps;

    d->ep_out = ep_out;
    d->ep_out_mps = ep_out_mps;

    d->toggle_in = 0;
    d->toggle_out = 0;

    mutex_init(&d->io_lock);

    d->max_lun = usb_msc_get_max_lun(d);

    (void)usb_msc_bulk_only_reset(d);

    if (!usb_msc_scsi_inquiry(d, 0)) {
        kfree(d);
        return 0;
    }

    (void)usb_msc_scsi_test_unit_ready(d, 0);

    uint32_t block_size = 0;
    uint64_t block_count = 0;

    if (!usb_msc_scsi_read_capacity_10(d, 0, &block_size, &block_count)) {
        kfree(d);
        return 0;
    }

    if (!g_usb_msc_idr_init) {
        idr_init(&g_usb_msc_idr);
        g_usb_msc_idr_init = 1;
    }

    if (!usb_msc_create_and_register_bdev(d, block_size, block_count)) {
        kfree(d);
        return 0;
    }

    usb_device_set_class_private(dev, d);

    return 1;
}

static void usb_msc_disconnect(usb_device_t* dev) {
    if (!dev) {
        return;
    }

    usb_msc_dev_t* d = (usb_msc_dev_t*)usb_device_get_class_private(dev);
    if (!d) {
        return;
    }

    mutex_lock(&d->io_lock);
    d->dead = 1;
    mutex_unlock(&d->io_lock);

    usb_device_set_class_private(dev, 0);

    usb_msc_unregister_and_destroy_bdev(d);

    kfree(d);
}


static const usb_class_driver_t g_usb_msc_drv = {
    .name = "usb_msc_bot",
    .probe = usb_msc_probe,
    .disconnect = usb_msc_disconnect,
};

int usb_msc_init(void) {
    if (!g_usb_msc_idr_init) {
        idr_init(&g_usb_msc_idr);
        g_usb_msc_idr_init = 1;
    }

    return usb_register_class_driver(&g_usb_msc_drv);
}
