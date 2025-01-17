/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Purpose: clipboard support
 *
 * Authors: @andrewbird @stsp
 */
#include <string.h>
#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif
#include "dosemu_debug.h"
#include "types.h"
#include "translate/translate.h"
#include "utilities.h"
#include "clipboard.h"

struct clipboard_system *Clipboard;

static char *clipboard_make_str_utf8(int type, const char *p, int size)
{
  char *q;

  if (type == CF_TEXT) {
    q = strndup(p, size);
  } else { // CF_OEMTEXT
    struct char_set_state state;
    int characters;
    t_unicode *str;

    init_charset_state(&state, trconfig.dos_charset);

    characters = character_count(&state, p, size);
    if (characters == -1) {
      v_printf("SDL_clipboard: Write invalid char count\n");
      return NULL;
    }

    str = malloc(sizeof(t_unicode) * (characters + 1));
    charset_to_unicode_string(&state, str, &p, _min(strlen(p) + 1, size),
            characters + 1);
    cleanup_charset_state(&state);
    q = unicode_string_to_charset((wchar_t *)str, "utf8");
    free(str);
  }
  return q;
}

static char *clipboard_make_str_dos(int type, const char *p, int size,
    const char **out)
{
  char *q;

  if (type == CF_TEXT) {
    q = strndup(p, size);
  } else { // CF_OEMTEXT
    struct char_set_state state;
    struct char_set *utf8;
    int characters;
    t_unicode *str;

    utf8 = lookup_charset("utf8");
    init_charset_state(&state, utf8);

    characters = character_count(&state, p, size);
    if (characters == -1) {
      v_printf("SDL_clipboard: _clipboard_grab_data()  invalid char count\n");
      return NULL;
    }

    str = malloc(sizeof(t_unicode) * (characters + 1));
    charset_to_unicode_string(&state, str, &p, _min(strlen(p) + 1, size),
            characters + 1);
    if (out)
      *out = p;
    cleanup_charset_state(&state);
    q = unicode_string_to_charset((wchar_t *)str, trconfig.dos_charset->names[0]);
    free(str);
  }
  return q;
}

int register_clipboard_system(struct clipboard_system *cs)
{
  Clipboard = cs;
  return 1;
}

char *clip_str;
static char *clip_rdbuf;
static const char *clip_pos;

static void do_clear(void)
{
  free(clip_str);
  clip_str = NULL;
  free(clip_rdbuf);
  clip_rdbuf = NULL;
}

static void add_clip_str(char *q)
{
  if (clip_str) {
    clip_str = realloc(clip_str, strlen(clip_str) + strlen(q) + 1);
    strcat(clip_str, q);
    free(q);
  } else {
    clip_str = q;
  }
}

int cnn_clear(void)
{
  do_clear();
  return TRUE;
}

int cnn_write(int type, const char *p, int size)
{
  char *q, *p1;

  if (type != CF_TEXT && type != CF_OEMTEXT) {
    v_printf("SDL_clipboard: Write failed, type (0x%02x) unsupported\n", type);
    return FALSE;
  }
  p1 = strdup_nl(p);
  q = clipboard_make_str_utf8(type, p1, size);
  free(p1);
  if (!q)
    return FALSE;
  add_clip_str(q);
  return TRUE;
}

int cnn_getsize(int type)
{
  char *q;
  int ret;

  if (type == CF_TEXT)
    v_printf("SDL_clipboard: GetSize of type CF_TEXT\n");
  else if (type == CF_OEMTEXT)
    v_printf("SDL_clipboard: GetSize of type CF_OEMTEXT\n");
  else {
    v_printf("SDL_clipboard: GetSize failed (type 0x%02x unsupported\n", type);
    return 0;
  }

  if (!clip_rdbuf) {
    v_printf("SDL_clipboard: GetSize failed (grabbed data is NULL\n");
    return 0;
  }
  q = clipboard_make_str_dos(type, clip_rdbuf, strlen(clip_rdbuf) + 1, NULL);
  if (!q)
    return 0;
  ret = strlen(q) + 1;
  free(q);
  return ret;
}

int cnn_getdata(int type, char *p, int size)
{
  char *q;
  const char *new_pos = NULL;
  int l;

  if (!clip_pos)
    return 0;
  q = clipboard_make_str_dos(type, clip_pos, _min(strlen(clip_pos) + 1, size),
        &new_pos);
  if (!q)
    return 0;
  /* if new_pos not reset, don't copy \0 */
  l = _min(strlen(q) + (new_pos ? 0 : 1), size);
  memcpy(p, q, l);
  free(q);
  clip_pos = new_pos;
  return l;
}

int cnn_open(void)
{
  if (clip_rdbuf)
    return FALSE;
  if (clip_str && clip_str[0])
    clip_rdbuf = strdup_crlf(clip_str);
  clip_pos = clip_rdbuf;
  return TRUE;
}

void cnn_close(void)
{
  free(clip_rdbuf);
  clip_rdbuf = NULL;
}
