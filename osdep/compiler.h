/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MPV_COMPILER_H
#define MPV_COMPILER_H

#define MP_EXPAND_ARGS(...) __VA_ARGS__

#ifdef __GNUC__

#define MP_NORETURN __attribute__((noreturn))

/** Use gcc attribute to check printf fns.  a1 is the 1-based index of
 * the parameter containing the format, and a2 the index of the first
 * argument. **/
#ifdef __MINGW32__
// MinGW maps "printf" to the non-standard MSVCRT functions, even if
// __USE_MINGW_ANSI_STDIO is defined and set to 1. We need to use "gnu_printf",
// which isn't necessarily available on other GCC compatible compilers.
#define PRINTF_ATTRIBUTE(a1, a2) __attribute__ ((format (gnu_printf, a1, a2)))
#else
#define PRINTF_ATTRIBUTE(a1, a2) __attribute__ ((format (printf, a1, a2)))
#endif

#else
#define PRINTF_ATTRIBUTE(a1, a2)
#define MP_NORETURN
#endif

#endif
