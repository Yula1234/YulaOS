#ifndef DRIVERS_DRIVER_H
#define DRIVERS_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DRIVER_STAGE_EARLY = 0,
    DRIVER_STAGE_CORE = 1,
    DRIVER_STAGE_VFS = 2,
    DRIVER_STAGE_LATE = 3,
} driver_stage_t;

typedef enum {
    DRIVER_CLASS_PSEUDO = 0,
    DRIVER_CLASS_CHAR = 1,
    DRIVER_CLASS_BLOCK = 2,
    DRIVER_CLASS_NET = 3,
    DRIVER_CLASS_GPU = 4,
    DRIVER_CLASS_INPUT = 5,
} driver_class_t;

typedef struct driver_desc {
    const char* name;

    driver_class_t klass;
    driver_stage_t stage;

    int (*init)(void);
    void (*shutdown)(void);
} driver_desc_t;

typedef struct device {
    const driver_desc_t* driver;

    const char* name;
    uint32_t flags;

    void* private_data;
} device_t;

#define DRIVER_REGISTER_GLUE_(a, b) a##b
#define DRIVER_REGISTER_GLUE(a, b) DRIVER_REGISTER_GLUE_(a, b)

#define DRIVER_REGISTER(...) \
    static const driver_desc_t DRIVER_REGISTER_GLUE(__driver_desc_, __COUNTER__) \
        __attribute__((section(".yosdrivers"), used, aligned(sizeof(void*)))) = { __VA_ARGS__ }

void drivers_init_stage(driver_stage_t stage);
void drivers_init_all(void);

#ifdef __cplusplus
}
#endif

#endif
