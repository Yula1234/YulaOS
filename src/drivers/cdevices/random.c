#include <drivers/cdev.h>
#include <drivers/driver.h>

#include <hal/lock.h>

#include <stdint.h>

extern volatile uint32_t timer_ticks;

static inline uint64_t rdtsc_read(void) {
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile(
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    );

    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline uint64_t rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t splitmix64_next(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

    return z ^ (z >> 31);
}

static inline uint64_t xoshiro256ss_next(uint64_t s[4]) {
    const uint64_t result = rotl64(s[1] * 5ull, 7) * 9ull;

    const uint64_t t = s[1] << 17;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];

    s[2] ^= t;
    s[3] = rotl64(s[3], 45);

    return result;
}

static inline void xor_mix_bytes(uint64_t s[4], const uint8_t* data, uint32_t n) {
    uint64_t a = 0x243F6A8885A308D3ull;
    uint64_t b = 0x13198A2E03707344ull;

    for (uint32_t i = 0u; i < n; i++) {
        const uint64_t v = data[i];

        a ^= v + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2);
        b ^= a + rotl64(v * 0xA0761D6478BD642Full, (int)(v & 63u));
    }

    s[0] ^= a;
    s[1] ^= b;
    s[2] ^= rotl64(a, 23);
    s[3] ^= rotl64(b, 41);
}

static struct {
    spinlock_t lock;

    uint64_t s[4];

    uint8_t seeded;
} g_rng;

static void rng_seed_if_needed(void) {
    if (__atomic_load_n(&g_rng.seeded, __ATOMIC_ACQUIRE) != 0u) {
        return;
    }

    const uint64_t t0 = rdtsc_read();
    const uint64_t t1 =
        ((uint64_t)timer_ticks << 32)
        ^ (uint64_t)(uintptr_t)&g_rng;

    uint64_t sm = t0 ^ rotl64(t1, 17);

    g_rng.s[0] = splitmix64_next(&sm);
    g_rng.s[1] = splitmix64_next(&sm);
    g_rng.s[2] = splitmix64_next(&sm);
    g_rng.s[3] = splitmix64_next(&sm);

    __atomic_store_n(&g_rng.seeded, 1u, __ATOMIC_RELEASE);
}

static void rng_mix_input(const void* buf, uint32_t size) {
    if (!buf || size == 0u) {
        return;
    }

    const uint8_t* b = (const uint8_t*)buf;

    {
        const uint32_t flags = spinlock_acquire_safe(&g_rng.lock);

        rng_seed_if_needed();

        xor_mix_bytes(g_rng.s, b, size);

        (void)xoshiro256ss_next(g_rng.s);

        spinlock_release_safe(&g_rng.lock, flags);
    }
}

static int random_read(
    vfs_node_t* node, uint32_t offset,
    uint32_t size, void* buffer
) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0u) {
        return 0;
    }

    uint8_t* const out = (uint8_t*)buffer;

    {
        const uint32_t flags = spinlock_acquire_safe(&g_rng.lock);

        rng_seed_if_needed();

        uint32_t i = 0u;

        for (; i + 8u <= size; i += 8u) {
            const uint64_t v = xoshiro256ss_next(g_rng.s);

            out[i + 0] = (uint8_t)(v >> 0);
            out[i + 1] = (uint8_t)(v >> 8);
            out[i + 2] = (uint8_t)(v >> 16);
            out[i + 3] = (uint8_t)(v >> 24);
            out[i + 4] = (uint8_t)(v >> 32);
            out[i + 5] = (uint8_t)(v >> 40);
            out[i + 6] = (uint8_t)(v >> 48);
            out[i + 7] = (uint8_t)(v >> 56);
        }

        if (i != size) {
            uint64_t tmp = xoshiro256ss_next(g_rng.s);

            for (; i < size; i++) {
                out[i] = (uint8_t)tmp;
                tmp >>= 8;
            }
        }

        spinlock_release_safe(&g_rng.lock, flags);
    }

    return (int)size;
}

static int random_write(
    vfs_node_t* node, uint32_t offset,
    uint32_t size, const void* buffer
) {
    (void)node;
    (void)offset;

    rng_mix_input(buffer, size);

    return (int)size;
}

static cdevice_t g_random_cdev = {
    .dev = {
        .name = "random",
    },
    .ops = {
        .read = random_read,
        .write = random_write,
    },
    .node_template = {
        .name = "random",
    },
};

static int random_driver_init(void) {
    spinlock_init(&g_rng.lock);

    return cdevice_register(&g_random_cdev);
}

DRIVER_REGISTER(
    .name = "random",
    .klass = DRIVER_CLASS_CHAR,
    .stage = DRIVER_STAGE_VFS,
    .init = random_driver_init,
    .shutdown = 0
);
