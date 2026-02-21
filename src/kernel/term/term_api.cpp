#include <kernel/term/term.h>

#include <stdint.h>

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>

#include <lib/string.h>
#include <mm/heap.h>

namespace kernel::term {

template <typename T>
class RawBuffer {
public:
    RawBuffer() = default;

    RawBuffer(const RawBuffer&) = delete;
    RawBuffer& operator=(const RawBuffer&) = delete;

    RawBuffer(RawBuffer&& other) noexcept {
        swap(other);
    }

    RawBuffer& operator=(RawBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            swap(other);
        }

        return *this;
    }

    ~RawBuffer() {
        reset();
    }

    void reset() {
        if (ptr_) {
            kfree(ptr_);
            ptr_ = nullptr;
        }

        cap_elems_ = 0;
    }

    int reserve_elems(size_t need_elems) {
        if (need_elems <= cap_elems_) {
            return 0;
        }

        if (need_elems == 0) {
            need_elems = 1;
        }

        size_t bytes = need_elems * sizeof(T);
        if (bytes / sizeof(T) != need_elems) {
            return -1;
        }

        void* np = krealloc(ptr_, bytes);
        if (!np) {
            return -1;
        }

        ptr_ = static_cast<T*>(np);
        cap_elems_ = need_elems;

        return 0;
    }

    T* data() {
        return ptr_;
    }

    const T* data() const {
        return ptr_;
    }

    size_t capacity_elems() const {
        return cap_elems_;
    }

    void swap(RawBuffer& other) noexcept {
        T* p = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = p;

        size_t c = cap_elems_;
        cap_elems_ = other.cap_elems_;
        other.cap_elems_ = c;
    }

private:
    T* ptr_ = nullptr;
    size_t cap_elems_ = 0;
};

static constexpr int kDefaultCols = 80;
static constexpr int kDefaultRows = 12;

struct TermState {
    char* buffer;

    uint32_t* fg_colors;
    uint32_t* bg_colors;

    uint64_t seq;
    uint64_t view_seq;

    int history_cap_rows;
    int history_rows;

    uint8_t* dirty_rows;
    int* dirty_x1;
    int* dirty_x2;
    int full_redraw;

    uint32_t curr_fg;
    uint32_t curr_bg;
    uint32_t def_fg;
    uint32_t def_bg;

    int cols;
    int view_rows;

    int col;
    int row;
    int view_row;
    int max_row;

    int saved_col;
    int saved_row;
    int esc_state;
    int csi_in_param;
    int csi_param_value;
    int csi_param_count;
    int csi_params[8];
    int ansi_bright;
    int ansi_inverse;
};

static constexpr uint32_t kAnsiColors[8] = {
    0x000000u,
    0x800000u,
    0x008000u,
    0x808000u,
    0x000080u,
    0x800080u,
    0x008080u,
    0xC0C0C0u,
};

static constexpr uint32_t kAnsiBrightColors[8] = {
    0x808080u,
    0xFF0000u,
    0x00FF00u,
    0xFFFF00u,
    0x0000FFu,
    0xFF00FFu,
    0x00FFFFu,
    0xFFFFFFu,
};

using CharBuf = kernel::unique_ptr<char, kernel::default_delete<char[]>>;
using U32Buf = kernel::unique_ptr<uint32_t, kernel::default_delete<uint32_t[]>>;
using U8Buf = kernel::unique_ptr<uint8_t, kernel::default_delete<uint8_t[]>>;
using I32Buf = kernel::unique_ptr<int, kernel::default_delete<int[]>>;

class TermSnapshotImpl {
public:
    TermSnapshotImpl() {
        state = {};
    }

    TermSnapshotImpl(const TermSnapshotImpl&) = delete;
    TermSnapshotImpl& operator=(const TermSnapshotImpl&) = delete;

    TermState state{};

    size_t cap_cells = 0;
    int cap_rows = 0;

    CharBuf buf;
    U32Buf fg;
    U32Buf bg;

    U8Buf dirty_rows;
    I32Buf dirty_x1;
    I32Buf dirty_x2;
};

class TermImpl {
public:
    explicit TermImpl(int cols, int view_rows) {
        term = {};

        if (cols < 1) {
            cols = 1;
        }

        if (view_rows < 1) {
            view_rows = 1;
        }

        term.seq = 1;
        term.view_seq = 1;

        term.cols = cols;
        term.view_rows = view_rows;

        term.curr_fg = 0xD4D4D4u;
        term.curr_bg = 0x141414u;
        term.def_fg = term.curr_fg;
        term.def_bg = term.curr_bg;

        term.max_row = 0;
        term.history_rows = 1;

        term.full_redraw = 1;
        term.view_row = 0;

        term.col = 0;
        term.row = 0;

        term.saved_col = 0;
        term.saved_row = 0;

        term.esc_state = 0;
        term.csi_in_param = 0;
        term.csi_param_value = 0;
        term.csi_param_count = 0;

        term.ansi_bright = 0;
        term.ansi_inverse = 0;

        (void)ensure_rows_locked(1);
        mark_all_dirty_locked();
        bump_view_seq_locked();
    }

    ~TermImpl() = default;

    TermImpl(const TermImpl&) = delete;
    TermImpl& operator=(const TermImpl&) = delete;

    void putc_locked(char c) {
        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        int view_rows = term.view_rows;
        if (view_rows <= 0) {
            view_rows = kDefaultRows;
        }

        if ((uint8_t)c == 0x0Cu) {
            term.col = 0;
            term.row = 0;
            term.view_row = 0;
            term.max_row = 0;
            term.history_rows = 1;

            clear_row_locked(0);

            invalidate_view_locked();
            return;
        }

        if (c == '\r') {
            term.col = 0;
            bump_view_seq_locked();
            return;
        }

        if (c == '\n') {
            if (ensure_rows_locked(term.row + 1) != 0) {
                return;
            }

            int idx = term.row * cols + term.col;
            uint32_t fg = effective_fg();
            uint32_t bg = effective_bg();
            int remaining = cols - term.col;

            for (int k = 0; k < remaining; k++) {
                term.bg_colors[idx + k] = bg;
                term.fg_colors[idx + k] = fg;
                term.buffer[idx + k] = ' ';
            }

            dirty_mark_range_locked(term.row, term.col, cols);

            term.col = 0;
            term.row++;

            clear_row_locked(term.row);
        } else if (c == '\b') {
            if (term.col > 0) {
                term.col--;
            }

            if (ensure_rows_locked(term.row + 1) != 0) {
                return;
            }

            int idx = term.row * cols + term.col;
            term.buffer[idx] = ' ';
            term.fg_colors[idx] = effective_fg();
            term.bg_colors[idx] = effective_bg();

            dirty_mark_range_locked(term.row, term.col, term.col + 1);
        } else {
            if (ensure_rows_locked(term.row + 1) != 0) {
                return;
            }

            int idx = term.row * cols + term.col;
            term.buffer[idx] = c;
            term.fg_colors[idx] = effective_fg();
            term.bg_colors[idx] = effective_bg();

            dirty_mark_range_locked(term.row, term.col, term.col + 1);
            term.col++;
        }

        if (term.col >= cols) {
            term.col = 0;
            term.row++;
            clear_row_locked(term.row);
        }

        if (term.row >= term.history_rows) {
            term.history_rows = term.row + 1;
        }

        if (term.row > term.max_row) {
            term.max_row = term.row;
        }

        int old_view_row = term.view_row;

        int at_bottom = (term.view_row + view_rows) >= term.row;
        if (at_bottom) {
            if (term.row >= view_rows) {
                term.view_row = term.row - view_rows + 1;
            } else {
                term.view_row = 0;
            }
        }

        bump_seq_locked();

        if (term.view_row != old_view_row) {
            invalidate_view_locked();
        } else {
            bump_view_seq_locked();
        }
    }

    void write_locked(const char* buf, uint32_t len) {
        if (!buf || len == 0) {
            return;
        }

        uint32_t i = 0;

        while (i < len) {
            char c = buf[i++];

            if (term.esc_state == 0) {
                if ((uint8_t)c == 0x1Bu) {
                    term.esc_state = 1;
                    continue;
                }

                putc_locked(c);
                continue;
            }

            if (term.esc_state == 1) {
                if (c == '[') {
                    term.esc_state = 2;
                    term.csi_param_count = 0;
                    term.csi_param_value = 0;
                    term.csi_in_param = 0;
                    continue;
                }

                if (c == '7') {
                    term.saved_row = term.row;
                    term.saved_col = term.col;
                    ansi_reset_locked();
                    continue;
                }

                if (c == '8') {
                    set_cursor_locked(term.saved_row, term.saved_col);
                    ansi_reset_locked();
                    continue;
                }

                ansi_reset_locked();
                continue;
            }

            if (term.esc_state == 2) {
                if (c >= '0' && c <= '9') {
                    term.csi_in_param = 1;
                    term.csi_param_value = term.csi_param_value * 10 + (int)(c - '0');

                    if (term.csi_param_value > 9999) {
                        term.csi_param_value = 9999;
                    }

                    continue;
                }

                if (c == ';') {
                    csi_push_param_locked();
                    continue;
                }

                if (term.csi_in_param || term.csi_param_count > 0) {
                    csi_push_param_locked();
                }

                handle_csi_locked(c);
                ansi_reset_locked();
            }
        }
    }

    void print_locked(const char* s) {
        if (!s) {
            return;
        }

        write_locked(s, (uint32_t)strlen(s));
    }

    void reflow_locked(int new_cols) {
        if (new_cols <= 0) {
            new_cols = 1;
        }

        int old_cols = term.cols;
        if (old_cols <= 0) {
            old_cols = kDefaultCols;
        }

        if (new_cols == old_cols) {
            term.cols = new_cols;
            return;
        }

        int old_last_row = term.max_row;
        if (old_last_row < 0) {
            old_last_row = 0;
        }

        if (old_last_row >= term.history_rows) {
            old_last_row = term.history_rows - 1;
        }

        if (old_last_row < 0) {
            old_last_row = 0;
        }

        size_t worst = ((size_t)(old_last_row + 1) * (size_t)old_cols) + (size_t)(old_last_row + 1);
        
        int cap_rows = (int)(worst / (size_t)new_cols) + 2;
        
        if (cap_rows < 1) {
            cap_rows = 1;
        }

        size_t cells = (size_t)cap_rows * (size_t)new_cols;

        RawBuffer<char> nb;
        RawBuffer<uint32_t> nfg;
        RawBuffer<uint32_t> nbg;
        RawBuffer<uint8_t> ndr;
        RawBuffer<int> ndx1;
        RawBuffer<int> ndx2;

        if (
            nb.reserve_elems(cells ? cells : 1) != 0
            || nfg.reserve_elems(cells ? cells : 1) != 0
            || nbg.reserve_elems(cells ? cells : 1) != 0
            || ndr.reserve_elems((size_t)cap_rows ? (size_t)cap_rows : 1u) != 0
            || ndx1.reserve_elems((size_t)cap_rows ? (size_t)cap_rows : 1u) != 0
            || ndx2.reserve_elems((size_t)cap_rows ? (size_t)cap_rows : 1u) != 0
        ) {
            return;
        }

        char* nb_data = nb.data();
        uint32_t* nfg_data = nfg.data();
        uint32_t* nbg_data = nbg.data();
        uint8_t* ndr_data = ndr.data();
        int* ndx1_data = ndx1.data();
        int* ndx2_data = ndx2.data();

        for (size_t i = 0; i < cells; i++) {
            nb_data[i] = ' ';
            nfg_data[i] = term.curr_fg;
            nbg_data[i] = term.curr_bg;
        }

        for (int r = 0; r < cap_rows; r++) {
            ndr_data[r] = 1;
            ndx1_data[r] = 0;
            ndx2_data[r] = new_cols;
        }

        int cur_row = term.row;
        int cur_col = term.col;

        if (cur_row < 0) {
            cur_row = 0;
        }

        if (cur_col < 0) {
            cur_col = 0;
        }

        if (cur_col > old_cols) {
            cur_col = old_cols;
        }

        int out_r = 0;
        int out_c = 0;

        int new_cur_r = 0;
        int new_cur_c = 0;
        int have_cur = 0;

        int new_view_r = 0;
        int have_view = 0;

        for (
            int r = 0;
            r <= old_last_row
                && out_r < cap_rows;
            r++
        ) {
            if (!have_view && r == term.view_row) {
                new_view_r = out_r;
                have_view = 1;
            }

            int end = old_cols - 1;
            while (
                end >= 0
                && term.buffer[(size_t)r * (size_t)old_cols + (size_t)end] == ' '
            ) {
                end--;
            }

            int row_len = end + 1;
            if (row_len < 0) {
                row_len = 0;
            }

            int take_cur = -1;
            if (r == cur_row) {
                take_cur = cur_col;
                if (take_cur > row_len) {
                    take_cur = row_len;
                }
            }

            for (
                int c = 0;
                c < row_len
                    && out_r < cap_rows;
                c++
            ) {
                if (!have_cur && r == cur_row && c == take_cur) {
                    new_cur_r = out_r;
                    new_cur_c = out_c;
                    have_cur = 1;
                }

                size_t dst = (size_t)out_r * (size_t)new_cols + (size_t)out_c;
                size_t src = (size_t)r * (size_t)old_cols + (size_t)c;

                nb_data[dst] = term.buffer[src];
                nfg_data[dst] = term.fg_colors[src];
                nbg_data[dst] = term.bg_colors[src];

                if (++out_c >= new_cols) {
                    out_c = 0;
                    out_r++;
                }
            }

            if (!have_cur && r == cur_row && take_cur == row_len) {
                new_cur_r = out_r;
                new_cur_c = out_c;
                have_cur = 1;
            }

            int hard_nl = (r < old_last_row && end < (old_cols - 1));
            if (hard_nl) {
                out_r++;
                out_c = 0;
            }
        }

        if (out_r >= cap_rows) {
            out_r = cap_rows - 1;
            out_c = 0;
        }

        buffer_.swap(nb);
        fg_colors_.swap(nfg);
        bg_colors_.swap(nbg);

        dirty_rows_.swap(ndr);
        dirty_x1_.swap(ndx1);
        dirty_x2_.swap(ndx2);

        sync_term_views();

        term.cols = new_cols;
        term.history_cap_rows = cap_rows;
        term.history_rows = out_r + 1;
        term.max_row = term.history_rows - 1;

        term.view_row = have_view ? new_view_r : term.view_row;

        if (term.view_row < 0) {
            term.view_row = 0;
        }

        if (term.view_row > term.max_row) {
            term.view_row = term.max_row;
        }

        term.row = have_cur ? new_cur_r : out_r;
        term.col = have_cur ? new_cur_c : out_c;

        if (term.row < 0) {
            term.row = 0;
        }

        if (term.row > term.max_row) {
            term.row = term.max_row;
        }

        if (term.col < 0) {
            term.col = 0;
        }

        if (term.col >= term.cols) {
            term.col = term.cols - 1;
        }

        term.full_redraw = 1;
        mark_all_dirty_locked();
        bump_seq_locked();
        bump_view_seq_locked();
    }

    void invalidate_view_locked() {
        term.full_redraw = 1;
        mark_all_dirty_locked();
        bump_view_seq_locked();
    }

    void bump_seq_locked() {
        term.seq++;
        if (term.seq == 0) {
            term.seq = 1;
        }
    }

    void bump_view_seq_locked() {
        term.view_seq++;
        if (term.view_seq == 0) {
            term.view_seq = 1;
        }
    }

    void mark_all_dirty_locked() {
        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        int rows = term.history_rows;
        if (rows < 1) {
            rows = 1;
        }

        if (rows > term.history_cap_rows) {
            rows = term.history_cap_rows;
        }

        if (
            !term.dirty_rows
            || !term.dirty_x1
            || !term.dirty_x2
        ) {
            term.full_redraw = 1;
            return;
        }

        for (int r = 0; r < rows; r++) {
            term.dirty_rows[r] = 1;
            term.dirty_x1[r] = 0;
            term.dirty_x2[r] = cols;
        }

        for (int r = rows; r < term.history_cap_rows; r++) {
            reset_dirty_row(r, cols);
        }
    }

    int dirty_extract_visible(
        uint8_t* out_rows,
        int* out_x1,
        int* out_x2,
        int out_rows_cap,
        int* out_full_redraw
    ) {
        if (out_full_redraw) {
            *out_full_redraw = 0;
        }

        if (
            !out_rows
            || !out_x1
            || !out_x2
            || out_rows_cap <= 0
        ) {
            return 0;
        }

        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        int view_rows = term.view_rows;
        if (view_rows <= 0) {
            view_rows = kDefaultRows;
        }

        int n = (view_rows < out_rows_cap) ? view_rows : out_rows_cap;

        int full = term.full_redraw;
        if (out_full_redraw) {
            *out_full_redraw = full ? 1 : 0;
        }

        if (
            full
            || !term.dirty_rows
            || !term.dirty_x1
            || !term.dirty_x2
        ) {
            for (int y = 0; y < n; y++) {
                out_rows[y] = 1;
                out_x1[y] = 0;
                out_x2[y] = cols;
            }

            term.full_redraw = 0;

            if (
                term.dirty_rows
                && term.dirty_x1
                && term.dirty_x2
            ) {
                int rows = term.history_rows;
                if (rows < 1) {
                    rows = 1;
                }

                if (rows > term.history_cap_rows) {
                    rows = term.history_cap_rows;
                }

                for (int r = 0; r < rows; r++) {
                    reset_dirty_row(r, cols);
                }
            }

            return n;
        }

        for (int y = 0; y < n; y++) {
            int src_row = term.view_row + y;

            if (src_row < 0 || src_row >= term.history_cap_rows) {
                out_rows[y] = 0;
                out_x1[y] = cols;
                out_x2[y] = -1;
                continue;
            }

            if (!term.dirty_rows[src_row]) {
                out_rows[y] = 0;
                out_x1[y] = cols;
                out_x2[y] = -1;
                continue;
            }

            out_rows[y] = 1;
            out_x1[y] = term.dirty_x1[src_row];
            out_x2[y] = term.dirty_x2[src_row];

            if (out_x1[y] < 0) {
                out_x1[y] = 0;
            }

            if (out_x2[y] > cols) {
                out_x2[y] = cols;
            }

            reset_dirty_row(src_row, cols);
        }

        return n;
    }

    TermState term{};

    kernel::SpinLock lock_;

    RawBuffer<char> buffer_;
    RawBuffer<uint32_t> fg_colors_;
    RawBuffer<uint32_t> bg_colors_;

    RawBuffer<uint8_t> dirty_rows_;
    RawBuffer<int> dirty_x1_;
    RawBuffer<int> dirty_x2_;

private:
    void sync_term_views() {
        term.buffer = buffer_.data();
        term.fg_colors = fg_colors_.data();
        term.bg_colors = bg_colors_.data();

        term.dirty_rows = dirty_rows_.data();
        term.dirty_x1 = dirty_x1_.data();
        term.dirty_x2 = dirty_x2_.data();
    }

    int ensure_rows_locked(int need_rows) {
        if (need_rows < 1) {
            need_rows = 1;
        }

        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        int old_cap = term.history_cap_rows;
        if (old_cap < 1) {
            old_cap = 0;
        }

        if (need_rows <= old_cap) {
            if (need_rows > term.history_rows) {
                term.history_rows = need_rows;
            }
            return 0;
        }

        int new_cap = old_cap ? old_cap : 1;
        while (new_cap < need_rows) {
            int next = new_cap << 1;
            if (next <= new_cap) {
                new_cap = need_rows;
                break;
            }
            new_cap = next;
        }

        size_t old_cells = (size_t)old_cap * (size_t)cols;
        size_t new_cells = (size_t)new_cap * (size_t)cols;

        RawBuffer<char> nb;
        RawBuffer<uint32_t> nfg;
        RawBuffer<uint32_t> nbg;

        RawBuffer<uint8_t> ndr;
        RawBuffer<int> ndx1;
        RawBuffer<int> ndx2;

        if (
            nb.reserve_elems(new_cells ? new_cells : 1) != 0
            || nfg.reserve_elems(new_cells ? new_cells : 1) != 0
            || nbg.reserve_elems(new_cells ? new_cells : 1) != 0
            || ndr.reserve_elems((size_t)new_cap ? (size_t)new_cap : 1u) != 0
            || ndx1.reserve_elems((size_t)new_cap ? (size_t)new_cap : 1u) != 0
            || ndx2.reserve_elems((size_t)new_cap ? (size_t)new_cap : 1u) != 0
        ) {
            return -1;
        }

        if (old_cells > 0) {
            memcpy(nb.data(), buffer_.data(), old_cells * sizeof(char));
            memcpy(nfg.data(), fg_colors_.data(), old_cells * sizeof(uint32_t));
            memcpy(nbg.data(), bg_colors_.data(), old_cells * sizeof(uint32_t));
        }

        if (old_cap > 0) {
            memcpy(ndr.data(), dirty_rows_.data(), (size_t)old_cap * sizeof(uint8_t));
            memcpy(ndx1.data(), dirty_x1_.data(), (size_t)old_cap * sizeof(int));
            memcpy(ndx2.data(), dirty_x2_.data(), (size_t)old_cap * sizeof(int));
        }

        buffer_.swap(nb);
        fg_colors_.swap(nfg);
        bg_colors_.swap(nbg);

        dirty_rows_.swap(ndr);
        dirty_x1_.swap(ndx1);
        dirty_x2_.swap(ndx2);

        sync_term_views();

        for (size_t i = old_cells; i < new_cells; i++) {
            term.buffer[i] = ' ';
            term.fg_colors[i] = term.curr_fg;
            term.bg_colors[i] = term.curr_bg;
        }

        for (int r = old_cap; r < new_cap; r++) {
            term.dirty_rows[r] = 1;
            term.dirty_x1[r] = 0;
            term.dirty_x2[r] = cols;
        }

        term.history_cap_rows = new_cap;

        if (need_rows > term.history_rows) {
            term.history_rows = need_rows;
        }

        return 0;
    }

    uint32_t effective_fg() const {
        return term.ansi_inverse ? term.curr_bg : term.curr_fg;
    }

    uint32_t effective_bg() const {
        return term.ansi_inverse ? term.curr_fg : term.curr_bg;
    }

    void dirty_mark_range_locked(int row, int x0, int x1) {
        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        if (
            !term.dirty_rows
            || !term.dirty_x1
            || !term.dirty_x2
        ) {
            term.full_redraw = 1;
            return;
        }

        if (row < 0 || row >= term.history_cap_rows) {
            return;
        }

        if (x0 < 0) {
            x0 = 0;
        }

        if (x1 > cols) {
            x1 = cols;
        }

        if (x0 >= x1) {
            return;
        }

        if (!term.dirty_rows[row]) {
            term.dirty_rows[row] = 1;
            term.dirty_x1[row] = x0;
            term.dirty_x2[row] = x1;
            return;
        }

        if (x0 < term.dirty_x1[row]) {
            term.dirty_x1[row] = x0;
        }

        if (x1 > term.dirty_x2[row]) {
            term.dirty_x2[row] = x1;
        }
    }

    void clear_row_range_locked(int row, int x0, int x1) {
        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        if (row < 0) {
            return;
        }

        if (x0 < 0) {
            x0 = 0;
        }

        if (x1 > cols) {
            x1 = cols;
        }

        if (x0 >= x1) {
            return;
        }

        if (ensure_rows_locked(row + 1) != 0) {
            return;
        }

        size_t base = (size_t)row * (size_t)cols + (size_t)x0;
        uint32_t fg = effective_fg();
        uint32_t bg = effective_bg();

        for (int x = x0; x < x1; x++) {
            term.buffer[base] = ' ';
            term.fg_colors[base] = fg;
            term.bg_colors[base] = bg;
            base++;
        }

        if (row >= term.history_rows) {
            term.history_rows = row + 1;
        }

        if (row > term.max_row) {
            term.max_row = row;
        }

        dirty_mark_range_locked(row, x0, x1);
        bump_seq_locked();
    }

    void clear_row_locked(int row) {
        if (row < 0) {
            return;
        }

        if (ensure_rows_locked(row + 1) != 0) {
            return;
        }

        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        size_t start = (size_t)row * (size_t)cols;

        for (int i = 0; i < cols; i++) {
            term.buffer[start + (size_t)i] = ' ';
            term.fg_colors[start + (size_t)i] = term.curr_fg;
            term.bg_colors[start + (size_t)i] = term.curr_bg;
        }

        if (row >= term.history_rows) {
            term.history_rows = row + 1;
        }

        dirty_mark_range_locked(row, 0, cols);
        bump_seq_locked();
    }

    void ansi_reset_locked() {
        term.esc_state = 0;
        term.csi_in_param = 0;
        term.csi_param_value = 0;
        term.csi_param_count = 0;
    }

    void csi_push_param_locked() {
        if (term.csi_param_count < (int)(sizeof(term.csi_params) / sizeof(term.csi_params[0]))) {
            int v = term.csi_in_param ? term.csi_param_value : 0;
            term.csi_params[term.csi_param_count++] = v;
        }

        term.csi_param_value = 0;
        term.csi_in_param = 0;
    }

    int csi_param_locked(int idx, int def) const {
        if (idx < 0 || idx >= term.csi_param_count) {
            return def;
        }

        int v = term.csi_params[idx];
        return (v == 0) ? def : v;
    }

    void set_cursor_locked(int row, int col) {
        int cols = term.cols;
        if (cols <= 0) {
            cols = kDefaultCols;
        }

        if (row < 0) {
            row = 0;
        }

        if (col < 0) {
            col = 0;
        }

        if (col >= cols) {
            col = cols - 1;
        }

        if (ensure_rows_locked(row + 1) != 0) {
            return;
        }

        term.row = row;
        term.col = col;

        if (term.row >= term.history_rows) {
            term.history_rows = term.row + 1;
        }

        if (term.row > term.max_row) {
            term.max_row = term.row;
        }

        bump_view_seq_locked();
    }

    void clear_all_locked() {
        int rows = term.history_rows;
        if (rows < 1) {
            rows = 1;
        }

        for (int r = 0; r < rows; r++) {
            clear_row_range_locked(r, 0, term.cols);
        }

        term.col = 0;
        term.row = 0;
        term.view_row = 0;
        term.max_row = 0;
        term.history_rows = 1;

        invalidate_view_locked();
    }

    void apply_sgr_locked() {
        if (term.csi_param_count == 0) {
            term.curr_fg = term.def_fg;
            term.curr_bg = term.def_bg;
            term.ansi_bright = 0;
            term.ansi_inverse = 0;
            return;
        }

        for (int i = 0; i < term.csi_param_count; i++) {
            int p = term.csi_params[i];

            if (p == 0) {
                term.curr_fg = term.def_fg;
                term.curr_bg = term.def_bg;
                term.ansi_bright = 0;
                term.ansi_inverse = 0;
            } else if (p == 1) {
                term.ansi_bright = 1;
            } else if (p == 22) {
                term.ansi_bright = 0;
            } else if (p == 7) {
                term.ansi_inverse = 1;
            } else if (p == 27) {
                term.ansi_inverse = 0;
            } else if (p == 39) {
                term.curr_fg = term.def_fg;
            } else if (p == 49) {
                term.curr_bg = term.def_bg;
            } else if (p >= 30 && p <= 37) {
                int idx = p - 30;
                term.curr_fg = term.ansi_bright ? kAnsiBrightColors[idx] : kAnsiColors[idx];
            } else if (p >= 90 && p <= 97) {
                int idx = p - 90;
                term.curr_fg = kAnsiBrightColors[idx];
            } else if (p >= 40 && p <= 47) {
                int idx = p - 40;
                term.curr_bg = term.ansi_bright ? kAnsiBrightColors[idx] : kAnsiColors[idx];
            } else if (p >= 100 && p <= 107) {
                int idx = p - 100;
                term.curr_bg = kAnsiBrightColors[idx];
            }
        }
    }

    void handle_csi_locked(char cmd) {
        if (cmd == 'A') {
            int n = csi_param_locked(0, 1);
            set_cursor_locked(term.row - n, term.col);
        } else if (cmd == 'B') {
            int n = csi_param_locked(0, 1);
            set_cursor_locked(term.row + n, term.col);
        } else if (cmd == 'C') {
            int n = csi_param_locked(0, 1);
            set_cursor_locked(term.row, term.col + n);
        } else if (cmd == 'D') {
            int n = csi_param_locked(0, 1);
            set_cursor_locked(term.row, term.col - n);
        } else if (cmd == 'H' || cmd == 'f') {
            int r = csi_param_locked(0, 1) - 1;
            int c = csi_param_locked(1, 1) - 1;
            set_cursor_locked(r, c);
        } else if (cmd == 'J') {
            int mode = term.csi_param_count > 0 ? term.csi_params[0] : 0;

            if (mode == 2) {
                clear_all_locked();
            } else if (mode == 0) {
                clear_row_range_locked(term.row, term.col, term.cols);

                for (int r = term.row + 1; r < term.view_row + term.view_rows; r++) {
                    clear_row_range_locked(r, 0, term.cols);
                }
            } else if (mode == 1) {
                for (int r = term.view_row; r < term.row; r++) {
                    clear_row_range_locked(r, 0, term.cols);
                }

                clear_row_range_locked(term.row, 0, term.col + 1);
            }
        } else if (cmd == 'K') {
            int mode = term.csi_param_count > 0 ? term.csi_params[0] : 0;

            if (mode == 0) {
                clear_row_range_locked(term.row, term.col, term.cols);
            } else if (mode == 1) {
                clear_row_range_locked(term.row, 0, term.col + 1);
            } else if (mode == 2) {
                clear_row_range_locked(term.row, 0, term.cols);
            }
        } else if (cmd == 'm') {
            apply_sgr_locked();
        } else if (cmd == 's') {
            term.saved_row = term.row;
            term.saved_col = term.col;
        } else if (cmd == 'u') {
            set_cursor_locked(term.saved_row, term.saved_col);
        }
    }

    void reset_dirty_row(int row, int cols) {
        if (
            !term.dirty_rows
            || !term.dirty_x1
            || !term.dirty_x2
        ) {
            return;
        }

        if (row < 0 || row >= term.history_cap_rows) {
            return;
        }

        term.dirty_rows[row] = 0;
        term.dirty_x1[row] = cols;
        term.dirty_x2[row] = -1;
    }
};

static size_t grow_pow2(size_t cur, size_t need) {
    size_t cap = cur ? cur : 1024u;

    while (cap < need) {
        size_t next = cap << 1;
        if (next <= cap) {
            cap = need;
            break;
        }
        cap = next;
    }

    return cap;
}

static int grow_pow2_i(int cur, int need) {
    int cap = cur ? cur : 128;

    while (cap < need) {
        int next = cap << 1;
        if (next <= cap) {
            cap = need;
            break;
        }
        cap = next;
    }

    return cap;
}

static int snapshot_reserve_cells(TermSnapshotImpl& impl, size_t cells) {
    if (cells <= impl.cap_cells) {
        return 0;
    }

    size_t new_cap = grow_pow2(impl.cap_cells, cells);

    CharBuf nb(new (kernel::nothrow) char[new_cap ? new_cap : 1u]);
    U32Buf nfg(new (kernel::nothrow) uint32_t[new_cap ? new_cap : 1u]);
    U32Buf nbg(new (kernel::nothrow) uint32_t[new_cap ? new_cap : 1u]);

    if (!nb || !nfg || !nbg) {
        return -1;
    }

    impl.buf = kernel::move(nb);
    impl.fg = kernel::move(nfg);
    impl.bg = kernel::move(nbg);
    impl.cap_cells = new_cap;

    return 0;
}

static int snapshot_reserve_rows(TermSnapshotImpl& impl, int rows) {
    if (rows <= impl.cap_rows) {
        return 0;
    }

    int new_cap = grow_pow2_i(impl.cap_rows, rows);

    U8Buf ndr(new (kernel::nothrow) uint8_t[(size_t)new_cap ? (size_t)new_cap : 1u]);
    I32Buf ndx1(new (kernel::nothrow) int[(size_t)new_cap ? (size_t)new_cap : 1u]);
    I32Buf ndx2(new (kernel::nothrow) int[(size_t)new_cap ? (size_t)new_cap : 1u]);

    if (!ndr || !ndx1 || !ndx2) {
        return -1;
    }

    impl.dirty_rows = kernel::move(ndr);
    impl.dirty_x1 = kernel::move(ndx1);
    impl.dirty_x2 = kernel::move(ndx2);
    impl.cap_rows = new_cap;

    return 0;
}

TermSnapshot::TermSnapshot()
    : impl_(kernel::make_unique<TermSnapshotImpl>()) {
}

TermSnapshot::~TermSnapshot() = default;

int TermSnapshot::cols() const {
    return impl_ ? impl_->state.cols : 0;
}

int TermSnapshot::view_rows() const {
    return impl_ ? impl_->state.view_rows : 0;
}

uint64_t TermSnapshot::seq() const {
    return impl_ ? impl_->state.seq : 0;
}

uint64_t TermSnapshot::view_seq() const {
    return impl_ ? impl_->state.view_seq : 0;
}

int TermSnapshot::cursor_row() const {
    return impl_ ? impl_->state.row : 0;
}

int TermSnapshot::cursor_col() const {
    return impl_ ? impl_->state.col : 0;
}

uint32_t TermSnapshot::curr_bg() const {
    return impl_ ? impl_->state.curr_bg : 0u;
}

int TermSnapshot::full_redraw() const {
    return impl_ ? impl_->state.full_redraw : 0;
}

int TermSnapshot::mark_dirty_cell(int row, int col) {
    if (!impl_) {
        return -1;
    }

    TermState& s = impl_->state;
    if (
        !s.dirty_rows
        || !s.dirty_x1
        || !s.dirty_x2
    ) {
        return -1;
    }

    int cols = s.cols;
    if (cols < 1) {
        cols = 1;
    }

    int rows = s.view_rows;
    if (rows < 1) {
        rows = 1;
    }

    if (row < 0 || row >= rows) {
        return -1;
    }

    int x = col;
    if (x < 0) {
        x = 0;
    }

    if (x >= cols) {
        x = cols - 1;
    }

    s.dirty_rows[row] = 1;

    if (s.dirty_x1[row] > x) {
        s.dirty_x1[row] = x;
    }

    if (s.dirty_x2[row] < x + 1) {
        s.dirty_x2[row] = x + 1;
    }

    return 0;
}

int TermSnapshot::dirty_bbox(int& out_x1, int& out_y1, int& out_x2, int& out_y2) const {
    out_x1 = 0;
    out_y1 = 0;
    out_x2 = 0;
    out_y2 = 0;

    if (!impl_) {
        return -1;
    }

    const TermState& s = impl_->state;
    if (
        !s.dirty_rows
        || !s.dirty_x1
        || !s.dirty_x2
    ) {
        return -1;
    }

    int cols = s.cols;
    if (cols < 1) {
        cols = 1;
    }

    int rows = s.view_rows;
    if (rows < 1) {
        rows = 1;
    }

    int bb_x1 = cols;
    int bb_y1 = rows;
    int bb_x2 = -1;
    int bb_y2 = -1;

    for (int y = 0; y < rows; y++) {
        if (!s.dirty_rows[y]) {
            continue;
        }

        int x0 = s.dirty_x1[y];
        int x1 = s.dirty_x2[y];

        if (x0 < 0) {
            x0 = 0;
        }

        if (x1 > cols) {
            x1 = cols;
        }

        if (x0 >= x1) {
            continue;
        }

        if (x0 < bb_x1) {
            bb_x1 = x0;
        }

        if (y < bb_y1) {
            bb_y1 = y;
        }

        if (x1 > bb_x2) {
            bb_x2 = x1;
        }

        if (y + 1 > bb_y2) {
            bb_y2 = y + 1;
        }
    }

    if (bb_x1 > bb_x2 || bb_y1 > bb_y2) {
        return -1;
    }

    out_x1 = bb_x1;
    out_y1 = bb_y1;
    out_x2 = bb_x2;
    out_y2 = bb_y2;

    return 0;
}

int TermSnapshot::dirty_row_range(int row, int& out_x1, int& out_x2) const {
    out_x1 = 0;
    out_x2 = 0;

    if (!impl_) {
        return -1;
    }

    const TermState& s = impl_->state;
    if (
        !s.dirty_rows
        || !s.dirty_x1
        || !s.dirty_x2
    ) {
        return -1;
    }

    int rows = s.view_rows;
    if (rows < 1) {
        rows = 1;
    }

    if (row < 0 || row >= rows) {
        return -1;
    }

    if (!s.dirty_rows[row]) {
        return -1;
    }

    out_x1 = s.dirty_x1[row];
    out_x2 = s.dirty_x2[row];

    if (out_x1 < 0) {
        out_x1 = 0;
    }

    int cols = s.cols;
    if (cols < 1) {
        cols = 1;
    }

    if (out_x2 > cols) {
        out_x2 = cols;
    }

    if (out_x1 >= out_x2) {
        return -1;
    }

    return 0;
}

static int clamp_cell(const TermState& s, int& io_row, int& io_col) {
    int cols = s.cols;
    if (cols < 1) {
        return -1;
    }

    int rows = s.view_rows;
    if (rows < 1) {
        return -1;
    }

    if (io_row < 0 || io_row >= rows) {
        return -1;
    }

    if (io_col < 0) {
        io_col = 0;
    }

    if (io_col >= cols) {
        io_col = cols - 1;
    }

    return 0;
}

char TermSnapshot::ch_at(int row, int col) const {
    if (!impl_) {
        return ' ';
    }

    const TermState& s = impl_->state;
    if (!s.buffer) {
        return ' ';
    }

    int r = row;
    int c = col;
    if (clamp_cell(s, r, c) != 0) {
        return ' ';
    }

    size_t i = (size_t)r * (size_t)s.cols + (size_t)c;
    return s.buffer[i];
}

uint32_t TermSnapshot::fg_at(int row, int col) const {
    if (!impl_) {
        return 0;
    }

    const TermState& s = impl_->state;
    if (!s.fg_colors) {
        return 0;
    }

    int r = row;
    int c = col;
    if (clamp_cell(s, r, c) != 0) {
        return 0;
    }

    size_t i = (size_t)r * (size_t)s.cols + (size_t)c;
    return s.fg_colors[i];
}

uint32_t TermSnapshot::bg_at(int row, int col) const {
    if (!impl_) {
        return 0;
    }

    const TermState& s = impl_->state;
    if (!s.bg_colors) {
        return 0;
    }

    int r = row;
    int c = col;
    if (clamp_cell(s, r, c) != 0) {
        return 0;
    }

    size_t i = (size_t)r * (size_t)s.cols + (size_t)c;
    return s.bg_colors[i];
}

Term::Term(int cols, int view_rows)
    : impl_(kernel::make_unique<TermImpl>(cols, view_rows)) {
}

Term::~Term() = default;

void Term::write(const char* buf, uint32_t len) {
    if (!impl_ || !buf || len == 0) {
        return;
    }

    kernel::SpinLockGuard g(impl_->lock_);
    impl_->write_locked(buf, len);
}

void Term::print(const char* s) {
    if (!impl_ || !s) {
        return;
    }

    kernel::SpinLockGuard g(impl_->lock_);
    impl_->print_locked(s);
}

void Term::putc(char c) {
    if (!impl_) {
        return;
    }

    kernel::SpinLockGuard g(impl_->lock_);
    impl_->putc_locked(c);
}

void Term::set_colors(uint32_t fg, uint32_t bg) {
    if (!impl_) {
        return;
    }

    kernel::SpinLockGuard g(impl_->lock_);

    impl_->term.curr_fg = fg;
    impl_->term.curr_bg = bg;
    impl_->term.def_fg = fg;
    impl_->term.def_bg = bg;

    impl_->invalidate_view_locked();
}

int Term::get_winsz(uint16_t& out_cols, uint16_t& out_rows) const {
    if (!impl_) {
        return -1;
    }

    kernel::SpinLockGuard g(impl_->lock_);

    out_cols = (uint16_t)(impl_->term.cols > 0 ? impl_->term.cols : 1);
    out_rows = (uint16_t)(impl_->term.view_rows > 0 ? impl_->term.view_rows : 1);

    return 0;
}

int Term::set_winsz(uint16_t cols, uint16_t rows) {
    if (!impl_) {
        return -1;
    }

    kernel::SpinLockGuard g(impl_->lock_);

    if (cols > 0) {
        impl_->term.cols = (int)cols;
    }

    if (rows > 0) {
        impl_->term.view_rows = (int)rows;
    }

    impl_->reflow_locked(impl_->term.cols);

    return 0;
}

int Term::scroll(int delta) {
    if (!impl_) {
        return -1;
    }

    kernel::SpinLockGuard g(impl_->lock_);

    int view_rows = impl_->term.view_rows;
    if (view_rows < 1) {
        view_rows = 1;
    }

    int max_view_row = impl_->term.max_row - view_rows + 1;
    if (max_view_row < 0) {
        max_view_row = 0;
    }

    int old_view_row = impl_->term.view_row;

    if (delta == 0) {
        impl_->term.view_row = max_view_row;
    } else if (delta > 0) {
        int next = impl_->term.view_row - delta;
        if (next < 0) {
            next = 0;
        }
        impl_->term.view_row = next;
    } else {
        int next = impl_->term.view_row - delta;
        if (next > max_view_row) {
            next = max_view_row;
        }
        impl_->term.view_row = next;
    }

    if (impl_->term.view_row != old_view_row) {
        impl_->invalidate_view_locked();
    }

    return 0;
}

void Term::invalidate_view() {
    if (!impl_) {
        return;
    }

    kernel::SpinLockGuard g(impl_->lock_);
    impl_->invalidate_view_locked();
}

uint64_t Term::seq() const {
    if (!impl_) {
        return 0;
    }

    kernel::SpinLockGuard g(impl_->lock_);
    return impl_->term.seq;
}

uint64_t Term::view_seq() const {
    if (!impl_) {
        return 0;
    }

    kernel::SpinLockGuard g(impl_->lock_);
    return impl_->term.view_seq;
}

int Term::capture_snapshot(TermSnapshot& out_snapshot) {
    if (!impl_ || !out_snapshot.impl_) {
        return -1;
    }

    kernel::SpinLockGuard g(impl_->lock_);

    TermState& dst = out_snapshot.impl_->state;
    dst = {};

    int cols = impl_->term.cols > 0 ? impl_->term.cols : kDefaultCols;
    int view_rows = impl_->term.view_rows > 0 ? impl_->term.view_rows : kDefaultRows;

    if (snapshot_reserve_rows(*out_snapshot.impl_, view_rows) != 0) {
        return -1;
    }

    size_t cells = (size_t)cols * (size_t)view_rows;
    if (snapshot_reserve_cells(*out_snapshot.impl_, cells) != 0) {
        return -1;
    }

    dst.cols = cols;
    dst.view_rows = view_rows;
    dst.view_row = 0;

    dst.history_rows = view_rows;
    dst.history_cap_rows = view_rows;

    dst.curr_fg = impl_->term.curr_fg;
    dst.curr_bg = impl_->term.curr_bg;
    dst.def_fg = impl_->term.def_fg;
    dst.def_bg = impl_->term.def_bg;

    dst.seq = impl_->term.seq;
    dst.view_seq = impl_->term.view_seq;

    dst.row = impl_->term.row - impl_->term.view_row;
    dst.col = impl_->term.col;
    dst.max_row = view_rows - 1;

    dst.buffer = out_snapshot.impl_->buf.get();
    dst.fg_colors = out_snapshot.impl_->fg.get();
    dst.bg_colors = out_snapshot.impl_->bg.get();

    dst.dirty_rows = out_snapshot.impl_->dirty_rows.get();
    dst.dirty_x1 = out_snapshot.impl_->dirty_x1.get();
    dst.dirty_x2 = out_snapshot.impl_->dirty_x2.get();

    dst.full_redraw = 0;

    for (int y = 0; y < view_rows; y++) {
        dst.dirty_rows[y] = 0;
        dst.dirty_x1[y] = cols;
        dst.dirty_x2[y] = -1;
    }

    int full_redraw = 0;

    int n = impl_->dirty_extract_visible(
        dst.dirty_rows,
        dst.dirty_x1,
        dst.dirty_x2,
        out_snapshot.impl_->cap_rows,
        &full_redraw
    );

    dst.full_redraw = full_redraw;

    uint32_t fg_def = impl_->term.curr_fg;
    uint32_t bg_def = impl_->term.curr_bg;

    int src_view_row = impl_->term.view_row;
    int src_history_rows = impl_->term.history_rows;

    for (int y = 0; y < n; y++) {
        if (!dst.dirty_rows[y]) {
            continue;
        }

        int x0 = dst.dirty_x1[y];
        int x1 = dst.dirty_x2[y];

        if (x0 < 0) {
            x0 = 0;
        }

        if (x1 > cols) {
            x1 = cols;
        }

        if (x0 >= x1) {
            continue;
        }

        int src_row = src_view_row + y;
        size_t row_dst = (size_t)y * (size_t)cols;

        if (src_row < 0 || src_row >= src_history_rows) {
            for (int x = x0; x < x1; x++) {
                size_t i = row_dst + (size_t)x;
                dst.buffer[i] = ' ';
                dst.fg_colors[i] = fg_def;
                dst.bg_colors[i] = bg_def;
            }
            continue;
        }

        size_t row_src = (size_t)src_row * (size_t)cols;

        memcpy(
            dst.buffer + row_dst + (size_t)x0,
            impl_->term.buffer + row_src + (size_t)x0,
            (size_t)(x1 - x0)
        );

        memcpy(
            dst.fg_colors + row_dst + (size_t)x0,
            impl_->term.fg_colors + row_src + (size_t)x0,
            (size_t)(x1 - x0) * sizeof(uint32_t)
        );

        memcpy(
            dst.bg_colors + row_dst + (size_t)x0,
            impl_->term.bg_colors + row_src + (size_t)x0,
            (size_t)(x1 - x0) * sizeof(uint32_t)
        );
    }

    return 0;
}

int Term::capture_cell(TermSnapshot& snapshot, int rel_row, int col) {
    if (!impl_ || !snapshot.impl_) {
        return -1;
    }

    TermState& dst = snapshot.impl_->state;

    int cols = dst.cols;
    if (cols < 1) {
        return -1;
    }

    int view_rows = dst.view_rows;
    if (view_rows < 1) {
        return -1;
    }

    if (rel_row < 0 || rel_row >= view_rows) {
        return -1;
    }

    int x = col;
    if (x < 0) {
        x = 0;
    }

    if (x >= cols) {
        x = cols - 1;
    }

    kernel::SpinLockGuard g(impl_->lock_);

    int src_row = impl_->term.view_row + rel_row;
    size_t dst_i = (size_t)rel_row * (size_t)cols + (size_t)x;

    if (src_row < 0 || src_row >= impl_->term.history_rows) {
        dst.buffer[dst_i] = ' ';
        dst.fg_colors[dst_i] = impl_->term.curr_fg;
        dst.bg_colors[dst_i] = impl_->term.curr_bg;
        return 0;
    }

    size_t src_i = (size_t)src_row * (size_t)cols + (size_t)x;

    dst.buffer[dst_i] = impl_->term.buffer[src_i];
    dst.fg_colors[dst_i] = impl_->term.fg_colors[src_i];
    dst.bg_colors[dst_i] = impl_->term.bg_colors[src_i];

    return 0;
}

}
