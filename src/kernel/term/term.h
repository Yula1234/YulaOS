#ifndef KERNEL_TERM_H
#define KERNEL_TERM_H


#include <stdint.h>
#include <stddef.h>

#include <lib/cpp/unique_ptr.h>

namespace kernel::term {

class TermSnapshotImpl;
class TermImpl;

class TermSnapshot {
public:
    TermSnapshot();
    TermSnapshot(const TermSnapshot&) = delete;
    TermSnapshot& operator=(const TermSnapshot&) = delete;

    TermSnapshot(TermSnapshot&&) = delete;
    TermSnapshot& operator=(TermSnapshot&&) = delete;

    ~TermSnapshot();

    int cols() const;
    int view_rows() const;

    uint64_t seq() const;
    uint64_t view_seq() const;

    int cursor_row() const;
    int cursor_col() const;

    uint32_t curr_bg() const;
    int full_redraw() const;

    int mark_dirty_cell(int row, int col);

    int dirty_bbox(int& out_x1, int& out_y1, int& out_x2, int& out_y2) const;

    int dirty_row_range(int row, int& out_x1, int& out_x2) const;

    char ch_at(int row, int col) const;
    
    uint32_t fg_at(int row, int col) const;
    uint32_t bg_at(int row, int col) const;

private:
    friend class Term;
    friend class VgaTermRenderer;

    kernel::unique_ptr<TermSnapshotImpl> impl_;
};

class Term {
public:
    Term(int cols, int view_rows);
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    Term(Term&&) = delete;
    Term& operator=(Term&&) = delete;

    ~Term();

    void write(const char* buf, uint32_t len);
    void print(const char* s);
    void putc(char c);

    void set_colors(uint32_t fg, uint32_t bg);

    int get_winsz(uint16_t& out_cols, uint16_t& out_rows) const;
    int set_winsz(uint16_t cols, uint16_t rows);

    int scroll(int delta);

    void invalidate_view();

    uint64_t seq() const;
    uint64_t view_seq() const;

    int capture_snapshot(TermSnapshot& out_snapshot);
    int capture_cell(TermSnapshot& snapshot, int rel_row, int col);

private:
    kernel::unique_ptr<TermImpl> impl_;
};

class VgaTermRenderer {
public:
    void render(const TermSnapshot& snapshot, int win_x, int win_y) const;
};

}

#endif
