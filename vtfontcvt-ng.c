/*-
 * Copyright (c) 2009, 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Ed Schouten under sponsorship from the
 * FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: head/usr.bin/vtfontcvt/vtfontcvt.c 331935 2018-04-03 18:43:00Z emaste $");

#include <sys/types.h>
#include <sys/fnv_hash.h>
#include <sys/endian.h>
#include <sys/param.h>
#include <sys/queue.h>

#include <assert.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	VFNT_MAPS		4
#define	VFNT_MAP_NORMAL		0
#define	VFNT_MAP_NORMAL_RIGHT	1
#define	VFNT_MAP_BOLD		2
#define	VFNT_MAP_BOLD_RIGHT	3
#define	VFNT_MAXGLYPHS		131072
#define	VFNT_MAXDIMENSION	128

static unsigned int width = 8, wbytes, height = 16;

struct glyph {
	TAILQ_ENTRY(glyph)	 g_list;
	SLIST_ENTRY(glyph)	 g_hash;
	uint8_t			*g_data;
	unsigned int		 g_index;
};

#define	FONTCVT_NHASH 4096
TAILQ_HEAD(glyph_list, glyph);
static SLIST_HEAD(, glyph) glyph_hash[FONTCVT_NHASH];
static struct glyph_list glyphs[VFNT_MAPS] = {
	TAILQ_HEAD_INITIALIZER(glyphs[0]),
	TAILQ_HEAD_INITIALIZER(glyphs[1]),
	TAILQ_HEAD_INITIALIZER(glyphs[2]),
	TAILQ_HEAD_INITIALIZER(glyphs[3]),
};
static unsigned int glyph_total, glyph_count[4], glyph_unique, glyph_dupe;

struct mapping {
	TAILQ_ENTRY(mapping)	 m_list;
	unsigned int		 m_char;
	unsigned int		 m_length;
	struct glyph		*m_glyph;
};

TAILQ_HEAD(mapping_list, mapping);
static struct mapping_list maps[VFNT_MAPS] = {
	TAILQ_HEAD_INITIALIZER(maps[0]),
	TAILQ_HEAD_INITIALIZER(maps[1]),
	TAILQ_HEAD_INITIALIZER(maps[2]),
	TAILQ_HEAD_INITIALIZER(maps[3]),
};
static unsigned int mapping_total, map_count[4], map_folded_count[4],
	mapping_unique, mapping_dupe;

static void
usage(void)
{

	(void)fprintf(stderr,
"usage: vtfontcvt [-w width] [-h height] [-v] normal.bdf [bold.bdf] out.fnt\n");
	exit(1);
}

static void *
xmalloc(size_t size)
{
	void *m;

	if ((m = malloc(size)) == NULL)
		errx(1, "memory allocation failure");
	memset(m, 0, size);

	return (m);
}

static int
add_mapping(struct glyph *gl, unsigned int c, unsigned int map_idx)
{
	struct mapping *mp, *mp_temp = NULL;
	struct mapping_list *ml;

	mapping_total++;

	mp = xmalloc(sizeof *mp);
	mp->m_char = c;
	mp->m_glyph = gl;
	mp->m_length = 0;

	ml = &maps[map_idx];
	TAILQ_FOREACH(mp_temp, ml, m_list) {
		if (mp_temp->m_char >= c)
			break;
	}
	if (mp_temp == NULL)
		TAILQ_INSERT_TAIL(ml, mp, m_list);
	else
		TAILQ_INSERT_BEFORE(mp_temp, mp, m_list);

	map_count[map_idx]++;
	mapping_unique++;

	return (0);
}

static int
dedup_mapping(unsigned int map_idx)
{
	struct mapping *mp_bold, *mp_normal, *mp_temp;
	unsigned normal_map_idx = map_idx - VFNT_MAP_BOLD;

	assert(map_idx == VFNT_MAP_BOLD || map_idx == VFNT_MAP_BOLD_RIGHT);
	mp_normal = TAILQ_FIRST(&maps[normal_map_idx]);
	TAILQ_FOREACH_SAFE(mp_bold, &maps[map_idx], m_list, mp_temp) {
		while (mp_normal->m_char < mp_bold->m_char)
			mp_normal = TAILQ_NEXT(mp_normal, m_list);
		if (mp_bold->m_char != mp_normal->m_char)
			errx(1, "Character %u not in normal font!",
			    mp_bold->m_char);
		if (mp_bold->m_glyph != mp_normal->m_glyph)
			continue;

		/* No mapping is needed if it's equal to the normal mapping. */
		TAILQ_REMOVE(&maps[map_idx], mp_bold, m_list);
		free(mp_bold);
		mapping_dupe++;
	}

	return (0);
}

static struct glyph *
add_glyph(const uint8_t *bytes, unsigned int map_idx, int fallback)
{
	struct glyph *gl;
	int hash;

	glyph_total++;
	glyph_count[map_idx]++;

	hash = fnv_32_buf(bytes, wbytes * height, FNV1_32_INIT) % FONTCVT_NHASH;
	SLIST_FOREACH(gl, &glyph_hash[hash], g_hash) {
		if (memcmp(gl->g_data, bytes, wbytes * height) == 0) {
			glyph_dupe++;
			return (gl);
		}
	}

	gl = xmalloc(sizeof *gl);
	gl->g_data = xmalloc(wbytes * height);
	memcpy(gl->g_data, bytes, wbytes * height);
	if (fallback)
		TAILQ_INSERT_HEAD(&glyphs[map_idx], gl, g_list);
	else
		TAILQ_INSERT_TAIL(&glyphs[map_idx], gl, g_list);
	SLIST_INSERT_HEAD(&glyph_hash[hash], gl, g_hash);

	glyph_unique++;

	if (glyph_unique > VFNT_MAXGLYPHS)
		errx(1, "Too large number of glyphs %u!", glyph_unique);

	return (gl);
}

static int
add_char(unsigned curchar, unsigned map_idx, uint8_t *bytes, uint8_t *bytes_r)
{
	struct glyph *gl;

	/* Prevent adding two glyphs for 0xFFFD */
	if (curchar == 0xFFFD) {
		if (map_idx < VFNT_MAP_BOLD)
			gl = add_glyph(bytes, 0, 1);
	} else if (curchar >= 0x20) {
		gl = add_glyph(bytes, map_idx, 0);
		if (add_mapping(gl, curchar, map_idx) != 0)
			return (1);
		if (bytes_r != NULL) {
			gl = add_glyph(bytes_r, map_idx + 1, 0);
			if (add_mapping(gl, curchar, map_idx + 1) != 0)
				return (1);
		}
	}

	return (0);
}

static int
rshift_bitmap_line(uint8_t *line, size_t size, size_t len, size_t shift)
{
	size_t d, s, i;
	uint16_t t;

	assert(size > 0 && len > 0);
	assert(size * 8 >= len);

	if (shift == 0)
		return (0);

	d = shift / 8;
	s = 8 - shift % 8;
	i = howmany(len, 8);

	while (i > 0) {
		i--;

		t = *(line + i);
		*(line + i) = 0;

		t <<= s;

		if (i + d + 1 < size)
			*(line + i + d + 1) |= (uint8_t) t;
		if (i + d < size)
			*(line + i + d) = t >> 8;
	}

	return (0);
}

static int
split_bitmap_line(uint8_t *left, uint8_t *right, uint8_t *line, size_t w)
{
	size_t s, i;

	s = wbytes * 8 - width;

	memcpy(left, line, wbytes);
	*(left + wbytes - 1) &= 0xFF << s;

	if (w > width) { // Double-width character
		uint8_t t;

		for (i = 0; i < wbytes; i++) {
			t = *(line + wbytes + i - 1);
			t <<= 8 - s;
			t |= *(line + wbytes + i) >> s;
			*(right + i) = t;
		}
		*(right + wbytes - 1) &= 0xFF << s;
	}

	return (0);
}

static void
set_width(int v)
{
	if (v < 1 || v > VFNT_MAXDIMENSION)
		errx(1, "invalid width %d", v);
	width = v;
	wbytes = howmany(width, 8);
}

static void
set_height(int v)
{
	if (v < 1 || v > VFNT_MAXDIMENSION)
		errx(1, "invalid height %d", v);
	height = v;
}

static int
parse_bdf(FILE *fp, unsigned int map_idx)
{
	char *ln, *p;;
	size_t length;
	unsigned int linenum = 0, i, j;
	int rv = 0;

	char spacing = '\0';
	int fbbw, fbbh, fbbox, fbboy, dwx, dwy;

	fbbw = fbbh = fbbox = fbboy = dwx = dwy = 0;

	while ((ln = fgetln(fp, &length)) != NULL) {
		linenum++;
		ln[length - 1] = '\0';

		if (strncmp(ln, "FONT ", 5) == 0) {
			p = ln + 5; i = 0;
			while ((p = strchr(p, '-')) != NULL) {
				p++; i++;
				if (i == 11) {
					spacing = *p;
					break;
				}
			}
		} else if (strncmp(ln, "FONTBOUNDINGBOX ", 16) == 0 &&
		    sscanf(ln + 16, "%d %d %d %d",
				&fbbw, &fbbh, &fbbox, &fbboy) == 4) {
			set_width(fbbw);
			set_height(fbbh);
			break;
		}
	}

	if (fbbw == 0 && fbbh == 0)
		errx(1, "Broken font header!");
	if (spacing != 'c' && spacing != 'C')
		errx(1, "Font spacing \"C\" (Character cell) required");

	while ((ln = fgetln(fp, &length)) != NULL) {
		linenum++;
		ln[length - 1] = '\0';

		if (strncmp(ln, "DWIDTH ", 7) == 0 &&
		    sscanf(ln + 7, "%d %d", &dwx, &dwy) == 2) {
			if (dwy != 0 || (dwx != fbbw && dwx * 2 != fbbw))
				errx(1, "Bitmap with unsupported "
					"DWIDTH %d %d at line %u",
					dwx, dwy, linenum);
			if (dwx < fbbw)
				set_width(dwx);
		}
	}

	uint8_t *bytes = NULL, *bytes_r = NULL, *line = NULL;
	unsigned int curchar = 0, bbwbytes;
	int bbw, bbh, bbox, bboy;

	linenum = 0;
	dwx = bbw = bbh = 0;
	rewind(fp);
	while ((ln = fgetln(fp, &length)) != NULL) {
		linenum++;
		ln[length - 1] = '\0';

		if (strncmp(ln, "ENCODING ", 9) == 0)
			curchar = atoi(ln + 9);
		else if (strncmp(ln, "DWIDTH ", 7) == 0)
			dwx = atoi(ln + 7);
		else if (strncmp(ln, "BBX ", 4) == 0 &&
				sscanf(ln + 4, "%d %d %d %d",
					&bbw, &bbh, &bbox, &bboy) == 4) {
			if (bbw < 1 || bbh < 1 || bbw > fbbw || bbh > fbbh ||
					bbox < fbbox || bboy < fbboy)
				errx(1, "Broken bitmap with "
					"BBX %d %d %d %d at line %u",
					bbw, bbh, bbox, bboy, linenum);
			bbwbytes = howmany(bbw, 8);
		} else if (strncmp(ln, "BITMAP", 6) == 0 &&
		    (ln[6] == ' ' || ln[6] == '\0')) {
			if (dwx ==  0 || bbw == 0 || bbh == 0)
				errx(1, "Broken char header at line %u!",
					linenum);
			if (bytes == NULL) {
				bytes = xmalloc(wbytes * height);
				bytes_r = xmalloc(wbytes * height);
				line = xmalloc(wbytes * 2);
			} else {
				memset(bytes,   0, wbytes * height);
				memset(bytes_r, 0, wbytes * height);
			}

			/*
			 * Assume that the next _bbh_ lines are bitmap data.
			 * ENDCHAR is allowed to terminate the bitmap
			 * early but is not otherwise checked; any extra data
			 * is ignored.
			 */
			for (i = (fbbh + fbboy) - (bbh + bboy);
			    i < (unsigned int) ((fbbh + fbboy) - bboy); i++) {
				if ((ln = fgetln(fp, &length)) == NULL)
					errx(1, "Unexpected EOF!");
				linenum++;
				ln[length - 1] = '\0';

				if (strcmp(ln, "ENDCHAR") == 0)
					break;

				if (strlen(ln) < bbwbytes * 2)
					errx(1, "Broken bitmap at line %u!",
						linenum);
				memset(line, 0, wbytes * 2);
				for (j = 0; j < bbwbytes; j++) {
					unsigned int val;
					if (sscanf(ln + j * 2, "%2x", &val) == 0)
						break;
					*(line + j) = (uint8_t) val;
				}

				rv = rshift_bitmap_line(line, wbytes * 2,
					bbw, bbox - fbbox);
				if (rv != 0)
					goto out;

				rv = split_bitmap_line(bytes + i * wbytes,
					bytes_r + i * wbytes, line, dwx);
				if (rv != 0)
					goto out;
			}

			rv = add_char(curchar, map_idx, bytes,
				dwx > (int) width ? bytes_r : NULL);
			if (rv != 0)
				goto out;

			dwx = bbw = bbh = 0;
		}
	}

out:
	free(bytes);
	free(bytes_r);
	free(line);

	return (rv);
}

static int
parse_hex(FILE *fp, unsigned int map_idx)
{
	char *ln, *p;
	size_t length;
	uint8_t *bytes = NULL, *bytes_r = NULL, *line = NULL;
	unsigned int curchar = 0, gwidth, gwbytes, i, j, chars_per_row;
	int rv = 0;

	while ((ln = fgetln(fp, &length)) != NULL) {
		ln[length - 1] = '\0';

		if (strncmp(ln, "# Height: ", 10) == 0) {
			if (bytes != NULL)
				errx(1, "malformed input: "
					"Height tag after font data");
			set_height(atoi(ln + 10));
		} else if (strncmp(ln, "# Width: ", 9) == 0) {
			if (bytes != NULL)
				errx(1, "malformed input: "
					"Width tag after font data");
			set_width(atoi(ln + 9));
		} else if (sscanf(ln, "%6x:", &curchar)) {
			if (bytes == NULL) {
				bytes = xmalloc(wbytes * height);
				bytes_r = xmalloc(wbytes * height);
				line = xmalloc(wbytes * 2);
			}

			/* ln is guaranteed to have a colon here. */
			p = strchr(ln, ':') + 1;
			chars_per_row = strlen(p) / height;
			if (chars_per_row < wbytes * 2)
				errx(1, "malformed input: "
					"Broken bitmap, character %06x",
					curchar);
			gwidth = width * 2;
			gwbytes = howmany(gwidth, 8);
			if (chars_per_row < gwbytes * 2 || gwidth <= 8) {
				gwidth = width; // Single-width character
				gwbytes = wbytes;
			}

			for (i = 0; i < height; i++) {
				for (j = 0; j < gwbytes; j++) {
					unsigned int val;
					if (sscanf(p + j * 2, "%2x", &val) == 0)
						break;
					*(line + j) = (uint8_t) val;
				}
				rv = split_bitmap_line(bytes + i * wbytes,
					bytes_r + i * wbytes, line, gwidth);
				if (rv != 0)
					goto out;
				p += gwbytes * 2;
			}

			rv = add_char(curchar, map_idx, bytes,
				gwidth != width ? bytes_r : NULL);
			if (rv != 0)
				goto out;
		}
	}

out:
	free(bytes);
	free(bytes_r);
	free(line);

	return (rv);
}

static int
parse_file(const char *filename, unsigned int map_idx)
{
	FILE *fp;
	size_t len;
	int rv;

	fp = fopen(filename, "r");
	if (fp == NULL) {
		perror(filename);
		return (1);
	}
	len = strlen(filename);
	if (len > 4 && strcasecmp(filename + len - 4, ".hex") == 0)
		rv = parse_hex(fp, map_idx);
	else
		rv = parse_bdf(fp, map_idx);
	fclose(fp);

	return (rv);
}

static void
number_glyphs(void)
{
	struct glyph *gl;
	unsigned int i, idx = 0;

	for (i = 0; i < VFNT_MAPS; i++)
		TAILQ_FOREACH(gl, &glyphs[i], g_list)
			gl->g_index = idx++;
}

static int
write_glyphs(FILE *fp)
{
	struct glyph *gl;
	unsigned int i;

	for (i = 0; i < VFNT_MAPS; i++) {
		TAILQ_FOREACH(gl, &glyphs[i], g_list)
			if (fwrite(gl->g_data, wbytes * height, 1, fp) != 1)
				return (1);
	}
	return (0);
}

static void
fold_mappings(unsigned int map_idx)
{
	struct mapping_list *ml = &maps[map_idx];
	struct mapping *mn, *mp, *mbase;

	mp = mbase = TAILQ_FIRST(ml);
	for (mp = mbase = TAILQ_FIRST(ml); mp != NULL; mp = mn) {
		mn = TAILQ_NEXT(mp, m_list);
		if (mn != NULL && mn->m_char == mp->m_char + 1 &&
		    mn->m_glyph->g_index == mp->m_glyph->g_index + 1)
			continue;
		mbase->m_length = mp->m_char - mbase->m_char + 1;
		mbase = mp = mn;
		map_folded_count[map_idx]++;
	}
}

struct file_mapping {
	uint32_t	source;
	uint16_t	destination;
	uint16_t	length;
} __packed;

static int
write_mappings(FILE *fp, unsigned int map_idx)
{
	struct mapping_list *ml = &maps[map_idx];
	struct mapping *mp;
	struct file_mapping fm;
	unsigned int i = 0, j = 0;

	TAILQ_FOREACH(mp, ml, m_list) {
		j++;
		if (mp->m_length > 0) {
			i += mp->m_length;
			fm.source = htobe32(mp->m_char);
			fm.destination = htobe16(mp->m_glyph->g_index);
			fm.length = htobe16(mp->m_length - 1);
			if (fwrite(&fm, sizeof fm, 1, fp) != 1)
				return (1);
		}
	}

	assert(i == j);

	return (0);
}

struct file_header {
	uint8_t		magic[8];
	uint8_t		width;
	uint8_t		height;
	uint16_t	pad;
	uint32_t	glyph_count;
	uint32_t	map_count[4];
} __packed;

static int
write_fnt(const char *filename)
{
	FILE *fp;
	struct file_header fh = {
		.magic = "VFNT0002",
	};

	fp = fopen(filename, "wb");
	if (fp == NULL) {
		perror(filename);
		return (1);
	}

	fh.width = width;
	fh.height = height;
	fh.glyph_count = htobe32(glyph_unique);
	fh.map_count[0] = htobe32(map_folded_count[0]);
	fh.map_count[1] = htobe32(map_folded_count[1]);
	fh.map_count[2] = htobe32(map_folded_count[2]);
	fh.map_count[3] = htobe32(map_folded_count[3]);
	if (fwrite(&fh, sizeof fh, 1, fp) != 1) {
		perror(filename);
		fclose(fp);
		return (1);
	}

	if (write_glyphs(fp) != 0 ||
	    write_mappings(fp, VFNT_MAP_NORMAL) != 0 ||
	    write_mappings(fp, VFNT_MAP_NORMAL_RIGHT) != 0 ||
	    write_mappings(fp, VFNT_MAP_BOLD) != 0 ||
	    write_mappings(fp, VFNT_MAP_BOLD_RIGHT) != 0) {
		perror(filename);
		fclose(fp);
		return (1);
	}

	fclose(fp);
	return (0);
}

static void
print_font_info(void)
{
	printf(
		"Statistics:\n"
		"- font_width:                  %6u\n"
		"- font_height:                 %6u\n"
		"- glyph_total:                 %6u\n"
		"- glyph_normal:                %6u\n"
		"- glyph_normal_right:          %6u\n"
		"- glyph_bold:                  %6u\n"
		"- glyph_bold_right:            %6u\n"
		"- glyph_unique:                %6u\n"
		"- glyph_dupe:                  %6u\n"
		"- mapping_total:               %6u\n"
		"- mapping_normal:              %6u\n"
		"- mapping_normal_folded:       %6u\n"
		"- mapping_normal_right:        %6u\n"
		"- mapping_normal_right_folded: %6u\n"
		"- mapping_bold:                %6u\n"
		"- mapping_bold_folded:         %6u\n"
		"- mapping_bold_right:          %6u\n"
		"- mapping_bold_right_folded:   %6u\n"
		"- mapping_unique:              %6u\n"
		"- mapping_dupe:                %6u\n",
		width, height,
		glyph_total,
		glyph_count[0],
		glyph_count[1],
		glyph_count[2],
		glyph_count[3],
		glyph_unique, glyph_dupe,
		mapping_total,
		map_count[0], map_folded_count[0],
		map_count[1], map_folded_count[1],
		map_count[2], map_folded_count[2],
		map_count[3], map_folded_count[3],
		mapping_unique, mapping_dupe
	);
}

int
main(int argc, char *argv[])
{
	int ch, verbose = 0;

	assert(sizeof(struct file_header) == 32);
	assert(sizeof(struct file_mapping) == 8);

	while ((ch = getopt(argc, argv, "h:vw:")) != -1) {
		switch (ch) {
		case 'h':
			height = atoi(optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		case 'w':
			width = atoi(optarg);
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 2 || argc > 3)
		usage();

	set_width(width);
	set_height(height);

	if (parse_file(argv[0], VFNT_MAP_NORMAL) != 0)
		return (1);
	argc--;
	argv++;
	if (argc == 2) {
		if (parse_file(argv[0], VFNT_MAP_BOLD) != 0)
			return (1);
		argc--;
		argv++;
	}
	number_glyphs();
	dedup_mapping(VFNT_MAP_BOLD);
	dedup_mapping(VFNT_MAP_BOLD_RIGHT);
	fold_mappings(0);
	fold_mappings(1);
	fold_mappings(2);
	fold_mappings(3);
	if (write_fnt(argv[0]) != 0)
		return (1);

	if (verbose)
		print_font_info();

	return (0);
}
