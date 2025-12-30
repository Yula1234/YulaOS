**YulaOS** is a operating system kernel developed from scratch in C and FASM. It features SMP support, a windowing system, and a self-hosted development toolchain.

## Features

*   **Multiprocessing:** SMP support for multicore systems.
*   **Storage:** Native SATA (AHCI) driver and a custom read/write filesystem (YulaFS).
*   **Memory:** Virtual memory management and dynamic heap allocation.
*   **Graphics:** Compositing window manager with support for transparent windows.
*   **Userland:**
    *   Native Shell with pipes and history.
    *   **Self-hosting:** Includes a custom assembler (`asmc`) and linker (`uld`).
    *   **Apps:** Text editor (`geditor`), file manager, and system utilities.

## Building & Running

**Requirements:** Linux environment, `gcc` (i686-elf or multilib), `fasm`, `make`, `qemu-system-i386`.

1.  **Clone:**
    ```bash
    git clone https://github.com/Yula1234/YulaOS.git
    cd YulaOS
    ```

2.  **Build:**
    ```bash
    ./build.sh
    ```