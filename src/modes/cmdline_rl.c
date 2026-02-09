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

#include "cmdline_rl.h"

#ifdef HAVE_READLINE

#include <curses.h>

#include <readline/readline.h>
#include <readline/history.h>

#include <stdlib.h> /* free() malloc() */
#include <string.h> /* strlen() */
#include <wchar.h> /* wchar_t wcslen() mbstowcs() wcstombs() wctomb() */

#include "../compat/curses.h"
#include "../compat/reallocarray.h"
#include "../utils/hist.h"
#include "../utils/macros.h"
#include "../utils/str.h"
#include "../status.h"
#include "cmdline.h"

/* Size of the internal getc buffer for feeding bytes to readline. */
#define GETC_BUF_SIZE 64

/* Whether readline is actively handling input. */
static int rl_active;

/* Set when Enter is pressed (accepted). */
static int line_accepted;

/* Set when Ctrl+C cancels input. */
static int cancelled;

/* Buffer for feeding bytes to readline's rl_getc_function. */
static unsigned char getc_buf[GETC_BUF_SIZE];
static int getc_buf_pos;
static int getc_buf_len;

static void noop_prep(int meta_flag);
static void noop_deprep(void);
static int vifm_rl_input_available(void);
static int vifm_rl_getc(FILE *stream);
static void vifm_rl_redisplay(void);
static void vifm_rl_accepted_cb(char *line);
static int vifm_rl_cancel(int count, int key);
static char **vifm_rl_completion(const char *text, int start, int end);
static char *vifm_rl_compentry(const char *text, int state);
static void load_vifm_history(const hist_t *hist);
static const char *ncurses_key_to_seq(int ncurses_key);
static int byte_offset_to_char_index(const char *s, int byte_off);

static void
noop_prep(int meta_flag)
{
	(void)meta_flag;
}

static void
noop_deprep(void)
{
}

void
cmdline_rl_init(void)
{
	/* Prevent readline from touching the terminal — Vifm/ncurses owns it. */
	rl_prep_term_function = noop_prep;
	rl_deprep_term_function = noop_deprep;
	rl_catch_signals = 0;
	rl_catch_sigwinch = 0;
	rl_change_environment = 0;

	/* Use our own rendering and character source. */
	rl_redisplay_function = vifm_rl_redisplay;
	rl_getc_function = vifm_rl_getc;
	rl_input_available_hook = vifm_rl_input_available;

	rl_initialize();

	/* Bind Ctrl+C to cancel. */
	rl_bind_key(CTRL('C'), vifm_rl_cancel);

	/* Use Vifm's completion system. */
	rl_attempted_completion_function = vifm_rl_completion;

	/* Disable readline's built-in filename completion fallback. */
	rl_completion_entry_function = vifm_rl_compentry;
}

void
cmdline_rl_start(void)
{
	char mb_prompt[NAME_MAX + 1];
	char mb_initial[4096];

	line_accepted = 0;
	cancelled = 0;
	getc_buf_pos = 0;
	getc_buf_len = 0;

	/* Convert prompt and initial text BEFORE rl_callback_handler_install(),
	 * because that function internally calls rl_redisplay(), which triggers our
	 * vifm_rl_redisplay() callback that syncs the (empty) rl_line_buffer back
	 * to input_stat.line, wiping the initial text. */
	modcline_get_prompt(mb_prompt, sizeof(mb_prompt));
	modcline_get_initial(mb_initial, sizeof(mb_initial));

	rl_callback_handler_install(mb_prompt, vifm_rl_accepted_cb);

	/* If there's initial text, insert it into readline's buffer. */
	if(mb_initial[0] != '\0')
	{
		rl_insert_text(mb_initial);
		vifm_rl_redisplay();
	}

	/* Load appropriate history. */
	const hist_t *hist = modcline_get_hist();
	if(hist != NULL)
	{
		load_vifm_history(hist);
	}

	rl_active = 1;
}

void
cmdline_rl_stop(void)
{
	rl_callback_handler_remove();
	clear_history();
	rl_active = 0;
	line_accepted = 0;
	cancelled = 0;
}

int
cmdline_rl_active(void)
{
	return rl_active;
}

void
cmdline_rl_feed(wint_t c)
{
	line_accepted = 0;
	cancelled = 0;
	getc_buf_pos = 0;
	getc_buf_len = 0;

	/* Check if this is a K()-wrapped ncurses functional key. */
	if(c >= (wint_t)0xe001)
	{
		int ncurses_key = (int)(c - (wint_t)0xe001);
		const char *seq = ncurses_key_to_seq(ncurses_key);
		if(seq != NULL)
		{
			while(*seq != '\0' && getc_buf_len < GETC_BUF_SIZE)
			{
				getc_buf[getc_buf_len++] = (unsigned char)*seq++;
			}
		}
		else
		{
			/* Unknown functional key — ignore. */
			return;
		}
	}
	else
	{
		/* Regular character — convert wchar_t to multibyte. */
		char mb[MB_LEN_MAX];
		int len = wctomb(mb, (wchar_t)c);
		if(len > 0)
		{
			int i;
			for(i = 0; i < len && getc_buf_len < GETC_BUF_SIZE; i++)
			{
				getc_buf[getc_buf_len++] = (unsigned char)mb[i];
			}
		}
		else
		{
			return;
		}
	}

	/* Save readline state before processing to detect no-op Esc. */
	const int old_point = rl_point;
	const int old_end = rl_end;

	/* Feed all buffered bytes to readline. */
	while(getc_buf_pos < getc_buf_len && !line_accepted && !cancelled)
	{
		rl_callback_read_char();
	}

	/* Detect Esc-as-cancel: if Esc was pressed and readline's cursor and line
	 * length didn't change, treat it as cancel.  In Vi mode the first Esc
	 * switches from insert to command mode (cursor moves), so only the second
	 * Esc (which is a no-op) triggers cancel.  In Emacs mode Esc alone just
	 * sets the Meta prefix without changing anything, so it cancels
	 * immediately — users can use Alt+key for Meta combinations instead. */
	if(c == L'\x1b' && !line_accepted && !cancelled
			&& rl_point == old_point && rl_end == old_end)
	{
		cancelled = 1;
	}
}

int
cmdline_rl_accepted(void)
{
	return line_accepted;
}

int
cmdline_rl_cancelled(void)
{
	return cancelled;
}

/* Readline's input-availability hook — tells readline whether our internal
 * buffer has data.  Without this, readline checks stdin directly via
 * select()/poll() to decide if more bytes follow an Esc (for escape sequence
 * disambiguation).  Since we feed characters through our own buffer, not
 * stdin, readline's default check can see stale terminal response bytes and
 * misinterpret a standalone Esc as the start of a Meta sequence. */
static int
vifm_rl_input_available(void)
{
	return getc_buf_pos < getc_buf_len;
}

/* Readline's getc function — reads from our internal buffer.  Must never
 * return EOF, because readline 8.x calls getc twice per
 * rl_callback_read_char() (once for the character, once for lookahead) and
 * interprets EOF as end-of-input, firing the handler prematurely.  Returning
 * 0 (NUL) when the buffer is empty is harmless — readline treats it as a
 * no-op character. */
static int
vifm_rl_getc(FILE *stream)
{
	(void)stream;

	if(getc_buf_pos < getc_buf_len)
	{
		return getc_buf[getc_buf_pos++];
	}

	return 0;
}

/* Readline's redisplay callback — syncs readline state to Vifm.  Skipped
 * after the line has been accepted or cancelled, because readline 8.x
 * re-initializes its line buffer after calling the handler and triggers
 * another redisplay with an empty buffer, which would wipe input_stat. */
static void
vifm_rl_redisplay(void)
{
	if(line_accepted || cancelled)
	{
		return;
	}

	int char_index = byte_offset_to_char_index(rl_line_buffer, rl_point);
	modcline_sync_from_readline(rl_line_buffer, char_index);
}

/* Readline's accept-line callback (Enter pressed).  Also called by readline
 * when our cancel function sets rl_done, so check the cancelled flag first. */
static void
vifm_rl_accepted_cb(char *line)
{
	(void)line;
	if(!cancelled)
	{
		line_accepted = 1;
	}
}

/* Custom readline function bound to Ctrl+C. */
static int
vifm_rl_cancel(int count, int key)
{
	(void)count;
	(void)key;
	rl_done = 1;
	cancelled = 1;
	return 0;
}

/* Readline completion function — bridges to Vifm's completion. */
static char **
vifm_rl_completion(const char *text, int start, int end)
{
	(void)text;
	(void)start;
	(void)end;

	/* Suppress readline's default filename completion. */
	rl_attempted_completion_over = 1;

	/* Trigger Vifm's completion, which updates input_stat directly. */
	modcline_do_completion();

	/* After Vifm's completion updates input_stat, sync back to readline. */
	char *mb_line = modcline_get_line_mb();
	if(mb_line != NULL)
	{
		rl_replace_line(mb_line, 0);
		rl_point = (int)strlen(mb_line);
		free(mb_line);
	}

	/* Return NULL — we've handled the completion ourselves by updating
	 * the line buffer directly. */
	return NULL;
}

/* Dummy completion entry function to suppress readline's default filename
 * completion. */
static char *
vifm_rl_compentry(const char *text, int state)
{
	(void)text;
	(void)state;
	return NULL;
}

/* Populates readline's history from a Vifm history. */
static void
load_vifm_history(const hist_t *hist)
{
	int i;

	clear_history();

	/* Add oldest first so newest is at the top. */
	for(i = hist->size - 1; i >= 0; i--)
	{
		if(hist->items[i].text != NULL)
		{
			add_history(hist->items[i].text);
		}
	}
}

/* Translates ncurses KEY_* constants to terminal escape sequences. */
static const char *
ncurses_key_to_seq(int ncurses_key)
{
	switch(ncurses_key)
	{
		case KEY_UP:        return "\033[A";
		case KEY_DOWN:      return "\033[B";
		case KEY_RIGHT:     return "\033[C";
		case KEY_LEFT:      return "\033[D";
		case KEY_HOME:      return "\033[H";
		case KEY_END:       return "\033[F";
		case KEY_DC:        return "\033[3~";
		case KEY_BACKSPACE: return "\177";
		case KEY_PPAGE:     return "\033[5~";
		case KEY_NPAGE:     return "\033[6~";
		default:            return NULL;
	}
}

/* Converts a byte offset in a multibyte string to a character index. */
static int
byte_offset_to_char_index(const char *s, int byte_off)
{
	int chars = 0;
	int bytes = 0;
	mbstate_t state;
	memset(&state, 0, sizeof(state));

	while(bytes < byte_off && s[bytes] != '\0')
	{
		size_t len = mbrlen(&s[bytes], (size_t)(byte_off - bytes), &state);
		if(len == (size_t)-1 || len == (size_t)-2)
		{
			/* Invalid or incomplete sequence — count as one byte/char. */
			bytes++;
			chars++;
			memset(&state, 0, sizeof(state));
		}
		else if(len == 0)
		{
			break;
		}
		else
		{
			bytes += (int)len;
			chars++;
		}
	}

	return chars;
}

#endif /* HAVE_READLINE */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
