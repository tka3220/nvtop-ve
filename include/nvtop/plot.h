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

// Size in bytes of the scratch buffer nvtop_line_plot needs for a plot window of
// rows x cols cells: one braille dot mask per cell for every line.
size_t nvtop_plot_braille_scratch_size(unsigned rows, unsigned cols);

// braille_masks is caller-owned scratch memory of
// nvtop_plot_braille_scratch_size(rows, cols) bytes; it is only used by the
// braille renderer.
void nvtop_line_plot(WINDOW *win, size_t num_samples, const double *data, unsigned num_lines,
                     enum nvtop_plot_style style, bool newest_left, uint8_t *braille_masks,
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
