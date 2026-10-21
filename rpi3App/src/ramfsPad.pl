#!/usr/bin/perl
# ramfsPad.pl FILE.tar.gz
#
# Pads a gzip file to (next multiple of 4096) + 4096 bytes by appending one
# extra gzip member that decompresses to zeros. The Raspberry Pi firmware
# loads the "initramfs" file into RAM, but the last partial 4 KB page
# arrived damaged on the Pi 3B+ (seen 2026-10-15, both boot tests); with
# this padding the real archive ends on a page boundary and a whole page
# of padding follows, so the damage lands in bytes nobody reads.
#
# zlib's inflate() returns Z_STREAM_END after the first member, which is
# what the RTEMS untar code uses; gzip(1) and tar(1) treat concatenated
# members as one stream (the padding becomes trailing zeros, i.e. tar's
# end-of-archive).
#
# Member layout (RFC 1952): 10-byte header, one stored deflate block
# (1 byte BFINAL/BTYPE, LEN, NLEN, data), CRC32 and ISIZE trailer.
use strict;
use warnings;

my $file = shift or die "usage: $0 file.tar.gz\n";
my $size = -s $file;
defined $size or die "$file: $!\n";
my $page = 4096;
my $target = (int(($size + $page - 1) / $page) + 1) * $page;
my $pad = $target - $size;
# a member with N zero bytes stored is N + 23 bytes long
if ($pad < 23) { $pad += $page; $target += $page; }
my $n = $pad - 23;
die "pad too large\n" if $n > 65535;

my $zeros = "\0" x $n;
my $crc = 0;
# crc32 of zeros, computed directly (no Compress::Zlib dependency)
my @tab;
for my $i (0 .. 255) {
    my $c = $i;
    for (1 .. 8) { $c = ($c & 1) ? (0xEDB88320 ^ ($c >> 1)) : ($c >> 1); }
    $tab[$i] = $c;
}
$crc = 0xFFFFFFFF;
for (1 .. $n) { $crc = $tab[($crc ^ 0) & 0xFF] ^ ($crc >> 8); }
$crc ^= 0xFFFFFFFF;

my $member = pack("C10", 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3)   # header, OS=Unix
           . pack("C", 1)                                      # BFINAL=1, BTYPE=00 (stored)
           . pack("v v", $n, (~$n) & 0xFFFF)                    # LEN, NLEN
           . $zeros
           . pack("V V", $crc, $n);                             # CRC32, ISIZE
length($member) == $pad or die "internal: member " . length($member) . " != $pad\n";

open(my $fh, '>>', $file) or die "$file: $!\n";
binmode $fh;
print $fh $member;
close $fh;
printf "PAD  %s: %d -> %d bytes (%d pages + 1 padding page)\n",
    $file, $size, $target, $target / $page - 1;
