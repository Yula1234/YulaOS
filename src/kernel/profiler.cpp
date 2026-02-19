#include <kernel/profiler.h>

#include <kernel/cpu.h>
#include <kernel/proc.h>
#include <kernel/symbols.h>

#include <hal/io.h>

#include <lib/cpp/atomic.h>

#include <limits.h>
#include <stdint.h>

namespace profiler_detail {

constexpr uint16_t kCom1Data = 0x3F8;
constexpr uint16_t kCom1LineStatus = 0x3FD;

constexpr uint16_t kQemuDebugcon = 0xE9;

constexpr uint32_t kFnTableSize = 8192;
constexpr uint32_t kEdgeTableSize = 8192;
constexpr uint32_t kMaxStackDepth = 64;

constexpr uint32_t kTopN = 20;
constexpr uint32_t kTopChildren = 8;

struct FnStat {
    uint32_t fn;
    uint32_t calls;
    uint64_t total_incl;
    uint64_t total_excl;
    uint64_t min_incl;
    uint64_t max_incl;
};

struct EdgeStat {
    uint32_t parent;
    uint32_t child;
    uint32_t calls;
    uint64_t total_incl;
    uint64_t min_incl;
    uint64_t max_incl;
};

struct StackFrame {
    uint32_t fn;
    uint32_t caller;
    uint64_t start_tsc;
    uint64_t child_cycles;
    uint64_t irq_snapshot;
};

static kernel::atomic<uint32_t> g_enabled;

static kernel::atomic<uint32_t> g_in_hook[MAX_CPUS];

static uint32_t g_irq_depth[MAX_CPUS];
static uint64_t g_irq_total_cycles[MAX_CPUS];
static uint64_t g_irq_enter_tsc[MAX_CPUS];

static uint32_t g_tsc_hz;

__attribute__((no_instrument_function))
static inline uint64_t u64_divmod_u32(uint64_t n, uint32_t d, uint32_t* out_rem);

static FnStat g_fn_stats[MAX_CPUS][kFnTableSize];
static EdgeStat g_edge_stats[MAX_CPUS][kEdgeTableSize];

static StackFrame g_stack[MAX_CPUS][kMaxStackDepth];
static uint32_t g_sp[MAX_CPUS];

__attribute__((no_instrument_function))
static inline uint64_t rdtsc_read() {
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile(
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    );

    return (static_cast<uint64_t>(hi) << 32) | lo;
}

__attribute__((no_instrument_function))
static inline uint16_t pit_read_counter0(void) {
    outb(0x43, 0x00);

    const uint8_t lo = inb(0x40);
    const uint8_t hi = inb(0x40);

    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

__attribute__((no_instrument_function))
static inline uint32_t read_eflags(void) {
    uint32_t v;
    __asm__ volatile(
        "pushf\n"
        "pop %0\n"
        : "=r"(v)
        :
        : "memory"
    );

    return v;
}

__attribute__((no_instrument_function))
static inline void write_eflags(uint32_t v) {
    __asm__ volatile(
        "push %0\n"
        "popf\n"
        :
        : "r"(v)
        : "memory", "cc"
    );
}

__attribute__((no_instrument_function))
static uint64_t profiler_calibrate_tsc_hz() {
    static constexpr uint32_t kPITInputHz = 1193182u;
    static constexpr uint32_t kCalMs = 10u;

    const uint32_t pit_div = (kPITInputHz * kCalMs) / 1000u;
    const uint16_t reload = (pit_div > 0xFFFFu) ? 0xFFFFu : static_cast<uint16_t>(pit_div);

    const uint32_t eflags = read_eflags();
    __asm__ volatile("cli" ::: "memory");

    outb(0x43, 0x30);
    outb(0x40, static_cast<uint8_t>(reload & 0xFFu));
    outb(0x40, static_cast<uint8_t>((reload >> 8) & 0xFFu));

    const uint64_t t0 = rdtsc_read();

    for (;;) {
        if (pit_read_counter0() == 0u) {
            break;
        }
        __asm__ volatile("pause" ::: "memory");
    }

    const uint64_t t1 = rdtsc_read();

    write_eflags(eflags);

    const uint64_t delta = (t1 > t0) ? (t1 - t0) : 0u;
    if (delta == 0u) {
        return 0u;
    }

    return u64_divmod_u32(delta * 1000ull, kCalMs, nullptr);
}

__attribute__((no_instrument_function))
static void serial_write(const char* s);

__attribute__((no_instrument_function))
static void serial_write_hex_u32(uint32_t v);

__attribute__((no_instrument_function))
static inline uint8_t profiler_active_cpu_count();

__attribute__((no_instrument_function))
static inline bool profiler_is_enabled();

__attribute__((no_instrument_function))
static void profiler_print_addr(uint32_t addr) {
    uint32_t sym_addr = 0;
    const char* name = symbols_resolve(addr, &sym_addr);

    if (!name || sym_addr == 0u || sym_addr > addr) {
        serial_write("0x");
        serial_write_hex_u32(addr);
        return;
    }

    serial_write(name);

    const uint32_t off = addr - sym_addr;
    if (off != 0u) {
        serial_write("+0x");
        serial_write_hex_u32(off);
    }
}

__attribute__((no_instrument_function))
static inline void serial_wait_tx() {
    while ((inb(kCom1LineStatus) & 0x20u) == 0u) {
        __asm__ volatile("pause" ::: "memory");
    }
}

__attribute__((no_instrument_function))
static inline void serial_putc(char c) {
    outb(kQemuDebugcon, static_cast<uint8_t>(c));

    serial_wait_tx();
    outb(kCom1Data, static_cast<uint8_t>(c));
}

__attribute__((no_instrument_function))
static void serial_write(const char* s) {
    if (!s) {
        return;
    }

    while (*s) {
        serial_putc(*s++);
    }
}

__attribute__((no_instrument_function))
static inline char hex_digit(uint8_t v) {
    v &= 0x0Fu;

    if (v < 10u) {
        return static_cast<char>('0' + v);
    }

    return static_cast<char>('a' + (v - 10u));
}

__attribute__((no_instrument_function))
static void serial_write_hex_u32(uint32_t v) {
    for (int i = 7; i >= 0; --i) {
        const uint8_t nybble = static_cast<uint8_t>((v >> (i * 4)) & 0x0Fu);
        serial_putc(hex_digit(nybble));
    }
}

__attribute__((no_instrument_function))
[[maybe_unused]] static void serial_write_hex_u64(uint64_t v) {
    const uint32_t hi = static_cast<uint32_t>(v >> 32);
    const uint32_t lo = static_cast<uint32_t>(v);

    serial_write_hex_u32(hi);
    serial_write_hex_u32(lo);
}

__attribute__((no_instrument_function))
static inline void serial_puts_ln(const char* s) {
    serial_write(s);
    serial_putc('\n');
}

__attribute__((no_instrument_function))
static void serial_write_dec_u32(uint32_t v) {
    char buf[11];
    uint32_t n = 0;

    if (v == 0u) {
        serial_putc('0');
        return;
    }

    while (v != 0u && n < sizeof(buf)) {
        buf[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    }

    while (n > 0u) {
        serial_putc(buf[--n]);
    }
}

__attribute__((no_instrument_function))
static inline uint64_t u64_divmod_u32(uint64_t n, uint32_t d, uint32_t* out_rem) {
    if (out_rem) {
        *out_rem = 0u;
    }

    if (d == 0u) {
        return 0u;
    }

    uint64_t q = 0u;
    uint64_t r = 0u;

    for (int i = 63; i >= 0; --i) {
        r = (r << 1u) | ((n >> static_cast<uint32_t>(i)) & 1u);
        if (r >= d) {
            r -= d;
            q |= (1ull << static_cast<uint32_t>(i));
        }
    }

    if (out_rem) {
        *out_rem = static_cast<uint32_t>(r);
    }

    return q;
}

__attribute__((no_instrument_function))
static void serial_write_dec_u64(uint64_t v) {
    char buf[21];
    uint32_t n = 0;

    if (v == 0u) {
        serial_putc('0');
        return;
    }

    while (v != 0u && n < sizeof(buf)) {
        buf[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    }

    while (n > 0u) {
        serial_putc(buf[--n]);
    }
}

__attribute__((no_instrument_function))
static inline uint64_t profiler_cycles_to_ns(uint64_t cycles) {
    const uint32_t hz = g_tsc_hz;
    if (hz == 0u) {
        return 0u;
    }

    uint32_t rem = 0u;
    const uint64_t sec = u64_divmod_u32(cycles, hz, &rem);
    const uint64_t nsec_from_sec = sec * 1000000000ull;
    const uint64_t nsec_from_rem = u64_divmod_u32(static_cast<uint64_t>(rem) * 1000000000ull, hz, nullptr);

    return nsec_from_sec + nsec_from_rem;
}

__attribute__((no_instrument_function))
static void profiler_write_ms_from_ns(uint64_t ns) {
    uint32_t frac = 0u;
    const uint64_t whole = u64_divmod_u32(ns, 1000000u, &frac);

    serial_write_dec_u64(whole);
    serial_putc('.');

    serial_putc(static_cast<char>('0' + static_cast<char>((frac / 100000u) % 10u)));
    serial_putc(static_cast<char>('0' + static_cast<char>((frac / 10000u) % 10u)));
    serial_putc(static_cast<char>('0' + static_cast<char>((frac / 1000u) % 10u)));
    serial_putc(static_cast<char>('0' + static_cast<char>((frac / 100u) % 10u)));
    serial_putc(static_cast<char>('0' + static_cast<char>((frac / 10u) % 10u)));
    serial_putc(static_cast<char>('0' + static_cast<char>(frac % 10u)));
}

__attribute__((no_instrument_function))
static inline uint8_t profiler_cpu_index() {
    uint16_t tr_sel = 0;
    __asm__ volatile("str %0" : "=m"(tr_sel));

    const int gdt_index = static_cast<int>(tr_sel >> 3);
    const int cpu_idx = gdt_index - 5;

    if (cpu_idx >= 0 && cpu_idx < MAX_CPUS) {
        return static_cast<uint8_t>(cpu_idx);
    }

    if (cpu_count == 0) {
        return 0;
    }

    return 0;
}

__attribute__((no_instrument_function))
static inline uint8_t profiler_active_cpu_count() {
    uint32_t active = 1u;
    if (ap_running_count > 0) {
        active += static_cast<uint32_t>(ap_running_count);
    }

    uint32_t cap = 1u;
    if (cpu_count > 0) {
        cap = static_cast<uint32_t>(cpu_count);
    }

    if (active > cap) {
        active = cap;
    }

    if (active > MAX_CPUS) {
        active = MAX_CPUS;
    }

    return static_cast<uint8_t>(active);
}

__attribute__((no_instrument_function))
static inline bool profiler_is_enabled() {
    return g_enabled.load(kernel::memory_order::relaxed) != 0u;
}

__attribute__((no_instrument_function))
static inline uint32_t hash_u32(uint32_t x) {
    return x * 2654435761u;
}

__attribute__((no_instrument_function))
static inline uint32_t hash_edge(uint32_t parent, uint32_t child) {
    return hash_u32(parent) ^ (hash_u32(child) >> 1);
}

__attribute__((no_instrument_function))
static FnStat& fn_stat_slot(uint8_t cpu, uint32_t fn) {
    const uint32_t mask = kFnTableSize - 1u;
    uint32_t idx = hash_u32(fn) & mask;

    for (;;) {
        FnStat& s = g_fn_stats[cpu][idx];
        if (s.fn == 0u || s.fn == fn) {
            if (s.fn == 0u) {
                s.fn = fn;
                s.min_incl = UINT64_MAX;
            }

            return s;
        }

        idx = (idx + 1u) & mask;
    }
}

__attribute__((no_instrument_function))
static EdgeStat& edge_stat_slot(uint8_t cpu, uint32_t parent, uint32_t child) {
    const uint32_t mask = kEdgeTableSize - 1u;
    uint32_t idx = hash_edge(parent, child) & mask;

    for (;;) {
        EdgeStat& s = g_edge_stats[cpu][idx];
        if ((s.parent == 0u && s.child == 0u) || (s.parent == parent && s.child == child)) {
            if (s.parent == 0u && s.child == 0u) {
                s.parent = parent;
                s.child = child;
                s.min_incl = UINT64_MAX;
            }

            return s;
        }

        idx = (idx + 1u) & mask;
    }
}

__attribute__((no_instrument_function))
static inline bool profiler_try_enter_hook(uint8_t cpu) {
    const uint32_t prev = g_in_hook[cpu].fetch_add(1u, kernel::memory_order::relaxed);
    if (prev != 0u) {
        (void)g_in_hook[cpu].fetch_sub(1u, kernel::memory_order::relaxed);
        return false;
    }

    return true;
}

__attribute__((no_instrument_function))
static inline void profiler_leave_hook(uint8_t cpu) {
    (void)g_in_hook[cpu].fetch_sub(1u, kernel::memory_order::relaxed);
}

__attribute__((no_instrument_function))
static inline void profiler_on_enter(uint8_t cpu, uint32_t fn, uint32_t caller) {
    uint32_t sp = g_sp[cpu];
    if (sp >= kMaxStackDepth) {
        return;
    }

    StackFrame& fr = g_stack[cpu][sp];
    fr.fn = fn;
    fr.caller = caller;
    fr.start_tsc = rdtsc_read();
    fr.child_cycles = 0;
    fr.irq_snapshot = g_irq_total_cycles[cpu];

    g_sp[cpu] = sp + 1u;
}

__attribute__((no_instrument_function))
static inline void profiler_on_exit(uint8_t cpu, uint32_t fn, uint32_t caller) {
    uint32_t sp = g_sp[cpu];
    if (sp == 0u) {
        return;
    }

    StackFrame fr = g_stack[cpu][sp - 1u];
    if (fr.fn != fn) {
        g_sp[cpu] = 0;
        return;
    }

    g_sp[cpu] = sp - 1u;

    const uint64_t end = rdtsc_read();
    const uint64_t dur_raw = end - fr.start_tsc;

    const uint64_t irq_total = g_irq_total_cycles[cpu];
    const uint64_t irq_delta = (irq_total > fr.irq_snapshot) ? (irq_total - fr.irq_snapshot) : 0u;

    const uint64_t dur = (dur_raw > irq_delta) ? (dur_raw - irq_delta) : 0u;
    const uint64_t excl = (dur > fr.child_cycles) ? (dur - fr.child_cycles) : 0u;

    FnStat& self = fn_stat_slot(cpu, fn);
    self.calls += 1u;
    self.total_incl += dur;
    self.total_excl += excl;
    if (dur < self.min_incl) {
        self.min_incl = dur;
    }
    if (dur > self.max_incl) {
        self.max_incl = dur;
    }

    if (g_sp[cpu] != 0u) {
        StackFrame& parent = g_stack[cpu][g_sp[cpu] - 1u];
        parent.child_cycles += dur;

        EdgeStat& edge = edge_stat_slot(cpu, parent.fn, fn);
        edge.calls += 1u;
        edge.total_incl += dur;
        if (dur < edge.min_incl) {
            edge.min_incl = dur;
        }
        if (dur > edge.max_incl) {
            edge.max_incl = dur;
        }
    } else {
        EdgeStat& edge = edge_stat_slot(cpu, caller, fn);
        edge.calls += 1u;
        edge.total_incl += dur;
        if (dur < edge.min_incl) {
            edge.min_incl = dur;
        }
        if (dur > edge.max_incl) {
            edge.max_incl = dur;
        }
    }
}

__attribute__((no_instrument_function))
static inline void profiler_irq_enter_cpu(uint8_t cpu) {
    if (!profiler_is_enabled()) {
        return;
    }

    if (g_in_hook[cpu].load(kernel::memory_order::relaxed) != 0u) {
        return;
    }

    const uint32_t depth = g_irq_depth[cpu];
    if (depth == 0u) {
        g_irq_enter_tsc[cpu] = rdtsc_read();
    }

    g_irq_depth[cpu] = depth + 1u;
}

__attribute__((no_instrument_function))
static inline void profiler_irq_exit_cpu(uint8_t cpu) {
    if (!profiler_is_enabled()) {
        return;
    }

    if (g_in_hook[cpu].load(kernel::memory_order::relaxed) != 0u) {
        return;
    }

    const uint32_t depth = g_irq_depth[cpu];
    if (depth == 0u) {
        return;
    }

    const uint32_t next = depth - 1u;
    g_irq_depth[cpu] = next;
    if (next != 0u) {
        return;
    }

    const uint64_t now = rdtsc_read();
    const uint64_t start = g_irq_enter_tsc[cpu];
    if (now > start) {
        g_irq_total_cycles[cpu] += now - start;
    }
}

__attribute__((no_instrument_function))
static void profiler_reset_cpu(uint8_t cpu) {
    for (uint32_t i = 0; i < kFnTableSize; ++i) {
        g_fn_stats[cpu][i] = {};
        g_fn_stats[cpu][i].min_incl = UINT64_MAX;
    }

    for (uint32_t i = 0; i < kEdgeTableSize; ++i) {
        g_edge_stats[cpu][i] = {};
        g_edge_stats[cpu][i].min_incl = UINT64_MAX;
    }

    g_sp[cpu] = 0;

    g_irq_depth[cpu] = 0;
    g_irq_total_cycles[cpu] = 0;
    g_irq_enter_tsc[cpu] = 0;
}

__attribute__((no_instrument_function))
static void profiler_print_cpu_header(uint8_t cpu) {
    serial_write("\nPROFILER CPU ");
    serial_write_dec_u32(static_cast<uint32_t>(cpu));
    serial_putc('\n');
}

__attribute__((no_instrument_function))
static void profiler_write_cycles(uint64_t v) {
    const uint64_t g = 1000000000ull;
    const uint64_t m = 1000000ull;
    const uint64_t k = 1000ull;

    if (v >= g) {
        serial_write_dec_u64(v / g);
        serial_putc('G');
        return;
    }

    if (v >= m) {
        serial_write_dec_u64(v / m);
        serial_putc('M');
        return;
    }

    if (v >= k) {
        serial_write_dec_u64(v / k);
        serial_putc('k');
        return;
    }

    serial_write_dec_u64(v);
}

__attribute__((no_instrument_function))
static void profiler_print_root_line(uint32_t rank, const FnStat& s) {
    const uint64_t avg = (s.calls != 0u) ? (s.total_incl / s.calls) : 0u;
    const uint64_t min = (s.min_incl == UINT64_MAX) ? 0u : s.min_incl;
    const uint64_t max = s.max_incl;

    serial_write_dec_u32(rank);
    serial_write(". ");
    profiler_print_addr(s.fn);

    serial_write(" [calls: ");
    serial_write_dec_u32(s.calls);

    serial_write(", min/avg/max: ");
    profiler_write_cycles(min);
    serial_write("/");
    profiler_write_cycles(avg);
    serial_write("/");
    profiler_write_cycles(max);
    serial_write(" cycles");

    if (g_tsc_hz != 0u) {
        serial_write(" | ");
        profiler_write_ms_from_ns(profiler_cycles_to_ns(min));
        serial_write("/");
        profiler_write_ms_from_ns(profiler_cycles_to_ns(avg));
        serial_write("/");
        profiler_write_ms_from_ns(profiler_cycles_to_ns(max));
        serial_write(" ms");
    }

    serial_write("]\n");
}

__attribute__((no_instrument_function))
static uint32_t edge_select_top_children(uint8_t cpu, uint32_t parent_fn, const EdgeStat** out, uint32_t out_cap) {
    for (uint32_t i = 0; i < out_cap; ++i) {
        out[i] = nullptr;
    }

    uint32_t found = 0;
    for (uint32_t i = 0; i < kEdgeTableSize; ++i) {
        const EdgeStat& e = g_edge_stats[cpu][i];
        if (e.parent != parent_fn || e.child == 0u || e.calls == 0u) {
            continue;
        }

        uint32_t pos = out_cap;
        for (uint32_t j = 0; j < out_cap; ++j) {
            if (!out[j] || e.total_incl > out[j]->total_incl) {
                pos = j;
                break;
            }
        }

        if (pos == out_cap) {
            continue;
        }

        for (uint32_t k = out_cap - 1u; k > pos; --k) {
            out[k] = out[k - 1u];
        }

        out[pos] = &e;
    }

    for (uint32_t i = 0; i < out_cap; ++i) {
        if (out[i]) {
            found = i + 1u;
        }
    }

    return found;
}

__attribute__((no_instrument_function))
static bool profiler_path_contains(const uint32_t* path, uint32_t path_len, uint32_t fn) {
    for (uint32_t i = 0; i < path_len; ++i) {
        if (path[i] == fn) {
            return true;
        }
    }

    return false;
}

__attribute__((no_instrument_function))
static void profiler_print_tree_prefix(const bool* last_stack, uint32_t depth) {
    for (uint32_t i = 0; i < depth; ++i) {
        if (last_stack[i]) {
            serial_write("   ");
        } else {
            serial_write("|  ");
        }
    }
}

__attribute__((no_instrument_function))
static void profiler_print_tree_edge_line_with_max(uint8_t cpu, const bool* last_stack, uint32_t depth, bool is_last, const EdgeStat& e) {
    (void)cpu;
    profiler_print_tree_prefix(last_stack, depth);
    serial_write(is_last ? "`-- " : "|-- ");

    profiler_print_addr(e.child);

    const uint64_t avg = (e.calls != 0u) ? (e.total_incl / e.calls) : 0u;
    const uint64_t min = (e.min_incl == UINT64_MAX) ? 0u : e.min_incl;
    const uint64_t max = e.max_incl;

    serial_write(" [calls: ");
    serial_write_dec_u32(e.calls);

    serial_write(", min/avg/max: ");
    profiler_write_cycles(min);
    serial_write("/");
    profiler_write_cycles(avg);
    serial_write("/");
    profiler_write_cycles(max);
    serial_write(" cycles");

    if (g_tsc_hz != 0u) {
        serial_write(" | ");
        profiler_write_ms_from_ns(profiler_cycles_to_ns(min));
        serial_write("/");
        profiler_write_ms_from_ns(profiler_cycles_to_ns(avg));
        serial_write("/");
        profiler_write_ms_from_ns(profiler_cycles_to_ns(max));
        serial_write(" ms");
    }

    serial_write("]\n");
}

__attribute__((no_instrument_function))
static void profiler_print_tree(uint8_t cpu, uint32_t fn, bool* last_stack, uint32_t depth, uint32_t* path, uint32_t path_len) {
    static constexpr uint32_t kTreeMaxDepth = 5;
    static constexpr uint32_t kTreeMaxChildren = 8;

    if (depth >= kTreeMaxDepth) {
        return;
    }

    if (profiler_path_contains(path, path_len, fn)) {
        return;
    }

    path[path_len] = fn;
    const uint32_t next_path_len = path_len + 1u;

    const EdgeStat* children[kTreeMaxChildren]{};
    const uint32_t child_count = edge_select_top_children(cpu, fn, children, kTreeMaxChildren);
    if (child_count == 0u) {
        return;
    }

    for (uint32_t i = 0; i < child_count; ++i) {
        const EdgeStat& e = *children[i];
        const bool is_last = (i + 1u == child_count);

        profiler_print_tree_edge_line_with_max(cpu, last_stack, depth, is_last, e);

        last_stack[depth] = is_last;
        profiler_print_tree(cpu, e.child, last_stack, depth + 1u, path, next_path_len);
    }
}

__attribute__((no_instrument_function))
static const FnStat* profiler_select_top(const FnStat* table, uint32_t cap, const FnStat** out, uint32_t out_cap) {
    for (uint32_t i = 0; i < out_cap; ++i) {
        out[i] = nullptr;
    }

    for (uint32_t i = 0; i < cap; ++i) {
        const FnStat& s = table[i];
        if (s.fn == 0u || s.calls == 0u) {
            continue;
        }

        uint32_t pos = out_cap;
        for (uint32_t j = 0; j < out_cap; ++j) {
            if (!out[j] || s.total_incl > out[j]->total_incl) {
                pos = j;
                break;
            }
        }

        if (pos == out_cap) {
            continue;
        }

        for (uint32_t k = out_cap - 1u; k > pos; --k) {
            out[k] = out[k - 1u];
        }
        out[pos] = &s;
    }

    return *out;
}

__attribute__((no_instrument_function))
static void profiler_dump_stats_cpu(uint8_t cpu) {
    profiler_print_cpu_header(cpu);

    const FnStat* top[kTopN]{};
    (void)profiler_select_top(&g_fn_stats[cpu][0], kFnTableSize, top, kTopN);

    for (uint32_t i = 0; i < kTopN; ++i) {
        if (!top[i]) {
            continue;
        }

        profiler_print_root_line(i + 1u, *top[i]);

        bool last_stack[8]{};
        uint32_t path[8]{};
        profiler_print_tree(cpu, top[i]->fn, last_stack, 0u, path, 0u);
    }
}

} // namespace profiler_detail

extern "C" {

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void* this_fn, void* call_site) {
    if (!profiler_detail::profiler_is_enabled()) {
        return;
    }

    const uint8_t cpu = profiler_detail::profiler_cpu_index();
    if (profiler_detail::g_irq_depth[cpu] != 0u) {
        return;
    }

    if (!profiler_detail::profiler_try_enter_hook(cpu)) {
        return;
    }

    profiler_detail::profiler_on_enter(
        cpu,
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this_fn)),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(call_site))
    );

    profiler_detail::profiler_leave_hook(cpu);
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void* this_fn, void* call_site) {
    if (!profiler_detail::profiler_is_enabled()) {
        return;
    }

    const uint8_t cpu = profiler_detail::profiler_cpu_index();
    if (profiler_detail::g_irq_depth[cpu] != 0u) {
        return;
    }

    if (!profiler_detail::profiler_try_enter_hook(cpu)) {
        return;
    }

    profiler_detail::profiler_on_exit(
        cpu,
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this_fn)),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(call_site))
    );

    profiler_detail::profiler_leave_hook(cpu);
}

__attribute__((no_instrument_function))
void profiler_init(void) {
    profiler_detail::g_enabled.store(1u, kernel::memory_order::relaxed);

    if (profiler_detail::g_tsc_hz == 0u) {
        const uint64_t hz64 = profiler_detail::profiler_calibrate_tsc_hz();
        if (hz64 > 0xFFFFFFFFull) {
            profiler_detail::g_tsc_hz = 0xFFFFFFFFu;
        } else {
            profiler_detail::g_tsc_hz = static_cast<uint32_t>(hz64);
        }
    }
}

__attribute__((no_instrument_function))
void profiler_reset_stats(void) {
    const uint32_t was = profiler_detail::g_enabled.exchange(0u, kernel::memory_order::relaxed);

    const uint8_t active = profiler_detail::profiler_active_cpu_count();
    for (uint8_t cpu = 0; cpu < active; ++cpu) {
        profiler_detail::profiler_reset_cpu(cpu);
    }

    profiler_detail::g_enabled.store(was, kernel::memory_order::relaxed);
}

__attribute__((no_instrument_function))
void profiler_irq_enter(void) {
    const uint8_t cpu = profiler_detail::profiler_cpu_index();
    profiler_detail::profiler_irq_enter_cpu(cpu);
}

__attribute__((no_instrument_function))
void profiler_irq_exit(void) {
    const uint8_t cpu = profiler_detail::profiler_cpu_index();
    profiler_detail::profiler_irq_exit_cpu(cpu);
}

__attribute__((no_instrument_function))
void profiler_dump_stats(void) {
    const uint32_t was = profiler_detail::g_enabled.exchange(0u, kernel::memory_order::relaxed);

    profiler_detail::serial_puts_ln("\n================ PROFILER WINDOW ================");

    const uint8_t active = profiler_detail::profiler_active_cpu_count();
    for (uint8_t cpu = 0; cpu < active; ++cpu) {
        profiler_detail::profiler_dump_stats_cpu(cpu);
    }

    profiler_detail::serial_puts_ln("=================================================");

    profiler_detail::g_enabled.store(was, kernel::memory_order::relaxed);
}

__attribute__((no_instrument_function))
void profiler_task(void* arg) {
    (void)arg;

    for (;;) {
        proc_usleep(5000000);
        profiler_dump_stats();
        profiler_reset_stats();
    }
}

}

