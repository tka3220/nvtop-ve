/*
 * Braille plot rendering tests: which line owns (colors) a character cell
 * shared by several lines, and the legend mark of lines hidden behind others.
 *
 * Plots are rendered into an off-screen ncurses window and read back cell by
 * cell. Line i is drawn with color pair 7 + i.
 */

#define _XOPEN_SOURCE_EXTENDED 1

#include "nvtop/plot.h"

#include <locale.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if !defined(NCURSES_WIDECHAR) || !NCURSES_WIDECHAR

// The braille renderer needs the wide character ncurses API.
int main(void) { return 77; }

#else

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                                  \
      return EXIT_FAILURE;                                                                                             \
    }                                                                                                                  \
  } while (0)

#define SKIP_RETURN_CODE 77
#define MAX_SAMPLES 64
#define PAIR_LINE0 7
#define PAIR_LINE1 8

static char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE] = {"A", "B", "C", "D"};

static short cell_pair(WINDOW *win, int y, int x) {
  cchar_t cell;
  wchar_t text[CCHARW_MAX + 1];
  attr_t attrs;
  short pair = -1;
  mvwin_wch(win, y, x, &cell);
  getcchar(&cell, text, &attrs, &pair, NULL);
  return pair;
}

static wchar_t cell_char(WINDOW *win, int y, int x) {
  cchar_t cell;
  wchar_t text[CCHARW_MAX + 1] = {0};
  attr_t attrs;
  short pair;
  mvwin_wch(win, y, x, &cell);
  getcchar(&cell, text, &attrs, &pair, NULL);
  return text[0];
}

// True when the cells [from, to) of a row of win have color pair `pair`.
static bool row_has_pair(WINDOW *win, int row, int from, int to, short pair) {
  for (int x = from; x < to; ++x)
    if (cell_pair(win, row, x) != pair)
      return false;
  return true;
}

// Two-line data, sample-major as nvtop_line_plot expects.
static void two_lines(double *data, size_t num_samples, const double *line0, const double *line1) {
  for (size_t s = 0; s < num_samples; ++s) {
    data[2 * s] = line0[s];
    data[2 * s + 1] = line1[s];
  }
}

// Plot num_shown samples per line given in screen order, sample-major. The
// renderer also takes the sample preceding the oldest shown one, which is not
// shown; here it repeats the oldest shown sample.
static void plot_shown(WINDOW *win, struct nvtop_braille_plot *state, size_t num_shown, unsigned num_lines,
                       const double *data, bool newest_left) {
  double extended[(2 * MAX_SAMPLES + 1) * MAX_LINES_PER_PLOT];
  size_t first_shown = newest_left ? 0 : 1;
  size_t hidden = newest_left ? num_shown : 0;
  size_t oldest = newest_left ? num_shown - 1 : 0;
  for (size_t s = 0; s < num_shown; ++s)
    for (unsigned line = 0; line < num_lines; ++line)
      extended[(s + first_shown) * num_lines + line] = data[s * num_lines + line];
  for (unsigned line = 0; line < num_lines; ++line)
    extended[hidden * num_lines + line] = data[oldest * num_lines + line];
  werase(win);
  nvtop_line_plot(win, num_shown + 1, extended, num_lines, nvtop_plot_style_braille, newest_left, state, legend);
}

static void render(WINDOW *win, struct nvtop_braille_plot *state, size_t num_shown, const double *data,
                   bool newest_left) {
  plot_shown(win, state, num_shown, 2, data, newest_left);
}

static int test_crossing_changes_owner(void) {
  enum { cols = 8 };
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  double line0[cols] = {100, 100, 100, 100, 0, 0, 0, 0};
  double line1[cols] = {0, 0, 0, 0, 100, 100, 100, 100};
  double data[2 * cols];
  two_lines(data, cols, line0, line1);
  render(win, &state, cols, data, false);
  // The line on top owns the cell: line 0 before the crossing, line 1 after.
  CHECK(row_has_pair(win, 0, 0, 4, PAIR_LINE0));
  CHECK(row_has_pair(win, 0, 4, cols, PAIR_LINE1));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_tie_keeps_previous_owner(void) {
  enum { cols = 8 };
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  // Line 1 starts on top, then both lines share the same level.
  double line0[cols] = {50, 50, 50, 50, 50, 50, 50, 50};
  double line1[cols] = {100, 50, 50, 50, 50, 50, 50, 50};
  double data[2 * cols];
  two_lines(data, cols, line0, line1);
  render(win, &state, cols, data, false);
  // No per-column alternation: the shared level keeps line 1's color.
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_one_dot_rise_takes_owner(void) {
  enum { cols = 8 };
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  // Line 1 owns the shared level (dot row 2), then line 0 rises by one dot
  // row: being strictly higher, it takes the cells over.
  double line0[cols] = {50, 50, 50, 50, 67, 67, 67, 67};
  double line1[cols] = {100, 50, 50, 50, 50, 50, 50, 50};
  double data[2 * cols];
  two_lines(data, cols, line0, line1);
  render(win, &state, cols, data, false);
  CHECK(row_has_pair(win, 0, 0, 4, PAIR_LINE1));
  CHECK(row_has_pair(win, 0, 4, cols, PAIR_LINE0));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_tie_without_history_uses_lowest_index(void) {
  enum { cols = 8 };
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  double line[cols] = {50, 50, 50, 50, 50, 50, 50, 50};
  double data[2 * cols];
  two_lines(data, cols, line, line);
  render(win, &state, cols, data, false);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE0));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

// The cell that gave line 1 the shared level scrolls out of the graph.
static int check_scroll_keeps_owner(bool newest_left) {
  enum { cols = 8 };
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  double level[cols] = {50, 50, 50, 50, 50, 50, 50, 50};
  double starts_on_top[cols] = {100, 50, 50, 50, 50, 50, 50, 50};
  double ends_on_top[cols] = {50, 50, 50, 50, 50, 50, 50, 100};
  double data[2 * cols];
  // Sample 0 is the leftmost one: the oldest, or the newest with newest_left.
  two_lines(data, cols, level, newest_left ? ends_on_top : starts_on_top);
  render(win, &state, cols, data, newest_left);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));

  // Redraws without new data render the same graph.
  render(win, &state, cols, data, newest_left);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));

  // One new sample: the oldest column, where line 1 was on top, is gone.
  nvtop_braille_plot_scrolled(&state);
  two_lines(data, cols, level, level);
  render(win, &state, cols, data, newest_left);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));
  render(win, &state, cols, data, newest_left);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));

  // Data that arrived without being drawn breaks the history: the inherited
  // owners describe columns that are long gone.
  nvtop_braille_plot_scrolled(&state);
  nvtop_braille_plot_scrolled(&state);
  render(win, &state, cols, data, newest_left);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE0));

  // A new plot (e.g., after a resize) starts without history.
  two_lines(data, cols, level, newest_left ? ends_on_top : starts_on_top);
  render(win, &state, cols, data, newest_left);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));
  nvtop_braille_plot_free(&state);
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  two_lines(data, cols, level, level);
  render(win, &state, cols, data, newest_left);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE0));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_scroll_keeps_owner(void) { return check_scroll_keeps_owner(false); }
static int test_scroll_keeps_owner_newest_left(void) { return check_scroll_keeps_owner(true); }

// Two samples per character column: a column leaves the graph every two
// samples.
static int test_double_resolution_scroll(void) {
  enum { cols = 4, samples = 2 * cols };
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  double level[samples] = {50, 50, 50, 50, 50, 50, 50, 50};
  double starts_on_top[samples] = {100, 100, 50, 50, 50, 50, 50, 50};
  double data[2 * samples];
  two_lines(data, samples, level, starts_on_top);
  render(win, &state, samples, data, false);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));
  nvtop_braille_plot_scrolled(&state);
  nvtop_braille_plot_scrolled(&state);
  two_lines(data, samples, level, level);
  render(win, &state, samples, data, false);
  CHECK(row_has_pair(win, 0, 0, cols, PAIR_LINE1));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

// First character of a legend mark after its leading space. The legend is
// drawn over the plot, so plot dots may follow a label without a mark.
static bool is_mark_start(wchar_t c) { return c == L'(' || c == L'\u2193' || c == L'v'; }

// Last character of a legend mark, before its trailing space when the mark
// precedes a right aligned label.
static bool is_mark_end(wchar_t c) { return c == L')' || c == L'\u2193' || c == L'v'; }

enum legend_mark { no_mark, long_mark, short_mark };

// Line 1's legend is on the second row, on the side of the oldest data. The
// label is "B" and the mark, in the color of the hiding line, is after it when
// the legend is on the left and before it when it is on the right.
static int render_legend_case(int cols, bool newest_left, double value0, double value1, enum legend_mark mark) {
  enum { rows = 3 };
  WINDOW *win = newwin(rows, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, rows, cols));
  double data[2 * MAX_SAMPLES];
  // One sample per column. The newest column holds the newest sample and the
  // dots joining it to the previous one, so these samples use the given values
  // (with two samples per column it would be the three newest ones). Line 0 is
  // on top everywhere else, so it owns the newest column on a tie and line 1 is
  // visible in every other column.
  enum { samples_per_column = 1, newest_column_samples = samples_per_column + 1 };
  for (int s = 0; s < cols; ++s) {
    int age = newest_left ? s : cols - 1 - s;
    data[2 * s] = age < newest_column_samples ? value0 : 100;
    data[2 * s + 1] = value1;
  }
  render(win, &state, (size_t)cols, data, newest_left);
  // The label never moves; the mark is on its side facing the plot.
  int label_x = newest_left ? cols - 1 : 0;
  CHECK(cell_char(win, 1, label_x) == L'B');
  CHECK(cell_pair(win, 1, label_x) == PAIR_LINE1);
  // Right aligned: "(behind) B" or "↓ B", the mark ending two columns before
  // the edge.
  int right_mark_end = cols - 3;
  if (mark == no_mark) {
    if (newest_left)
      CHECK(!is_mark_end(cell_char(win, 1, right_mark_end)));
    else
      CHECK(!is_mark_start(cell_char(win, 1, 2)));
  } else {
    int mark_width = mark == long_mark ? (int)strlen("(behind)") : 1;
    int mark_x = newest_left ? right_mark_end - (mark_width - 1) : 2;
    CHECK(cell_char(win, 1, mark_x) == (mark == long_mark ? L'(' : L'↓'));
    CHECK(cell_pair(win, 1, mark_x) == PAIR_LINE0);
    CHECK(cell_char(win, 1, mark_x + mark_width - 1) == (mark == long_mark ? L')' : L'↓'));
    // The separating space, even over plot dots.
    CHECK(cell_char(win, 1, newest_left ? cols - 2 : 1) == L' ');
    // Line 0 owns the newest column: it is not marked.
    if (newest_left) {
      CHECK(cell_char(win, 0, cols - 1) == L'A');
      CHECK(!is_mark_end(cell_char(win, 0, right_mark_end)));
    } else {
      CHECK(!is_mark_start(cell_char(win, 0, 2)));
    }
  }
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_legend_marks_hidden_line(void) {
  // Identical lines: line 1 is fully hidden behind line 0.
  return render_legend_case(30, false, 0, 0, long_mark);
}

static int test_legend_marks_hidden_line_right(void) { return render_legend_case(30, true, 0, 0, long_mark); }

static int test_legend_mark_over_plot(void) {
  // Identical lines at 50% run through the legend row: the space between the
  // label and the mark is not a plot dot.
  if (render_legend_case(30, false, 50, 50, long_mark) != EXIT_SUCCESS)
    return EXIT_FAILURE;
  return render_legend_case(30, true, 50, 50, long_mark);
}

static int test_legend_short_mark(void) {
  // "B (behind)" does not fit in 5 columns, "B ↓" does (or "↓ B" on the right).
  return render_legend_case(5, false, 0, 0, short_mark);
}

static int test_legend_short_mark_right(void) { return render_legend_case(5, true, 0, 0, short_mark); }

// Exact fits: "B (behind)" or "(behind) B" in 10 columns, "B ↓" or "↓ B" in 3.
static int test_legend_exact_fit(void) {
  if (render_legend_case(10, false, 0, 0, long_mark) != EXIT_SUCCESS ||
      render_legend_case(10, true, 0, 0, long_mark) != EXIT_SUCCESS ||
      render_legend_case(3, false, 0, 0, short_mark) != EXIT_SUCCESS)
    return EXIT_FAILURE;
  return render_legend_case(3, true, 0, 0, short_mark);
}

static int test_legend_no_mark_when_visible(void) {
  // Same character row but different dots: line 1 is drawn in line 0's color,
  // yet visible, so it is not marked.
  return render_legend_case(30, false, 27, 0, no_mark);
}

static int test_legend_no_mark_for_owner(void) {
  // Line 1 is on top: it owns the cell and is not marked.
  return render_legend_case(30, true, 0, 27, no_mark);
}

// Feed a ring of mostly tied, sometimes crossing lines to the renderer the way
// the interface does: after each new sample, the graph is rebuilt from the
// newest samples, plus the hidden one preceding them. Every character column
// that scrolled by one position must keep its glyph and color; only the newest
// column is new. The legend corner is skipped.
enum { stable_lines = 3, stable_rows = 3, stable_cols = 24, stable_steps = 400, legend_width = 12 };

struct frame {
  wchar_t glyph[stable_rows][stable_cols];
  short pair[stable_rows][stable_cols];
};

static void snapshot(WINDOW *win, struct frame *frame) {
  for (int y = 0; y < stable_rows; ++y)
    for (int x = 0; x < stable_cols; ++x) {
      frame->glyph[y][x] = cell_char(win, y, x);
      frame->pair[y][x] = cell_pair(win, y, x);
    }
}

static bool in_legend(int y, int x, bool newest_left) {
  int from_oldest = newest_left ? stable_cols - 1 - x : x;
  return y < stable_rows - 1 && from_oldest < legend_width;
}

static unsigned next_random(unsigned *seed) {
  *seed = *seed * 1103515245u + 12345u;
  return (*seed >> 16) & 0x7fff;
}

static double clamp_value(double value) { return value < 0 ? 0 : value > 100 ? 100 : value; }

static int check_scroll_stability(unsigned samples_per_column, bool newest_left) {
  enum { max_samples = 2 * stable_cols + 1 };
  // The sample count the interface gives the renderer.
  size_t num_samples = nvtop_plot_braille_samples(stable_cols, samples_per_column);
  WINDOW *win = newwin(stable_rows, stable_cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, stable_rows, stable_cols));
  static double history[stable_steps][stable_lines];
  unsigned seed = 42;
  double base = 50;
  for (int t = 0; t < stable_steps; ++t) {
    // Mostly small moves, with an occasional steep jump.
    if (next_random(&seed) % 8 == 0)
      base = clamp_value(base + (double)((int)(next_random(&seed) % 121) - 60));
    else
      base = clamp_value(base + (double)((int)(next_random(&seed) % 21) - 10));
    for (int line = 0; line < stable_lines; ++line) {
      // Mostly on the same level, sometimes slightly or clearly apart.
      static const double offsets[] = {0, 0, 0, 0, 3, -3, 12, -12};
      history[t][line] = clamp_value(base + offsets[next_random(&seed) % 8]);
    }
  }

  // Frames one column scroll apart group the samples the same way.
  static struct frame frames[PLOT_MAX_SAMPLES_PER_COLUMN];
  struct frame current, redraw;
  double data[max_samples * stable_lines];
  int compared = 0;
  for (int t = 0; t < stable_steps; ++t) {
    if (t > 0)
      nvtop_braille_plot_scrolled(&state);
    memset(data, 0, sizeof(data));
    for (size_t j = 0; j < num_samples && (int)j <= t; ++j) {
      size_t index = newest_left ? j : num_samples - 1 - j;
      for (int line = 0; line < stable_lines; ++line)
        data[index * stable_lines + line] = history[t - (int)j][line];
    }
    werase(win);
    nvtop_line_plot(win, num_samples, data, stable_lines, nvtop_plot_style_braille, newest_left, &state, legend);
    snapshot(win, &current);
    // A redraw without new data is identical.
    werase(win);
    nvtop_line_plot(win, num_samples, data, stable_lines, nvtop_plot_style_braille, newest_left, &state, legend);
    snapshot(win, &redraw);
    CHECK(memcmp(&current, &redraw, sizeof(current)) == 0);

    // Compare frames one whole character column apart.
    struct frame *previous = &frames[t % samples_per_column];
    if (t >= (int)samples_per_column) {
      int shift = newest_left ? 1 : -1;
      for (int y = 0; y < stable_rows; ++y) {
        for (int x = 0; x < stable_cols; ++x) {
          int old_x = x - shift;
          bool newest = newest_left ? x == 0 : x == stable_cols - 1;
          if (newest || old_x < 0 || old_x >= stable_cols || in_legend(y, x, newest_left) ||
              in_legend(y, old_x, newest_left))
            continue;
          if (current.glyph[y][x] != previous->glyph[y][old_x]) {
            fprintf(stderr, "t=%d row %d col %d: glyph changed after scrolling\n", t, y, x);
            return EXIT_FAILURE;
          }
          if (current.pair[y][x] != previous->pair[y][old_x]) {
            fprintf(stderr, "t=%d row %d col %d: color %d became %d after scrolling\n", t, y, x,
                    previous->pair[y][old_x], current.pair[y][x]);
            return EXIT_FAILURE;
          }
          compared++;
        }
      }
    }
    *previous = current;
  }
  // The data must actually exercise shared cells.
  CHECK(compared > stable_steps * 4);
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_scroll_stability(void) { return check_scroll_stability(1, false); }
static int test_scroll_stability_newest_left(void) { return check_scroll_stability(1, true); }
static int test_scroll_stability_double(void) { return check_scroll_stability(2, false); }
static int test_scroll_stability_double_newest_left(void) { return check_scroll_stability(2, true); }

// Render one line and compare the braille glyphs of a one-row plot with the
// expected dot masks (0 for an empty cell). values are the shown samples, the
// hidden one repeats the oldest of them, unless with_hidden is set: values
// then include it.
static int check_glyphs_of(int cols, size_t num_values, const double *values, bool newest_left, bool with_hidden,
                           const uint8_t *expected) {
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, (unsigned)cols));
  werase(win);
  if (with_hidden)
    nvtop_line_plot(win, num_values, values, 1, nvtop_plot_style_braille, newest_left, &state, legend);
  else
    plot_shown(win, &state, num_values, 1, values, newest_left);
  for (int x = 0; x < cols; ++x) {
    wchar_t want = expected[x] ? (wchar_t)(0x2800 + expected[x]) : L' ';
    if (cell_char(win, 0, x) != want) {
      fprintf(stderr, "column %d: got U+%04X, expected U+%04X\n", x, (unsigned)cell_char(win, 0, x), (unsigned)want);
      return EXIT_FAILURE;
    }
  }
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int check_glyphs(int cols, size_t num_shown, const double *values, bool newest_left, const uint8_t *expected) {
  return check_glyphs_of(cols, num_shown, values, newest_left, false, expected);
}

// One row has four dot rows: values 100, 67, 34 and 0 are on dot rows 0 to 3.
// With one sample per column, samples are on the dot column facing the newest
// data and the dots joining two samples are in the column between them. The
// oldest shown sample is joined to the hidden one (here on the same level).
static int test_glyphs_flat(void) {
  const double values[] = {0, 0, 0, 0};
  const uint8_t newest_right[] = {0xc0, 0xc0, 0xc0, 0xc0};
  const uint8_t newest_left[] = {0xc0, 0xc0, 0xc0, 0xc0};
  if (check_glyphs(4, 4, values, false, newest_right) != EXIT_SUCCESS)
    return EXIT_FAILURE;
  return check_glyphs(4, 4, values, true, newest_left);
}

static int test_glyphs_one_dot_climbs(void) {
  // A climb of one dot keeps the joining dot on the older sample's level.
  const double rising[] = {0, 34, 67, 100};
  const uint8_t newest_right[] = {0xc0, 0x60, 0x14, 0x0a};
  if (check_glyphs(4, 4, rising, false, newest_right) != EXIT_SUCCESS)
    return EXIT_FAILURE;
  const double rising_newest_first[] = {100, 67, 34, 0};
  const uint8_t newest_left[] = {0x11, 0x22, 0x84, 0xc0};
  return check_glyphs(4, 4, rising_newest_first, true, newest_left);
}

static int test_glyphs_steep_climbs(void) {
  // A steep climb is a vertical stroke between the two samples' dot rows.
  const double values[] = {0, 100, 100, 0};
  const uint8_t newest_right[] = {0xc0, 0x0e, 0x09, 0x86};
  const uint8_t newest_left[] = {0x70, 0x09, 0x31, 0xc0};
  if (check_glyphs(4, 4, values, false, newest_right) != EXIT_SUCCESS)
    return EXIT_FAILURE;
  return check_glyphs(4, 4, values, true, newest_left);
}

static int test_glyphs_double_resolution(void) {
  // Two samples per column: the climb is in the newer sample's dot column.
  const double rising[] = {0, 0, 100, 100};
  const uint8_t newest_right[] = {0xc0, 0x0f};
  if (check_glyphs(2, 4, rising, false, newest_right) != EXIT_SUCCESS)
    return EXIT_FAILURE;
  const double rising_newest_first[] = {100, 100, 0, 0};
  const uint8_t newest_left[] = {0x39, 0xc0};
  return check_glyphs(2, 4, rising_newest_first, true, newest_left);
}

static int test_glyphs_hidden_sample(void) {
  // The hidden sample (100) is not shown, but the climb joining it to the
  // oldest shown sample (0) is.
  const double newest_last[] = {100, 0, 0, 0, 0};
  const uint8_t newest_right[] = {0x86, 0xc0, 0xc0, 0xc0};
  if (check_glyphs_of(4, 5, newest_last, false, true, newest_right) != EXIT_SUCCESS)
    return EXIT_FAILURE;
  const double newest_first[] = {0, 0, 0, 0, 100};
  const uint8_t newest_left[] = {0xc0, 0xc0, 0xc0, 0x70};
  return check_glyphs_of(4, 5, newest_first, true, true, newest_left);
}

static int test_oldest_column_keeps_owner(void) {
  // Line 1 owns the second column through the climb joining it to the first
  // sample, above line 0. After a scroll that column is the oldest one: its
  // join to the scrolled out sample is still drawn, so it keeps its glyph and
  // its color.
  enum { cols = 4, samples = cols + 1 };
  WINDOW *win = newwin(1, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, 1, cols));
  const double line1_history[samples + 1] = {0, 100, 0, 0, 0, 0};
  double data[2 * samples];
  for (int s = 0; s < samples; ++s) {
    data[2 * s] = 34;
    data[2 * s + 1] = line1_history[s];
  }
  werase(win);
  nvtop_line_plot(win, samples, data, 2, nvtop_plot_style_braille, false, &state, legend);
  wchar_t glyph = cell_char(win, 0, 1);
  CHECK(cell_pair(win, 0, 1) == PAIR_LINE1);
  nvtop_braille_plot_scrolled(&state);
  for (int s = 0; s < samples; ++s)
    data[2 * s + 1] = line1_history[s + 1];
  werase(win);
  nvtop_line_plot(win, samples, data, 2, nvtop_plot_style_braille, false, &state, legend);
  CHECK(cell_char(win, 0, 0) == glyph);
  CHECK(cell_pair(win, 0, 0) == PAIR_LINE1);
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

// Render lines with constant values, except for the newest sample, in a plot
// whose newest column is on the right.
static void render_newest_step(WINDOW *win, struct nvtop_braille_plot *state, int cols, unsigned num_lines,
                               const double *older, const double *newest) {
  double data[MAX_LINES_PER_PLOT * MAX_SAMPLES];
  for (int s = 0; s < cols; ++s)
    for (unsigned line = 0; line < num_lines; ++line)
      data[(size_t)s * num_lines + line] = s == cols - 1 ? newest[line] : older[line];
  plot_shown(win, state, (size_t)cols, num_lines, data, false);
}

static int test_legend_visible_lower_down(void) {
  // Three rows of four dots. In the newest column, line 1 climbs from dot row
  // 7 to 0 and line 0 from dot row 4 to 0: line 0 draws all of line 1's dots in
  // the top row, but line 1 is alone in the second row, so it is visible.
  enum { rows = 3, cols = 30 };
  WINDOW *win = newwin(rows, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, rows, cols));
  const double older[] = {64, 36};
  const double newest[] = {100, 100};
  render_newest_step(win, &state, cols, 2, older, newest);
  CHECK(cell_char(win, 1, 0) == L'B');
  CHECK(!is_mark_start(cell_char(win, 1, 2)));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_legend_visible_higher_up(void) {
  // In the newest column, line 1 climbs from dot row 7 to 2 and line 0 from 7
  // to 0: line 0 draws all of line 1's dots in the second row, but line 1's
  // newest dot in the top row is its own, so it is visible.
  enum { rows = 3, cols = 30 };
  WINDOW *win = newwin(rows, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, rows, cols));
  const double older[] = {36, 36};
  const double newest[] = {100, 80};
  render_newest_step(win, &state, cols, 2, older, newest);
  CHECK(cell_char(win, 1, 0) == L'B');
  CHECK(!is_mark_start(cell_char(win, 1, 2)));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

static int test_legend_reports_highest_hider(void) {
  // Four rows of four dots. In the newest column, line 2 climbs from dot row 7
  // to 0: line 0 draws the same dots in the top row and line 1 in the second
  // row. The mark uses the color of the line hiding line 2's highest dot.
  enum { rows = 4, cols = 30 };
  WINDOW *win = newwin(rows, cols, 0, 0);
  struct nvtop_braille_plot state;
  CHECK(nvtop_braille_plot_init(&state, rows, cols));
  const double older[] = {73, 53, 53};
  const double newest[] = {100, 80, 100};
  render_newest_step(win, &state, cols, 3, older, newest);
  CHECK(cell_char(win, 2, 0) == L'C');
  CHECK(cell_char(win, 2, 2) == L'(');
  CHECK(cell_pair(win, 2, 2) == PAIR_LINE0);
  // Line 1 has a dot of its own in the top row: it is not marked.
  CHECK(!is_mark_start(cell_char(win, 1, 2)));
  nvtop_braille_plot_free(&state);
  delwin(win);
  return EXIT_SUCCESS;
}

struct test_case {
  const char *name;
  int (*run)(void);
};

static const struct test_case tests[] = {
    {"crossing_changes_owner", test_crossing_changes_owner},
    {"tie_keeps_previous_owner", test_tie_keeps_previous_owner},
    {"one_dot_rise_takes_owner", test_one_dot_rise_takes_owner},
    {"tie_without_history_uses_lowest_index", test_tie_without_history_uses_lowest_index},
    {"scroll_keeps_owner", test_scroll_keeps_owner},
    {"scroll_keeps_owner_newest_left", test_scroll_keeps_owner_newest_left},
    {"double_resolution_scroll", test_double_resolution_scroll},
    {"legend_marks_hidden_line", test_legend_marks_hidden_line},
    {"legend_marks_hidden_line_right", test_legend_marks_hidden_line_right},
    {"legend_mark_over_plot", test_legend_mark_over_plot},
    {"legend_short_mark", test_legend_short_mark},
    {"legend_short_mark_right", test_legend_short_mark_right},
    {"legend_exact_fit", test_legend_exact_fit},
    {"legend_no_mark_when_visible", test_legend_no_mark_when_visible},
    {"legend_no_mark_for_owner", test_legend_no_mark_for_owner},
    {"glyphs_flat", test_glyphs_flat},
    {"glyphs_one_dot_climbs", test_glyphs_one_dot_climbs},
    {"glyphs_steep_climbs", test_glyphs_steep_climbs},
    {"glyphs_double_resolution", test_glyphs_double_resolution},
    {"glyphs_hidden_sample", test_glyphs_hidden_sample},
    {"oldest_column_keeps_owner", test_oldest_column_keeps_owner},
    {"legend_visible_lower_down", test_legend_visible_lower_down},
    {"legend_visible_higher_up", test_legend_visible_higher_up},
    {"legend_reports_highest_hider", test_legend_reports_highest_hider},
    {"scroll_stability", test_scroll_stability},
    {"scroll_stability_newest_left", test_scroll_stability_newest_left},
    {"scroll_stability_double", test_scroll_stability_double},
    {"scroll_stability_double_newest_left", test_scroll_stability_double_newest_left},
};

int main(int argc, char **argv) {
  if (!setlocale(LC_ALL, "C.UTF-8") && !setlocale(LC_ALL, "en_US.UTF-8")) {
    fprintf(stderr, "No UTF-8 locale available\n");
    return SKIP_RETURN_CODE;
  }
  if (!nvtop_plot_supports_braille()) {
    fprintf(stderr, "Braille plots are not supported by this build\n");
    return SKIP_RETURN_CODE;
  }
  FILE *out = fopen("/dev/null", "w");
  FILE *in = fopen("/dev/null", "r");
  SCREEN *screen = out && in ? newterm("xterm-256color", out, in) : NULL;
  if (!screen)
    screen = out && in ? newterm("xterm", out, in) : NULL;
  if (!screen) {
    fprintf(stderr, "Cannot create an ncurses screen\n");
    return SKIP_RETURN_CODE;
  }
  start_color();
  for (short pair = PAIR_LINE0; pair < PAIR_LINE0 + MAX_LINES_PER_PLOT; ++pair)
    init_pair(pair, COLOR_WHITE, COLOR_BLACK);

  int status = EXIT_SUCCESS;
  bool ran = false;
  for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); ++i) {
    if (argc > 1 && strcmp(argv[1], tests[i].name) != 0)
      continue;
    ran = true;
    if (tests[i].run() != EXIT_SUCCESS) {
      fprintf(stderr, "FAILED: %s\n", tests[i].name);
      status = EXIT_FAILURE;
    }
  }
  if (!ran) {
    fprintf(stderr, "Unknown test: %s\n", argv[1]);
    status = EXIT_FAILURE;
  }
  endwin();
  delscreen(screen);
  fclose(out);
  fclose(in);
  return status;
}

#endif // NCURSES_WIDECHAR
