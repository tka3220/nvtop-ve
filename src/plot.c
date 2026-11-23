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

// Mark next to the legend of a line hidden behind another one, drawn in the
// color of the line hiding it, on the side of the label facing the plot: after
// a left aligned label, before a right aligned one, so the label itself never
// moves. The arrow is the fallback for narrow plots; an ASCII "v" replaces it
// where the locale does not print it in one column. A space separates the mark
// from the label.
#define PLOT_LEGEND_BEHIND "(behind)"
#define PLOT_LEGEND_BEHIND_SHORT_WIDTH 1

#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
static const wchar_t *plot_legend_behind_short(void) {
  static int arrow_printable = -1;
  if (arrow_printable < 0)
    arrow_printable = wcwidth(L'\u2193') == 1;
  return arrow_printable ? L"\u2193" : L"v";
}
#endif

// behind[i] is the line hiding line i, or -1. Only the braille renderer, which
// requires a UTF-8 locale, reports hidden lines.
static void draw_plot_legend(WINDOW *win, int rows, int cols, unsigned num_lines, bool legend_left,
                             char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE],
                             const int behind[MAX_LINES_PER_PLOT]) {
  int plot_y_position = 0;
  for (unsigned i = 0; i < num_lines && plot_y_position < rows - 1; ++i) {
    size_t length = strlen(legend[i]);
    enum { mark_none, mark_long, mark_short } mark = mark_none;
    // Mark and separating space.
    size_t mark_width = 0;
#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
    if (behind[i] >= 0) {
      if (length + 1 + strlen(PLOT_LEGEND_BEHIND) <= (size_t)cols) {
        mark = mark_long;
        mark_width = 1 + strlen(PLOT_LEGEND_BEHIND);
      } else if (length + 1 + PLOT_LEGEND_BEHIND_SHORT_WIDTH <= (size_t)cols) {
        mark = mark_short;
        mark_width = 1 + PLOT_LEGEND_BEHIND_SHORT_WIDTH;
      }
    }
#endif
    wcolor_set(win, plot_line_colors[i], NULL);
    int mark_x;
    if (legend_left) {
      mvwprintw(win, plot_y_position, 0, "%.*s", cols, legend[i]);
      mark_x = (int)length + 1;
    } else if (length <= (size_t)cols) {
      mvwprintw(win, plot_y_position, cols - (int)length, "%s", legend[i]);
      mark_x = cols - (int)(length + mark_width);
    } else {
      mvwprintw(win, plot_y_position, 0, "%s", legend[i] + length - cols);
      mark_x = 0;
    }
    if (mark != mark_none) {
      wcolor_set(win, plot_line_colors[behind[i]], NULL);
#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
      if (mark == mark_long)
        mvwprintw(win, plot_y_position, mark_x, "%s", PLOT_LEGEND_BEHIND);
      else
        mvwaddwstr(win, plot_y_position, mark_x, plot_legend_behind_short());
      // The separator overwrites any plot dot between the label and the mark.
      mvwaddch(win, plot_y_position, legend_left ? (int)length : cols - (int)length - 1, ' ');
#endif
    }
    (void)mark_x;
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

static inline void braille_set_dot(uint8_t *masks, unsigned rows, unsigned cols, unsigned line, int x, int y) {
  if (x < 0 || y < 0 || x >= (int)(cols * BRAILLE_CELL_COLS) || y >= (int)(rows * BRAILLE_CELL_ROWS))
    return;
  unsigned cell_col = (unsigned)x / BRAILLE_CELL_COLS;
  unsigned cell_row = (unsigned)y / BRAILLE_CELL_ROWS;
  size_t cell = (size_t)cell_row * cols + cell_col;
  masks[(size_t)line * rows * cols + cell] |=
      braille_dot_bits[(unsigned)y % BRAILLE_CELL_ROWS][(unsigned)x % BRAILLE_CELL_COLS];
}

static inline void braille_set_vertical(uint8_t *masks, unsigned rows, unsigned cols, unsigned line, int x, int y0,
                                        int y1) {
  int step = y0 <= y1 ? 1 : -1;
  for (int y = y0;; y += step) {
    braille_set_dot(masks, rows, cols, line, x, y);
    if (y == y1)
      break;
  }
}

// Bresenham line drawing in dot space, for sample layouts that are not aligned
// on the character cells.
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

// Connect an older sample to the next newer one. Each sample is a single dot
// and the climb between them is drawn in the dot column between them, or in
// the newer sample's column when they are adjacent. With the cell aligned
// layouts of braille_sample_column these dots are always in the newer sample's
// character cell, so a cell is final once its samples are known and moves as
// a whole when the graph scrolls. Dots outside the graph are dropped.
static void braille_connect_samples(uint8_t *masks, unsigned rows, unsigned cols, unsigned line, int x_old, int y_old,
                                    int x_new, int y_new) {
  braille_set_dot(masks, rows, cols, line, x_old, y_old);
  braille_set_dot(masks, rows, cols, line, x_new, y_new);
  int distance = abs(x_new - x_old);
  int climb = abs(y_new - y_old);
  int step = y_old < y_new ? 1 : -1;
  if (distance == 2) {
    int x_mid = (x_old + x_new) / 2;
    if (climb <= 1)
      braille_set_dot(masks, rows, cols, line, x_mid, y_old);
    else
      braille_set_vertical(masks, rows, cols, line, x_mid, y_old + step, y_new - step);
  } else if (distance == 1) {
    if (climb > 1)
      braille_set_vertical(masks, rows, cols, line, x_new, y_old + step, y_new - step);
  } else {
    braille_draw_segment(masks, rows, cols, line, x_old, y_old, x_new, y_new);
  }
}

// Highest row (smallest index) touched by a braille mask. The line with the
// highest dot owns a cell shared by several lines.
static unsigned braille_top_row(uint8_t mask) {
  if (mask & 0x09) // dots 1 and 4
    return 0;
  if (mask & 0x12) // dots 2 and 5
    return 1;
  if (mask & 0x24) // dots 3 and 6
    return 2;
  return 3;
}

// Map a sample index onto the braille dot columns.
//
// nvtop draws one or two samples per character column, plus one older sample
// that is not shown: only the dots joining it to the oldest shown sample are,
// so the oldest column looks the same as before it became the oldest. Samples
// are then on whole dot columns (*aligned is set) and scrolling by one column's
// worth of samples moves the graph by exactly one character column. With one
// sample per column, the sample is on the dot of its cell facing the newest
// data: the newest sample touches the edge of the graph and the free dot
// column between two samples belongs to the newer cell. Other sample counts
// are spread over the full width.
static int braille_sample_column(size_t sample, size_t num_samples, unsigned columns, bool newest_left, bool *aligned) {
  *aligned = true;
  size_t shown = num_samples - 1;
  // Without the hidden sample, the indices are shifted by one when the oldest
  // sample comes first.
  int shift = newest_left ? 0 : 1;
  if (num_samples > 1 && shown * BRAILLE_CELL_COLS == columns)
    return ((int)sample - shift) * BRAILLE_CELL_COLS + shift;
  if (num_samples > 1 && shown == columns)
    return (int)sample - shift;
  *aligned = false;
  if (num_samples <= 1 || columns <= 1)
    return 0;
  double column = (double)sample * (double)(columns - 1) / (double)(num_samples - 1);
  return (int)llround(column);
}

// Draw the plot using Unicode braille patterns. Each character cell provides a
// 2x4 dot matrix, i.e. eight times more detail than a regular character.
// Every line keeps all its dots; a cell shared by several lines takes the color
// of its owner (see struct nvtop_braille_plot). behind[line] is set to the
// owner hiding that line in the newest column, or -1 when the line is visible
// there: a line is hidden when it owns no cell of the newest column and every
// dot it has there is also drawn by another line.
static void nvtop_line_plot_braille(WINDOW *win, size_t num_samples, const double *data, unsigned num_lines, int rows,
                                    int cols, bool newest_left, struct nvtop_braille_plot *state,
                                    int behind[MAX_LINES_PER_PLOT]) {
  uint8_t *masks = state->masks;
  size_t cell_count = (size_t)rows * (size_t)cols;
  memset(masks, 0, (size_t)num_lines * cell_count);

  unsigned dot_cols = (unsigned)cols * BRAILLE_CELL_COLS;
  unsigned dot_rows = (unsigned)rows * BRAILLE_CELL_ROWS;

  bool aligned = false;
  for (unsigned line = 0; line < num_lines; ++line) {
    int prev_x = 0, prev_y = 0;
    for (size_t sample = 0; sample < num_samples; ++sample) {
      double value = data[sample * num_lines + line];
      int x = braille_sample_column(sample, num_samples, dot_cols, newest_left, &aligned);
      int y = (int)plot_value_to_dot_row(value, dot_rows);
      if (sample == 0)
        braille_set_dot(masks, (unsigned)rows, (unsigned)cols, line, x, y);
      else if (newest_left) // Lower indices are newer
        braille_connect_samples(masks, (unsigned)rows, (unsigned)cols, line, x, y, prev_x, prev_y);
      else
        braille_connect_samples(masks, (unsigned)rows, (unsigned)cols, line, prev_x, prev_y, x, y);
      prev_x = x;
      prev_y = y;
    }
  }

  // When the oldest column scrolls out, the next one becomes the oldest and
  // inherits what it inherited before (see next_seed below). With several
  // samples per column, that is the frame grouping the samples the same way, as
  // many samples later as a column holds. Spread layouts do not scroll by whole
  // columns and have no history.
  uint8_t *seed_owner = NULL, *next_seed = NULL;
  if (aligned) {
    size_t samples_per_column = (num_samples - 1) / (size_t)cols;
    unsigned grouping = (unsigned)(state->scrolls % samples_per_column);
    seed_owner = state->seed_owner[grouping];
    next_seed = state->next_seed[grouping];
    if (state->seed_scroll[grouping] != state->scrolls) {
      // First frame since new data arrived: inherit from the frame one column
      // scroll ago, or start without history if that frame was not drawn.
      if (state->next_seed_scroll[grouping] + samples_per_column == state->scrolls)
        memcpy(seed_owner, next_seed, (size_t)rows);
      else
        memset(seed_owner, 0, (size_t)rows);
      state->seed_scroll[grouping] = state->scrolls;
    }
    state->next_seed_scroll[grouping] = state->scrolls;
  }

  bool visible[MAX_LINES_PER_PLOT];
  for (unsigned line = 0; line < num_lines; ++line) {
    visible[line] = false;
    behind[line] = -1;
  }

  int newest_col = newest_left ? 0 : cols - 1;
  for (unsigned row = 0; row < (unsigned)rows; ++row) {
    // Owner of the previous cell in time order (line + 1, 0 for none).
    unsigned last_owner = seed_owner ? seed_owner[row] : 0;
    // Walk from the oldest column to the newest one.
    for (unsigned step = 0; step < (unsigned)cols; ++step) {
      int col = newest_left ? cols - 1 - (int)step : (int)step;
      size_t cell = (size_t)row * cols + (size_t)col;
      uint8_t mask = 0;
      unsigned best_top = BRAILLE_CELL_ROWS;
      bool last_owner_on_top = false;
      unsigned owner = 0;
      for (unsigned line = 0; line < num_lines; ++line) {
        uint8_t line_mask = masks[(size_t)line * cell_count + cell];
        if (!line_mask)
          continue;
        mask |= line_mask;
        unsigned top = braille_top_row(line_mask);
        if (top < best_top) {
          best_top = top;
          owner = line;
          last_owner_on_top = false;
        }
        if (top == best_top && line + 1 == last_owner)
          last_owner_on_top = true;
      }
      if (mask) {
        // Among the lines tied for the highest dot, the previous owner keeps
        // the cell; otherwise the first of them (lowest index) gets it.
        if (last_owner_on_top)
          owner = last_owner - 1;
        last_owner = owner + 1;
        wchar_t glyph[2] = {(wchar_t)(BRAILLE_BASE + mask), L'\0'};
        wcolor_set(win, plot_line_colors[owner], NULL);
        mvwaddnwstr(win, row, col, glyph, 1);

        if (col == newest_col) {
          for (unsigned line = 0; line < num_lines; ++line) {
            uint8_t line_mask = masks[(size_t)line * cell_count + cell];
            if (!line_mask || visible[line])
              continue;
            uint8_t others = 0;
            for (unsigned other = 0; other < num_lines; ++other)
              if (other != line)
                others |= masks[(size_t)other * cell_count + cell];
            if (line == owner || (line_mask & ~others)) {
              visible[line] = true;
              behind[line] = -1;
            } else if (behind[line] < 0) {
              // Rows are walked top to bottom: the first hiding owner is the
              // one covering the highest dot of the line.
              behind[line] = (int)owner;
            }
          }
        }
      }
      // After the next column scroll, the oldest column is this frame's second
      // one, which inherits the running owner after this frame's first one.
      if (step == 0 && next_seed)
        next_seed[row] = (uint8_t)last_owner;
    }
  }
}

#endif // NCURSES_WIDECHAR

bool nvtop_braille_plot_init(struct nvtop_braille_plot *plot, unsigned rows, unsigned cols) {
  memset(plot, 0, sizeof(*plot));
  plot->rows = rows;
  plot->cols = cols;
  // Sized for the most lines a plot can hold, independently of the plot style.
  // An empty window still gets a buffer, so allocation failures are the only
  // reason to fail.
  size_t mask_count = (size_t)MAX_LINES_PER_PLOT * (size_t)rows * (size_t)cols;
  plot->masks = malloc(mask_count ? mask_count : 1);
  // A fresh plot has no owner history.
  size_t row_count = rows ? rows : 1;
  uint8_t *owners = calloc(row_count, 2 * PLOT_MAX_SAMPLES_PER_COLUMN);
  if (!plot->masks || !owners) {
    free(owners);
    nvtop_braille_plot_free(plot);
    return false;
  }
  for (unsigned i = 0; i < PLOT_MAX_SAMPLES_PER_COLUMN; ++i) {
    plot->seed_owner[i] = owners + 2 * i * row_count;
    plot->next_seed[i] = owners + (2 * i + 1) * row_count;
  }
  return true;
}

void nvtop_braille_plot_free(struct nvtop_braille_plot *plot) {
  free(plot->masks);
  // All the owner arrays share the allocation of the first one.
  free(plot->seed_owner[0]);
  memset(plot, 0, sizeof(*plot));
}

void nvtop_braille_plot_scrolled(struct nvtop_braille_plot *plot) { plot->scrolls++; }

size_t nvtop_plot_braille_samples(unsigned cols, unsigned samples_per_column) {
  return (size_t)cols * samples_per_column + 1;
}

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
                     enum nvtop_plot_style style, bool newest_left, struct nvtop_braille_plot *braille,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  if (num_samples == 0 || num_lines == 0 || num_lines > MAX_LINES_PER_PLOT)
    return;
#if !defined(NCURSES_WIDECHAR) || !NCURSES_WIDECHAR
  // Only the braille renderer, which needs wide characters, uses them.
  (void)style;
  (void)braille;
#endif
  int rows, cols;
  getmaxyx(win, rows, cols);
  if (rows <= 0 || cols <= 0)
    return;

  int behind[MAX_LINES_PER_PLOT];
  for (unsigned line = 0; line < num_lines; ++line)
    behind[line] = -1;
#if defined(NCURSES_WIDECHAR) && NCURSES_WIDECHAR
  // The braille state must match the window; otherwise its buffers are too
  // small and the legacy renderer is used instead.
  bool braille_state_fits =
      braille && braille->masks && braille->rows == (unsigned)rows && braille->cols == (unsigned)cols;
  if (style == nvtop_plot_style_braille && nvtop_plot_supports_braille() && braille_state_fits) {
    nvtop_line_plot_braille(win, num_samples, data, num_lines, rows, cols, newest_left, braille, behind);
  } else
#endif
    nvtop_line_plot_line(win, num_samples, data, num_lines, newest_left);

  // The legend sits on the side of the oldest data.
  draw_plot_legend(win, rows, cols, num_lines, !newest_left, legend, behind);
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
