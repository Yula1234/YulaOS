// SPDX-License-Identifier: GPL-2.0

#include <mm/iomem.h>
#include <mm/heap.h>

#include <hal/mmio.h>
#include <hal/pmio.h>

#define IOMEM_TYPE_MMIO 0u
#define IOMEM_TYPE_PMIO 1u

struct __iomem_region {
    uint32_t type_;

    union {
        mmio_region_t* mmio;
        pmio_region_t* pmio;
    } u_;
};

__iomem* iomem_request_mmio(uint32_t phys_start, uint32_t size, const char* name) {
    if (size == 0u) {
        return 0;
    }

    mmio_region_t* mmio = mmio_request_region(phys_start, size, name);

    if (!mmio) {
        return 0;
    }

    __iomem* iomem = (__iomem*)kmalloc(sizeof(__iomem));

    if (!iomem) {
        mmio_release_region(mmio);

        return 0;
    }

    iomem->type_ = IOMEM_TYPE_MMIO;
    iomem->u_.mmio = mmio;

    return iomem;
}

__iomem* iomem_request_mmio_wc(uint32_t phys_start, uint32_t size, const char* name) {
    if (size == 0u) {
        return 0;
    }

    mmio_region_t* mmio = mmio_request_region_wc(phys_start, size, name);

    if (!mmio) {
        return 0;
    }

    __iomem* iomem = (__iomem*)kmalloc(sizeof(__iomem));

    if (!iomem) {
        mmio_release_region(mmio);

        return 0;
    }

    iomem->type_ = IOMEM_TYPE_MMIO;
    iomem->u_.mmio = mmio;

    return iomem;
}

__iomem* iomem_request_pmio(uint16_t port_start, uint16_t size, const char* name) {
    if (size == 0u) {
        return 0;
    }

    pmio_region_t* pmio = pmio_request_region(port_start, size, name);

    if (!pmio) {
        return 0;
    }

    __iomem* iomem = (__iomem*)kmalloc(sizeof(__iomem));

    if (!iomem) {
        pmio_release_region(pmio);

        return 0;
    }

    iomem->type_ = IOMEM_TYPE_PMIO;
    iomem->u_.pmio = pmio;

    return iomem;
}

void iomem_free(__iomem* region) {
    if (!region) {
        return;
    }

    if (region->type_ == IOMEM_TYPE_MMIO) {
        mmio_release_region(region->u_.mmio);
    } else if (region->type_ == IOMEM_TYPE_PMIO) {
        pmio_release_region(region->u_.pmio);
    }

    kfree(region);
}

uint8_t ioread8(__iomem* region, uint32_t offset) {
    if (!region) {
        return 0xFFu;
    }

    uint8_t value = 0xFFu;

    if (region->type_ == IOMEM_TYPE_MMIO) {
        (void)mmio_read8(region->u_.mmio, offset, &value);
    } else if (region->type_ == IOMEM_TYPE_PMIO) {
        if (offset <= 0xFFFFu) {
            (void)pmio_readb(region->u_.pmio, (uint16_t)offset, &value);
        }
    }

    return value;
}

uint16_t ioread16(__iomem* region, uint32_t offset) {
    if (!region) {
        return 0xFFFFu;
    }

    uint16_t value = 0xFFFFu;

    if (region->type_ == IOMEM_TYPE_MMIO) {
        (void)mmio_read16(region->u_.mmio, offset, &value);
    } else if (region->type_ == IOMEM_TYPE_PMIO) {
        if (offset <= 0xFFFFu) {
            (void)pmio_readw(region->u_.pmio, (uint16_t)offset, &value);
        }
    }

    return value;
}

uint32_t ioread32(__iomem* region, uint32_t offset) {
    if (!region) {
        return 0xFFFFFFFFu;
    }

    uint32_t value = 0xFFFFFFFFu;

    if (region->type_ == IOMEM_TYPE_MMIO) {
        (void)mmio_read32(region->u_.mmio, offset, &value);
    } else if (region->type_ == IOMEM_TYPE_PMIO) {
        if (offset <= 0xFFFFu) {
            (void)pmio_readl(region->u_.pmio, (uint16_t)offset, &value);
        }
    }

    return value;
}

void iowrite8(__iomem* region, uint32_t offset, uint8_t value) {
    if (!region) {
        return;
    }

    if (region->type_ == IOMEM_TYPE_MMIO) {
        (void)mmio_write8(region->u_.mmio, offset, value);
    } else if (region->type_ == IOMEM_TYPE_PMIO) {
        if (offset <= 0xFFFFu) {
            (void)pmio_writeb(region->u_.pmio, (uint16_t)offset, value);
        }
    }
}

void iowrite16(__iomem* region, uint32_t offset, uint16_t value) {
    if (!region) {
        return;
    }

    if (region->type_ == IOMEM_TYPE_MMIO) {
        (void)mmio_write16(region->u_.mmio, offset, value);
    } else if (region->type_ == IOMEM_TYPE_PMIO) {
        if (offset <= 0xFFFFu) {
            (void)pmio_writew(region->u_.pmio, (uint16_t)offset, value);
        }
    }
}

void iowrite32(__iomem* region, uint32_t offset, uint32_t value) {
    if (!region) {
        return;
    }

    if (region->type_ == IOMEM_TYPE_MMIO) {
        (void)mmio_write32(region->u_.mmio, offset, value);
    } else if (region->type_ == IOMEM_TYPE_PMIO) {
        if (offset <= 0xFFFFu) {
            (void)pmio_writel(region->u_.pmio, (uint16_t)offset, value);
        }
    }
}