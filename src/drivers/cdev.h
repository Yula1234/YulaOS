#ifndef DRIVERS_CDEV_H
#define DRIVERS_CDEV_H

#include <drivers/driver.h>

#include <fs/vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdevice {
    device_t dev;

    vfs_ops_t ops;
    vfs_node_t node_template;
} cdevice_t;

int cdevice_register(cdevice_t* dev);
int cdevice_unregister(const char* name);

#ifdef __cplusplus
}
#endif

#endif
