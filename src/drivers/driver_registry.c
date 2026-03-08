#include <drivers/driver.h>

#include <kernel/panic.h>

#include <kernel/output/kprintf.h>

extern const driver_desc_t __yosdrivers_start;
extern const driver_desc_t __yosdrivers_end;

static const driver_desc_t* drivers_begin(void) {
    return &__yosdrivers_start;
}

static const driver_desc_t* drivers_end(void) {
    return &__yosdrivers_end;
}

static int stage_is_critical(driver_stage_t stage) {
    return stage == DRIVER_STAGE_EARLY || stage == DRIVER_STAGE_CORE;
}

static void drivers_init_stage_impl(driver_stage_t stage) {
    const driver_desc_t* it = drivers_begin();
    const driver_desc_t* end = drivers_end();

    for (; it < end; it++) {
        if (it->stage != stage) {
            continue;
        }

        if (!it->init) {
            continue;
        }

        const int rc = it->init();
        if (rc >= 0) {
            if (it->name) {
                kprintf(
                    "[drivers] inited: %s stage=%u class=%u rc=%d\n",
                    it->name,
                    (unsigned)it->stage,
                    (unsigned)it->klass,
                    rc
                );
            } else {
                kprintf(
                    "[drivers] init ok: <noname> stage=%u class=%u rc=%d\n",
                    (unsigned)it->stage,
                    (unsigned)it->klass,
                    rc
                );
            }

            continue;
        }

        if (rc < 0) {
            if (stage_is_critical(stage)) {
                panic("driver init failed");
            }

            if (it->name) {
                kprintf(
                    "[drivers] init failed: %s stage=%u class=%u rc=%d\n",
                    it->name,
                    (unsigned)it->stage,
                    (unsigned)it->klass,
                    rc
                );
            } else {
                kprintf(
                    "[drivers] init failed: <noname> stage=%u class=%u rc=%d\n",
                    (unsigned)it->stage,
                    (unsigned)it->klass,
                    rc
                );
            }
        }
    }
}

void drivers_init_stage(driver_stage_t stage) {
    drivers_init_stage_impl(stage);
}

void drivers_init_all(void) {
    drivers_init_stage_impl(DRIVER_STAGE_EARLY);
    drivers_init_stage_impl(DRIVER_STAGE_CORE);
    drivers_init_stage_impl(DRIVER_STAGE_VFS);
    drivers_init_stage_impl(DRIVER_STAGE_LATE);
}
