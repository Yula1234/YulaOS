#include <drivers/cdev.h>

#include <lib/string.h>

int cdevice_register(cdevice_t* dev) {
    if (!dev) {
        return -1;
    }

    vfs_node_t* node = &dev->node_template;

    if (node->name[0] == '\0') {
        return -1;
    }

    node->ops = &dev->ops;

    devfs_register(node);
    return 0;
}

int cdevice_unregister(const char* name) {
    if (!name || name[0] == '\0') {
        return -1;
    }

    return devfs_unregister(name);
}
