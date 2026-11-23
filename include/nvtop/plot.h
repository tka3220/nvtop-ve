/*
 *
 * Copyright (C) 2018 Maxime Schmitt <maxime.schmitt91@gmail.com>
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

#ifndef __PLOT_H_
#define __PLOT_H_

#include "nvtop/common.h"

#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLOT_MAX_LEGEND_SIZE 35

// Maximum number of data samples drawn inside one character column. Unicode
// braille cells are two dots wide, so two samples per column use the full
// horizontal resolution of the renderer without overplotting samples.
#define PLOT_MAX_SAMPLES_PER_COLUMN 2

// Per plot window state of the braille renderer.
//
// A braille cell has a single color, so when several lines share a cell the
// line with the highest dot owns it. Lines tied for the highest dot keep the
// owner of the previous (older) cell of the same character row, so a shared
// level keeps one color and the color only changes where the lines cross.
// The owners the oldest column inherits are kept across frames so they do not
// change when the cell that started a shared level scrolls out of the graph.
// With several samples per character column, the frames group the samples in
// columns in as many different ways; each grouping has its own history.
struct nvtop_braille_plot {
  unsigned rows, cols;
  // Scratch: one dot mask per cell for every line.
  uint8_t *masks;
  // Per grouping and character row: owner (line index + 1, 0 for none) the
  // oldest column inherits in the current frame, and the running owner after
  // the oldest column, which the second oldest column inherits; that column
  // is the oldest one a column scroll later. A fresh plot has no history: all
  // owners start as 0.
  uint8_t *seed_owner[PLOT_MAX_SAMPLES_PER_COLUMN];
  uint8_t *next_seed[PLOT_MAX_SAMPLES_PER_COLUMN];
  // Scroll count seed_owner is valid for and next_seed was recorded at.
  unsigned long long seed_scroll[PLOT_MAX_SAMPLES_PER_COLUMN];
  unsigned long long next_seed_scroll[PLOT_MAX_SAMPLES_PER_COLUMN];
  // Samples appended since the plot was initialized.
  unsigned long long scrolls;
};

// Allocate the state for a plot window of rows x cols cells. Returns false on
// allocation failure.
bool nvtop_braille_plot_init(struct nvtop_braille_plot *plot, unsigned rows, unsigned cols);
void nvtop_braille_plot_free(struct nvtop_braille_plot *plot);
// Record that a new sample was appended to every line of the plot, i.e., the
// graph scrolled by one sample. Redraws without new data must not call this.
void nvtop_braille_plot_scrolled(struct nvtop_braille_plot *plot);

// Samples per line the braille renderer expects for a plot window cols
// characters wide, with samples_per_column (1 or PLOT_MAX_SAMPLES_PER_COLUMN)
// samples per character column: the shown samples plus the one preceding the
// oldest shown sample, which is not shown itself but joined to the oldest one.
// Other sample counts are spread over the width, without owner history.
size_t nvtop_plot_braille_samples(unsigned cols, unsigned samples_per_column);

// braille is only used by the braille renderer and must have been initialized
// for the size of win; otherwise the legacy renderer is used.
void nvtop_line_plot(WINDOW *win, size_t num_samples, const double *data, unsigned num_lines,
                     enum nvtop_plot_style style, bool newest_left, struct nvtop_braille_plot *braille,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]);

// Whether the current build and locale can display Unicode braille patterns.
bool nvtop_plot_supports_braille(void);

// Whether the current build and locale can display Unicode block elements
// (used for sub-column bar precision).
bool nvtop_plot_supports_unicode_blocks(void);

// Whether the middle dot (U+00B7) used for the meter track is a single-column
// printable character in the current locale.
bool nvtop_plot_supports_middle_dot(void);

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY);

#endif // __PLOT_H_
