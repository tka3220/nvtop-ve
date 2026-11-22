/*
 *
 * Copyright (C) 2019-2021 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/plot.h"
#include "nvtop/common.h"

#include <assert.h>
#include <langinfo.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tgmath.h>
#include <wchar.h>

static const short plot_line_colors[MAX_LINES_PER_PLOT] = {7, 8, 9, 10};

// Plot values are percentages (callers already scale every series to 0..100).
// Non-finite samples map to 0 and the rest is clamped, so both renderers place
// any sample the same way.
static inline double plot_clamp_percentage(double value) {
  if (!isfinite(value) || value < 0.)
    return 0.;
  if (value > 100.)
    return 100.;
  return value;
}

static inline int data_level(double rows, double data, double increment) {
  return (int)(rows - round(plot_clamp_percentage(data) / increment));
}

static void draw_plot_legend(WINDOW *win, int rows, int cols, unsigned num_lines, bool legend_left,
                             char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  int plot_y_position = 0;
  for (unsigned i = 0; i < num_lines && plot_y_position < rows - 1; ++i) {
    wcolor_set(win, plot_line_colors[i], NULL);
    if (legend_left) {
      mvwprintw(win, plot_y_position, 0, "%.*s", cols, legend[i]);
    } else {
      size_t length = strlen(legend[i]);
      if (length <= (size_t)cols) {
        mvwprintw(win, plot_y_position, cols - length, "%s", legend[i]);
      } else {
        mvwprintw(win, plot_y_position, 0, "%s", legend[i] + length - cols);
      }
    }
    plot_y_position++;
  }
}

// Legacy renderer using ncurses line-drawing characters. Kept as a fallback
// for terminals or locales where Unicode braille patterns are unavailable.
// Each update occupies num_lines consecutive columns, one per metric, so the
// graph advances by num_lines columns per update.
static void nvtop_line_plot_line(WINDOW *win, size_t num_samples, const double *data, unsigned num_lines,
                                 bool newest_left) {
  int rows = getmaxy(win);
  int cols = getmaxx(win);
  rows -= 1;
  if (rows <= 0 || cols <= 0)
    return;
  double increment = 100. / (double)(rows);

  assert(num_lines <= MAX_LINES_PER_PLOT && "Cannot plot more than " EXPAND_AND_QUOTE(MAX_LINES_PER_PLOT) " lines");
  unsigned lvl_before[MAX_LINES_PER_PLOT];
  for (size_t k = 0; k < num_lines; ++k)
    lvl_before[k] = data_level(rows, data[k], increment);

  for (size_t sample = 0; sample < num_samples; ++sample) {
    for (unsigned k = 0; k < num_lines; ++k) {
      int base_x = newest_left ? 0 : cols - (int)(num_samples * num_lines);
      int x = base_x + (int)(sample * num_lines + k);
      if (x < 0 || x >= cols)
        continue;
      unsigned lvl_now_k = data_level(rows, data[sample * num_lines + k], increment);
      wcolor_set(win, plot_line_colors[k], NULL);
      // Three cases: has increased, has decreased and remained level
      if (lvl_before[k] < lvl_now_k || lvl_before[k] > lvl_now_k) {
        // Case 1 and 2: has increased/decreased

        // An increase goes down on the plot because (0,0) is top left
        bool drawing_down = lvl_before[k] < lvl_now_k;
        unsigned bottom = drawing_down ? lvl_before[k] : lvl_now_k;
        unsigned top = drawing_down ? lvl_now_k : lvl_before[k];

        // Draw the vertical line corners
        mvwaddch(win, bottom, x, drawing_down ? ACS_URCORNER : ACS_ULCORNER);
        mvwaddch(win, top, x, drawing_down ? ACS_LLCORNER : ACS_LRCORNER);
        // Draw the vertical line between the corners
        if (top - bottom > 1) {
          mvwvline(win, bottom + 1, x, 0, top - bottom - 1);
        }

        // Draw the continuation of the other metrics
        for (unsigned j = 0; j < num_lines; ++j) {
          if (j != k) {
            if (lvl_before[j] == top)
              // The continuation is at the same level as the bottom corner
              mvwaddch(win, top, x, ACS_BTEE);
            else if (lvl_before[j] == bottom)
              // The continuation is at the same level as the top corner
              mvwaddch(win, bottom, x, ACS_TTEE);
            else if (lvl_before[j] > bottom && lvl_before[j] < top)
              // The continuation lies on the vertical line
              mvwaddch(win, lvl_before[j], x, ACS_PLUS);
            else {
              // The continuation lies outside the update interval so keep the
              // color
              wcolor_set(win, plot_line_colors[j], NULL);
              mvwaddch(win, lvl_before[j], x, ACS_HLINE);
              wcolor_set(win, plot_line_colors[k], NULL);
            }
          }
        }
      } else {
        // Case 3: stayed level
        mvwhline(win, lvl_now_k, x, 0, 1);
        for (unsigned j = 0; j < num_lines; ++j) {
          if (j != k) {
            if (lvl_before[j] != lvl_now_k) {
              // Add the continuation of other metric lines
              wcolor_set(win, plot_line_colors[j], NULL);
              mvwaddch(win, lvl_before[j], x, ACS_HLINE);
              wcolor_set(win, plot_line_colors[k], NULL);
            }
          }
        }
      }
      lvl_before[k] = lvl_now_k;
    }
  }
}

#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR

#define BRAILLE_BASE 0x2800u
#define BRAILLE_CELL_ROWS 4
#define BRAILLE_CELL_COLS 2

// Braille dots are numbered 1..8 and stored in bits that do not follow the
// on-screen 2x4 layout. This table maps a (row, column) position inside a
// character cell to the corresponding bit.
static const uint8_t braille_dot_bits[BRAILLE_CELL_ROWS][BRAILLE_CELL_COLS] = {
    {0x01, 0x08}, // row 0: dots 1 and 4
    {0x02, 0x10}, // row 1: dots 2 and 5
    {0x04, 0x20}, // row 2: dots 3 and 6
    {0x40, 0x80}, // row 3: dots 7 and 8
};

static bool locale_encodes_utf8(void) {
  const char *codeset = nl_langinfo(CODESET);
  return codeset && (strcasecmp(codeset, "UTF-8") == 0 || strcasecmp(codeset, "UTF8") == 0);
}

static bool terminal_supports_braille(void) {
  // Braille is only printable when the locale is UTF-8 and the wide character
  // is printable in that encoding.
  return MB_CUR_MAX > 1 && locale_encodes_utf8() && wcwidth((wchar_t)(BRAILLE_BASE + 0xFF)) > 0;
}

static unsigned plot_value_to_dot_row(double value, unsigned dot_rows) {
  double row = (100. - plot_clamp_percentage(value)) * (double)(dot_rows - 1) / 100.;
  return (unsigned)llround(row);
}

// Spread the samples over the full width of the graph so each plot line uses
// every column available in the window.
static inline void braille_set_dot(uint8_t *masks, unsigned rows, unsigned cols, unsigned line, int x, int y) {
  if (x < 0 || y < 0 || x >= (int)(cols * BRAILLE_CELL_COLS) || y >= (int)(rows * BRAILLE_CELL_ROWS))
    return;
  unsigned cell_col = (unsigned)x / BRAILLE_CELL_COLS;
  unsigned cell_row = (unsigned)y / BRAILLE_CELL_ROWS;
  size_t cell = (size_t)cell_row * cols + cell_col;
  masks[(size_t)line * rows * cols + cell] |=
      braille_dot_bits[(unsigned)y % BRAILLE_CELL_ROWS][(unsigned)x % BRAILLE_CELL_COLS];
}

// Bresenham line drawing in dot space. Connecting consecutive samples produces
// a continuous curve instead of a chain of horizontal/vertical steps.
static void braille_draw_segment(uint8_t *masks, unsigned rows, unsigned cols, unsigned line, int x0, int y0, int x1,
                                 int y1) {
  int dx = abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    braille_set_dot(masks, rows, cols, line, x0, y0);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static unsigned bit_count8(uint8_t value) {
  unsigned count = 0;
  while (value) {
    count += value & 1u;
    value >>= 1;
  }
  return count;
}

// Highest row (smallest index) touched by a braille mask. Used to color
// overlapping lines the same way a painter would: the line visible on top
// owns the cell.
static unsigned braille_top_row(uint8_t mask) {
  if (mask & 0x09) // dots 1 and 4
    return 0;
  if (mask & 0x12) // dots 2 and 5
    return 1;
  if (mask & 0x24) // dots 3 and 6
    return 2;
  return 3;
}

// Map a sample index onto the braille dot columns, spreading the samples over
// the full width.
static unsigned sample_to_column(size_t sample, size_t num_samples, unsigned columns) {
  if (num_samples <= 1 || columns <= 1)
    return 0;
  double column = (double)sample * (double)(columns - 1) / (double)(num_samples - 1);
  return (unsigned)llround(column);
}

// Draw the plot using Unicode braille patterns. Each character cell provides a
// 2x4 dot matrix, i.e. eight times more detail than a regular character.
// masks must hold num_lines * rows * cols bytes.
static void nvtop_line_plot_braille(WINDOW *win, size_t num_samples, const double *data, unsigned num_lines, int rows,
                                    int cols, uint8_t *masks) {
  size_t cell_count = (size_t)rows * (size_t)cols;
  memset(masks, 0, (size_t)num_lines * cell_count);

  unsigned dot_cols = (unsigned)cols * BRAILLE_CELL_COLS;
  unsigned dot_rows = (unsigned)rows * BRAILLE_CELL_ROWS;

  for (unsigned line = 0; line < num_lines; ++line) {
    int prev_x = 0, prev_y = 0;
    for (size_t sample = 0; sample < num_samples; ++sample) {
      double value = data[sample * num_lines + line];
      int x = (int)sample_to_column(sample, num_samples, dot_cols);
      int y = (int)plot_value_to_dot_row(value, dot_rows);
      if (sample == 0)
        braille_set_dot(masks, (unsigned)rows, (unsigned)cols, line, x, y);
      else
        braille_draw_segment(masks, (unsigned)rows, (unsigned)cols, line, prev_x, prev_y, x, y);
      prev_x = x;
      prev_y = y;
    }
  }

  for (unsigned row = 0; row < (unsigned)rows; ++row) {
    for (unsigned col = 0; col < (unsigned)cols; ++col) {
      size_t cell = (size_t)row * cols + col;
      uint8_t mask = 0;
      unsigned tied[MAX_LINES_PER_PLOT];
      unsigned tied_count = 0;
      unsigned best_top = BRAILLE_CELL_ROWS, best_dots = 0;
      for (unsigned line = 0; line < num_lines; ++line) {
        uint8_t line_mask = masks[(size_t)line * cell_count + cell];
        if (!line_mask)
          continue;
        mask |= line_mask;
        unsigned top = braille_top_row(line_mask);
        unsigned dots = bit_count8(line_mask);
        if (top < best_top || (top == best_top && dots > best_dots)) {
          best_top = top;
          best_dots = dots;
          tied_count = 0;
          tied[tied_count++] = line;
        } else if (top == best_top && dots == best_dots) {
          tied[tied_count++] = line;
        }
      }
      if (mask) {
        // Lines that overlap exactly tie for the cell. Rotate the color between
        // them instead of always letting the first line win, so both series
        // remain visible along a shared level.
        unsigned owner = tied[col % tied_count];
        wchar_t glyph[2] = {(wchar_t)(BRAILLE_BASE + mask), L'\0'};
        wcolor_set(win, plot_line_colors[owner], NULL);
        mvwaddnwstr(win, row, col, glyph, 1);
      }
    }
  }
}

#endif // NCURSES_WIDECHAR

// The locale is set once at startup (before any drawing), so the capability
// checks are computed on first use and cached.
bool nvtop_plot_supports_braille(void) {
#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
  static int supported = -1;
  if (supported < 0)
    supported = terminal_supports_braille();
  return supported;
#else
  return false;
#endif
}

bool nvtop_plot_supports_unicode_blocks(void) {
#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
  static int supported = -1;
  if (supported < 0)
    supported = MB_CUR_MAX > 1 && locale_encodes_utf8() && wcwidth((wchar_t)0x258F) > 0;
  return supported;
#else
  return false;
#endif
}

bool nvtop_plot_supports_middle_dot(void) {
#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
  static int supported = -1;
  if (supported < 0)
    supported = nvtop_plot_supports_unicode_blocks() && wcwidth((wchar_t)0x00B7) == 1;
  return supported;
#else
  return false;
#endif
}

void nvtop_line_plot(WINDOW *win, size_t num_samples, const double *data, unsigned num_lines,
                     enum nvtop_plot_style style, bool newest_left, uint8_t *braille_masks,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  if (num_samples == 0 || num_lines == 0 || num_lines > MAX_LINES_PER_PLOT)
    return;
#if !defined(NCURSES_WIDECHAR) || !NCURSES_WIDECHAR
  // Only the braille renderer, which needs wide characters, uses them.
  (void)style;
  (void)braille_masks;
#endif
  int rows, cols;
  getmaxyx(win, rows, cols);
  if (rows <= 0 || cols <= 0)
    return;

#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
  if (style == nvtop_plot_style_braille && nvtop_plot_supports_braille())
    nvtop_line_plot_braille(win, num_samples, data, num_lines, rows, cols, braille_masks);
  else
#endif
    nvtop_line_plot_line(win, num_samples, data, num_lines, newest_left);

  // The legend sits on the side of the oldest data.
  draw_plot_legend(win, rows, cols, num_lines, !newest_left, legend);
}

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY) {
  mvwhline(win, startY, startX + 1, 0, sizeX - 2);
  mvwhline(win, startY + sizeY - 1, startX + 1, 0, sizeX - 2);

  mvwvline(win, startY + 1, startX, 0, sizeY - 2);
  mvwvline(win, startY + 1, startX + sizeX - 1, 0, sizeY - 2);

  mvwaddch(win, startY, startX, ACS_ULCORNER);
  mvwaddch(win, startY, startX + sizeX - 1, ACS_URCORNER);
  mvwaddch(win, startY + sizeY - 1, startX, ACS_LLCORNER);
  mvwaddch(win, startY + sizeY - 1, startX + sizeX - 1, ACS_LRCORNER);
}
