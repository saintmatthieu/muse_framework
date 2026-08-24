/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <mutex>

namespace muse {
//! Serializes spawning child processes (fork) with loading dynamic libraries
//! in-process (dlopen). glibc's fork() and dlopen() acquire overlapping
//! loader/allocator locks, so forking from worker threads while another
//! thread is inside dlopen is deadlock-prone. IProcess implementations hold
//! this mutex around the fork; code that dlopens third-party binaries
//! (e.g. audio plugins) while child processes may be spawning concurrently
//! should hold it around the load.
std::mutex& processSpawnMutex();
}
