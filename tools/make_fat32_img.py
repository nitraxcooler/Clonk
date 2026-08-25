#!/usr/bin/env python3
"""Generate a FAT32 disk image for MyOS testing (1.44 MB)."""
import struct

SECTOR = 512
NSECTORS = 2880
RESERVED = 32
FAT_COUNT = 2
FAT_SECTORS = 32
SPC = 1
DATA_START = RESERVED + FAT_COUNT * FAT_SECTORS   # 96
ROOT_CL = 2
OUT = "build/fat32.img"

def cl_sector(cl):
    return DATA_START + (cl - ROOT_CL) * SPC

def dir_entry(name8, ext3, attr, size, first_cl):
    """32-byte FAT32 short entry via explicit bytearray."""
    e = bytearray(32)
    nb = (name8.encode('ascii').upper() + b' ' * 8)[:8]
    xb = (ext3.encode('ascii').upper() + b' ' * 3)[:3]
    e[0:8] = nb
    e[8:11] = xb
    e[0x0B] = attr
    struct.pack_into('<H', e, 0x14, (first_cl >> 16) & 0xFFFF)  # cluster high
    struct.pack_into('<H', e, 0x16, 0x5A70)                     # write time
    struct.pack_into('<H', e, 0x18, 0x5A70)                     # write date
    struct.pack_into('<H', e, 0x1A, first_cl & 0xFFFF)          # cluster low
    struct.pack_into('<I', e, 0x1C, size)                       # file size
    return bytes(e)

def make():
    img = bytearray(NSECTORS * SECTOR)

    # FAT
    fat = bytearray(FAT_SECTORS * SECTOR)
    for idx, val in [(0, 0x0FFFFFF8), (1, 0x0FFFFFFF),
                     (2, 0x0FFFFFFF), (3, 0x0FFFFFFF),
                     (4, 0x0FFFFFFF), (5, 0x0FFFFFFF)]:
        struct.pack_into('<I', fat, idx * 4, val)
    for i in range(FAT_COUNT):
        off = (RESERVED + i * FAT_SECTORS) * SECTOR
        img[off:off + len(fat)] = fat

    # File data
    hello   = b"Hello, MyOS FAT32 world!\n"
    welcome = b"Welcome to MyOS. FAT32 is working.\n"
    test    = bytes(range(16))

    # Root directory
    root_off = cl_sector(ROOT_CL) * SECTOR
    entries = bytearray()
    entries += dir_entry('.', '',      0x10, 0, ROOT_CL)
    entries += dir_entry('..', '',     0x10, 0, ROOT_CL)
    entries += dir_entry('HELLO', 'TXT', 0x20, len(hello), 3)
    entries += dir_entry('WELCOME', 'TXT', 0x20, len(welcome), 5)
    entries += dir_entry('TEST', 'BIN', 0x20, len(test), 4)
    assert len(entries) == 5 * 32
    img[root_off:root_off + len(entries)] = entries

    img[cl_sector(3)*SECTOR:cl_sector(3)*SECTOR+len(hello)]   = hello
    img[cl_sector(5)*SECTOR:cl_sector(5)*SECTOR+len(welcome)] = welcome
    img[cl_sector(4)*SECTOR:cl_sector(4)*SECTOR+len(test)]    = test

    # Boot sector
    b = bytearray(SECTOR)
    b[0:3] = b'\xEB\x58\x90'
    b[3:11] = b'MYOSFAT3'
    struct.pack_into('<H', b, 11, SECTOR)
    b[13] = SPC
    struct.pack_into('<H', b, 14, RESERVED)
    b[16] = FAT_COUNT
    b[21] = 0xF8
    struct.pack_into('<I', b, 32, NSECTORS)
    struct.pack_into('<I', b, 36, FAT_SECTORS)
    struct.pack_into('<H', b, 40, 1)
    struct.pack_into('<H', b, 42, 6)
    struct.pack_into('<I', b, 44, ROOT_CL)
    b[48] = 0x80
    b[50] = 0x29
    struct.pack_into('<I', b, 51, 0x12345678)
    b[55:66] = b'MyOS FAT32 '  # 11-char volume label
    b[66:74] = b'FAT32   '
    b[90:93] = b'\x90\xEB\xFE'
    b[510] = 0x55
    b[511] = 0xAA
    img[0:SECTOR] = b

    assert len(img) == NSECTORS * SECTOR
    with open(OUT, 'wb') as f:
        f.write(img)
    print('wrote', OUT, len(img), 'bytes')

if __name__ == '__main__':
    make()
