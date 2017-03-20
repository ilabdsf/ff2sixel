#include <arpa/inet.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Sixel output buffer.
 */

static uint32_t cursor_x;
static uint8_t sixel_buf; /* Buffered sixel. */
static uint32_t sixel_count; /* Number of buffered sixels. */

/* Flush sixel buffer to stdout. */
static void
sixel_flush(void)
{
	/* Choose shortest representation. */
	if (sixel_count > 3) {
		/* Use run length encoding. */
		printf("!%d%c", sixel_count, sixel_buf);
	} else {
		while (sixel_count--)
			putchar(sixel_buf);
	}
	sixel_count = 0;
}

/* Put one sixel into buffer. */
static void
sixel_put(uint8_t sixel)
{
	sixel += '?'; /* Convert sixel to printable character. */
	if (sixel != sixel_buf) {
		sixel_flush();
	}
	sixel_buf = sixel;
	sixel_count++;
}

/* Carriage return. */
static void
sixel_cr(void) {
	putchar('$');
	cursor_x = 0;
}

/*
 * Palette.
 */

typedef struct {
	uint8_t red, green, blue;
} Color;

typedef struct Paint Paint;
struct Paint {
	Color color;
	bool introduced; /* Color was introduced and can be referenced by index. */
	bool used; /* Entry is allocated. */
};

static Paint *palette_selected = NULL;
static Paint *palette_top;
static Paint palette_tab[256];

/* Initialize palette. */
static void
palette_init(void)
{
	int n;

	palette_top = NULL;

	for (n = 255; n >= 0; n--) {
		palette_tab[n].introduced = false;
		palette_tab[n].used = false;
	}
}

/* Return color number. */
static uint8_t
palette_index(Paint *p) {
	return p - palette_tab;
}

/* Reset `used' flag on all palette entries. */
static void
palette_reset_used(void)
{
	uint16_t i;

	for (i = 0; i < 256; i++)
		palette_tab[i].used = false;

}

/* Select color from palette. */
static void
palette_select(Paint *color)
{
	if (palette_selected == color)
		return;

	printf("#%d", palette_index(color));
	if (!color->introduced) {
		printf(";2;%d;%d;%d", color->color.red, color->color.green, color->color.blue);
		color->introduced = true;
	}

	palette_selected = color;
}

static Paint *
palette_alloc(Color color)
{
	uint16_t i;
	Paint *curr;

	for (i = 0; i < 256; i++) {
		curr = &palette_tab[i];

		if (curr->color.red == color.red &&
		    curr->color.green == color.green &&
		    curr->color.blue == color.blue) {
			curr->used = true;
			return curr;
		}

		if (!curr->used)
			break;
	}

	if (curr->used) {
		/* Reached end of palette */
		return NULL;
	}

	if (palette_selected == curr)
		palette_selected = NULL;
	curr->color = color;
	curr->introduced = false;
	curr->used = true;
	return curr;
}

/*
 * Spans.
 */

typedef struct Span Span;
struct Span {
	Span *next;
	Paint *color;
	uint32_t lo, hi;
	uint8_t	*map;
};

static Span *span_top;

static uint32_t width;
static uint32_t height;
static uint8_t *sixels;

static void
span_add(Paint *color, uint32_t lo, uint32_t hi, uint8_t *map)
{
	Span **prev, *curr, *node;

	node = malloc(sizeof *node);
	if (node == NULL)
		err(1, "malloc");

	node->color = color;
	node->lo = lo;
	node->hi = hi;
	node->map = map;

	prev = &span_top;
	for (curr = *prev; curr != NULL; curr = curr->next) {
		if (node->lo < curr->lo)
			break;
		if (node->lo == curr->lo && node->hi > curr->hi)
			break;
		prev = &curr->next;
	}

	node->next = curr;
	*prev = node;
}

static void
span_line(uint8_t index, uint8_t *map)
{
	Paint *color = &palette_tab[index];
	uint32_t lo, hi;

	for (lo = 0; lo < width; lo++) {
		if (map[lo] == 0)
			continue;

		for (hi = lo + 1; hi < width; hi++) {
			if (map[hi] == 0)
				break;
		}

		span_add(color, lo, hi, map);
		lo = hi - 1;
	}
}

static void
span_flush_iter(void)
{
	Span **prev, *curr;
	prev = &span_top;
	for (curr = *prev; curr != NULL; curr = *prev) {
		if (curr->lo < cursor_x) {
			prev = &curr->next;
			continue;
		}
		palette_select(curr->color);
		while (cursor_x < curr->lo) {
			sixel_put(0);
			cursor_x++;
		}
		while (cursor_x < curr->hi) {
			sixel_put(curr->map[cursor_x]);
			curr->map[cursor_x] = 0;
			cursor_x++;
		}
		sixel_flush();

		*prev = curr->next;
		free(curr);
	}
}

static void
span_flush(void)
{
	for (uint32_t n = 0; n < 256; n++)
		span_line(n, sixels + width * n);

	/* Iterate until there are no spans. */
	for (; span_top != NULL; sixel_cr()) {
		span_flush_iter();
	}

	/* Now we are sure no colors are referenced by spans. */
	palette_reset_used();
}

int
main(int argc, char *argv[])
{
	if (argc != 1) {
		fputs("usage: ff2sixel\n", stderr);
		return 1;
	}

	uint32_t hdr[4];
	uint32_t x, y;
	uint16_t c[4];

	if (fread(hdr, sizeof(*hdr), 4, stdin) != 4)
		goto readerr;

	if (memcmp("farbfeld", hdr, sizeof("farbfeld") - 1))
		errx(1, "invalid magic value");

	width = ntohl(hdr[2]);
	height = ntohl(hdr[3]);

	if (width > SIZE_MAX / 256)
		errx(1, "row length integer overflow");

	int i;

	sixels = malloc(256 * width);
	if (sixels == NULL)
	    err(1, "malloc");

	memset(sixels, 0, 256 * width);

	palette_init();

	printf("\033Pq"
	       "\"1;1;%" PRIu32 ";%" PRIu32 "\n", width, height);
	for (y = 0; y < height; y += 6) {
		for (i = 0; i < 6 && y + i < height; i++) {
			for (x = 0; x < width; x++) {
				if (fread(c, sizeof(uint16_t), 4, stdin) != 4) {
					goto readerr;
				}
				uint16_t alpha = ntohs(c[3]);
				if (alpha == 0) {
					continue;
				}
				for (uint8_t j = 0; j < 3; j++) {
					/* Black background is assumed. */
					c[j] = ((uint32_t) ntohs(c[j]) * (uint32_t) alpha) >> 16;
				}
				Color color = {
					c[0] * 100 / 65536,
					c[1] * 100 / 65536,
					c[2] * 100 / 65536
				};

				Paint *entry = palette_alloc(color);
				if (entry == NULL) {
					/* Flush palette. */
					span_flush();
					entry = palette_alloc(color);
					assert(entry != NULL);
				}

				sixels[palette_index(entry) * width + x] |= 1 << i;
			}
		}
		span_flush();
		fputs("-", stdout);
	}
	fputs("\033\\", stdout);

	free(sixels);
	return 0;

readerr:
	if (ferror(stdin)) {
		err(1, "fread");
	} else {
		errx(1, "unexpected end of file\n");
	}
}
