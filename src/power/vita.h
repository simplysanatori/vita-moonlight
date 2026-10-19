/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer, Sunguk Lee
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

bool vitapower_init();
void vitapower_config(CONFIGURATION config);

void vitapower_start();
void vitapower_stop();

// Boost the clocks (if boost_clocks is on) for CPU-heavy network work outside
// a stream: connecting, pairing, the app list and starting a stream. Calls
// nest; each begin needs its end.
void vitapower_busy_begin();
void vitapower_busy_end();
