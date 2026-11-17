# Index file format, version 1

This is the layout of the file written by `msearch index`. All integers are little-endian. Every
section starts at an offset that is a multiple of 8. A reader never casts a pointer into the file; it
copies numbers out with `memcpy`.

The point of the layout is that **nothing has to be parsed to open it**: the term table is sorted
and its entries have a fixed size, so a lookup is a binary search done in place on the mapped file.

## Header (256 bytes; bytes not listed are zero)

| Offset | Type | Field |
|---:|---|---|
| 0 | char[8] | magic, `MSEINDEX` |
| 8 | u32 | version, 1 |
| 12 | u32 | flags; bit 0 set means postings are gap + VByte |
| 16 | u32 | num_docs |
| 20 | u32 | num_terms |
| 24 | f64 | avg_doc_len (tokens per doc) |
| 32 | u64 | total_postings |
| 40 | u64 | file_size, which must equal the real size of the file |
| 48 | 9 x {u64 offset, u64 size} | the sections below, in order (ends at byte 192) |
| 192 | u32, u32 | corpus root: offset and length inside the `strings` section |
| 200 | u32 | num_parts |

## Sections

| # | Name | Contents | Entry size |
|---:|---|---|---:|
| 0 | doc_lengths | u32 per doc: number of tokens | 4 |
| 1 | doc_refs | {u32 part, u32 length, u64 offset}: where the doc's line is in its part file, without the `\n` | 16 |
| 2 | doc_titles | {u32 offset, u32 length} into `strings` | 8 |
| 3 | part_names | {u32 offset, u32 length} into `strings`: part file names, relative to the corpus root | 8 |
| 4 | terms | term entries, **sorted by the bytes of the term** | 24 |
| 5 | strings | raw bytes: corpus root, part names, titles, then the terms | |
| 6 | postings | one block per term, in term-table order | |
| 7 | spell_nodes | {u32 term_index, u32 first_edge, u32 edge_count}; node 0 is the root | 12 |
| 8 | spell_edges | {u32 distance, u32 child_node}; a node's edges are contiguous, sorted by distance | 8 |

A term entry is `u32 str_off, u32 str_len, u32 df, u32 postings_size, u64 postings_off`. Offsets are
relative to their own section (`str_off` to `strings`, `postings_off` to `postings`).

## Postings blocks

- **raw** (flag bit 0 clear): `df` pairs of `{u32 doc, u32 tf}`.
- **gap + VByte** (flag bit 0 set): `df` pairs of `{uleb128 gap, uleb128 tf}`, where `gap` is the doc id
  minus the previous doc id (the previous one starts at 0).

A uleb128 number is written 7 bits at a time, lowest bits first, and every byte except the last has
its top bit set. So 0 to 127 take one byte, 128 to 16383 take two, and so on up to five bytes. Doc ids
in a list only go up, so the gaps are small, and the commonest words have the smallest gaps.

## Checks when a file is opened

Each is O(1); opening never scans the term table, because that would read every page and defeat lazy
loading. The size is at least 256 bytes; the magic and version match; `file_size` equals the real size;
every section is 8-aligned, inside the file, after the header and after the section before it; each
fixed-size section has exactly count x entry size bytes; the corpus root lies inside `strings`.
Everything else (a term's string range, its postings range, a title range, a part index) is checked
when it is used.

## Walk-through of a real file

The file below is the index of the six-document test corpus `tests/data/tiny_corpus`. The corpus root
is stored as an absolute path, which changes the size of the `strings` section, so use this exact
path to get the same bytes:

```sh
cp -r tests/data/tiny_corpus /tmp/mse-tiny
build/release/msearch index /tmp/mse-tiny -o /tmp/mse-tiny.bin
xxd /tmp/mse-tiny.bin | head -13
```

```
00000000: 4d53 4549 4e44 4558 0100 0000 0100 0000  MSEINDEX........
00000010: 0600 0000 1b00 0000 5555 5555 5555 1540  ........UUUUUU.@
00000020: 1d00 0000 0000 0000 3805 0000 0000 0000  ........8.......
00000030: 0001 0000 0000 0000 1800 0000 0000 0000  ................
00000040: 1801 0000 0000 0000 6000 0000 0000 0000  ........`.......
00000050: 7801 0000 0000 0000 3000 0000 0000 0000  x.......0.......
00000060: a801 0000 0000 0000 1000 0000 0000 0000  ................
00000070: b801 0000 0000 0000 8802 0000 0000 0000  ................
00000080: 4004 0000 0000 0000 b700 0000 0000 0000  @...............
00000090: f804 0000 0000 0000 3a00 0000 0000 0000  ........:.......
000000a0: 3805 0000 0000 0000 0000 0000 0000 0000  8...............
000000b0: 3805 0000 0000 0000 0000 0000 0000 0000  8...............
000000c0: 0000 0000 0d00 0000 0200 0000 0000 0000  ................
```

Reading it from the start:

- bytes 0 to 7: `MSEINDEX`
- bytes 8 to 11: `01 00 00 00`, version 1
- bytes 12 to 15: `01 00 00 00`, flags 1: gap + VByte postings
- bytes 16 to 23: num_docs 6, num_terms 27
- bytes 24 to 31: avg_doc_len as a double, 5.3333 (32 tokens in 6 docs)
- bytes 32 to 39: total_postings 29
- bytes 40 to 47: file_size 1336, the real size of the file
- from byte 48: nine sections, each an offset and a size:

| # | Section | Offset | Size |
|---:|---|---:|---:|
| 0 | doc_lengths | 256 | 24 |
| 1 | doc_refs | 280 | 96 |
| 2 | doc_titles | 376 | 48 |
| 3 | part_names | 424 | 16 |
| 4 | terms | 440 | 648 |
| 5 | strings | 1088 | 183 |
| 6 | postings | 1272 | 58 |
| 7 | spell_nodes | 1336 | 0 |
| 8 | spell_edges | 1336 | 0 |

  The six docs take 6 x 4 = 24 bytes of lengths, 6 x 16 = 96 of refs and 6 x 8 = 48 of title
  references; there are 2 parts (2 x 8 = 16) and 27 terms (27 x 24 = 648). The spell sections are
  empty here because no term of this corpus occurs in 3 or more docs, and only those are stored
  in the spell tree.
- bytes 192 to 199: the corpus root, offset 0 and length 13 in `strings`
  (`/tmp/mse-tiny`); bytes 200 to 203: num_parts 2.

### One term, from its table entry to its postings

Term 18 of 27 is `learning`. Its entry is 24 bytes at offset 872 (the terms section starts at
440, and 18 x 24 = 432):

```
00000368: 8400 0000 0800 0000 0200 0000 0400 0000  ................
00000378: 2400 0000 0000 0000                      $.......
```

- `str_off` = 132, `str_len` = 8: the bytes at 1088 + 132 = 1220 in the file are
  `learning`
- `df` = 2: the term is in 2 docs
- `postings_size` = 4 bytes, `postings_off` = 36: the block is at 1272 + 36 = 1308

```
0000051c: 0102 0102                                ....
```

The block is `01 02 01 02`, read as pairs of numbers: gap 1, tf 2, then gap 1, tf 2. The
first gap counts from 0, so that is doc 1, and the second is doc 1 + 1 = doc 2. "learning" occurs twice in
each of docs 1 (`Beta`) and 2 (`Gamma`). In raw mode the same block would be 16 bytes.

Because the terms are sorted by their bytes, a lookup does a binary search over the 27 entries,
compares each probe against its string in `strings`, and stops at the entry for the word, with no
parsing step.

### The spell tree

The spell sections hold a BK-tree over the terms that occur in at least 3 docs. A second small corpus,
`tests/data/spell_corpus`, has exactly two such terms, `learning` (term 10 of its table) and
`machine` (term 11), so its tree is a root and one child:

```sh
cp -r tests/data/spell_corpus /tmp/mse-spell
build/release/msearch index /tmp/mse-spell -o /tmp/mse-spell.bin
xxd -s 1320 -l 32 /tmp/mse-spell.bin
```

```
00000528: 0a00 0000 0000 0000 0100 0000 0b00 0000  ................
00000538: 0100 0000 0000 0000 0500 0000 0100 0000  ................
```

- `spell_nodes` is at 1320 and is 24 bytes: two nodes of 12 bytes. Node 0, the root, is
  `{term_index 10, first_edge 0, edge_count 1}`: it holds `learning`. Node 1 is
  `{term_index 11, first_edge 1, edge_count 0}`: it holds `machine` and has no children.
- `spell_edges` is at 1344 and is 8 bytes: one edge `{distance 5, child_node 1}`, meaning that the
  edit distance from `learning` to `machine` is 5.

To find the best correction of a word, a search starts at the root, computes the distance `d` from
the word to the node's term, and keeps that term if `d` is within the limit (1 for words of up to 4
bytes, 2 for longer ones). It then follows only the edges whose distance is between `d - limit` and
`d + limit`: by the triangle inequality no term below any other edge can be close enough. The
suggestion is the term with the smallest distance, then the most docs, then the alphabetically
first.
