# yulaOS Kernel Documentation

## 1. System Architecture Overview
yulaOS is a 32-bit, monolithic, preemptive multitasking operating system kernel for the x86 architecture. It features a custom graphical user interface (GUI) embedded within the kernel, a proprietary file system (YulaFS), and a UNIX-like process model.

**Key Technical Specifications:**
*   **Architecture:** x86 (IA-32).
*   **Boot Protocol:** Multiboot 1 compliant.
*   **Kernel Type:** Monolithic (Drivers and GUI reside in ring 0).
*   **Multitasking:** Preemptive Round-Robin Scheduler.
*   **Binary Format:** ELF32 (Executable and Linkable Format).
*   **Graphics:** VESA Linear Framebuffer with SSE software rendering.

## 2. Memory Management (MM)

The memory management subsystem handles physical allocation, virtual paging, and the kernel heap.

### 2.1. Physical Memory Manager (PMM)
*   **Algorithm:** Bitmap-based allocator.
*   **Granularity:** 4KB blocks (pages).
*   **Implementation:** The entire physical memory map is represented by a bit array (`bitmap`). Allocation searches for the first free bit (0), sets it, and calculates the physical address.
*   **Limits:** Currently configured to manage up to 128MB of RAM.

### 2.2. Virtual Memory Manager (Paging)
*   **Structure:** Standard 2-level x86 paging (Page Directory -> Page Tables).
*   **Mapping Strategy:**
    *   **Kernel Space:** Identity mapped or higher-half mapped depending on the specific region (Video memory is explicitly mapped).
    *   **User Space:** Processes receive distinct Page Directories (`CR3` switching).
    *   **COW/Shared:** Basic support for shared memory pages (e.g., window buffers mapping).
*   **Protection:** Supervisor (Ring 0) and User (Ring 3) bits are enforced.

### 2.3. Kernel Heap
*   **Allocator:** Linked-list based `kmalloc` with boundary coalescing (merging adjacent free blocks).
*   **Expansion:** Automatically requests new pages from PMM and maps them into the kernel virtual address space when the heap is exhausted.
*   **Alignment:** Supports page-aligned allocation (`kmalloc_a`) required for DMA buffers and paging structures.

## 3. Process Management & Scheduling

### 3.1. Process Model
*   **Structure:** Represented by `task_t`.
*   **Resources:** Each task maintains its own:
    *   Virtual Address Space (Page Directory).
    *   Kernel Stack (`kstack`) for ring 0 operations and interrupt handling.
    *   File Descriptors table (pointers to VFS nodes).
    *   Signal handlers (POSIX-style implementation).
*   **Loading:** The kernel includes a native ELF parser (`elf.h`, `proc_spawn_elf`) that reads headers, maps segments (`PT_LOAD`) into memory, and sets up the user stack.

### 3.2. Scheduler
*   **Algorithm:** Preemptive Round-Robin.
*   **Timing:** Driven by the Local APIC (LAPIC) timer, calibrated to approximately 100Hz.
*   **Context Switching:**
    *   Performed in assembly (`ctx_switch`).
    *   Saves Callee-saved registers (EBP, EBX, ESI, EDI) and ESP.
    *   Updates the TSS (Task State Segment) `esp0` field to ensure subsequent interrupts land on the correct kernel stack.
*   **States:** `TASK_RUNNING`, `TASK_RUNNABLE`, `TASK_WAITING`, `TASK_ZOMBIE`.
*   **Synchronization:** Implements Spinlocks (`spinlock_t`) for atomic kernel operations and `wait/notify` mechanisms for IPC.

## 4. Graphical User Interface (GUI)

The GUI subsystem is tightly integrated into the kernel (`kernel/gui_task.c`, `window.c`).

### 4.1. Window Manager (Compositor)
*   **Architecture:** Stacking window manager with Z-order sorting.
*   **Buffering:** Each window possesses a private framebuffer (`canvas`). Applications render to this canvas (via `syscall 21` memory mapping).
*   **Compositing:** The kernel blits active window canvases onto the backbuffer, respecting Z-order and transparency.
*   **Dirty Rectangles:** To optimize performance, the system tracks "dirty" regions (`dirty_x1`, `dirty_y1`, etc.). Only changed screen areas are redrawn and flipped to VRAM.

### 4.2. Rendering Engine (`drivers/vga.c`)
*   **Target:** VESA Linear Framebuffer (32-bit depth).
*   **Optimization:** Heavily utilizes **SSE2 instructions** (`movups`, `movntdq`, `pmullw` for alpha blending) to accelerate memory copies, rect fills, and font rendering.
*   **Features:**
    *   Alpha Blending (transparency).
    *   Hardware-accelerated (via SSE) text rendering.
    *   Software double-buffering.

## 5. File System (VFS & YulaFS)

### 5.1. Virtual File System (VFS)
*   **Abstraction:** Provides a unified interface (`open`, `read`, `write`, `close`) for different resources.
*   **Nodes:** Supports file nodes, directory nodes, and character devices (`/dev/kbd`, `/dev/console`).
*   **Pipes:** Implements kernel-level pipes (`fs/pipe.c`) with circular buffers for Inter-Process Communication (IPC).

### 5.2. YulaFS (Native File System)
*   **Type:** Simple Inode-based filesystem.
*   **Layout:**
    1.  **Superblock:** Magic signature and FS geometry.
    2.  **Inode Bitmap & Block Bitmap:** Track free/used resources.
    3.  **Inode Table:** Stores file metadata.
    4.  **Data Blocks:** Actual file content.
*   **Addressing:** Supports direct blocks and single-indirect blocks, allowing files larger than the direct block limit.
*   **Driver:** Uses ATA PIO and Bus Master DMA for disk access.

## 6. Hardware Abstraction Layer (HAL) & Drivers

*   **Interrupts:**
    *   **GDT/IDT:** Standard protected mode descriptor tables.
    *   **PIC:** Remapped 8259 PIC (mostly masked in favor of APIC).
    *   **APIC:** Local APIC configuration for high-precision timer interrupts.
*   **ATA Driver:** Detects PCI IDE controllers and utilizes Bus Master DMA (Direct Memory Access) for high-speed disk I/O, falling back to PIO only for control commands.
*   **Input:**
    *   **Keyboard:** PS/2 controller driver with scan code translation and buffer queue.
    *   **Mouse:** PS/2 mouse driver with packet parsing (3-byte protocol) for X/Y deltas and buttons.

## 7. System Calls (API) Reference

The yulaOS kernel exposes its services to user space via a software interrupt mechanism.

**Calling Convention:**
*   **Interrupt:** `0x80`
*   **Syscall Number:** `EAX`
*   **Argument 1:** `EBX`
*   **Argument 2:** `ECX`
*   **Argument 3:** `EDX`
*   **Return Value:** `EAX` (Negative values usually indicate errors).

---

### 7.1. Process Management

#### `exit` (0)
Terminates the calling process immediately. All file descriptors are closed, and memory is reclaimed.
*   **EBX:** `int status` (Exit code, currently unused by parent).
*   **Return:** Does not return.

#### `getpid` (2)
Returns the Process ID (PID) of the calling process.
*   **Return:** `pid_t` (Always positive).

#### `sleep` (7)
Suspends execution of the calling process for a specified number of milliseconds.
*   **EBX:** `uint32_t ms` (Milliseconds to sleep).
*   **Return:** 0 on success.

#### `sbrk` (8)
Changes the program break (end of the process's data segment). Used by `malloc`.
*   **EBX:** `int increment` (Bytes to add/remove. If 0, returns current break).
*   **Return:** Previous program break address on success, or -1 on failure (OOM).

#### `kill` (9)
Sends a termination signal to a specific process.
*   **EBX:** `int pid` (Target Process ID).
*   **Return:** 0 on success, -1 if PID not found.

#### `usleep` (11)
Suspends execution for microsecond resolution (high precision).
*   **EBX:** `uint32_t us` (Microseconds).
*   **Return:** 0 on success.

#### `signal` (17)
Registers a user-space function to handle specific signals (e.g., SIGINT).
*   **EBX:** `int signum` (Signal number).
*   **ECX:** `void* handler` (Function pointer to handler).
*   **Return:** 0 on success, -1 on error.

#### `sigreturn` (18)
Returns from a signal handler, restoring the cpu context of the interrupted code.
*   **Note:** Not called directly by user; inserted by the kernel into the stack frame.

---

### 7.2. File I/O & Filesystem

#### `open` (3)
Opens a file or device and returns a file descriptor.
*   **EBX:** `const char* path` (Null-terminated string).
*   **ECX:** `int flags` (0 = Read, 1 = Create/Write).
*   **Return:** File descriptor (>= 0) on success, -1 on error.

#### `read` (4)
Reads data from an open file descriptor into a buffer.
*   **EBX:** `int fd` (File descriptor).
*   **ECX:** `void* buf` (Destination buffer).
*   **EDX:** `uint32_t count` (Bytes to read).
*   **Return:** Number of bytes read, 0 on EOF, -1 on error.

#### `write` (5)
Writes data from a buffer to an open file descriptor.
*   **EBX:** `int fd` (File descriptor).
*   **ECX:** `const void* buf` (Source buffer).
*   **EDX:** `uint32_t count` (Bytes to write).
*   **Return:** Number of bytes written, -1 on error.

#### `close` (6)
Closes a file descriptor.
*   **EBX:** `int fd`.
*   **Return:** 0 on success.

#### `mkdir` (13)
Creates a new directory.
*   **EBX:** `const char* path`.
*   **Return:** 0 on success, -1 on error.

#### `unlink` (14)
Removes a file entry (standard `rm` implementation).
*   **EBX:** `const char* path`.
*   **Return:** 0 on success, -1 on error.

#### `pipe` (29)
Creates a unidirectional data channel.
*   **EBX:** `int* fds` (Pointer to an array of 2 integers).
    *   `fds[0]` will receive the read end.
    *   `fds[1]` will receive the write end.
*   **Return:** 0 on success, -1 on error.

#### `dup2` (30)
Duplicates a file descriptor.
*   **EBX:** `int oldfd`.
*   **ECX:** `int newfd`.
*   **Return:** `newfd` on success, -1 on error.

---

### 7.3. Graphical User Interface (yulaGUI)

#### `create_window` (20)
Creates a new window managed by the kernel compositor.
*   **EBX:** `int width` (Width in pixels).
*   **ECX:** `int height` (Height in pixels).
*   **EDX:** `const char* title` (Window title).
*   **Return:** `int win_id` (Window handle) or -1 on failure.

#### `map_window` (21)
Maps the window's backbuffer into the user process's virtual address space.
*   **EBX:** `int win_id`.
*   **Return:** `void*` (Virtual address of the canvas, e.g., 0x40000000).

#### `update_window` (22)
Marks the window as "dirty," triggering the kernel compositor to redraw it.
*   **EBX:** `int win_id`.
*   **Return:** 0.

#### `get_event` (23)
Polls for input events (mouse/keyboard) targeting the window.
*   **EBX:** `int win_id`.
*   **ECX:** `yula_event_t* event` (Pointer to event struct).
*   **Return:** 1 if event received, 0 if queue is empty.

#### `set_console_color` (28)
Sets the text attributes for the built-in terminal emulation layer.
*   **EBX:** `uint32_t fg` (Foreground color, RGB hex).
*   **ECX:** `uint32_t bg` (Background color, RGB hex).
*   **Return:** 0.

---

### 7.4. Miscellaneous

#### `print` (1)
Legacy/Debug system call to print a string directly to the active console.
*   **EBX:** `const char* s`.
*   **Return:** Void.

#### `get_mem_stats` (12)
Retrieves global memory usage statistics.
*   **EBX:** `uint32_t* used` (Pointer to store used bytes).
*   **ECX:** `uint32_t* free` (Pointer to store free bytes).
*   **Return:** 0.

#### `get_time` (15)
Retrieves the current RTC time as a string.
*   **EBX:** `char* buf` (Buffer of at least 9 bytes, format "HH:MM:SS").
*   **Return:** 0.

#### `clipboard_set` (25)
Copies text to the global system clipboard.
*   **EBX:** `const char* buf`.
*   **ECX:** `int len`.
*   **Return:** Bytes copied or -1.

#### `clipboard_get` (26)
Pastes text from the global system clipboard.
*   **EBX:** `char* buf`.
*   **ECX:** `int max_len`.
*   **Return:** Bytes pasted or -1.