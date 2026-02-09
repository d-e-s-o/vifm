/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef VIFM__MODES__CMDLINE_RL_H__
#define VIFM__MODES__CMDLINE_RL_H__

#ifdef HAVE_READLINE

#include <wchar.h> /* wint_t */

/* Initializes readline integration once at startup. */
void cmdline_rl_init(void);

/* Starts a readline session when entering command-line mode. */
void cmdline_rl_start(void);

/* Stops the readline session when leaving command-line mode. */
void cmdline_rl_stop(void);

/* Returns non-zero if readline is currently handling input. */
int cmdline_rl_active(void);

/* Feeds one character from the event loop to readline. */
void cmdline_rl_feed(wint_t c);

/* Returns non-zero if line was accepted (Enter pressed). */
int cmdline_rl_accepted(void);

/* Returns non-zero if input was cancelled (Ctrl+C pressed). */
int cmdline_rl_cancelled(void);

#endif /* HAVE_READLINE */

#endif /* VIFM__MODES__CMDLINE_RL_H__ */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
