#!/usr/bin/env python3
"""Print the zlib crc32 of each file given, in the form the IOC prints it.

The iocsh command rpiKexec prints "image crc32 xxxxxxxx" for the kernel it
fetched over TFTP; compare it with

    scripts/crc32.py bin/RTEMS-raspberrypi2/rpi3_plain.img

before letting it boot the image.
"""
import sys
import zlib

for name in sys.argv[1:]:
    with open(name, "rb") as f:
        print("%08x  %s" % (zlib.crc32(f.read()) & 0xffffffff, name))
