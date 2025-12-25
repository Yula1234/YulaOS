#!/bin/bash

set -e

DIRS=("bin/obj" "bin/usr" "bin/tools" "bin/isodir/boot/grub")

ISODIR="bin/isodir"

DISK_IMG="disk.img"

TOOL="bin/tools/yulafs_tool"

USER_APPS=("test" "edit" "geditor" "asmc" "dasm" "grep" "cat" "uld")

CC="gcc -m32"
LD="ld -m elf_i386"
ASM="fasm"

CFLAGS_BASE="-ffreestanding -fno-pie -fno-stack-protector -fno-builtin"

CFLAGS_KERN="$CFLAGS_BASE -mno-sse -mno-sse2 -std=gnu99 -O3 -Wall -Wextra -I src"

CFLAGS_USER="$CFLAGS_BASE -I usr"

LDFLAGS_USER="--no-warn-rwx-segments -T usr/linker.ld"

for d in "${DIRS[@]}"; do mkdir -p "$d"; done

gcc tools/yulafs_tool.c -o "$TOOL"

echo "[asm] assembling kernel..."
OBJ_FILES=""
for FILE in $(find src -name "*.asm" | sort); do
    if [[ "$FILE" != *"src/usr"* ]]; then
        # Transform path: src/arch/file.asm -> arch_file.o
        OBJ_NAME=$(echo $FILE | sed 's|src/||' | sed 's|/|_|g' | sed 's|\.asm|.o|')
        $ASM "$FILE" "bin/obj/$OBJ_NAME"
        OBJ_FILES="$OBJ_FILES bin/obj/$OBJ_NAME"
    fi
done

echo "[c] compiling kernel..."
for FILE in $(find src -name "*.c" | sort); do
    if [[ "$FILE" != *"src/usr"* ]]; then
        # Transform path: src/kernel/file.c -> kernel_file.o
        OBJ_NAME=$(echo $FILE | sed 's|src/||' | sed 's|/|_|g' | sed 's|\.c|.o|')
        $CC $CFLAGS_KERN -c "$FILE" -o "bin/obj/$OBJ_NAME"
        OBJ_FILES="$OBJ_FILES bin/obj/$OBJ_NAME"
    fi
done

echo "[ld] linking kernel..."
$LD -T src/linker.ld -o bin/kernel.bin $OBJ_FILES

$ASM usr/start.asm bin/usr/start.o
$CC $CFLAGS_USER -c usr/lib/malloc.c -o bin/obj/malloc.o
$CC $CFLAGS_USER -c usr/lib/stdio.c  -o bin/obj/stdio.o

USER_LIBS="bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o"

for APP in "${USER_APPS[@]}"; do
    $CC $CFLAGS_USER -c "programs/$APP.c" -o "bin/usr/$APP.o"
    $LD $LDFLAGS_USER -o "bin/usr/$APP.exe" $USER_LIBS "bin/usr/$APP.o"
done

if [ ! -f "$DISK_IMG" ]; then
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=10 status=none
    "$TOOL" "$DISK_IMG" format
fi

for APP in "${USER_APPS[@]}"; do
    "$TOOL" "$DISK_IMG" import "bin/usr/$APP.exe" "/bin/$APP.exe" > /dev/null
done

"$TOOL" "$DISK_IMG" import programs/loop.asm /home/loop.asm > /dev/null

cp bin/kernel.bin "$ISODIR/boot/kernel.bin"

cat << EOF > "$ISODIR/boot/grub/grub.cfg"
set timeout=0
set default=0
menuentry "YulaOS" {
    multiboot /boot/kernel.bin
    boot
}
EOF

grub-mkrescue -o bin/yulaos.iso "$ISODIR" 2> /dev/null

echo "[run] qemu..."

QEMU_ARGS="-device ahci,id=ahci
-device ide-hd,drive=disk,bus=ahci.0
-drive id=disk,file=${DISK_IMG},if=none,format=raw
-enable-kvm -vga std -m 2G"

qemu-system-i386 -cdrom bin/yulaos.iso $QEMU_ARGS