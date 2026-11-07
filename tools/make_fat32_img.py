#!/usr/bin/env python3
"""Build the FAT32 images used by the default boot and optional AHCI test."""
import argparse
import os
import struct

SECTOR = 512
NSECTORS = 2880
RESERVED = 32
FAT_COUNT = 2
SPC = 1
ROOT_CL = 2
OUT = "build/fat32.img"

def dir_entry(name8, ext3, attr, size, first_cl):
    """Build one 32-byte FAT32 short entry by hand."""
    entry = bytearray(32)
    entry[0:8] = (name8.encode("ascii").upper() + b" " * 8)[:8]
    entry[8:11] = (ext3.encode("ascii").upper() + b" " * 3)[:3]
    entry[0x0B] = attr
    struct.pack_into("<H", entry, 0x14, (first_cl >> 16) & 0xFFFF)
    struct.pack_into("<H", entry, 0x16, 0x5A70)
    struct.pack_into("<H", entry, 0x18, 0x5A70)
    struct.pack_into("<H", entry, 0x1A, first_cl & 0xFFFF)
    struct.pack_into("<I", entry, 0x1C, size)
    return bytes(entry)

def short_name(filename):
    if filename.count(".") != 1:
        raise ValueError("FAT32 files must use a short 8.3 name: " + filename)
    base, extension = filename.split(".")
    allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~"
    if (not 1 <= len(base) <= 8 or not 1 <= len(extension) <= 3 or
            any(char.upper() not in allowed for char in base + extension)):
        raise ValueError("FAT32 files must use a short 8.3 name: " + filename)
    return base.upper(), extension.upper()

def make(out=OUT, nsectors=NSECTORS, ahci_test=False, executables=None):
    if nsectors <= RESERVED:
        raise ValueError("disk image is too small")

    fat_sectors = 1
    while True:
        data_sectors = nsectors - RESERVED - FAT_COUNT * fat_sectors
        clusters = data_sectors // SPC
        required_fat_sectors = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if required_fat_sectors <= fat_sectors:
            break
        fat_sectors = required_fat_sectors
    if data_sectors <= 0:
        raise ValueError("disk image has no data clusters")
    if ahci_test and clusters < 65525:
        raise ValueError("AHCI test image must have a FAT32-sized cluster count")

    data_start = RESERVED + FAT_COUNT * fat_sectors
    image = bytearray(nsectors * SECTOR)

    def cluster_sector(cluster):
        return data_start + (cluster - ROOT_CL) * SPC

    hello = b"Hello, Clonk FAT32 world!\n"
    welcome = b"Welcome to Clonk. FAT32 is working.\n"
    test = bytes(range(16))

    files = [("HELLO.TXT", hello), ("WELCOME.TXT", welcome),
             ("TEST.BIN", test), ("EMPTY.ELF", b""),
             ("TOOBIG.ELF", bytes(1024 * 1024 + 1))]
    if executables:
        files.extend(executables.items())

    fat_entries = {0: 0x0FFFFFF8, 1: 0x0FFFFFFF,
                   ROOT_CL: 0x0FFFFFFF}
    root_entries = [
        dir_entry(".", "", 0x10, 0, ROOT_CL),
        dir_entry("..", "", 0x10, 0, ROOT_CL),
    ]
    next_cluster = ROOT_CL + 1
    allocated_names = set()
    cluster_bytes = SPC * SECTOR
    cluster_limit = 2 + clusters - int(ahci_test)
    for filename, content in files:
        name8, ext3 = short_name(filename)
        name = name8 + "." + ext3
        if name in allocated_names:
            raise ValueError("duplicate FAT32 filename: " + name)
        allocated_names.add(name)
        if len(content) > 0xFFFFFFFF:
            raise ValueError("file exceeds the FAT32 size field: " + name)
        count = (len(content) + cluster_bytes - 1) // cluster_bytes
        if count and (next_cluster > cluster_limit or
                      count > cluster_limit - next_cluster):
            raise ValueError("disk image has insufficient clusters for " + name)

        chain = list(range(next_cluster, next_cluster + count))
        next_cluster += count
        for index, cluster in enumerate(chain):
            fat_entries[cluster] = (chain[index + 1] if index + 1 < count
                                    else 0x0FFFFFFF)
            begin = index * cluster_bytes
            chunk = content[begin:begin + cluster_bytes]
            offset = cluster_sector(cluster) * SECTOR
            image[offset:offset + len(chunk)] = chunk

        first_cluster = chain[0] if chain else 0
        root_entries.append(dir_entry(name8, ext3, 0x20, len(content),
                                      first_cluster))

    if len(root_entries) * 32 > cluster_bytes:
        raise ValueError("root directory exceeds its allocated cluster")
    root_offset = cluster_sector(ROOT_CL) * SECTOR
    image[root_offset:root_offset + len(root_entries) * 32] = b"".join(root_entries)

    fat = bytearray(fat_sectors * SECTOR)
    for cluster, value in fat_entries.items():
        struct.pack_into("<I", fat, cluster * 4, value)
    if ahci_test:
        struct.pack_into("<I", fat, (clusters + 1) * 4, 0x0FFFFFFF)
    for index in range(FAT_COUNT):
        offset = (RESERVED + index * fat_sectors) * SECTOR
        image[offset:offset + len(fat)] = fat

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"CLONKFS3"
    struct.pack_into("<H", boot, 11, SECTOR)
    boot[13] = SPC
    struct.pack_into("<H", boot, 14, RESERVED)
    boot[16] = FAT_COUNT
    boot[21] = 0xF8
    struct.pack_into("<I", boot, 32, nsectors)
    struct.pack_into("<I", boot, 36, fat_sectors)
    struct.pack_into("<I", boot, 44, ROOT_CL)
    struct.pack_into("<H", boot, 48, 1)
    struct.pack_into("<H", boot, 50, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into("<I", boot, 67, 0x12345678)
    boot[71:82] = b"CLONKFS    "
    boot[82:90] = b"FAT32   "
    boot[90:93] = b"\x90\xEB\xFE"
    boot[510:512] = b"\x55\xAA"
    image[:SECTOR] = boot

    fsinfo = bytearray(SECTOR)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    allocated_clusters = next_cluster - ROOT_CL
    struct.pack_into("<I", fsinfo, 488,
                     clusters - allocated_clusters - int(ahci_test))
    struct.pack_into("<I", fsinfo, 492,
                     next_cluster if next_cluster < cluster_limit else 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)
    image[SECTOR:2 * SECTOR] = fsinfo
    image[6 * SECTOR:7 * SECTOR] = boot
    image[7 * SECTOR:8 * SECTOR] = fsinfo

    if ahci_test:
        marker = b"CLONK-AHCI-END"
        image[-SECTOR:-SECTOR + len(marker)] = marker

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as image_file:
        image_file.write(image)
    print("wrote", out, len(image), "bytes; FAT32 clusters:", clusters)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default=OUT)
    parser.add_argument("--sectors", type=int, default=NSECTORS)
    parser.add_argument("--ahci-test", action="store_true")
    parser.add_argument("--hello-elf")
    parser.add_argument("--exit-elf")
    parser.add_argument("--sched-a-elf")
    parser.add_argument("--sched-b-elf")
    parser.add_argument("--fault-elf")
    parser.add_argument("--bad-elf")
    options = parser.parse_args()
    executables = {}
    for option, name in (
            (options.hello_elf, "HELLO.ELF"),
            (options.exit_elf, "EXIT.ELF"),
            (options.sched_a_elf, "SCHED-A.ELF"),
            (options.sched_b_elf, "SCHED-B.ELF"),
            (options.fault_elf, "FAULT.ELF"),
            (options.bad_elf, "BAD.ELF")):
        if option:
            with open(option, "rb") as program:
                executables[name] = program.read()
    make(options.output, options.sectors, options.ahci_test, executables)
