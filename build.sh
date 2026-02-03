#!/bin/bash

# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2025 Yula1234

set -e

DIRS=("bin/obj" "bin/usr" "bin/tools" "bin/isodir/boot/grub")

ISODIR="bin/isodir"

DISK_IMG="disk.img"

TOOL="bin/tools/yulafs_tool"

USER_APPS=("edit" "geditor" "asmc" "dasm" "grep" "cat" "uld" "scc" "explorer" "cp" "mv" "touch" "tree" "ld" "paint" "flux" "axwm" "launcher" "ush" "term" "ps" "time" "neofetch" "ls" "rm" "mkdir" "kill")

if command -v ccache &> /dev/null; then
    CC="ccache gcc -m32"
else
    CC="gcc -m32"
fi

LD="ld -m elf_i386 --gc-sections"
ASM="fasm"

CFLAGS_BASE="-ffreestanding -fno-pie -fno-stack-protector -fno-builtin -pipe -march=i686 -I include"

CFLAGS_KERN="$CFLAGS_BASE -std=gnu99 -O2 -Wall -Wextra -I src -mno-mmx -mno-sse -mno-sse2 -mno-80387 -fomit-frame-pointer
-fno-strict-aliasing -fno-delete-null-pointer-checks -fno-strict-overflow -fwrapv -fno-common -Wstack-usage=16384 -mno-avx"
# -fanalyzer

CFLAGS_USER="$CFLAGS_BASE -I usr -msse -msse2 -O3 -fomit-frame-pointer" 

LDFLAGS_USER="--no-warn-rwx-segments -T usr/linker.ld"

mkdir -p "${DIRS[@]}"

gcc -O3 tools/yulafs_tool.c -o "$TOOL" &

$ASM src/boot/smp_trampoline.asm bin/smp_trampoline.bin > /dev/null &

wait

echo "[kernel] compiling asm..."
KERNEL_OBJ_FILES=""
for FILE in $(find src -name "*.asm"); do
    if [[ "$FILE" == src/usr* || "$FILE" == src/boot/smp_trampoline.asm ]]; then continue; fi
    
    REL="${FILE#src/}"     
    FLAT="${REL//\//_}"  
    OBJ="bin/obj/${FLAT%.*}.o"
    
    $ASM "$FILE" "$OBJ" > /dev/null &
    KERNEL_OBJ_FILES="$KERNEL_OBJ_FILES $OBJ"
done

echo "[kernel] compiling C..."
for FILE in $(find src -name "*.c"); do
    
    REL="${FILE#src/}"
    FLAT="${REL//\//_}"
    OBJ="bin/obj/${FLAT%.*}.o"
    
    $CC $CFLAGS_KERN -c "$FILE" -o "$OBJ" &
    KERNEL_OBJ_FILES="$KERNEL_OBJ_FILES $OBJ"
done

echo "[user] compiling libs..."
$ASM usr/start.asm bin/usr/start.o &
$CC $CFLAGS_USER -c usr/lib/malloc.c -o bin/obj/malloc.o &
$CC $CFLAGS_USER -c usr/lib/stdio.c  -o bin/obj/stdio.o &
$CC $CFLAGS_USER -c usr/lib/string.c  -o bin/obj/string.o &
$CC $CFLAGS_USER -c usr/lib/stdlib.c  -o bin/obj/stdlib.o &

USER_LIBS="bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o
bin/obj/string.o bin/obj/stdlib.o"

echo "[user] compiling apps..."
declare -A USER_APP_OBJS
for APP in "${USER_APPS[@]}"; do
    if [ -d "programs/$APP" ] && [ -n "$(find "programs/$APP" -name "*.c" -print -quit)" ]; then
        OBJS=""
        for FILE in $(find "programs/$APP" -name "*.c" | sort); do
            REL="${FILE#programs/$APP/}"
            FLAT="${REL//\//_}"
            OBJ="bin/usr/${APP}_${FLAT%.*}.o"
            $CC $CFLAGS_USER -c "$FILE" -o "$OBJ" &
            OBJS="$OBJS $OBJ"
        done
        USER_APP_OBJS["$APP"]="$OBJS"
    else
        OBJ="bin/usr/$APP.o"
        $CC $CFLAGS_USER -c "programs/$APP.c" -o "$OBJ" &
        USER_APP_OBJS["$APP"]="$OBJ"
    fi
done

wait

echo "[ld] linking kernel..."
SORTED_K_OBJS=$(echo $KERNEL_OBJ_FILES | tr ' ' '\n' | sort | tr '\n' ' ')
$LD -T src/linker.ld -o bin/kernel.bin $SORTED_K_OBJS &

for APP in "${USER_APPS[@]}"; do
    (
        read -r -a OBJS <<< "${USER_APP_OBJS[$APP]}"
        $LD $LDFLAGS_USER -o "bin/usr/$APP.exe" $USER_LIBS "${OBJS[@]}"
        strip --strip-all "bin/usr/$APP.exe"
    ) &
done

wait

if [ ! -f "$DISK_IMG" ]; then
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=40 status=none
fi

echo "[fs] importing files..."
for APP in "${USER_APPS[@]}"; do
    "$TOOL" "$DISK_IMG" import "bin/usr/$APP.exe" "/bin/$APP.exe" > /dev/null
done

"$TOOL" "$DISK_IMG" import bin/usr/start.o /bin/start.o > /dev/null
"$TOOL" "$DISK_IMG" import bin/obj/malloc.o /bin/malloc.o > /dev/null
"$TOOL" "$DISK_IMG" import bin/obj/string.o /bin/string.o > /dev/null
"$TOOL" "$DISK_IMG" import bin/obj/stdlib.o /bin/stdlib.o > /dev/null
"$TOOL" "$DISK_IMG" import bin/obj/stdio.o /bin/stdio.o > /dev/null
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

export vblank_mode=0 
export __GL_SYNC_TO_VBLANK=0

QEMU_ARGS="-device ahci,id=ahci -global kvm-pit.lost_tick_policy=discard
-device ide-hd,drive=disk,bus=ahci.0 -machine pcspk-audiodev=snd0 -m 1G
-device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0,port=1
-drive id=disk,file=${DISK_IMG},if=none,format=raw,cache=unsafe,aio=io_uring
-accel kvm -vga virtio -display sdl,gl=on -smp 3 -mem-prealloc
-cpu host,migratable=no,+invtsc,l3-cache=on -audiodev pa,id=snd0
-device usb-mouse,bus=uhci.0,port=2
"

qemu-system-x86_64 -cdrom bin/yulaos.iso $QEMU_ARGS