#!/usr/bin/env python3
"""Self-check for computed-jump table reading.  python tools/test_find_entries.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_entries import byte_table, word_table  # noqa: E402


def duff(guard=True):
    """A Duff's device: a byte table at the jmp's base, then 2-byte arms.

    0x08  move.b d(pc,Dn.w),d1   the index comes out of a byte table
    0x0c  jmp    $10(pc,d1.w)    base = extension word (0x0e) + disp 2
    0x10  08 06 04 04            the table: arms at base+8, +6, +4, +4
    0x14  12 d8 x3               move.b (a0)+,(a1)+
    0x1a  4e 75                  rts
    """
    d = bytearray(0x1c)
    d[0x08:0x0c] = bytes([0x12, 0x3B, 0x00, 0x06]) if guard else bytes(4)
    d[0x0c:0x10] = bytes([0x4E, 0xFB, 0x10, 0x02])
    d[0x10:0x14] = bytes([0x08, 0x06, 0x04, 0x04])
    d[0x14:0x1A] = bytes([0x12, 0xD8]) * 3
    d[0x1A:0x1C] = bytes([0x4E, 0x75])
    return bytes(d)


def test_byte_table_finds_every_arm():
    # Each arm is named only by a byte in the table, so a word-sized read finds
    # nothing and the routine behind the jump stays unreachable.
    assert sorted(set(byte_table(duff(), 0x10, 0x0C))) == [0x14, 0x16, 0x18]


def test_byte_table_stops_at_the_end_of_the_table():
    # The first arm's own opcode byte would index past the segment; that is the
    # signal the table ended, not another entry.
    assert len(byte_table(duff(), 0x10, 0x0C)) == 4


def test_byte_table_needs_the_move_b_that_indexes_it():
    # Without it every ordinary word switch would be read a second time as
    # garbage bytes.
    assert byte_table(duff(guard=False), 0x10, 0x0C) == []


def test_word_table_reads_offsets_after_the_jump():
    # $4EFB, extension word, then 16-bit offsets measured from the extension
    # word: 0x20 and 0x24 past it, and then a backwards one that ends the table.
    d = bytearray(0x40)
    d[0x00:0x04] = bytes([0x4E, 0xFB, 0x00, 0x00])
    d[0x04:0x0A] = bytes([0x00, 0x20, 0x00, 0x24, 0x00, 0x00])
    assert word_table(d, 0x02) == [0x22, 0x26]


def main():
    test_byte_table_finds_every_arm()
    test_byte_table_stops_at_the_end_of_the_table()
    test_byte_table_needs_the_move_b_that_indexes_it()
    test_word_table_reads_offsets_after_the_jump()
    print("find_entries self-check OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
