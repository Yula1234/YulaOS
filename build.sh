#!/bin/bash
set -e

# Папки
mkdir -p bin/obj
mkdir -p bin/usr
mkdir -p bin/tools
ISODIR="bin/isodir"
mkdir -p "$ISODIR/boot/grub"

gcc tools/yulafs_tool.c -o bin/tools/yulafs_tool

OBJ_FILES=""

echo "[asm] assembling kernel..."
ASM_FILES=$(find src -name "*.asm" | sort)
for FILE in $ASM_FILES; do
  if [[ "$FILE" != *"src/usr"* ]]; then
      OBJ_NAME=$(echo $FILE | sed 's|src/||' | sed 's|/|_|g' | sed 's|\.asm|.o|')
      fasm "$FILE" "bin/obj/$OBJ_NAME"
      OBJ_FILES="$OBJ_FILES bin/obj/$OBJ_NAME"
  fi
done

echo "[c] compiling kernel..."
C_FILES=$(find src -name "*.c" | sort)
for FILE in $C_FILES; do
  if [[ "$FILE" != *"src/usr"* ]]; then
      OBJ_NAME=$(echo $FILE | sed 's|src/||' | sed 's|/|_|g' | sed 's|\.c|.o|')
      gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
          -mno-sse -mno-sse2 \
          -std=gnu99 -O3 -Wall -Wextra \
          -I src \
          -c "$FILE" -o "bin/obj/$OBJ_NAME"
      OBJ_FILES="$OBJ_FILES bin/obj/$OBJ_NAME"
  fi
done

echo "[ld] linking kernel..."
ld -m elf_i386 -T src/linker.ld -o bin/kernel.bin $OBJ_FILES

fasm usr/start.asm bin/usr/start.o

gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c usr/lib/malloc.c -o bin/obj/malloc.o
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c usr/lib/stdio.c -o bin/obj/stdio.o

gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c programs/test.c -o bin/usr/test.o
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c programs/edit.c -o bin/usr/edit.o
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c programs/geditor.c -o bin/usr/geditor.o
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c programs/asmc.c -o bin/usr/asmc.o
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c programs/dasm.c -o bin/usr/dasm.o
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c programs/grep.c -o bin/usr/grep.o
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
    -I usr -c programs/cat.c -o bin/usr/cat.o

ld -m elf_i386 --no-warn-rwx-segments -T usr/linker.ld -o bin/usr/test.exe bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o bin/usr/test.o
ld -m elf_i386 --no-warn-rwx-segments -T usr/linker.ld -o bin/usr/edit.exe bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o bin/usr/edit.o
ld -m elf_i386 --no-warn-rwx-segments -T usr/linker.ld -o bin/usr/geditor.exe bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o bin/usr/geditor.o
ld -m elf_i386 --no-warn-rwx-segments -T usr/linker.ld -o bin/usr/asmc.exe bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o bin/usr/asmc.o
ld -m elf_i386 --no-warn-rwx-segments -T usr/linker.ld -o bin/usr/dasm.exe bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o bin/usr/dasm.o
ld -m elf_i386 --no-warn-rwx-segments -T usr/linker.ld -o bin/usr/grep.exe bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o bin/usr/grep.o
ld -m elf_i386 --no-warn-rwx-segments -T usr/linker.ld -o bin/usr/cat.exe bin/obj/malloc.o bin/obj/stdio.o bin/usr/start.o bin/usr/cat.o

DISK_IMG="disk.img"

if [ ! -f $DISK_IMG ]; then
    dd if=/dev/zero of=$DISK_IMG bs=1M count=10
    ./bin/tools/yulafs_tool $DISK_IMG format
fi

# ./bin/tools/yulafs_tool disk.img format

./bin/tools/yulafs_tool $DISK_IMG import bin/usr/test.exe /bin/test.exe
./bin/tools/yulafs_tool $DISK_IMG import bin/usr/edit.exe /bin/edit.exe
./bin/tools/yulafs_tool $DISK_IMG import bin/usr/geditor.exe /bin/geditor.exe
./bin/tools/yulafs_tool $DISK_IMG import bin/usr/asmc.exe /bin/asmc.exe
./bin/tools/yulafs_tool $DISK_IMG import bin/usr/dasm.exe /bin/dasm.exe
./bin/tools/yulafs_tool $DISK_IMG import bin/usr/grep.exe /bin/grep.exe
./bin/tools/yulafs_tool $DISK_IMG import bin/usr/cat.exe /bin/cat.exe

./bin/tools/yulafs_tool $DISK_IMG import programs/loop.asm /home/loop.asm

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
qemu-system-i386 -cdrom bin/yulaos.iso -drive file=$DISK_IMG,format=raw,index=0,media=disk -enable-kvm -vga std -m 512 -cpu host