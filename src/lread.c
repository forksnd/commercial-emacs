/* Lisp parsing and input streams.

Copyright (C) 1985-1989, 1993-1995, 1997-2024 Free Software Foundation,
Inc.

This file is NOT part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

/* Tell globals.h to define tables needed by init_obarray.  */
#define DEFINE_SYMBOLS

#include <config.h>
#include "sysstdio.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stat-time.h>
#include "lisp.h"
#include "dispextern.h"
#include "intervals.h"
#include "character.h"
#include "buffer.h"
#include "charset.h"
#include <epaths.h>
#include "commands.h"
#include "keyboard.h"
#include "systime.h"
#include "termhooks.h"
#include "blockinput.h"
#include "pdumper.h"
#include <c-ctype.h>
#include <vla.h>

#ifdef MSDOS
#include "msdos.h"
#endif

#ifdef HAVE_NS
#include "nsterm.h"
#endif

#include <unistd.h>
#include <fcntl.h>

#ifdef HAVE_FSEEKO
#define file_offset off_t
#define file_tell ftello
#else
#define file_offset long
#define file_tell ftell
#endif

#if IEEE_FLOATING_POINT
# include <ieee754.h>
# ifndef INFINITY
#  define INFINITY ((union ieee754_double) {.ieee = {.exponent = -1}}.d)
# endif
#else
# ifndef INFINITY
#  define INFINITY HUGE_VAL
# endif
#endif

/* The objects or placeholders read with the #n=object form.

   A hash table maps a number to either a placeholder (while the
   object is still being parsed, in case it's referenced within its
   own definition) or to the completed object.  With small integers
   for keys, it's effectively little more than a vector, but it'll
   manage any needed resizing for us.

   The variable must be reset to an empty hash table before all
   top-level calls to read0.  In between calls, it may be an empty
   hash table left unused from the previous call (to reduce
   allocations), or nil.  */
static Lisp_Object read_objects_map;

/* The recursive objects read with the #n=object form.

   Objects that might have circular references are stored here, so
   that recursive substitution knows not to keep processing them
   multiple times.

   Only objects that are completely processed, including substituting
   references to themselves (but not necessarily replacing
   placeholders for other objects still being read), are stored.

   A hash table is used for efficient lookups of keys.  We don't care
   what the value slots hold.  The variable must be set to an empty
   hash table before all top-level calls to read0.  In between calls,
   it may be an empty hash table left unused from the previous call
   (to reduce allocations), or nil.  */
static Lisp_Object read_objects_completed;

/* File and lookahead for get-file-char and get-emacs-mule-file-char
   to read from.  Used by Fload.  */
static struct infile
{
  /* The input stream.  */
  FILE *stream;

  /* Lookahead byte count.  */
  signed char lookahead;

  /* Lookahead bytes, in reverse order.  Keep these here because it is
     not portable to ungetc more than one byte at a time.  */
  unsigned char buf[MAX_MULTIBYTE_LENGTH - 1];
} *infile;

/* For use within read-from-string (this reader is non-reentrant!!)  */
static ptrdiff_t read_from_string_index;
static ptrdiff_t read_from_string_index_byte;
static ptrdiff_t read_from_string_limit;

static EMACS_INT readchar_charpos; /* one-indexed */

struct saved_string {
  char *string;		        /* string in allocated buffer */
  ptrdiff_t size;		/* allocated size of buffer */
  ptrdiff_t length;		/* length of string in buffer */
  file_offset position;		/* position in file the string came from */
};

/* The last two strings skipped with #@ (most recent first).  */
static struct saved_string saved_strings[2];

/* A list of file names for files being loaded in Fload.  Used to
   check for recursive loads.  */

static Lisp_Object Vloads_in_progress;

static int read_emacs_mule_char (int, int (*) (int, Lisp_Object),
                                 Lisp_Object);

static void readevalloop (Lisp_Object, struct infile *, Lisp_Object, bool,
                          Lisp_Object, Lisp_Object,
                          Lisp_Object, Lisp_Object);

static void build_load_history (Lisp_Object, bool);

static Lisp_Object oblookup_considering_shorthand (Lisp_Object, const char *,
						   ptrdiff_t, ptrdiff_t,
						   char **, ptrdiff_t *,
						   ptrdiff_t *);

/* Functions that read one byte from the current source READCHARFUN
   or unreads one byte.  If the integer argument C is -1, it returns
   one read byte, or -1 when there's no more byte in the source.  If C
   is 0 or positive, it unreads C, and the return value is not
   interesting.  */

static int readbyte_for_lambda (int, Lisp_Object);
static int readbyte_from_file (int, Lisp_Object);
static int readbyte_from_string (int, Lisp_Object);

/* Handle unreading and rereading of characters.
   Write READCHAR to read a character,
   UNREAD(c) to unread c to be read again.

   These macros correctly read/unread multibyte characters.  */

#define READCHAR readchar (readcharfun, NULL)
#define UNREAD(c) unreadchar (readcharfun, c)

/* Same as READCHAR but set *MULTIBYTE to the multibyteness of the source.  */
#define READCHAR_REPORT_MULTIBYTE(multibyte) readchar (readcharfun, multibyte)

/* When READCHARFUN is Qget_file_char, Qget_emacs_mule_file_char,
   Qlambda, or a cons, we use this to keep an unread character because
   a file stream can't handle multibyte-char unreading.  The value -1
   means that there's no unread character.  */
static int unread_char = -1;

static int
readchar (Lisp_Object readcharfun, bool *multibyte)
{
  Lisp_Object tem;
  register int c;
  int (*readbyte) (int, Lisp_Object);
  unsigned char buf[MAX_MULTIBYTE_LENGTH];
  int i, len;
  bool emacs_mule_encoding = 0;

  if (multibyte)
    *multibyte = 0;

  readchar_charpos++;

  if (BUFFERP (readcharfun))
    {
      register struct buffer *inbuffer = XBUFFER (readcharfun);

      ptrdiff_t pt_byte = BUF_PT_BYTE (inbuffer);

      if (!BUFFER_LIVE_P (inbuffer))
	return -1;

      if (pt_byte >= BUF_ZV_BYTE (inbuffer))
	return -1;

      if (!NILP (BVAR (inbuffer, enable_multibyte_characters)))
	{
	  /* Fetch the character code from the buffer.  */
	  unsigned char *p = BUF_BYTE_ADDRESS (inbuffer, pt_byte);
	  int clen;
	  c = string_char_and_length (p, &clen);
	  pt_byte += clen;
	  if (multibyte)
	    *multibyte = 1;
	}
      else
	{
	  c = BUF_FETCH_BYTE (inbuffer, pt_byte);
	  if (!ASCII_CHAR_P (c))
	    c = BYTE8_TO_CHAR (c);
	  pt_byte++;
	}
      SET_BUF_PT_BOTH (inbuffer, BUF_PT (inbuffer) + 1, pt_byte);

      return c;
    }
  if (MARKERP (readcharfun))
    {
      register struct buffer *inbuffer = XMARKER (readcharfun)->buffer;

      ptrdiff_t bytepos = marker_byte_position (readcharfun);

      if (bytepos >= BUF_ZV_BYTE (inbuffer))
	return -1;

      if (!NILP (BVAR (inbuffer, enable_multibyte_characters)))
	{
	  /* Fetch the character code from the buffer.  */
	  unsigned char *p = BUF_BYTE_ADDRESS (inbuffer, bytepos);
	  int clen;
	  c = string_char_and_length (p, &clen);
	  bytepos += clen;
	  if (multibyte)
	    *multibyte = 1;
	}
      else
	{
	  c = BUF_FETCH_BYTE (inbuffer, bytepos);
	  if (!ASCII_CHAR_P (c))
	    c = BYTE8_TO_CHAR (c);
	  bytepos++;
	}

      XMARKER (readcharfun)->bytepos = bytepos;
      XMARKER (readcharfun)->charpos++;

      return c;
    }

  if (EQ (readcharfun, Qlambda))
    {
      readbyte = readbyte_for_lambda;
      goto read_multibyte;
    }

  if (EQ (readcharfun, Qget_file_char))
    {
      eassert (infile);
      readbyte = readbyte_from_file;
      goto read_multibyte;
    }

  if (STRINGP (readcharfun))
    {
      if (read_from_string_index >= read_from_string_limit)
	c = -1;
      else if (STRING_MULTIBYTE (readcharfun))
	{
	  if (multibyte)
	    *multibyte = 1;
	  c = (fetch_string_char_advance_no_check
	       (readcharfun,
		&read_from_string_index,
		&read_from_string_index_byte));
	}
      else
	{
	  c = SREF (readcharfun, read_from_string_index_byte);
	  read_from_string_index++;
	  read_from_string_index_byte++;
	}
      return c;
    }

  if (CONSP (readcharfun) && STRINGP (XCAR (readcharfun)))
    {
      /* This is the case that read_vector is reading from a unibyte
	 string that contains a byte sequence previously skipped
	 because of #@NUMBER.  The car part of readcharfun is that
	 string, and the cdr part is a value of readcharfun given to
	 read_vector.  */
      readbyte = readbyte_from_string;
      eassert (infile);
      if (EQ (XCDR (readcharfun), Qget_emacs_mule_file_char))
	emacs_mule_encoding = 1;
      goto read_multibyte;
    }

  if (EQ (readcharfun, Qget_emacs_mule_file_char))
    {
      readbyte = readbyte_from_file;
      eassert (infile);
      emacs_mule_encoding = 1;
      goto read_multibyte;
    }

  tem = call0 (readcharfun);

  if (NILP (tem))
    return -1;
  return XFIXNUM (tem);

 read_multibyte:
  if (unread_char >= 0)
    {
      c = unread_char;
      unread_char = -1;
      return c;
    }
  c = (*readbyte) (-1, readcharfun);
  if (c < 0)
    return c;
  if (multibyte)
    *multibyte = 1;
  if (ASCII_CHAR_P (c))
    return c;
  if (emacs_mule_encoding)
    return read_emacs_mule_char (c, readbyte, readcharfun);
  i = 0;
  buf[i++] = c;
  len = BYTES_BY_CHAR_HEAD (c);
  while (i < len)
    {
      buf[i++] = c = (*readbyte) (-1, readcharfun);
      if (c < 0 || !TRAILING_CODE_P (c))
	{
	  for (i -= c < 0; 0 < --i; )
	    (*readbyte) (buf[i], readcharfun);
	  return BYTE8_TO_CHAR (buf[0]);
	}
    }
  return STRING_CHAR (buf);
}

#define FROM_FILE_P(readcharfun)			\
  (EQ (readcharfun, Qget_file_char)			\
   || EQ (readcharfun, Qget_emacs_mule_file_char))

static void
skip_dyn_bytes (Lisp_Object readcharfun, ptrdiff_t n)
{
  if (FROM_FILE_P (readcharfun))
    {
      block_input ();		/* FIXME: Not sure if it's needed.  */
      fseek (infile->stream, n - infile->lookahead, SEEK_CUR);
      unblock_input ();
      infile->lookahead = 0;
    }
  else
    { /* We're not reading directly from a file.  In that case, it's difficult
	 to reliably count bytes, since these are usually meant for the file's
	 encoding, whereas we're now typically in the internal encoding.
	 But luckily, skip_dyn_bytes is used to skip over a single
	 dynamic-docstring (or dynamic byte-code) which is always quoted such
	 that \037 is the final char.  */
      int c;
      do {
	c = READCHAR;
      } while (c >= 0 && c != '\037');
    }
}

static void
skip_dyn_eof (Lisp_Object readcharfun)
{
  if (FROM_FILE_P (readcharfun))
    {
      block_input ();		/* FIXME: Not sure if it's needed.  */
      fseek (infile->stream, 0, SEEK_END);
      unblock_input ();
      infile->lookahead = 0;
    }
  else
    while (READCHAR >= 0);
}

/* Unread the character C in the way appropriate for the stream READCHARFUN.
   If the stream is a user function, call it with the char as argument.  */

static void
unreadchar (Lisp_Object readcharfun, int c)
{
  readchar_charpos--;
  if (c == -1)
    /* Don't back up the pointer if we're unreading the end-of-input mark,
       since readchar didn't advance it when we read it.  */
    ;
  else if (BUFFERP (readcharfun))
    {
      struct buffer *b = XBUFFER (readcharfun);
      ptrdiff_t charpos = BUF_PT (b);
      ptrdiff_t bytepos = BUF_PT_BYTE (b);

      if (!NILP (BVAR (b, enable_multibyte_characters)))
	bytepos -= buf_prev_char_len (b, bytepos);
      else
	bytepos--;

      SET_BUF_PT_BOTH (b, charpos - 1, bytepos);
    }
  else if (MARKERP (readcharfun))
    {
      struct buffer *b = XMARKER (readcharfun)->buffer;
      ptrdiff_t bytepos = XMARKER (readcharfun)->bytepos;

      XMARKER (readcharfun)->charpos--;
      if (!NILP (BVAR (b, enable_multibyte_characters)))
	bytepos -= buf_prev_char_len (b, bytepos);
      else
	bytepos--;

      XMARKER (readcharfun)->bytepos = bytepos;
    }
  else if (STRINGP (readcharfun))
    {
      read_from_string_index--;
      read_from_string_index_byte
	= string_char_to_byte (readcharfun, read_from_string_index);
    }
  else if (CONSP (readcharfun) && STRINGP (XCAR (readcharfun)))
    {
      unread_char = c;
    }
  else if (EQ (readcharfun, Qlambda))
    {
      unread_char = c;
    }
  else if (FROM_FILE_P (readcharfun))
    {
      unread_char = c;
    }
  else
    call1 (readcharfun, make_fixnum (c));
}

static int
readbyte_for_lambda (int c, Lisp_Object readcharfun)
{
  return read_bytecode_char (c >= 0);
}

static int
readbyte_from_stdio (void)
{
  if (infile->lookahead)
    return infile->buf[--infile->lookahead];

  int c;
  FILE *instream = infile->stream;

  block_input ();

  /* Interrupted reads have been observed while reading over the network.  */
  while ((c = getc (instream)) == EOF && errno == EINTR && ferror (instream))
    {
      unblock_input ();
      maybe_quit ();
      block_input ();
      clearerr (instream);
    }

  unblock_input ();

  return (c == EOF ? -1 : c);
}

static int
readbyte_from_file (int c, Lisp_Object readcharfun)
{
  eassert (infile);
  if (c >= 0)
    {
      eassert (infile->lookahead < sizeof infile->buf);
      infile->buf[infile->lookahead++] = c;
      return 0;
    }

  return readbyte_from_stdio ();
}

static int
readbyte_from_string (int c, Lisp_Object readcharfun)
{
  Lisp_Object string = XCAR (readcharfun);

  if (c >= 0)
    {
      read_from_string_index--;
      read_from_string_index_byte
	= string_char_to_byte (string, read_from_string_index);
    }

  return (read_from_string_index < read_from_string_limit
	  ? fetch_string_char_advance (string,
				       &read_from_string_index,
				       &read_from_string_index_byte)
	  : -1);
}


/* Signal Qinvalid_read_syntax error.
   S is error string of length N (if > 0)  */

static AVOID
invalid_syntax_lisp (Lisp_Object s, Lisp_Object readcharfun)
{
  if (BUFFERP (readcharfun))
    {
      ptrdiff_t line, column;

      /* Get the line/column in the readcharfun buffer.  */
      {
	specpdl_ref count = SPECPDL_INDEX ();

	record_unwind_protect_excursion ();
	set_buffer_internal (XBUFFER (readcharfun));
	line = count_lines (BEGV_BYTE, PT_BYTE) + 1;
	column = current_column ();
	unbind_to (count, Qnil);
      }

      xsignal (Qinvalid_read_syntax,
	       list3 (s, make_fixnum (line), make_fixnum (column)));
    }
  else
    xsignal1 (Qinvalid_read_syntax, s);
}

static AVOID
invalid_syntax (const char *s, Lisp_Object readcharfun)
{
  invalid_syntax_lisp (build_string (s), readcharfun);
}


/* Read one non-ASCII character from INFILE.  The character is
   encoded in `emacs-mule' and the first byte is already read in
   C.  */

static int
read_emacs_mule_char (int c, int (*readbyte) (int, Lisp_Object), Lisp_Object readcharfun)
{
  /* Emacs-mule coding uses at most 4-byte for one character.  */
  unsigned char buf[4];
  int len = emacs_mule_bytes[c];
  struct charset *charset;
  int i;
  unsigned code;

  if (len == 1)
    /* C is not a valid leading-code of `emacs-mule'.  */
    return BYTE8_TO_CHAR (c);

  i = 0;
  buf[i++] = c;
  while (i < len)
    {
      buf[i++] = c = (*readbyte) (-1, readcharfun);
      if (c < 0xA0)
	{
	  for (i -= c < 0; 0 < --i; )
	    (*readbyte) (buf[i], readcharfun);
	  return BYTE8_TO_CHAR (buf[0]);
	}
    }

  if (len == 2)
    {
      charset = CHARSET_FROM_ID (emacs_mule_charset[buf[0]]);
      code = buf[1] & 0x7F;
    }
  else if (len == 3)
    {
      if (buf[0] == EMACS_MULE_LEADING_CODE_PRIVATE_11
	  || buf[0] == EMACS_MULE_LEADING_CODE_PRIVATE_12)
	{
	  charset = CHARSET_FROM_ID (emacs_mule_charset[buf[1]]);
	  code = buf[2] & 0x7F;
	}
      else
	{
	  charset = CHARSET_FROM_ID (emacs_mule_charset[buf[0]]);
	  code = ((buf[1] << 8) | buf[2]) & 0x7F7F;
	}
    }
  else
    {
      charset = CHARSET_FROM_ID (emacs_mule_charset[buf[1]]);
      code = ((buf[2] << 8) | buf[3]) & 0x7F7F;
    }
  c = DECODE_CHAR (charset, code);
  if (c < 0)
    invalid_syntax ("invalid multibyte form", readcharfun);
  return c;
}


/* An in-progress substitution of OBJECT for PLACEHOLDER.  */
struct subst
{
  Lisp_Object object;
  Lisp_Object placeholder;

  /* Hash table of subobjects of OBJECT that might be circular.  If
     Qt, all such objects might be circular.  */
  Lisp_Object completed;

  /* List of subobjects of OBJECT that have already been visited.  */
  Lisp_Object seen;
};

static Lisp_Object read_internal_start (Lisp_Object, Lisp_Object,
                                        Lisp_Object, bool);
static Lisp_Object read0 (Lisp_Object, bool);

static Lisp_Object substitute_object_recurse (struct subst *, Lisp_Object);
static void substitute_in_interval (INTERVAL *, void *);


/* Get a character from the tty.  */

/* Read input events until we get one that's acceptable for our purposes.

   If NO_SWITCH_FRAME, switch-frame events are stashed
   until we get a character we like, and then stuffed into
   unread_switch_frame.

   If ASCII_REQUIRED, check function key events to see
   if the unmodified version of the symbol has a Qascii_character
   property, and use that character, if present.

   If ERROR_NONASCII, signal an error if the input we
   get isn't an ASCII character with modifiers.  If it's false but
   ASCII_REQUIRED is true, just re-read until we get an ASCII
   character.

   If INPUT_METHOD, invoke the current input method
   if the character warrants that.

   If SECONDS is a number, wait that many seconds for input, and
   return Qnil if no input arrives within that time.  */

static Lisp_Object
read_filtered_event (bool no_switch_frame, bool ascii_required,
		     bool error_nonascii, bool input_method, Lisp_Object seconds)
{
  Lisp_Object val, delayed_switch_frame;
  struct timespec end_time;

#ifdef HAVE_WINDOW_SYSTEM
  cancel_hourglass ();
#endif

  delayed_switch_frame = Qnil;

  /* Compute timeout.  */
  if (NUMBERP (seconds))
    {
      double duration = XFLOATINT (seconds);
      struct timespec wait_time = dtotimespec (duration);
      end_time = timespec_add (current_timespec (), wait_time);
    }

  /* Read until we get an acceptable event.  */
 retry:
  do
    {
      val = read_char (0, Qnil, (input_method ? Qnil : Qt), 0,
		       NUMBERP (seconds) ? &end_time : NULL);
    }
  while (FIXNUMP (val) && XFIXNUM (val) == -2); /* wrong_kboard_jmpbuf */

  if (BUFFERP (val))
    goto retry;

  /* `switch-frame' events are put off until after the next ASCII
     character.  This is better than signaling an error just because
     the last characters were typed to a separate minibuffer frame,
     for example.  Eventually, some code which can deal with
     switch-frame events will read it and process it.  */
  if (no_switch_frame
      && EVENT_HAS_PARAMETERS (val)
      && EQ (EVENT_HEAD_KIND (EVENT_HEAD (val)), Qswitch_frame))
    {
      delayed_switch_frame = val;
      goto retry;
    }

  if (ascii_required && !(NUMBERP (seconds) && NILP (val)))
    {
      /* Convert certain symbols to their ASCII equivalents.  */
      if (SYMBOLP (val))
	{
	  Lisp_Object tem, tem1;
	  tem = Fget (val, Qevent_symbol_element_mask);
	  if (!NILP (tem))
	    {
	      tem1 = Fget (Fcar (tem), Qascii_character);
	      /* Merge this symbol's modifier bits
		 with the ASCII equivalent of its basic code.  */
	      if (!NILP (tem1))
		XSETFASTINT (val, XFIXNUM (tem1) | XFIXNUM (Fcar (Fcdr (tem))));
	    }
	}

      /* If we don't have a character now, deal with it appropriately.  */
      if (!FIXNUMP (val))
	{
	  if (error_nonascii)
	    {
	      Vunread_command_events = list1 (val);
	      error ("Non-character input-event");
	    }
	  else
	    goto retry;
	}
    }

  if (!NILP (delayed_switch_frame))
    unread_switch_frame = delayed_switch_frame;

#if 0

#ifdef HAVE_WINDOW_SYSTEM
  if (display_hourglass_p)
    start_hourglass ();
#endif

#endif

  return val;
}

DEFUN ("read-char", Fread_char, Sread_char, 0, 3, 0,
       doc: /* Read a character event from the command input (keyboard or macro).
It is returned as a number.
If the event has modifiers, they are resolved and reflected in the
returned character code if possible (e.g. C-SPC yields 0 and C-a yields 97).
If some of the modifiers cannot be reflected in the character code, the
returned value will include those modifiers, and will not be a valid
character code: it will fail the `characterp' test.  Use `event-basic-type'
to recover the character code with the modifiers removed.

If the user generates an event which is not a character (i.e. a mouse
click or function key event), `read-char' signals an error.  As an
exception, switch-frame events are put off until non-character events
can be read.
If you want to read non-character events, or ignore them, call
`read-event' or `read-char-exclusive' instead.

If the optional argument PROMPT is non-nil, display that as a prompt.
If PROMPT is nil or the string \"\", the key sequence/events that led
to the current command is used as the prompt.

If the optional argument INHERIT-INPUT-METHOD is non-nil and some
input method is turned on in the current buffer, that input method
is used for reading a character.

If the optional argument SECONDS is non-nil, it should be a number
specifying the maximum number of seconds to wait for input.  If no
input arrives in that time, return nil.  SECONDS may be a
floating-point value.

If `inhibit-interaction' is non-nil, this function will signal an
`inhibited-interaction' error.  */)
  (Lisp_Object prompt, Lisp_Object inherit_input_method, Lisp_Object seconds)
{
  Lisp_Object val;

  barf_if_interaction_inhibited ();

  if (!NILP (prompt))
    {
      cancel_echoing ();
      message_with_string ("%s", prompt, 0);
    }
  val = read_filtered_event (1, 1, 1, !NILP (inherit_input_method), seconds);

  return (NILP (val) ? Qnil
	  : make_fixnum (char_resolve_modifier_mask (XFIXNUM (val))));
}

DEFUN ("read-event", Fread_event, Sread_event, 0, 3, 0,
       doc: /* Read an event object from the input stream.

If you want to read non-character events, consider calling `read-key'
instead.  `read-key' will decode events via `input-decode-map' that
`read-event' will not.  On a terminal this includes function keys such
as <F7> and <RIGHT>, or mouse events generated by `xterm-mouse-mode'.

If the optional argument PROMPT is non-nil, display that as a prompt.
If PROMPT is nil or the string \"\", the key sequence/events that led
to the current command is used as the prompt.

If the optional argument INHERIT-INPUT-METHOD is non-nil and some
input method is turned on in the current buffer, that input method
is used for reading a character.

If the optional argument SECONDS is non-nil, it should be a number
specifying the maximum number of seconds to wait for input.  If no
input arrives in that time, return nil.  SECONDS may be a
floating-point value.

If `inhibit-interaction' is non-nil, this function will signal an
`inhibited-interaction' error.  */)
  (Lisp_Object prompt, Lisp_Object inherit_input_method, Lisp_Object seconds)
{
  barf_if_interaction_inhibited ();

  if (!NILP (prompt))
    {
      cancel_echoing ();
      message_with_string ("%s", prompt, 0);
    }
  return read_filtered_event (0, 0, 0, !NILP (inherit_input_method), seconds);
}

DEFUN ("read-char-exclusive", Fread_char_exclusive, Sread_char_exclusive, 0, 3, 0,
       doc: /* Read a character event from the command input (keyboard or macro).
It is returned as a number.  Non-character events are ignored.
If the event has modifiers, they are resolved and reflected in the
returned character code if possible (e.g. C-SPC yields 0 and C-a yields 97).
If some of the modifiers cannot be reflected in the character code, the
returned value will include those modifiers, and will not be a valid
character code: it will fail the `characterp' test.  Use `event-basic-type'
to recover the character code with the modifiers removed.

If the optional argument PROMPT is non-nil, display that as a prompt.
If PROMPT is nil or the string \"\", the key sequence/events that led
to the current command is used as the prompt.

If the optional argument INHERIT-INPUT-METHOD is non-nil and some
input method is turned on in the current buffer, that input method
is used for reading a character.

If the optional argument SECONDS is non-nil, it should be a number
specifying the maximum number of seconds to wait for input.  If no
input arrives in that time, return nil.  SECONDS may be a
floating-point value.

If `inhibit-interaction' is non-nil, this function will signal an
`inhibited-interaction' error.  */)
(Lisp_Object prompt, Lisp_Object inherit_input_method, Lisp_Object seconds)
{
  Lisp_Object val;

  barf_if_interaction_inhibited ();

  if (!NILP (prompt))
    {
      cancel_echoing ();
      message_with_string ("%s", prompt, 0);
    }

  val = read_filtered_event (1, 1, 0, !NILP (inherit_input_method), seconds);

  return (NILP (val) ? Qnil
	  : make_fixnum (char_resolve_modifier_mask (XFIXNUM (val))));
}

/* Return true if the lisp code read using READCHARFUN defines a non-nil
   `lexical-binding' file variable.  After returning, the stream is
   positioned following the first line, if it is a comment or #! line,
   otherwise nothing is read.  */

static bool
lisp_file_lexically_bound_p (Lisp_Object readcharfun)
{
  int ch = READCHAR;

  if (ch == '#')
    {
      ch = READCHAR;
      if (ch != '!')
        {
          UNREAD (ch);
          UNREAD ('#');
          return 0;
        }
      while (ch != '\n' && ch != EOF)
        ch = READCHAR;
      if (ch == '\n') ch = READCHAR;
      /* It is OK to leave the position after a #! line, since
	 that is what read0 does.  */
    }

  if (ch != ';')
    /* The first line isn't a comment, just give up.  */
    {
      UNREAD (ch);
      return 0;
    }
  else
    /* Look for an appropriate file-variable in the first line.  */
    {
      bool rv = 0;
      enum {
	NOMINAL, AFTER_FIRST_DASH, AFTER_ASTERIX
      } beg_end_state = NOMINAL;
      bool in_file_vars = 0;

#define UPDATE_BEG_END_STATE(ch)				\
  if (beg_end_state == NOMINAL)					\
    beg_end_state = (ch == '-' ? AFTER_FIRST_DASH : NOMINAL);	\
  else if (beg_end_state == AFTER_FIRST_DASH)			\
    beg_end_state = (ch == '*' ? AFTER_ASTERIX : NOMINAL);	\
  else if (beg_end_state == AFTER_ASTERIX)			\
    {								\
      if (ch == '-')						\
	in_file_vars = !in_file_vars;				\
      beg_end_state = NOMINAL;					\
    }

      /* Skip until we get to the file vars, if any.  */
      do
	{
	  ch = READCHAR;
	  UPDATE_BEG_END_STATE (ch);
	}
      while (!in_file_vars && ch != '\n' && ch != EOF);

      while (in_file_vars)
	{
	  char var[100], val[100];
	  unsigned i;

	  ch = READCHAR;

	  /* Read a variable name.  */
	  while (ch == ' ' || ch == '\t')
	    ch = READCHAR;

	  i = 0;
	  beg_end_state = NOMINAL;
	  while (ch != ':' && ch != '\n' && ch != EOF && in_file_vars)
	    {
	      if (i < sizeof var - 1)
		var[i++] = ch;
	      UPDATE_BEG_END_STATE (ch);
	      ch = READCHAR;
	    }

	  /* Stop scanning if no colon was found before end marker.  */
	  if (!in_file_vars || ch == '\n' || ch == EOF)
	    break;

	  while (i > 0 && (var[i - 1] == ' ' || var[i - 1] == '\t'))
	    i--;
	  var[i] = '\0';

	  if (ch == ':')
	    {
	      /* Read a variable value.  */
	      ch = READCHAR;

	      while (ch == ' ' || ch == '\t')
		ch = READCHAR;

	      i = 0;
	      beg_end_state = NOMINAL;
	      while (ch != ';' && ch != '\n' && ch != EOF && in_file_vars)
		{
		  if (i < sizeof val - 1)
		    val[i++] = ch;
		  UPDATE_BEG_END_STATE (ch);
		  ch = READCHAR;
		}
	      if (!in_file_vars)
		/* The value was terminated by an end-marker, which remove.  */
		i -= 3;
	      while (i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t'))
		i--;
	      val[i] = '\0';

	      if (strcmp (var, "lexical-binding") == 0)
		/* This is it...  */
		{
		  rv = (strcmp (val, "nil") != 0);
		  break;
		}
	    }
	}

      while (ch != '\n' && ch != EOF)
	ch = READCHAR;

      return rv;
    }
}

/* Return version byte from .elc header, else zero.  */

static int
elc_version (Lisp_Object file, int fd)
{
  struct stat st;
  char buf[512];
  int version = 0;
  if (fstat (fd, &st) == 0 && S_ISREG (st.st_mode))
    {
      for (int i = 0, nbytes = emacs_read_quit (fd, buf, sizeof buf);
	   i < nbytes;
	   ++i)
	{
	  if (i >= 4 && buf[i] == '\n')
	    {
	      /* Only trust version if regexp found after newline.  */
	      if (++i <= nbytes - 1
		  && 0 <= fast_c_string_match_ignore_case (Vbytecomp_version_regexp,
							   buf + i, nbytes - i))
		version = buf[4]; /* version byte after initial `;ELC` */
	      break;
	    }
	}
      if (lseek (fd, 0, SEEK_SET) < 0)
	report_file_error ("Rewinding file pointer", file);
    }
  return version;
}


/* Callback for record_unwind_protect.  Restore the old load list OLD,
   after loading a file successfully.  */

static void
record_load_unwind (Lisp_Object old)
{
  Vloads_in_progress = old;
}

/* This handler function is used via internal_condition_case_1.  */

static Lisp_Object
load_error_handler (Lisp_Object /* err */)
{
  return Qnil;
}

static void
load_warn_unescaped_character_literals (Lisp_Object file)
{
  Lisp_Object warning =
    safe_calln (intern ("byte-run--unescaped-character-literals-warning"));
  if (!NILP (warning))
    {
      AUTO_STRING (format, "Loading `%s': %s");
      CALLN (Fmessage, format, file, warning);
    }
}

DEFUN ("get-load-suffixes", Fget_load_suffixes, Sget_load_suffixes, 0, 0, 0,
       doc: /* Return the suffixes that `load' should try if a suffix is \
required.
This uses the variables `load-suffixes' and `load-file-rep-suffixes'.  */)
  (void)
{
  Lisp_Object lst = Qnil, suffixes = Vload_suffixes;
  FOR_EACH_TAIL (suffixes)
    {
      Lisp_Object exts = Vload_file_rep_suffixes;
      Lisp_Object suffix = XCAR (suffixes);
      FOR_EACH_TAIL (exts)
	lst = Fcons (concat2 (suffix, XCAR (exts)), lst);
    }
  return Fnreverse (lst);
}

/* Return true if STRING ends with SUFFIX.  */
bool
suffix_p (Lisp_Object string, const char *suffix)
{
  ptrdiff_t suffix_len = strlen (suffix);
  ptrdiff_t string_len = SBYTES (string);

  return (suffix_len <= string_len
	  && strcmp (SSDATA (string) + string_len - suffix_len, suffix) == 0);
}

static void
close_infile_unwind (void *arg)
{
  struct infile *prev_infile = arg;
  eassert (infile && infile != prev_infile);
  fclose (infile->stream);
  infile = prev_infile;
}

static void
loadhist_initialize (Lisp_Object filename)
{
  eassert (STRINGP (filename) || NILP (filename));
  specbind (Qcurrent_load_list, Fcons (filename, Qnil));
}

#ifdef HAVE_NATIVE_COMP
static Lisp_Object
load_retry (ptrdiff_t nargs, Lisp_Object *args)
{
  eassert (nargs == 5);
  return Fload (args[0], args[1], args[2], args[3], args[4]);
}

static Lisp_Object
load_retry_handler (Lisp_Object err, ptrdiff_t nargs, Lisp_Object *args)
{
  return err;
}

static Lisp_Object
eln_inconsistent_handler (Lisp_Object err)
{
  if (CONSP (err))
    {
      AUTO_STRING (format, "%s");
      CALLN (Fmessage, format, Ferror_message_string (err));
    }
  return err;
}
#endif /* HAVE_NATIVE_COMP */

DEFUN ("load", Fload, Sload, 1, 5, 0,
       doc: /* Execute a file of Lisp code named FILE.
Iterates over directories in `load-path' to find FILE.  The variable
`load-suffixes' specifies the order in which suffixes to FILE are tried
(usually FILE.{so,dylib}[.gz], then FILE.elc[.gz], then FILE.el[.gz]).

The empty suffix is tried last.  Under NOSUFFIX, only the empty suffix
is tried.  Under MUST-SUFFIX, the empty suffix is not tried.
MUST-SUFFIX is ignored if FILE already ends in one of `load-suffixes' or
if FILE includes a directory.

Signals an error if a FILE variant cannot be found unless NOERROR.

Bookends loading with status messages unless NOMESSAGE (although
`force-load-messages' overrides).

During the actual loading of the FILE variant, the variable
`load-in-progress' is set true, and the variable `load-file-name' is
assigned the variant's file name.

Environment variables in FILE are interpolated with
`substitute-in-file-name'.

Return t if on success.  */)
  (Lisp_Object file, Lisp_Object noerror, Lisp_Object nomessage,
   Lisp_Object nosuffix, Lisp_Object must_suffix)
{
  FILE *stream UNINIT;
  int fd = -1;
  specpdl_ref fd_index UNINIT;
  specpdl_ref count = SPECPDL_INDEX ();
  Lisp_Object found = Qnil, suffixes = Qnil;
  struct infile input;

  CHECK_STRING (file);

  /* If file name is magic, call the handler.  */
  Lisp_Object handler = Ffind_file_name_handler (file, Qload);
  if (!NILP (handler))
    return call6 (handler, Qload, file, noerror, nomessage, nosuffix, must_suffix);

  /* The presence of this call is the result of a historical accident:
     it used to be in every file-operation and when it got removed
     everywhere, it accidentally stayed here.  Since then, enough people
     supposedly have things like (load "$PROJECT/foo.el") in their .emacs
     that it seemed risky to remove.  */
  if (!NILP (noerror))
    {
      file = internal_condition_case_1 (Fsubstitute_in_file_name, file,
					Qt, load_error_handler);
      if (NILP (file))
	return Qnil;
    }
  else
    file = Fsubstitute_in_file_name (file);

  if (SCHARS (file))
    {
      if (!NILP (must_suffix)
	  && (suffix_p (file, ".el")
	      || suffix_p (file, ".elc")
#ifdef HAVE_MODULES
	      || suffix_p (file, MODULES_SUFFIX)
#ifdef MODULES_SECONDARY_SUFFIX
	      || suffix_p (file, MODULES_SECONDARY_SUFFIX)
#endif
#endif
#ifdef HAVE_NATIVE_COMP
	      || suffix_p (file, NATIVE_SUFFIX)
#endif
	      || !NILP (Ffile_name_directory (file))))
	/* FILE already ends with suffix or contains directory.  */
	must_suffix = Qnil;

      suffixes = NILP (nosuffix)
	? CALLN (Fappend, Fget_load_suffixes (),
		 NILP (must_suffix) ? Vload_file_rep_suffixes : Qnil)
	: Qnil;
      fd = openp (Vload_path, file, suffixes, &found, Qnil);
    }

  if (fd == -1)
    {
      if (SCHARS (file) == 0)
	errno = ENOENT;
      if (NILP (noerror))
	report_file_error ("Cannot open load file", file);
      return Qnil;
    }

  /* Tell startup.el whether or not we found the user's init file.  */
  if (EQ (Qt, Vuser_init_file))
    Vuser_init_file = found;

  /* If FD is -2, that means openp found a magic file.  */
  if (fd == -2)
    {
      if (NILP (Fequal (found, file)))
	/* If FOUND is a different file name from FILE,
	   find its handler even if we have already inhibited
	   the `load' operation on FILE.  */
	handler = Ffind_file_name_handler (found, Qt);
      else
	handler = Ffind_file_name_handler (found, Qload);
      if (!NILP (handler))
	return call5 (handler, Qload, found, noerror, nomessage, Qt);
#ifdef DOS_NT
      /* Tramp has to deal with semi-broken packages that prepend
	 drive letters to remote files.  For that reason, Tramp
	 catches file operations that test for file existence, which
	 makes openp think X:/foo.elc files are remote.  However,
	 Tramp does not catch `load' operations for such files, so we
	 end up with a nil as the `load' handler above.  If we would
	 continue with fd = -2, we will behave wrongly, and in
	 particular try reading a .elc file in the "rt" mode instead
	 of "rb".  See bug #9311 for the results.  To work around
	 this, we try to open the file locally, and go with that if it
	 succeeds.  */
      fd = emacs_open (SSDATA (ENCODE_FILE (found)), O_RDONLY, 0);
      if (fd == -1)
	fd = -2;
#endif
    }

  if (fd >= 0)
    {
      fd_index = SPECPDL_INDEX ();
      record_unwind_protect_int (close_file_unwind, fd);
    }

#ifdef HAVE_MODULES
  bool is_module =
    suffix_p (found, MODULES_SUFFIX)
#ifdef MODULES_SECONDARY_SUFFIX
    || suffix_p (found, MODULES_SECONDARY_SUFFIX)
#endif
    ;
#else
  bool is_module = false;
#endif

  bool is_native = suffix_p (found, NATIVE_SUFFIX);

  /* Check if we're stuck recursively loading. */
  {
    int load_count = 0;
    Lisp_Object tem = Vloads_in_progress;
    FOR_EACH_TAIL_SAFE (tem)
      if (!NILP (Fequal (found, XCAR (tem))) && (++load_count > 3))
	signal_error ("Recursive load", Fcons (found, Vloads_in_progress));
  }
  record_unwind_protect (record_load_unwind, Vloads_in_progress);
  Vloads_in_progress = Fcons (found, Vloads_in_progress);

  /* Default to dynamic scoping.  */
  specbind (Qlexical_binding, Qnil);

  /* Warn about unescaped character literals.  */
  specbind (Qlread_unescaped_character_literals, Qnil);
  record_unwind_protect (load_warn_unescaped_character_literals, file);

  const int elc_ver = elc_version (found, fd);
  if (elc_ver || is_native)
    {
      /* Warn out-of-date .el[cn]. */
      struct stat s1, s2;
      const char *elcn = SSDATA (ENCODE_FILE (found));
      USE_SAFE_ALLOCA;
      char *el = SAFE_ALLOCA (strlen (elcn));
      memcpy (el, elcn, strlen (elcn) - 1);
      el[strlen (elcn) - 1] = '\0';
      if (0 == emacs_fstatat (AT_FDCWD, elcn, &s1, 0)
	  && 0 == emacs_fstatat (AT_FDCWD, el, &s2, 0)
	  && 0 > timespec_cmp (get_stat_mtime (&s1), get_stat_mtime (&s2)))
	message_with_string ("Loading %s despite modified .el", found, 1);
      SAFE_FREE ();
    }

  Lisp_Object ret;
  if (!elc_ver
      && !is_module
      && !is_native
      && !NILP (Vload_source_file_function))
    {
      /* UGLY: For the common case of interpreting uncompiled .el, call
	 load-with-code-conversion then short-circuit return.  RMS
	 begged off writing load-with-code-conversion in C so it looks
	 disturbingly like the remainder of Fload, replete with
	 identical diagnostic messages.  */
      if (fd >= 0)
	{
	  emacs_close (fd);
	  clear_unwind_protect (fd_index);
	}
      ret = unbind_to
	(count,
	 call4 (Vload_source_file_function, found,
		concat2 (Ffile_name_directory (file),
			 Ffile_name_nondirectory (found)),
		NILP (noerror) ? Qnil : Qt,
		(NILP (nomessage) || force_load_messages) ? Qnil : Qt));
      goto done; /* !!! */
    }

  if (is_module || is_native)
    {
      /* Can dismiss FD now since module-load handles.  */
      if (fd >= 0)
        {
          emacs_close (fd);
          clear_unwind_protect (fd_index);
        }
    }
  else
    {
      if (fd < 0)
	{
	  stream = NULL;
	  errno = EINVAL;
	}
      else
	{
	  const char *fmode = elc_ver ? "r" FOPEN_BINARY : "r" FOPEN_TEXT;
#ifdef WINDOWSNT
	  emacs_close (fd);
	  clear_unwind_protect (fd_index);
	  stream = emacs_fopen (SSDATA (ENCODE_FILE (found)), fmode);
#else
	  stream = fdopen (fd, fmode);
#endif
	}
      if (!stream)
        report_file_error ("Opening stdio stream", file);
      set_unwind_protect_ptr (fd_index, close_infile_unwind, infile);
      input.stream = stream;
      input.lookahead = 0;
      infile = &input;
      unread_char = -1;
    }

#define MESSAGE_LOADING(done)						\
  do {									\
    if (NILP (nomessage) || force_load_messages)			\
      {									\
	if (is_module)							\
	  message_with_string ("Loading %s (module)..." done, file, 1); \
	else if (is_native)					\
	  message_with_string ("Loading %s (native)..." done, file, 1); \
	else if (!elc_ver)						\
	  message_with_string ("Loading %s.el (source)..." done, file, 1); \
	else								\
	  message_with_string ("Loading %s..." done, file, 1);		\
      }									\
  } while (0);

  MESSAGE_LOADING ("");

  specbind (Qload_file_name, found);
  specbind (Qinhibit_file_name_operation, Qnil);
  specbind (Qload_in_progress, Qt);

  if (is_module)
    {
#ifdef HAVE_MODULES
      loadhist_initialize (found);
      Fmodule_load (found);
      build_load_history (found, true);
#else
      emacs_abort ();
#endif
    }
  else if (is_native)
    {
#ifdef HAVE_NATIVE_COMP
      loadhist_initialize (found);
      if (CONSP (internal_condition_case_1 (Fnative__load, found,
					    list1 (Qnative_lisp_file_inconsistent),
					    eln_inconsistent_handler)))
	{
	  /* hit Qnative_lisp_file_inconsistent, remove ".eln"
	     from `load-suffixes' and try again.  */
	  Lisp_Object restore_suffixes = Fcopy_sequence (Vload_suffixes),
	    tail = Vload_suffixes, head = Qnil;
	  FOR_EACH_TAIL (tail)
	    {
	      if (STRINGP (XCAR (tail))
		  && 0 == strcmp (SSDATA (CAR (tail)), NATIVE_SUFFIX))
		{
		  if (NILP (head))
		    Vload_suffixes = CDR (tail);
		  else
		    XSETCDR (head, CDR (tail));
		  break;
		}
	      head = tail;
	    }
	  ret = unbind_to
	    (count, internal_condition_case_n (load_retry, 5,
					       ((Lisp_Object [])
						{ file, noerror, nomessage,
						  nosuffix, must_suffix }),
					       Qt, load_retry_handler));
	  Vload_suffixes = restore_suffixes;
	  if (CONSP (ret))
	    xsignal (CAR (ret), CDR (ret));
	  goto done; /* !!! */
	}
      build_load_history (found, true);
#else
      emacs_abort ();
#endif
    }
  else
    {
      if (lisp_file_lexically_bound_p (Qget_file_char))
	set_internal (Qlexical_binding, Qt, Qnil, SET_INTERNAL_SET);
      readevalloop (Qget_file_char, &input, found, 0, Qnil, Qnil, Qnil, Qnil);
    }

  ret = unbind_to (count, Qt);

  /* Run any eval-after-load forms for this file.  */
  if (!NILP (Ffboundp (Qdo_after_load_evaluation)))
    call1 (Qdo_after_load_evaluation, found);

  for (int i = 0; i < ARRAYELTS (saved_strings); ++i)
    {
      xfree (saved_strings[i].string);
      saved_strings[i].string = NULL;
      saved_strings[i].size = 0;
    }

  if (!noninteractive)
    MESSAGE_LOADING ("done");

#undef MESSAGE_LOADING

 done:
  return ret;
}

Lisp_Object
save_match_data_load (Lisp_Object file, Lisp_Object noerror,
		      Lisp_Object nomessage, Lisp_Object nosuffix,
		      Lisp_Object must_suffix)
{
  specpdl_ref count = SPECPDL_INDEX ();
  record_unwind_save_match_data ();
  Lisp_Object result = Fload (file, noerror, nomessage, nosuffix, must_suffix);
  return unbind_to (count, result);
}

static bool
complete_filename_p (Lisp_Object pathname)
{
  const unsigned char *s = SDATA (pathname);
  return (IS_DIRECTORY_SEP (s[0])
	  || (SCHARS (pathname) > 2
	      && IS_DEVICE_SEP (s[1]) && IS_DIRECTORY_SEP (s[2])));
}

DEFUN ("locate-file-internal", Flocate_file_internal, Slocate_file_internal, 2, 4, 0,
       doc: /* Search for FILENAME through PATH.
Returns the file's name in absolute form, or nil if not found.
If SUFFIXES is non-nil, it should be a list of suffixes to append to
file name when searching.
If non-nil, PREDICATE is used instead of `file-readable-p'.
PREDICATE can also be an integer to pass to the faccessat(2) function,
in which case file-name-handlers are ignored.
This function will normally skip directories, so if you want it to find
directories, make sure the PREDICATE function returns `dir-ok' for them.  */)
  (Lisp_Object filename, Lisp_Object path, Lisp_Object suffixes, Lisp_Object predicate)
{
  Lisp_Object file;
  int fd = openp (path, filename, suffixes, &file, predicate);
  if (NILP (predicate) && fd >= 0)
    emacs_close (fd);
  return file;
}

/* Ridiculousness originating with Blandy then continually made worse.

   Ostensibly returns first file descriptor found in PATH for STR or STR
   catenated with one of SUFFIXES.

   PREDICATE is a lisp function, t, or a fixnum passed to access().  A
   non-nil PREDICATE has the important side effect of avoiding opening
   files -- useful when files are problematic (binary).  A trivial
   PREDICATE of t is only interested in this side effect.

   A non-null STOREPTR is populated with the found file name as a Lisp
   string, or nil if not found.

   Return -2 if the file found is remote.

   Return -2 if PREDICATE is satisfied.
*/

int
openp (Lisp_Object path, Lisp_Object str, Lisp_Object suffixes,
       Lisp_Object *storeptr, Lisp_Object predicate)
{
  ptrdiff_t fn_size = 100;
  char buf[100];
  char *fn = buf;
  bool absolute;
  ptrdiff_t want_length;
  Lisp_Object filename, string, tail, encoded_fn, best_string;
  ptrdiff_t max_suffix_len = 0;
  int last_errno = ENOENT;
  int best_fd = -1;
  USE_SAFE_ALLOCA;

  CHECK_STRING (str);

  tail = suffixes;
  FOR_EACH_TAIL_SAFE (tail)
    {
      CHECK_STRING_CAR (tail);
      max_suffix_len = max (max_suffix_len, SBYTES (XCAR (tail)));
    }

  string = filename = encoded_fn = best_string = Qnil;

  if (storeptr)
    *storeptr = Qnil;

  absolute = complete_filename_p (str);

  AUTO_LIST1 (just_use_str, Qnil);
  if (NILP (path))
    path = just_use_str;

  if (FIXNATP (predicate) && XFIXNAT (predicate) > INT_MAX)
    {
      last_errno = EINVAL;
      goto openp_out;
    }

  FOR_EACH_TAIL_SAFE (path)
   {
     ptrdiff_t baselen, prefixlen;

     if (EQ (path, just_use_str))
       filename = str;
     else
       filename = Fexpand_file_name (str, XCAR (path));

     if (!complete_filename_p (filename)) // complete === absolute
       {
	 filename = Fexpand_file_name (filename, BVAR (current_buffer, directory));
	 if (!complete_filename_p (filename))
	   continue;
       }

     /* Ensure FN big enough.  */
     want_length = max_suffix_len + SBYTES (filename);
     if (fn_size <= want_length)
       {
	 fn_size = 100 + want_length;
	 fn = SAFE_ALLOCA (fn_size);
       }

     /* Copy FILENAME's data to FN but remove starting /: if any.  */
     prefixlen = ((SCHARS (filename) > 2
		   && SREF (filename, 0) == '/'
		   && SREF (filename, 1) == ':')
		  ? 2 : 0);
     baselen = SBYTES (filename) - prefixlen;
     memcpy (fn, SDATA (filename) + prefixlen, baselen);

     /* Loop over suffixes.  */
     AUTO_LIST1 (empty_string_only, empty_unibyte_string);
     tail = NILP (suffixes) ? empty_string_only : suffixes;

     FOR_EACH_TAIL_SAFE (tail)
       {
	 Lisp_Object suffix = XCAR (tail);
	 ptrdiff_t fnlen, lsuffix = SBYTES (suffix);
	 Lisp_Object handler;
	 int fd = -1;

	 /* Make complete filename by appending SUFFIX.  */
	 memcpy (fn + baselen, SDATA (suffix), lsuffix + 1);
	 fnlen = baselen + lsuffix;

	 if (!STRING_MULTIBYTE (filename) && !STRING_MULTIBYTE (suffix))
	   /* Prefer unibyte to let loadup decide (loadup switches
	      between several default-file-name-coding-system).  */
	   string = make_unibyte_string (fn, fnlen);
	 else
	   string = make_string (fn, fnlen);
	 handler = Ffind_file_name_handler (string, Qfile_exists_p);

	 if (FIXNATP (predicate)
	     || (NILP (handler) && (NILP (predicate) || EQ (predicate, Qt))))
	   {
	     // In this case, no arbitrary lisp needs executing.
	     encoded_fn = ENCODE_FILE (string);
	     const char *pfn = SSDATA (encoded_fn);

	     bool q_good = FIXNATP (predicate)
	       ? 0 == faccessat (AT_FDCWD, pfn, XFIXNAT (predicate), AT_EACCESS)
	       : (fd = emacs_open (pfn, O_RDONLY, 0), fd >= 0);

	     if (q_good)
	       {
		 if (file_directory_p (encoded_fn))
		   {
		     last_errno = EISDIR;
		     if (fd >= 0)
		       emacs_close (fd);
		     fd = -1;
		   }
		 else if (FIXNATP (predicate)
			  && (errno == ENOENT || errno == ENOTDIR))
		   {
		     best_fd = 1; // just something not zero
		     best_string = string;
		     goto openp_out;
		   }
	       }
	     else if (errno != ENOENT && errno != ENOTDIR)
	       {
		 eassume (fd < 0);
		 last_errno = errno;
	       }

	     if (fd >= 0)
	       {
		 if (best_fd >= 0)
		   emacs_close (best_fd);
		 best_fd = fd;
		 best_string = string;
		 goto openp_out;
	       }
	   }
	 else
	   {
	     // Assert arbitrary lisp needs executing
	     eassert (!NILP (handler) || (!NILP (predicate) && !EQ (predicate, Qt)));
	     bool exists = false;
	     if (NILP (predicate) || EQ (predicate, Qt))
	       exists = !NILP (Ffile_readable_p (string));
	     else
	       {
		 Lisp_Object val = call1 (predicate, string);
		 if (!NILP (val))
		   {
		     if (EQ (val, Qdir_ok)
			 || NILP (Ffile_directory_p (string)))
		       exists = true;
		     else
		       last_errno = EISDIR;
		   }
	       }

	     if (exists)
	       {
		 best_string = string;
		 best_fd = -2;
		 goto openp_out;
	       }
	     eassume (fd == -1 && best_fd == -1);
	   }
       } /* FOR_EACH suffix */
     if (best_fd >= 0 || absolute)
       break;
   } /* FOR_EACH path */

 openp_out:
  if (!NILP (best_string))
    if (storeptr)
      *storeptr = best_string;
  SAFE_FREE ();
  errno = last_errno;
  return best_fd;
}


/* Merge the list we've accumulated of globals from the current input source
   into the load_history variable.  The details depend on whether
   the source has an associated file name or not.

   FILENAME is the file name that we are loading from.

   ENTIRE is true if loading that entire file, false if evaluating
   part of it.  */

static void
build_load_history (Lisp_Object filename, bool entire)
{
  Lisp_Object tail, prev, newelt;
  Lisp_Object tem, tem2;
  bool foundit = 0;

  tail = Vload_history;
  prev = Qnil;

  FOR_EACH_TAIL (tail)
    {
      tem = XCAR (tail);

      /* Find the feature's previous assoc list...  */
      if (!NILP (Fequal (filename, Fcar (tem))))
	{
	  foundit = 1;

	  /*  If we're loading the entire file, remove old data.  */
	  if (entire)
	    {
	      if (NILP (prev))
		Vload_history = XCDR (tail);
	      else
		Fsetcdr (prev, XCDR (tail));
	    }

	  /*  Otherwise, cons on new symbols that are not already members.  */
	  else
	    {
	      tem2 = Vcurrent_load_list;

	      FOR_EACH_TAIL (tem2)
		{
		  newelt = XCAR (tem2);
		  if (NILP (Fmember (newelt, tem)))
		    Fsetcar (tail, Fcons (XCAR (tem),
		     			  Fcons (newelt, XCDR (tem))));
		}
	    }
	}
      else
	prev = tail;
    }

  /* If we're loading an entire file, cons the new assoc onto the
     front of load-history, the most-recently-loaded position.  Also
     do this if we didn't find an existing member for the file.  */
  if (entire || !foundit)
    {
      Lisp_Object tem = Fnreverse (Vcurrent_load_list);
      eassert (EQ (filename, Fcar (tem)));
      if (!NILP (tem))
	Vload_history = Fcons (tem, Vload_history);
      /* FIXME: There should be an unbind_to right after calling us which
         should re-establish the previous value of Vcurrent_load_list.  */
      Vcurrent_load_list = Qt;
    }
}

static void
readevalloop_1 (int old)
{
  load_convert_to_unibyte = old;
}

/* Signal an `end-of-file' error, if possible with file name
   information.  */

static AVOID
end_of_file_error (void)
{
  if (STRINGP (Vload_file_name))
    xsignal1 (Qend_of_file, Vload_file_name);
  xsignal0 (Qend_of_file);
}

static Lisp_Object
readevalloop_eager_expand_eval (Lisp_Object val, Lisp_Object macroexpand)
{
  /* If we macroexpand the toplevel form non-recursively and it ends
     up being a `progn' (or if it was a progn to start), treat each
     form in the progn as a top-level form.  This way, if one form in
     the progn defines a macro, that macro is in effect when we expand
     the remaining forms.  See similar code in bytecomp.el.  */
  val = call2 (macroexpand, val, Qnil);
  if (EQ (CAR_SAFE (val), Qprogn))
    {
      Lisp_Object subforms = XCDR (val);
      val = Qnil;
      FOR_EACH_TAIL (subforms)
	val = readevalloop_eager_expand_eval (XCAR (subforms), macroexpand);
    }
  else
      val = eval_form (call2 (macroexpand, val, Qt));
  return val;
}

/* UNIBYTE configures load_convert_to_unibyte.  READFUN supplants `read'
   if non-nil.

   START, END delimits the region read, and are nil for non-buffer
   input.  */

static void
readevalloop (Lisp_Object readcharfun,
	      struct infile *infile0,
	      Lisp_Object sourcename,
	      bool printflag,
	      Lisp_Object unibyte, Lisp_Object readfun,
	      Lisp_Object start, Lisp_Object end)
{
  int c;
  Lisp_Object val;
  specpdl_ref count = SPECPDL_INDEX ();
  struct buffer *b = 0;
  bool continue_reading_p;
  bool first_sexp = 1;
  bool whole_buffer = 0; /* true if reading entire buffer */
  Lisp_Object macroexpand = intern ("internal-macroexpand-for-load");

  if (!NILP (sourcename))
    CHECK_STRING (sourcename);

  if (NILP (Ffboundp (macroexpand))
      || (STRINGP (sourcename) && (suffix_p (sourcename, ".elc")
				   || suffix_p (sourcename, NATIVE_SUFFIX))))
    /* Don't macroexpand before the corresponding function is defined
       and don't bother macroexpanding in .elc files, since it should have
       been done already.  */
    macroexpand = Qnil;

  if (MARKERP (readcharfun) && NILP (start))
    start = readcharfun;

  if (BUFFERP (readcharfun))
    b = XBUFFER (readcharfun);
  else if (MARKERP (readcharfun))
    b = XMARKER (readcharfun)->buffer;

  /* We assume START is nil when input is not from a buffer.  */
  if (!NILP (start) && !b)
    emacs_abort ();

  specbind (Qstandard_input, readcharfun);
  record_unwind_protect_int (readevalloop_1, load_convert_to_unibyte);
  load_convert_to_unibyte = !NILP (unibyte);

  Lisp_Object lexical_p = find_symbol_value (XSYMBOL (Qlexical_binding),
					     current_buffer);
  record_lexical_environment ();
  current_thread->lexical_environment = !NILP (lexical_p) && !EQ (lexical_p, Qunbound)
    ? list1 (Qt) : Qnil;

  specbind (Qmacroexp__dynvars, Vmacroexp__dynvars);

  /* Ensure sourcename is absolute, except whilst preloading.  */
  if (!will_dump_p ()
      && !NILP (sourcename)
      && !NILP (Ffile_name_absolute_p (sourcename)))
    sourcename = Fexpand_file_name (sourcename, Qnil);

  loadhist_initialize (sourcename);

  continue_reading_p = 1;
  while (continue_reading_p)
    {
      specpdl_ref count1 = SPECPDL_INDEX ();

      if (b != 0 && !BUFFER_LIVE_P (b))
	error ("Reading from killed buffer");

      if (!NILP (start))
	{
	  /* Switch to the buffer we are reading from.  */
	  record_unwind_protect_excursion ();
	  set_buffer_internal (b);

	  /* Save point in it.  */
	  record_unwind_protect_excursion ();
	  /* Save ZV in it.  */
	  record_unwind_protect (save_restriction_restore, save_restriction_save ());
	  /* Those get unbound after we read one expression.  */

	  /* Set point and ZV around stuff to be read.  */
	  Fgoto_char (start);
	  if (!NILP (end))
	    Fnarrow_to_region (make_fixnum (BEGV), end);

	  /* Just for cleanliness, convert END to a marker
	     if it is an integer.  */
	  if (FIXNUMP (end))
	    end = Fpoint_max_marker ();
	}

      /* On the first cycle, we can easily test here
	 whether we are reading the whole buffer.  */
      if (b && first_sexp)
	whole_buffer = (BUF_PT (b) == BUF_BEG (b) && BUF_ZV (b) == BUF_Z (b));

      eassert (!infile0 || infile == infile0);
    read_next:
      c = READCHAR;
      if (c == ';')
	{
	  while ((c = READCHAR) != '\n' && c != -1);
	  goto read_next;
	}
      if (c < 0)
	{
	  unbind_to (count1, Qnil);
	  break;
	}

      /* Ignore whitespace here, so we can detect eof.  */
      if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'
	  || c == NO_BREAK_SPACE)
	goto read_next;
      UNREAD (c);

      if (!HASH_TABLE_P (read_objects_map)
	  || XHASH_TABLE (read_objects_map)->count)
	read_objects_map
	  = make_hash_table (&hashtest_eq, DEFAULT_HASH_SIZE, Weak_None, false);
      if (!HASH_TABLE_P (read_objects_completed)
	  || XHASH_TABLE (read_objects_completed)->count)
	read_objects_completed
	  = make_hash_table (&hashtest_eq, DEFAULT_HASH_SIZE, Weak_None, false);
      if (!NILP (Vpdumper__pure_pool) && c == '(')
	val = read0 (readcharfun, false);
      else
	{
	  if (!NILP (readfun))
	    {
	      val = call1 (readfun, readcharfun);

	      /* If READCHARFUN has set point to ZV, we should
	         stop reading, even if the form read sets point
		 to a different value when evaluated.  */
	      if (BUFFERP (readcharfun))
		{
		  struct buffer *buf = XBUFFER (readcharfun);
		  if (BUF_PT (buf) == BUF_ZV (buf))
		    continue_reading_p = 0;
		}
	    }
	  else if (!NILP (Vload_read_function))
	    val = call1 (Vload_read_function, readcharfun);
	  else
	    val = read_internal_start (readcharfun, Qnil, Qnil, false);
	}
      /* Empty hashes can be reused; otherwise, reset on next call.  */
      if (HASH_TABLE_P (read_objects_map)
	  && XHASH_TABLE (read_objects_map)->count > 0)
	read_objects_map = Qnil;
      if (HASH_TABLE_P (read_objects_completed)
	  && XHASH_TABLE (read_objects_completed)->count > 0)
	read_objects_completed = Qnil;

      if (!NILP (start) && continue_reading_p)
	start = Fpoint_marker ();

      /* Restore saved point and BEGV.  */
      unbind_to (count1, Qnil);

      /* Now eval what we just read.  */
      if (!NILP (macroexpand))
        val = readevalloop_eager_expand_eval (val, macroexpand);
      else
	val = eval_form (val);

      if (printflag)
	{
	  Vvalues = Fcons (val, Vvalues);
	  if (EQ (Vstandard_output, Qt))
	    Fprin1 (val, Qnil, Qnil);
	  else
	    Fprint (val, Qnil);
	}

      first_sexp = 0;
    }

  build_load_history (sourcename,
		      infile0 || whole_buffer);

  unbind_to (count, Qnil);
}

DEFUN ("eval-buffer", Feval_buffer, Seval_buffer, 0, 5, "",
       doc: /* Execute the accessible portion of current buffer as Lisp code.
You can use \\[narrow-to-region] to limit the part of buffer to be evaluated.
When called from a Lisp program (i.e., not interactively), this
function accepts up to five optional arguments:
BUFFER is the buffer to evaluate (nil means use current buffer),
 or a name of a buffer (a string).
PRINTFLAG controls printing of output by any output functions in the
 evaluated code, such as `print', `princ', and `prin1':
  a value of nil means discard it; anything else is the stream to print to.
  See Info node `(elisp)Output Streams' for details on streams.
FILENAME specifies the file name to use for `load-history'.
UNIBYTE, if non-nil, specifies `load-convert-to-unibyte' for this
 invocation.
DO-ALLOW-PRINT, if non-nil, specifies that output functions in the
 evaluated code should work normally even if PRINTFLAG is nil, in
 which case the output is displayed in the echo area.

This function ignores the current value of the `lexical-binding'
variable.  Instead it will heed any
  -*- lexical-binding: t -*-
settings in the buffer, and if there is no such setting, the buffer
will be evaluated without lexical binding.

This function preserves the position of point.  */)
  (Lisp_Object buffer, Lisp_Object printflag, Lisp_Object filename, Lisp_Object unibyte, Lisp_Object do_allow_print)
{
  specpdl_ref count = SPECPDL_INDEX ();
  Lisp_Object tem, buf;

  if (NILP (buffer))
    buf = Fcurrent_buffer ();
  else
    buf = Fget_buffer (buffer);
  if (NILP (buf))
    error ("No such buffer");

  if (NILP (printflag) && NILP (do_allow_print))
    tem = Qsymbolp;
  else
    tem = printflag;

  if (NILP (filename))
    filename = BVAR (XBUFFER (buf), filename);

  specbind (Qeval_buffer_list, Fcons (buf, Veval_buffer_list));
  specbind (Qstandard_output, tem);
  record_unwind_protect_excursion ();
  BUF_TEMP_SET_PT (XBUFFER (buf), BUF_BEGV (XBUFFER (buf)));
  specbind (Qlexical_binding, lisp_file_lexically_bound_p (buf) ? Qt : Qnil);
  BUF_TEMP_SET_PT (XBUFFER (buf), BUF_BEGV (XBUFFER (buf)));
  readevalloop (buf, 0, filename,
		!NILP (printflag), unibyte, Qnil, Qnil, Qnil);
  return unbind_to (count, Qnil);
}

DEFUN ("eval-region", Feval_region, Seval_region, 2, 4, "r",
       doc: /* Execute the region as Lisp code.
When called from programs, expects two arguments,
giving starting and ending indices in the current buffer
of the text to be executed.
Programs can pass third argument PRINTFLAG which controls output:
 a value of nil means discard it; anything else is stream for printing it.
 See Info node `(elisp)Output Streams' for details on streams.
Also the fourth argument READ-FUNCTION, if non-nil, is used
instead of `read' to read each expression.  It gets one argument
which is the input stream for reading characters.

This function does not move point.  */)
  (Lisp_Object start, Lisp_Object end, Lisp_Object printflag, Lisp_Object read_function)
{
  /* FIXME: Do the eval-sexp-add-defvars dance!  */
  specpdl_ref count = SPECPDL_INDEX ();
  Lisp_Object tem, cbuf;

  cbuf = Fcurrent_buffer ();

  if (NILP (printflag))
    tem = Qsymbolp;
  else
    tem = printflag;
  specbind (Qstandard_output, tem);
  specbind (Qeval_buffer_list, Fcons (cbuf, Veval_buffer_list));

  /* `readevalloop' calls functions which check the type of start and end.  */
  readevalloop (cbuf, 0, BVAR (XBUFFER (cbuf), filename),
		!NILP (printflag), Qnil, read_function,
		start, end);

  return unbind_to (count, Qnil);
}

DEFUN ("read-annotated", Fread_annotated, Sread_annotated, 1, 1, 0,
       doc: /* Return parsed s-expr as `read' with each atom bundled
with its charpos as (CHARPOS . ATOM).  */)
  (Lisp_Object buffer)
{
  Lisp_Object retval, warning;
  specpdl_ref count = SPECPDL_INDEX ();

  CHECK_BUFFER (buffer);
  specbind (Qlread_unescaped_character_literals, Qnil);
  retval = read_internal_start (buffer, Qnil, Qnil, true);

  warning = safe_calln (intern ("byte-run--unescaped-character-literals-warning"));
  if (!NILP (warning))
    call2 (intern ("byte-compile-warn"), build_string ("%s"), warning);

  return unbind_to (count, retval);
}

DEFUN ("read", Fread, Sread, 0, 1, 0,
       doc: /* Read one Lisp expression as text from STREAM, return as Lisp object.
If STREAM is nil, use the value of `standard-input' (which see).
STREAM or the value of `standard-input' may be:
 a buffer (read from point and advance it)
 a marker (read from where it points and advance it)
 a function (call it with no arguments for each character,
     call it with a char as argument to push a char back)
 a string (takes text from string, starting at the beginning)
 t (read text line using minibuffer and use it, or read from
    standard input in batch mode).  */)
  (Lisp_Object stream)
{
  if (NILP (stream))
    stream = Vstandard_input;
  if (EQ (stream, Qt))
    stream = Qread_char;
  if (EQ (stream, Qread_char))
    /* FIXME: ?! This is used when the reader is called from the
       minibuffer without a stream, as in (read).  But is this feature
       ever used, and if so, why?  IOW, will anything break if this
       feature is removed !?  */
    return call1 (intern ("read-minibuffer"),
		  build_string ("Lisp expression: "));

  return read_internal_start (stream, Qnil, Qnil, false);
}

DEFUN ("read-from-string", Fread_from_string, Sread_from_string, 1, 3, 0,
       doc: /* Read one Lisp expression which is represented as text by STRING.
Returns a cons: (OBJECT-READ . FINAL-STRING-INDEX).
FINAL-STRING-INDEX is an integer giving the position of the next
remaining character in STRING.  START and END optionally delimit
a substring of STRING from which to read;  they default to 0 and
\(length STRING) respectively.  Negative values are counted from
the end of STRING.  */)
  (Lisp_Object string, Lisp_Object start, Lisp_Object end)
{
  Lisp_Object ret;
  CHECK_STRING (string);
  /* `read_internal_start' sets `read_from_string_index'.  */
  ret = read_internal_start (string, start, end, false);
  return Fcons (ret, make_fixnum (read_from_string_index));
}

/* Function to set up the global context we need in toplevel read
   calls.  START and END only used when STREAM is a string.  */
static Lisp_Object
read_internal_start (Lisp_Object stream, Lisp_Object start,
		     Lisp_Object end, bool annotated)
{
  Lisp_Object retval;

  readchar_charpos = BUFFERP (stream) ? XBUFFER (stream)->pt : 1;
  /* We can get called from readevalloop which may have set these
     already.  */
  if (!HASH_TABLE_P (read_objects_map)
      || XHASH_TABLE (read_objects_map)->count)
    read_objects_map
      = make_hash_table (&hashtest_eq, DEFAULT_HASH_SIZE, Weak_None, false);
  if (!HASH_TABLE_P (read_objects_completed)
      || XHASH_TABLE (read_objects_completed)->count)
    read_objects_completed
      = make_hash_table (&hashtest_eq, DEFAULT_HASH_SIZE, Weak_None, false);

  if (STRINGP (stream)
      || ((CONSP (stream) && STRINGP (XCAR (stream)))))
    {
      ptrdiff_t startval, endval;
      Lisp_Object string;

      if (STRINGP (stream))
	string = stream;
      else
	string = XCAR (stream);

      validate_subarray (string, start, end, SCHARS (string),
			 &startval, &endval);

      read_from_string_index = startval;
      read_from_string_index_byte = string_char_to_byte (string, startval);
      read_from_string_limit = endval;
    }

  retval = read0 (stream, annotated);
  if (HASH_TABLE_P (read_objects_map)
      && XHASH_TABLE (read_objects_map)->count > 0)
    read_objects_map = Qnil;
  if (HASH_TABLE_P (read_objects_completed)
      && XHASH_TABLE (read_objects_completed)->count > 0)
    read_objects_completed = Qnil;
  return retval;
}

/* Grow a read buffer BUF that contains OFFSET useful bytes of data,
   by at least MAX_MULTIBYTE_LENGTH bytes.  Update *BUF_ADDR and
   *BUF_SIZE accordingly; 0 <= OFFSET <= *BUF_SIZE.  If *BUF_ADDR is
   initially null, BUF is on the stack: copy its data to the new heap
   buffer.  Otherwise, BUF must equal *BUF_ADDR and can simply be
   reallocated.  Either way, remember the heap allocation (which is at
   pdl slot COUNT) so that it can be freed when unwinding the stack.*/

static char *
grow_read_buffer (char *buf, ptrdiff_t offset,
		  char **buf_addr, ptrdiff_t *buf_size, specpdl_ref count)
{
  char *p = xpalloc (*buf_addr, buf_size, MAX_MULTIBYTE_LENGTH, -1, 1);
  if (!*buf_addr)
    {
      memcpy (p, buf, offset);
      record_unwind_protect_ptr (xfree, p);
    }
  else
    set_unwind_protect_ptr (count, xfree, p);
  *buf_addr = p;
  return p;
}

/* Return the scalar value that has the Unicode character name NAME.
   Raise 'invalid-read-syntax' if there is no such character.  */
static int
character_name_to_code (char const *name, ptrdiff_t name_len,
			Lisp_Object readcharfun)
{
  /* For "U+XXXX", pass the leading '+' to string_to_number to reject
     monstrosities like "U+-0000".  */
  ptrdiff_t len = name_len - 1;
  Lisp_Object code
    = (name[0] == 'U' && name[1] == '+'
       ? string_to_number (name + 1, 16, &len)
       : call2 (Qchar_from_name, make_unibyte_string (name, name_len), Qt));

  if (!RANGED_FIXNUMP (0, code, MAX_UNICODE_CHAR)
      || len != name_len - 1
      || char_surrogate_p (XFIXNUM (code)))
    {
      AUTO_STRING (format, "\\N{%s}");
      AUTO_STRING_WITH_LEN (namestr, name, name_len);
      invalid_syntax_lisp (CALLN (Fformat, format, namestr), readcharfun);
    }

  return XFIXNUM (code);
}

/* Bound on the length of a Unicode character name.  As of
   Unicode 9.0.0 the maximum is 83, so this should be safe.  */
enum { UNICODE_CHARACTER_NAME_LENGTH_BOUND = 200 };

/* Read a character escape sequence, assuming we just read a backslash
   and one more character (next_char).  */
static int
read_char_escape (Lisp_Object readcharfun, int next_char)
{
  int modifiers = 0;
  ptrdiff_t ncontrol = 0;
  int chr;

 again: ;
  int c = next_char;
  int unicode_hex_count;
  int mod;

  switch (c)
    {
    case -1:
      end_of_file_error ();

    case 'a': chr = '\a'; break;
    case 'b': chr = '\b'; break;
    case 'd': chr =  127; break;
    case 'e': chr =   27; break;
    case 'f': chr = '\f'; break;
    case 'n': chr = '\n'; break;
    case 'r': chr = '\r'; break;
    case 't': chr = '\t'; break;
    case 'v': chr = '\v'; break;

    case '\n':
      /* ?\LF is an error; it's probably a user mistake.  */
      error ("Invalid escape char syntax: \\<newline>");

    /* \M-x etc: set modifier bit and parse the char to which it applies,
       allowing for chains such as \M-\S-\A-\H-\s-\C-q.  */
    case 'M': mod = meta_modifier;  goto mod_key;
    case 'S': mod = shift_modifier; goto mod_key;
    case 'H': mod = hyper_modifier; goto mod_key;
    case 'A': mod = alt_modifier;   goto mod_key;
    case 's': mod = super_modifier; goto mod_key;

    mod_key:
      {
	int c1 = READCHAR;
	if (c1 != '-')
	  {
	    if (c == 's')
	      {
		/* \s not followed by a hyphen is SPC.  */
		UNREAD (c1);
		chr = ' ';
		break;
	      }
	    else
	      /* \M, \S, \H, \A not followed by a hyphen is an error.  */
	      error ("Invalid escape char syntax: \\%c not followed by -", c);
	  }
	modifiers |= mod;
	c1 = READCHAR;
	if (c1 == '\\')
	  {
	    next_char = READCHAR;
	    goto again;
	  }
	chr = c1;
	break;
      }

    /* Control modifiers (\C-x or \^x) are messy and not actually idempotent.
       For example, ?\C-\C-a = ?\C-\001 = 0x4000001.
       Keep a count of them and apply them separately.  */
    case 'C':
      {
	int c1 = READCHAR;
	if (c1 != '-')
	  error ("Invalid escape char syntax: \\%c not followed by -", c);
      }
      FALLTHROUGH;
    /* The prefixes \C- and \^ are equivalent.  */
    case '^':
      {
	ncontrol++;
	int c1 = READCHAR;
	if (c1 == '\\')
	  {
	    next_char = READCHAR;
	    goto again;
	  }
	chr = c1;
	break;
      }

    /* 1-3 octal digits.  Values in 0x80..0xff are encoded as raw bytes.  */
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7':
      {
	int i = c - '0';
	int count = 0;
	while (count < 2)
	  {
	    int c = READCHAR;
	    if (c < '0' || c > '7')
	      {
		UNREAD (c);
		break;
	      }
	    i = (i << 3) + (c - '0');
	    count++;
	  }

	if (i >= 0x80 && i < 0x100)
	  i = BYTE8_TO_CHAR (i);
	chr = i;
	break;
      }

    /* 1 or more hex digits.  Values may encode modifiers.
       Values in 0x80..0xff using 2 hex digits are encoded as raw bytes.  */
    case 'x':
      {
	unsigned int i = 0;
	int count = 0;
	while (1)
	  {
	    int c = READCHAR;
	    int digit = char_hexdigit (c);
	    if (digit < 0)
	      {
		UNREAD (c);
		break;
	      }
	    i = (i << 4) + digit;
	    /* Allow hex escapes as large as ?\xfffffff, because some
	       packages use them to denote characters with modifiers.  */
	    if (i > (CHAR_META | (CHAR_META - 1)))
	      error ("Hex character out of range: \\x%x...", i);
	    count += count < 3;
	  }

	if (count == 0)
	  error ("Invalid escape char syntax: \\x not followed by hex digit");
	if (count < 3 && i >= 0x80)
	  i = BYTE8_TO_CHAR (i);
	modifiers |= i & CHAR_MODIFIER_MASK;
	chr = i & ~CHAR_MODIFIER_MASK;
	break;
      }

    /* 8-digit Unicode hex escape: \UHHHHHHHH */
    case 'U':
      unicode_hex_count = 8;
      goto unicode_hex;

    /* 4-digit Unicode hex escape: \uHHHH */
    case 'u':
      unicode_hex_count = 4;
    unicode_hex:
      {
	unsigned int i = 0;
	for (int count = 0; count < unicode_hex_count; count++)
	  {
	    int c = READCHAR;
	    if (c < 0)
	      error ("Malformed Unicode escape: \\%c%x",
		     unicode_hex_count == 4 ? 'u' : 'U', i);
	    int digit = char_hexdigit (c);
	    if (digit < 0)
	      error ("Non-hex character used for Unicode escape: %c (%d)",
		     c, c);
	    i = (i << 4) + digit;
	  }
	if (i > 0x10FFFF)
	  error ("Non-Unicode character: 0x%x", i);
	chr = i;
	break;
      }

    /* Named character: \N{name} */
    case 'N':
      {
        int c = READCHAR;
        if (c != '{')
          invalid_syntax ("Expected opening brace after \\N", readcharfun);
        char name[UNICODE_CHARACTER_NAME_LENGTH_BOUND + 1];
        bool whitespace = false;
        ptrdiff_t length = 0;
        while (true)
          {
            int c = READCHAR;
            if (c < 0)
              end_of_file_error ();
            if (c == '}')
              break;
            if (c >= 0x80)
              {
                AUTO_STRING (format,
                             "Invalid character U+%04X in character name");
		invalid_syntax_lisp (CALLN (Fformat, format,
					    make_fixed_natnum (c)),
				     readcharfun);
              }
            /* Treat multiple adjacent whitespace characters as a
               single space character.  This makes it easier to use
               character names in e.g. multi-line strings.  */
            if (c_isspace (c))
              {
                if (whitespace)
                  continue;
                c = ' ';
                whitespace = true;
              }
            else
              whitespace = false;
            name[length++] = c;
            if (length >= sizeof name)
              invalid_syntax ("Character name too long", readcharfun);
          }
        if (length == 0)
          invalid_syntax ("Empty character name", readcharfun);
	name[length] = '\0';

	/* character_name_to_code can invoke read0, recursively.
	   This is why read0 needs to be re-entrant.  */
	chr = character_name_to_code (name, length, readcharfun);
	break;
      }

    default:
      chr = c;
      break;
    }
  eassert (chr >= 0 && chr < (1 << CHARACTERBITS));

  /* Apply Control modifiers, using the rules:
     \C-X = ascii_ctrl(nomod(X)) | mods(X)  if nomod(X) is one of:
                                                A-Z a-z ? @ [ \ ] ^ _

            X | ctrl_modifier               otherwise

     where
         nomod(c) = c without modifiers
	 mods(c)  = the modifiers of c
         ascii_ctrl(c) = 127       if c = '?'
                         c & 0x1f  otherwise
  */
  while (ncontrol > 0)
    {
      if ((chr >= '@' && chr <= '_') || (chr >= 'a' && chr <= 'z'))
	chr &= 0x1f;
      else if (chr == '?')
	chr = 127;
      else
	modifiers |= ctrl_modifier;
      ncontrol--;
    }

  return chr | modifiers;
}

/* Return the digit that CHARACTER stands for in the given BASE.
   Return -1 if CHARACTER is out of range for BASE,
   and -2 if CHARACTER is not valid for any supported BASE.  */
static int
digit_to_number (int character, int base)
{
  int digit;

  if ('0' <= character && character <= '9')
    digit = character - '0';
  else if ('a' <= character && character <= 'z')
    digit = character - 'a' + 10;
  else if ('A' <= character && character <= 'Z')
    digit = character - 'A' + 10;
  else
    return -2;

  return digit < base ? digit : -1;
}

static void
invalid_radix_integer (EMACS_INT radix, Lisp_Object readcharfun)
{
  char buf[64];
  int n = snprintf (buf, sizeof buf, "integer, radix %"pI"d", radix);
  eassert (n < sizeof buf);
  invalid_syntax (buf, readcharfun);
}

/* Read an integer in radix RADIX using READCHARFUN to read
   characters.  RADIX must be in the interval [2..36].
   Value is the integer read.
   Signal an error if encountering invalid read syntax.  */

static Lisp_Object
read_integer (Lisp_Object readcharfun, int radix)
{
  char stackbuf[20];
  char *read_buffer = stackbuf;
  ptrdiff_t read_buffer_size = sizeof stackbuf;
  char *p = read_buffer;
  char *heapbuf = NULL;
  int valid = -1; /* 1 if valid, 0 if not, -1 if incomplete.  */
  specpdl_ref count = SPECPDL_INDEX ();

  int c = READCHAR;
  if (c == '-' || c == '+')
    {
      *p++ = c;
      c = READCHAR;
    }

  if (c == '0')
    {
      *p++ = c;
      valid = 1;

      /* Ignore redundant leading zeros, so the buffer doesn't
	 fill up with them.  */
      do
	c = READCHAR;
      while (c == '0');
    }

  for (int digit; (digit = digit_to_number (c, radix)) >= -1; )
    {
      if (digit == -1)
	valid = 0;
      if (valid < 0)
	valid = 1;
      /* Allow 1 extra byte for the \0.  */
      if (p + 1 == read_buffer + read_buffer_size)
	{
	  ptrdiff_t offset = p - read_buffer;
	  read_buffer = grow_read_buffer (read_buffer, offset,
					  &heapbuf, &read_buffer_size,
					  count);
	  p = read_buffer + offset;
	}
      *p++ = c;
      c = READCHAR;
    }

  UNREAD (c);

  if (valid != 1)
    invalid_radix_integer (radix, readcharfun);

  *p = '\0';
  return unbind_to (count, string_to_number (read_buffer, radix, NULL));
}

/* Read a character literal (preceded by `?').  */
static Lisp_Object
read_char_literal (Lisp_Object readcharfun)
{
  int ch = READCHAR;
  if (ch < 0)
    end_of_file_error ();

  /* Accept `single space' syntax like (list ? x) where the
     whitespace character is SPC or TAB.
     Other literal whitespace like NL, CR, and FF are not accepted,
     as there are well-established escape sequences for these.  */
  if (ch == ' ' || ch == '\t')
    return make_fixnum (ch);

  if (   ch == '(' || ch == ')' || ch == '[' || ch == ']'
      || ch == '"' || ch == ';')
    {
      CHECK_LIST (Vlread_unescaped_character_literals);
      Lisp_Object char_obj = make_fixed_natnum (ch);
      if (NILP (Fmemq (char_obj, Vlread_unescaped_character_literals)))
	Vlread_unescaped_character_literals =
	  Fcons (char_obj, Vlread_unescaped_character_literals);
    }

  if (ch == '\\')
    ch = read_char_escape (readcharfun, READCHAR);

  int modifiers = ch & CHAR_MODIFIER_MASK;
  ch &= ~CHAR_MODIFIER_MASK;
  if (CHAR_BYTE8_P (ch))
    ch = CHAR_TO_BYTE8 (ch);
  ch |= modifiers;

  int nch = READCHAR;
  UNREAD (nch);
  if (nch <= 32
      || nch == '"' || nch == '\'' || nch == ';' || nch == '('
      || nch == ')' || nch == '['  || nch == ']' || nch == '#'
      || nch == '?' || nch == '`'  || nch == ',' || nch == '.')
    return make_fixnum (ch);

  invalid_syntax ("?", readcharfun);
}

/* Read a string literal (preceded by '"').  */
static Lisp_Object
read_string_literal (Lisp_Object readcharfun)
{
  char stackbuf[1024];
  char *read_buffer = stackbuf;
  ptrdiff_t read_buffer_size = sizeof stackbuf;
  specpdl_ref count = SPECPDL_INDEX ();
  char *heapbuf = NULL;
  char *p = read_buffer;
  char *end = read_buffer + read_buffer_size;
  /* True if we saw an escape sequence specifying
     a multibyte character.  */
  bool force_multibyte = false;
  /* True if we saw an escape sequence specifying
     a single-byte character.  */
  bool force_singlebyte = false;
  ptrdiff_t nchars = 0;

  int ch;
  while ((ch = READCHAR) >= 0 && ch != '\"')
    {
      if (end - p < MAX_MULTIBYTE_LENGTH)
	{
	  ptrdiff_t offset = p - read_buffer;
	  read_buffer = grow_read_buffer (read_buffer, offset,
					  &heapbuf, &read_buffer_size,
					  count);
	  p = read_buffer + offset;
	  end = read_buffer + read_buffer_size;
	}

      if (ch == '\\')
	{
	  /* First apply string-specific escape rules:  */
	  ch = READCHAR;
	  switch (ch)
	    {
	    case 's':
	      /* `\s' is always a space in strings.  */
	      ch = ' ';
	      break;
	    case ' ':
	    case '\n':
	      /* `\SPC' and `\LF' generate no characters at all.  */
	      continue;
	    default:
	      ch = read_char_escape (readcharfun, ch);
	      break;
	    }

	  int modifiers = ch & CHAR_MODIFIER_MASK;
	  ch &= ~CHAR_MODIFIER_MASK;

	  if (CHAR_BYTE8_P (ch))
	    force_singlebyte = true;
	  else if (!ASCII_CHAR_P (ch))
	    force_multibyte = true;
	  else		/* I.e. ASCII_CHAR_P (ch).  */
	    {
	      /* Allow `\C-SPC' and `\^SPC'.  This is done here because
		 the literals ?\C-SPC and ?\^SPC (rather inconsistently)
		 yield (' ' | CHAR_CTL); see bug#55738.  */
	      if (modifiers == CHAR_CTL && ch == ' ')
		{
		  ch = 0;
		  modifiers = 0;
		}
	      if (modifiers & CHAR_SHIFT)
		{
		  /* Shift modifier is valid only with [A-Za-z].  */
		  if (ch >= 'A' && ch <= 'Z')
		    modifiers &= ~CHAR_SHIFT;
		  else if (ch >= 'a' && ch <= 'z')
		    {
		      ch -= ('a' - 'A');
		      modifiers &= ~CHAR_SHIFT;
		    }
		}

	      if (modifiers & CHAR_META)
		{
		  /* Move the meta bit to the right place for a
		     string.  */
		  modifiers &= ~CHAR_META;
		  ch = BYTE8_TO_CHAR (ch | 0x80);
		  force_singlebyte = true;
		}
	    }

	  /* Any modifiers remaining are invalid.  */
	  if (modifiers)
	    invalid_syntax ("Invalid modifier in string", readcharfun);
	  p += CHAR_STRING (ch, (unsigned char *) p);
	}
      else
	{
	  p += CHAR_STRING (ch, (unsigned char *) p);
	  if (CHAR_BYTE8_P (ch))
	    force_singlebyte = true;
	  else if (!ASCII_CHAR_P (ch))
	    force_multibyte = true;
	}
      nchars++;
    }

  if (ch < 0)
    end_of_file_error ();

  if (!force_multibyte && force_singlebyte)
    {
      /* READ_BUFFER contains raw 8-bit bytes and no multibyte
	 forms.  Convert it to unibyte.  */
      nchars = str_as_unibyte ((unsigned char *) read_buffer,
			       p - read_buffer);
      p = read_buffer + nchars;
    }
  Lisp_Object obj = (force_multibyte || (nchars != p - read_buffer))
    ? make_multibyte_string (read_buffer, nchars, p - read_buffer)
    : make_unibyte_string (read_buffer, p - read_buffer);
  return unbind_to (count, obj);
}

/* Make a hash table from the constructor plist.  */
static Lisp_Object
hash_table_from_plist (Lisp_Object plist)
{
  Lisp_Object params[4 * 2];
  Lisp_Object *par = params;

  /* This is repetitive but fast and simple.  */
#define ADDPARAM(name)					\
  do {							\
    Lisp_Object val = plist_get (plist, Q ## name);	\
    if (!NILP (val))					\
      {							\
	*par++ = QC ## name;				\
	*par++ = val;					\
      }							\
  } while (0)

  ADDPARAM (test);
  ADDPARAM (weakness);
  ADDPARAM (purecopy);

  Lisp_Object data = plist_get (plist, Qdata);
  if (!(NILP (data) || CONSP (data)))
    error ("Hash table data is not a list");
  ptrdiff_t data_len = list_length (data);
  if (data_len & 1)
    error ("Hash table data length is odd");
  *par++ = QCsize;
  *par++ = make_fixnum (data_len / 2);

  /* Now use params to make a new hash table and fill it.  */
  Lisp_Object ht = Fmake_hash_table (par - params, params);

  while (!NILP (data))
    {
      Lisp_Object key = XCAR (data);
      data = XCDR (data);
      Lisp_Object val = XCAR (data);
      Fputhash (key, val, ht);
      data = XCDR (data);
    }

  return ht;
}

static Lisp_Object
record_from_list (Lisp_Object elems)
{
  ptrdiff_t size = list_length (elems);
  Lisp_Object obj = Fmake_record (XCAR (elems),
				  make_fixnum (size - 1),
				  Qnil);
  Lisp_Object tl = XCDR (elems);
  for (int i = 1; i < size; i++)
    {
      ASET (obj, i, XCAR (tl));
      tl = XCDR (tl);
    }
  return obj;
}

/* Turn a reversed list into a vector.  */
static Lisp_Object
vector_from_rev_list (Lisp_Object elems)
{
  ptrdiff_t size = list_length (elems);
  Lisp_Object obj = initialize_vector (size, Qnil);
  Lisp_Object *vec = XVECTOR (obj)->contents;
  for (ptrdiff_t i = size - 1; i >= 0; i--)
    {
      vec[i] = XCAR (elems);
      Lisp_Object next = XCDR (elems);
      free_cons (XCONS (elems));
      elems = next;
    }
  return obj;
}

static Lisp_Object get_lazy_string (Lisp_Object val);

static Lisp_Object
bytecode_from_rev_list (Lisp_Object elems, Lisp_Object readcharfun)
{
  Lisp_Object obj = vector_from_rev_list (elems);
  Lisp_Object *vec = XVECTOR (obj)->contents;
  ptrdiff_t size = ASIZE (obj);

  if (infile && size >= CLOSURE_CONSTANTS)
    {
      /* Always read 'lazily-loaded' bytecode (generated by the
         `byte-compile-dynamic' feature prior to Emacs 30) eagerly, to
         avoid code in the fast path during execution.  */
      if (CONSP (vec[CLOSURE_CODE])
          && FIXNUMP (XCDR (vec[CLOSURE_CODE])))
        vec[CLOSURE_CODE] = get_lazy_string (vec[CLOSURE_CODE]);

      /* Lazily-loaded bytecode is represented by the constant slot being nil
         and the bytecode slot a (lazily loaded) string containing the
         print representation of (BYTECODE . CONSTANTS).  Unpack the
         pieces by coerceing the string to unibyte and reading the result.  */
      if (NILP (vec[CLOSURE_CONSTANTS]) && STRINGP (vec[CLOSURE_CODE]))
        {
          Lisp_Object enc = vec[CLOSURE_CODE];
          Lisp_Object pair = Fread (Fcons (enc, readcharfun));
          if (!CONSP (pair))
	    invalid_syntax ("Invalid byte-code object", readcharfun);

          vec[CLOSURE_CODE] = XCAR (pair);
          vec[CLOSURE_CONSTANTS] = XCDR (pair);
        }
    }

  if (!(size >= CLOSURE_STACK_DEPTH && size <= CLOSURE_INTERACTIVE + 1
	&& (FIXNUMP (vec[CLOSURE_ARGLIST])
	    || CONSP (vec[CLOSURE_ARGLIST])
	    || NILP (vec[CLOSURE_ARGLIST]))
	&& ((STRINGP (vec[CLOSURE_CODE]) /* Byte-code function.  */
	     && VECTORP (vec[CLOSURE_CONSTANTS])
	     && size > CLOSURE_STACK_DEPTH
	     && (FIXNATP (vec[CLOSURE_STACK_DEPTH])))
	    || (CONSP (vec[CLOSURE_CODE]) /* Interpreted function.  */
	        && (CONSP (vec[CLOSURE_CONSTANTS])
	            || NILP (vec[CLOSURE_CONSTANTS]))))))
    invalid_syntax ("Invalid byte-code object", readcharfun);

  if (STRINGP (vec[CLOSURE_CODE]))
    {
      if (STRING_MULTIBYTE (vec[CLOSURE_CODE]))
        /* BYTESTR must have been produced by Emacs 20.2 or earlier
           because it produced a raw 8-bit string for byte-code and
           now such a byte-code string is loaded as multibyte with
           raw 8-bit characters converted to multibyte form.
           Convert them back to the original unibyte form.  */
        vec[CLOSURE_CODE] = Fstring_as_unibyte (vec[CLOSURE_CODE]);

      /* Bytecode must be immovable.  */
      pin_string (vec[CLOSURE_CODE]);
    }

  XSETPVECTYPE (XVECTOR (obj), PVEC_CLOSURE);
  return obj;
}

static Lisp_Object
char_table_from_rev_list (Lisp_Object elems, Lisp_Object readcharfun)
{
  Lisp_Object obj = vector_from_rev_list (elems);
  if (ASIZE (obj) < CHAR_TABLE_STANDARD_SLOTS)
    invalid_syntax ("Invalid size char-table", readcharfun);
  XSETPVECTYPE (XVECTOR (obj), PVEC_CHAR_TABLE);
  return obj;
}

static Lisp_Object
sub_char_table_from_rev_list (Lisp_Object elems, Lisp_Object readcharfun)
{
  /* A sub-char-table can't be read as a regular vector because of two
     C integer fields.  */
  elems = Fnreverse (elems);
  ptrdiff_t size = list_length (elems);
  if (size < 2)
    error ("Invalid size of sub-char-table");

  if (!RANGED_FIXNUMP (1, XCAR (elems), 3))
    error ("Invalid depth in sub-char-table");
  int depth = XFIXNUM (XCAR (elems));

  if (chartab_size[depth] != size - 2)
    error ("Invalid size in sub-char-table");
  elems = XCDR (elems);

  if (!RANGED_FIXNUMP (0, XCAR (elems), MAX_CHAR))
    error ("Invalid minimum character in sub-char-table");
  int min_char = XFIXNUM (XCAR (elems));
  elems = XCDR (elems);

  Lisp_Object tbl = make_sub_char_table (depth, min_char);
  for (int i = 0; i < size - 2; i++)
    {
      XSUB_CHAR_TABLE (tbl)->contents[i] = XCAR (elems);
      elems = XCDR (elems);
    }
  return tbl;
}

static Lisp_Object
string_props_from_rev_list (Lisp_Object elems, Lisp_Object readcharfun)
{
  elems = Fnreverse (elems);
  if (NILP (elems) || !STRINGP (XCAR (elems)))
    invalid_syntax ("#", readcharfun);
  Lisp_Object obj = XCAR (elems);
  for (Lisp_Object tl = XCDR (elems); !NILP (tl);)
    {
      Lisp_Object beg = XCAR (tl);
      tl = XCDR (tl);
      if (NILP (tl))
	invalid_syntax ("Invalid string property list", readcharfun);
      Lisp_Object end = XCAR (tl);
      tl = XCDR (tl);
      if (NILP (tl))
	invalid_syntax ("Invalid string property list", readcharfun);
      Lisp_Object plist = XCAR (tl);
      tl = XCDR (tl);
      Fset_text_properties (beg, end, plist, obj);
    }
  return obj;
}

/* Read a bool vector (preceded by "#&").  */
static Lisp_Object
read_bool_vector (Lisp_Object readcharfun)
{
  EMACS_INT length = 0;
  for (;;)
    {
      int c = READCHAR;
      if (c < '0' || c > '9')
	{
	  if (c != '"')
	    invalid_syntax ("#&", readcharfun);
	  break;
	}
      if (ckd_mul (&length, length, 10)
	  || ckd_add (&length, length, c - '0'))
	invalid_syntax ("#&", readcharfun);
    }
  if (BOOL_VECTOR_LENGTH_MAX < length)
    invalid_syntax ("#&", readcharfun);

  ptrdiff_t size_in_chars = bool_vector_bytes (length);
  Lisp_Object str = read_string_literal (readcharfun);
  if (STRING_MULTIBYTE (str)
      || !(size_in_chars == SCHARS (str)
	   /* We used to print 1 char too many when the number of bits
	      was a multiple of 8.  Accept such input in case it came
	      from an old version.  */
	   || length == (SCHARS (str) - 1) * BOOL_VECTOR_BITS_PER_CHAR))
    invalid_syntax ("#&...", readcharfun);

  Lisp_Object obj = make_bool_vector (length);
  unsigned char *data = bool_vector_uchar_data (obj);
  memcpy (data, SDATA (str), size_in_chars);
  /* Clear the extraneous bits in the last byte.  */
  if (length != size_in_chars * BOOL_VECTOR_BITS_PER_CHAR)
    data[size_in_chars - 1] &= (1 << (length % BOOL_VECTOR_BITS_PER_CHAR)) - 1;
  return obj;
}

/* Skip (and optionally remember) a lazily-loaded string
   preceded by "#@".  Return true if this was a normal skip,
   false if we read #@00 (which skips to EOB/EOF).  */
static bool
skip_lazy_string (Lisp_Object readcharfun)
{
  ptrdiff_t nskip = 0;
  ptrdiff_t digits = 0;
  for (;;)
    {
      int c = READCHAR;
      if (c < '0' || c > '9')
	{
	  if (nskip > 0)
	    /* We can't use UNREAD here, because in the code below we side-step
	       READCHAR.  Instead, assume the first char after #@NNN occupies
	       a single byte, which is the case normally since it's just
	       a space.  */
	    nskip--;
	  else
	    UNREAD (c);
	  break;
	}
      if (ckd_mul (&nskip, nskip, 10)
	  || ckd_add (&nskip, nskip, c - '0'))
	invalid_syntax ("#@", readcharfun);
      digits++;
      if (digits == 2 && nskip == 0)
	{
	  /* #@00 means "read nil and skip to end" */
	  skip_dyn_eof (readcharfun);
	  return false;
	}
    }

  if (load_force_doc_strings && FROM_FILE_P (readcharfun))
    {
      /* If we are supposed to force doc strings into core right now,
	 record the last string that we skipped,
	 and record where in the file it comes from.  */

      /* First exchange the two saved_strings.  */
      static_assert (ARRAYELTS (saved_strings) == 2);
      struct saved_string t = saved_strings[0];
      saved_strings[0] = saved_strings[1];
      saved_strings[1] = t;

      enum { extra = 100 };
      struct saved_string *ss = &saved_strings[0];
      if (ss->size == 0)
	{
	  ss->size = nskip + extra;
	  ss->string = xmalloc (ss->size);
	}
      else if (nskip > ss->size)
	{
	  ss->size = nskip + extra;
	  ss->string = xrealloc (ss->string, ss->size);
	}

      FILE *instream = infile->stream;
      ss->position = (file_tell (instream) - infile->lookahead);

      /* Copy that many bytes into the saved string.  */
      ptrdiff_t i = 0;
      int c = 0;
      for (int n = min (nskip, infile->lookahead); n > 0; n--)
	ss->string[i++] = c = infile->buf[--infile->lookahead];
      block_input ();
      for (; i < nskip && c >= 0; i++)
	ss->string[i] = c = getc (instream);
      unblock_input ();

      ss->length = i;
    }
  else
    /* Skip that many bytes.  */
    skip_dyn_bytes (readcharfun, nskip);

  return true;
}

/* Given a lazy-loaded string designator VAL, return the actual string.
   VAL is (FILENAME . POS).  */
static Lisp_Object
get_lazy_string (Lisp_Object val)
{
  /* Get a doc string from the file we are loading.
     If it's in a saved string, get it from there.

     Here, we don't know if the string is a bytecode string or a doc
     string.  As a bytecode string must be unibyte, we always return a
     unibyte string.  If it is actually a doc string, caller must make
     it multibyte.  */

  /* We used to emit negative positions for 'user variables' (whose doc
     strings started with an asterisk); take the absolute value for
     compatibility.  */
  EMACS_INT pos = eabs (XFIXNUM (XCDR (val)));
  struct saved_string *ss = &saved_strings[0];
  struct saved_string *ssend = ss + ARRAYELTS (saved_strings);
  while (ss < ssend
	 && !(pos >= ss->position && pos < ss->position + ss->length))
    ss++;
  if (ss >= ssend)
    return get_doc_string (val, 1, 0);

  ptrdiff_t start = pos - ss->position;
  char *str = ss->string;
  ptrdiff_t from = start;
  ptrdiff_t to = start;

  /* Process quoting with ^A, and find the end of the string,
     which is marked with ^_ (037).  */
  while (str[from] != 037)
    {
      int c = str[from++];
      if (c == 1)
	{
	  c = str[from++];
	  str[to++] = (c == 1 ? c
		       : c == '0' ? 0
		       : c == '_' ? 037
		       : c);
	}
      else
	str[to++] = c;
    }

  return make_unibyte_string (str + start, to - start);
}

/* Length of prefix only consisting of symbol constituent characters.  */
static ptrdiff_t
symbol_char_span (const char *s)
{
  const char *p = s;
  while (   *p == '^' || *p == '*' || *p == '+' || *p == '-' || *p == '/'
	 || *p == '<' || *p == '=' || *p == '>' || *p == '_' || *p == '|')
    p++;
  return p - s;
}

static void
skip_space_and_comments (Lisp_Object readcharfun)
{
  int c;
  do
    {
      c = READCHAR;
      if (c == ';')
	do
	  c = READCHAR;
	while (c >= 0 && c != '\n');
      if (c < 0)
	end_of_file_error ();
    }
  while (c <= 32 || c == NO_BREAK_SPACE);
  UNREAD (c);
}

/* When an object is read, the type of the top read stack entry indicates
   the syntactic context.  */
enum read_entry_type
{
				/* preceding syntactic context */

  RE_vector,			/* "[" (* OBJECT) */
  RE_record,			/* "#s(" (* OBJECT) */
  RE_char_table,		/* "#^[" (* OBJECT) */
  RE_sub_char_table,		/* "#^^[" (* OBJECT) */
  RE_byte_code,			/* "#[" (* OBJECT) */
  RE_string_props,		/* "#(" (* OBJECT) */
  RE_special,			/* "'" | "#'" | "`" | "," | ",@" */
  RE_quoted_max,                /* preclude ANNOTATE for earlier types */

  RE_list_start,		/* "(" */

  RE_list,			/* "(" (+ OBJECT) */
  RE_list_dot,			/* "(" (+ OBJECT) "." */

  RE_numbered,			/* "#" (+ DIGIT) "=" */
};

struct read_stack_entry
{
  enum read_entry_type type;
  union {
    /* RE_list, RE_list_dot */
    struct {
      Lisp_Object head;		/* first cons of list */
      Lisp_Object tail;		/* last cons of list */
    } list;

    /* RE_vector, RE_record, RE_char_table, RE_sub_char_table,
       RE_byte_code, RE_string_props */
    struct {
      Lisp_Object elems;	/* list of elements in reverse order */
    } vector;

    /* RE_special */
    struct {
      Lisp_Object symbol;	/* symbol from special syntax */
    } special;

    /* RE_numbered */
    struct {
      Lisp_Object number;	/* number as a fixnum */
      Lisp_Object placeholder;	/* placeholder object */
    } numbered;
  } u;
};

struct read_stack
{
  struct read_stack_entry *stack;  /* base of stack */
  ptrdiff_t size;		   /* allocated size in entries */
  ptrdiff_t sp;			   /* current number of entries */
};

static struct read_stack rdstack = {NULL, 0, 0};

static inline bool
quoted_parse_state (size_t *qcounts)
{
  for (ptrdiff_t i = 0; i < RE_quoted_max; ++i)
    if (qcounts[i])
      return true;
  return false;
}

#define ANNOTATE(atom)					\
  (annotated && !quoted_parse_state(qcounts)	\
   ? Fcons (make_fixnum (initial_charpos), atom)	\
   : atom)

void
mark_lread (void)
{
  /* Mark the read stack, which may contain data not otherwise traced */
  for (ptrdiff_t i = 0; i < rdstack.sp; i++)
    {
      struct read_stack_entry *e = &rdstack.stack[i];
      switch (e->type)
	{
	case RE_list_start:
	  break;
	case RE_list:
	case RE_list_dot:
	  mark_object (&e->u.list.head);
	  mark_object (&e->u.list.tail);
	  break;
	case RE_vector:
	case RE_record:
	case RE_char_table:
	case RE_sub_char_table:
	case RE_byte_code:
	case RE_string_props:
	  mark_object (&e->u.vector.elems);
	  break;
	case RE_special:
	  mark_object (&e->u.special.symbol);
	  break;
	case RE_numbered:
	  mark_object (&e->u.numbered.number);
	  mark_object (&e->u.numbered.placeholder);
	  break;
	default:
	  emacs_abort ();
	  break;
	}
    }
}

static inline struct read_stack_entry *
read_stack_top (void)
{
  eassume (rdstack.sp > 0);
  return &rdstack.stack[rdstack.sp - 1];
}

static inline struct read_stack_entry *
read_stack_pop (size_t *qcounts)
{
  eassume (rdstack.sp > 0);
  ptrdiff_t quote_type = read_stack_top ()->type;
  if (quote_type < RE_quoted_max)
    --qcounts[quote_type];
  return &rdstack.stack[--rdstack.sp];
}

static inline bool
read_stack_empty_p (ptrdiff_t base_sp)
{
  return rdstack.sp <= base_sp;
}

NO_INLINE static void
grow_read_stack (void)
{
  struct read_stack *rs = &rdstack;
  eassert (rs->sp == rs->size);
  rs->stack = xpalloc (rs->stack, &rs->size, 1, -1, sizeof *rs->stack);
  eassert (rs->sp < rs->size);
}

static inline void
read_stack_push (struct read_stack_entry e, size_t *qcounts)
{
  if (rdstack.sp >= rdstack.size)
    grow_read_stack ();
  rdstack.stack[rdstack.sp++] = e;
  ptrdiff_t quote_type = read_stack_top ()->type;
  if (quote_type < RE_quoted_max)
    ++qcounts[quote_type];
}

static void
read_stack_reset (intmax_t sp)
{
  eassert (sp <= rdstack.sp);
  rdstack.sp = sp;
}

/* Read a Lisp object.  */
static Lisp_Object
read0 (Lisp_Object readcharfun, bool annotated)
{
  char stackbuf[64];
  char *read_buffer = stackbuf;
  ptrdiff_t read_buffer_size = sizeof stackbuf;
  char *heapbuf = NULL;
  size_t qcounts[RE_quoted_max] = {0}; /* quoted counts */

  specpdl_ref base_pdl = SPECPDL_INDEX ();
  ptrdiff_t base_sp = rdstack.sp;
  record_unwind_protect_intmax (read_stack_reset, base_sp);
  specpdl_ref count = SPECPDL_INDEX ();

  EMACS_INT initial_charpos;
  bool uninterned_symbol;
  bool skip_shorthand;

  Lisp_Object obj;
  bool multibyte;

  /* Read an object into `obj'.  */
 read_obj: ;
  initial_charpos = readchar_charpos;
  int c = READCHAR_REPORT_MULTIBYTE (&multibyte);
  if (c < 0)
    end_of_file_error ();

  switch (c)
    {
    case '(':
      read_stack_push ((struct read_stack_entry) {.type = RE_list_start},
		       qcounts);
      goto read_obj;

    case ')':
      if (read_stack_empty_p (base_sp))
	invalid_syntax (")", readcharfun);
      switch (read_stack_top ()->type)
	{
	case RE_list_start:
	  read_stack_pop (qcounts);
	  obj = ANNOTATE (Qnil);
	  break;
	case RE_list:
	  obj = read_stack_pop (qcounts)->u.list.head;
	  break;
	case RE_record:
	  {
	    Lisp_Object elems = Fnreverse (read_stack_pop (qcounts)->u.vector.elems);
	    if (NILP (elems))
	      invalid_syntax ("#s", readcharfun);

	    if (EQ (XCAR (elems), Qhash_table))
	      obj = ANNOTATE (hash_table_from_plist (XCDR (elems)));
	    else
	      obj = ANNOTATE (record_from_list (elems));
	    break;
	  }
	case RE_string_props:
	  obj = string_props_from_rev_list (read_stack_pop (qcounts)->u.vector.elems,
					    readcharfun);
	  obj = ANNOTATE (obj);
	  break;
	default:
	  invalid_syntax (")", readcharfun);
	  break;
	}
      break;

    case '[':
      read_stack_push ((struct read_stack_entry) {
	  .type = RE_vector,
	  .u.vector.elems = Qnil,
	}, qcounts);
      goto read_obj;

    case ']':
      if (read_stack_empty_p (base_sp))
	invalid_syntax ("]", readcharfun);
      switch (read_stack_top ()->type)
	{
	case RE_vector:
	  obj = vector_from_rev_list (read_stack_pop (qcounts)->u.vector.elems);
	  break;
	case RE_byte_code:
	  obj = bytecode_from_rev_list (read_stack_pop (qcounts)->u.vector.elems,
					readcharfun);
	  break;
	case RE_char_table:
	  obj = char_table_from_rev_list (read_stack_pop (qcounts)->u.vector.elems,
					  readcharfun);
	  break;
	case RE_sub_char_table:
	  obj = sub_char_table_from_rev_list (read_stack_pop (qcounts)->u.vector.elems,
					      readcharfun);
	  break;
	default:
	  invalid_syntax ("]", readcharfun);
	  break;
	}
      obj = ANNOTATE (obj);
      break;

    case '#':
      {
	int ch = READCHAR;
	switch (ch)
	  {
	  case '\'':
	    /* #'X -- special syntax for (function X) */
	    read_stack_push ((struct read_stack_entry) {
		.type = RE_special,
		.u.special.symbol = Qfunction,
	      }, qcounts);
	    goto read_obj;

	  case '#':
	    /* ## -- the empty symbol */
	    obj = ANNOTATE (Fintern (empty_unibyte_string, Qnil));
	    break;

	  case 's':
	    /* #s(...) -- a record or hash-table */
	    ch = READCHAR;
	    if (ch != '(')
	      {
		UNREAD (ch);
		invalid_syntax ("#s", readcharfun);
	      }
	    read_stack_push ((struct read_stack_entry) {
		.type = RE_record,
		.u.vector.elems = Qnil,
	      }, qcounts);
	    goto read_obj;

	  case '^':
	    /* #^[...]  -- char-table
	       #^^[...] -- sub-char-table */
	    ch = READCHAR;
	    if (ch == '^')
	      {
		ch = READCHAR;
		if (ch == '[')
		  {
		    read_stack_push ((struct read_stack_entry) {
			.type = RE_sub_char_table,
			.u.vector.elems = Qnil,
		      }, qcounts);
		    goto read_obj;
		  }
		else
		  {
		    UNREAD (ch);
		    invalid_syntax ("#^^", readcharfun);
		  }
	      }
	    else if (ch == '[')
	      {
		read_stack_push ((struct read_stack_entry) {
		    .type = RE_char_table,
		    .u.vector.elems = Qnil,
		  }, qcounts);
		goto read_obj;
	      }
	    else
	      {
		UNREAD (ch);
		invalid_syntax ("#^", readcharfun);
	      }

	  case '(':
	    /* #(...) -- string with properties */
	    read_stack_push ((struct read_stack_entry) {
		.type = RE_string_props,
		.u.vector.elems = Qnil,
	      }, qcounts);
	    goto read_obj;

	  case '[':
	    /* #[...] -- byte-code */
	    read_stack_push ((struct read_stack_entry) {
		.type = RE_byte_code,
		.u.vector.elems = Qnil,
	      }, qcounts);
	    goto read_obj;

	  case '&':
	    /* #&N"..." -- bool-vector */
	    obj = ANNOTATE (read_bool_vector (readcharfun));
	    break;

	  case '!':
	    /* #! appears at the beginning of an executable file.
	       Skip the rest of the line.  */
	    {
	      int c;
	      do
		c = READCHAR;
	      while (c >= 0 && c != '\n');
	      goto read_obj;
	    }

	  case 'x':
	  case 'X':
	    obj = ANNOTATE (read_integer (readcharfun, 16));
	    break;

	  case 'o':
	  case 'O':
	    obj = ANNOTATE (read_integer (readcharfun, 8));
	    break;

	  case 'b':
	  case 'B':
	    obj = ANNOTATE (read_integer (readcharfun, 2));
	    break;

	  case '@':
	    /* #@NUMBER is used to skip NUMBER following bytes.
	       That's used in .elc files to skip over doc strings
	       and function definitions that can be loaded lazily.  */
	    if (skip_lazy_string (readcharfun))
	      goto read_obj;
	    obj = Qnil;	      /* #@00 skips to EOB/EOF and yields nil.  */
	    break;

	  case '$':
	    /* #$ -- reference to lazy-loaded string */
	    obj = ANNOTATE (Vload_file_name);
	    break;

	  case ':':
	    /* #:X -- uninterned symbol */
	    c = READCHAR;
	    if (c <= 32 || c == NO_BREAK_SPACE
		|| c == '"' || c == '\'' || c == ';' || c == '#'
		|| c == '(' || c == ')'  || c == '[' || c == ']'
		|| c == '`' || c == ',')
	      {
		/* No symbol character follows: this is the empty symbol.  */
		UNREAD (c);
		obj = ANNOTATE (Fmake_symbol (empty_unibyte_string));
		break;
	      }
	    uninterned_symbol = true;
	    skip_shorthand = false;
	    goto read_symbol;

	  case '_':
	    /* #_X -- symbol without shorthand */
	    c = READCHAR;
	    if (c <= 32 || c == NO_BREAK_SPACE
		|| c == '"' || c == '\'' || c == ';' || c == '#'
		|| c == '(' || c == ')'  || c == '[' || c == ']'
		|| c == '`' || c == ',')
	      {
		/* No symbol character follows: this is the empty symbol.  */
		UNREAD (c);
		obj = ANNOTATE (Fintern (empty_unibyte_string, Qnil));
		break;
	      }
	    uninterned_symbol = false;
	    skip_shorthand = true;
	    goto read_symbol;

	  default:
	    if (ch >= '0' && ch <= '9')
	      {
		/* #N=OBJ or #N# -- first read the number N */
		EMACS_INT n = ch - '0';
		int c;
		for (;;)
		  {
		    c = READCHAR;
		    if (c < '0' || c > '9')
		      break;
		    if (ckd_mul (&n, n, 10)
			|| ckd_add (&n, n, c - '0'))
		      invalid_syntax ("#", readcharfun);
		  }
		if (c == 'r' || c == 'R')
		  {
		    /* #NrDIGITS -- radix-N number */
		    if (n < 0 || n > 36)
		      invalid_radix_integer (n, readcharfun);
		    obj = ANNOTATE (read_integer (readcharfun, n));
		    break;
		  }
		else if (n <= MOST_POSITIVE_FIXNUM && !NILP (Vread_circle))
		  {
		    if (c == '=')
		      {
			/* #N=OBJ -- assign number N to OBJ */
			Lisp_Object placeholder = Fcons (Qnil, Qnil);

			struct Lisp_Hash_Table *h
			  = XHASH_TABLE (read_objects_map);
			Lisp_Object number = make_fixnum (n);
			hash_hash_t hash;
			ptrdiff_t i = hash_lookup_get_hash (h, number, &hash);
			if (i >= 0)
			  /* Not normal, but input could be malformed.  */
			  set_hash_value_slot (h, i, placeholder);
			else
			  hash_put (h, number, placeholder, hash);
			read_stack_push ((struct read_stack_entry) {
			    .type = RE_numbered,
			    .u.numbered.number = number,
			    .u.numbered.placeholder = placeholder,
			  }, qcounts);
			goto read_obj;
		      }
		    else if (c == '#')
		      {
			/* #N# -- reference to numbered object */
			struct Lisp_Hash_Table *h
			  = XHASH_TABLE (read_objects_map);
			ptrdiff_t i = hash_lookup (h, make_fixnum (n));
			if (i < 0)
			  invalid_syntax ("#", readcharfun);
			obj = ANNOTATE (HASH_VALUE (h, i));
			break;
		      }
		    else
		      invalid_syntax ("#", readcharfun);
		  }
		else
		  invalid_syntax ("#", readcharfun);
	      }
	    else
	      invalid_syntax ("#", readcharfun);
	    break;
	  }
	break;
      }

    case '?':
      obj = ANNOTATE (read_char_literal (readcharfun));
      break;

    case '"':
      obj = ANNOTATE (read_string_literal (readcharfun));
      break;

    case '\'':
      read_stack_push ((struct read_stack_entry) {
	  .type = RE_special,
	  .u.special.symbol = Qquote,
	}, qcounts);
      goto read_obj;

    case '`':
      read_stack_push ((struct read_stack_entry) {
	  .type = RE_special,
	  .u.special.symbol = Qbackquote,
	}, qcounts);
      goto read_obj;

    case ',':
      {
	int ch = READCHAR;
	Lisp_Object sym;
	if (ch == '@')
	  sym = Qcomma_at;
	else
	  {
	    if (ch >= 0)
	      UNREAD (ch);
	    sym = Qcomma;
	  }
	read_stack_push ((struct read_stack_entry) {
	    .type = RE_special,
	    .u.special.symbol = sym,
	  }, qcounts);
	goto read_obj;
      }

    case ';':
      {
	int c;
	do
	  c = READCHAR;
	while (c >= 0 && c != '\n');
	goto read_obj;
      }

    case '.':
      {
	int nch = READCHAR;
	UNREAD (nch);
	if (nch <= 32 || nch == NO_BREAK_SPACE
	    || nch == '"' || nch == '\'' || nch == ';'
	    || nch == '(' || nch == '[' || nch == '#'
	    || nch == '?' || nch == '`' || nch == ',')
	  {
	    if (!read_stack_empty_p (base_sp)
		&& read_stack_top ()->type ==  RE_list)
	      {
		read_stack_top ()->type = RE_list_dot;
		goto read_obj;
	      }
	    invalid_syntax (".", readcharfun);
	  }
      }
      /* may be a number or symbol starting with a dot */
      FALLTHROUGH;

    default:
      if (c <= 32 || c == NO_BREAK_SPACE)
	goto read_obj;

      uninterned_symbol = false;
      skip_shorthand = false;
      /* symbol or number */
    read_symbol:
      {
	char *p = read_buffer;
	char *end = read_buffer + read_buffer_size;
	bool quoted = false;
	ptrdiff_t nchars = 0;
	Lisp_Object result = Qnil;

	do
	  {
	    if (end - p < MAX_MULTIBYTE_LENGTH + 1)
	      {
		ptrdiff_t offset = p - read_buffer;
		read_buffer = grow_read_buffer (read_buffer, offset,
						&heapbuf, &read_buffer_size,
						count);
		p = read_buffer + offset;
		end = read_buffer + read_buffer_size;
	      }

	    if (c == '\\')
	      {
		c = READCHAR;
		if (c < 0)
		  end_of_file_error ();
		quoted = true;
	      }

	    if (multibyte)
	      p += CHAR_STRING (c, (unsigned char *) p);
	    else
	      *p++ = c;
	    c = READCHAR;
	  }
	while (c > 32
	       && c != NO_BREAK_SPACE
	       && (c >= 128
		   || !(   c == '"' || c == '\'' || c == ';' || c == '#'
			|| c == '(' || c == ')'  || c == '[' || c == ']'
			|| c == '`' || c == ',')));

	*p = 0;
	ptrdiff_t nbytes = p - read_buffer;
	UNREAD (c);

	/* Only attempt to parse the token as a number if it starts as one.  */
	char c0 = read_buffer[0];
	if (((c0 >= '0' && c0 <= '9') || c0 == '.' || c0 == '-' || c0 == '+')
	    && !quoted && !uninterned_symbol && !skip_shorthand)
	  {
	    ptrdiff_t len;
	    result = string_to_number (read_buffer, 10, &len);
	    if (!NILP (result) && len == nbytes)
	      {
		obj = ANNOTATE (result);
		break;
	      }
	  }

	/* symbol, possibly uninterned */
	nchars = (multibyte
		  ? multibyte_chars_in_text ((unsigned char *)read_buffer, nbytes)
		  : nbytes);
	if (uninterned_symbol)
	  {
	    Lisp_Object name = !NILP (Vpdumper__pure_pool)
	      ? make_pure_string (read_buffer, nchars, nbytes, multibyte)
	      : make_specified_string (read_buffer, nbytes, multibyte);
	    result = Fmake_symbol (name);
	  }
	else
	  {
	    /* Intern NAME if not already registered with Vobarray.
	       Then assign RESULT to the interned symbol.  */
	    Lisp_Object found;
	    Lisp_Object obarray = check_obarray (Vobarray);
	    char *longhand = NULL;
	    ptrdiff_t longhand_chars = 0, longhand_bytes = 0;

	    if (skip_shorthand
		/* Symbols composed entirely of "symbol constituents"
		   are exempt from shorthands.  */
		|| symbol_char_span (read_buffer) >= nbytes)
	      found = oblookup (obarray, read_buffer, nchars, nbytes);
	    else
	      found = oblookup_considering_shorthand (obarray, read_buffer,
						      nchars, nbytes, &longhand,
						      &longhand_chars,
						      &longhand_bytes);
	    if (SYMBOLP (found))
	      result = found;
	    else if (longhand)
	      {
		Lisp_Object name
		  = (multibyte
		     ? make_multibyte_string (longhand, longhand_chars, longhand_bytes)
		     : make_unibyte_string (longhand, longhand_bytes));
		xfree (longhand);
		result = intern_driver (name, Vobarray, found);
	      }
	    else
	      {
		Lisp_Object name
		  = (multibyte
		     ? make_multibyte_string (read_buffer, nchars, nbytes)
		     : make_unibyte_string (read_buffer, nbytes));
		result = intern_driver (name, Vobarray, found);
	      }
	  }

	obj = ANNOTATE (result);
	break;
      }
    }

  /* Now figure what to do with OBJ.  */
  while (rdstack.sp > base_sp)
    {
      struct read_stack_entry *e = read_stack_top ();
      switch (e->type)
	{
	case RE_list_start:
	  e->type = RE_list;
	  e->u.list.head = e->u.list.tail = Fcons (obj, Qnil);
	  goto read_obj;

	case RE_list:
	  {
	    Lisp_Object tl = Fcons (obj, Qnil);
	    XSETCDR (e->u.list.tail, tl);
	    e->u.list.tail = tl;
	    goto read_obj;
	  }

	case RE_list_dot:
	  {
	    skip_space_and_comments (readcharfun);
	    int ch = READCHAR;
	    if (ch != ')')
	      invalid_syntax ("expected )", readcharfun);
	    XSETCDR (e->u.list.tail, obj);
	    read_stack_pop (qcounts);
	    obj = e->u.list.head;

	    /* Hack: immediately convert (#$ . FIXNUM) to the corresponding
	       string if load-force-doc-strings is set.  */
	    if (load_force_doc_strings
		&& EQ (XCAR (obj), Vload_file_name)
		&& !NILP (XCAR (obj))
		&& FIXNUMP (XCDR (obj)))
	      obj = get_lazy_string (obj);

	    break;
	  }

	case RE_vector:
	case RE_record:
	case RE_char_table:
	case RE_sub_char_table:
	case RE_byte_code:
	case RE_string_props:
	  e->u.vector.elems = Fcons (obj, e->u.vector.elems);
	  goto read_obj;

	case RE_special:
	  read_stack_pop (qcounts);
	  obj = ANNOTATE (list2 (e->u.special.symbol, obj));
	  break;

	case RE_numbered:
	  {
	    read_stack_pop (qcounts);
	    Lisp_Object placeholder = e->u.numbered.placeholder;
	    if (CONSP (obj))
	      {
		if (EQ (obj, placeholder))
		  /* Catch silly games like #1=#1# */
		  invalid_syntax ("nonsensical self-reference", readcharfun);

		/* Optimization: since the placeholder is already
		   a cons, repurpose it as the actual value.
		   This allows us to skip the substitution below,
		   since the placeholder is already referenced
		   inside OBJ at the appropriate places.  */
		Fsetcar (placeholder, XCAR (obj));
		Fsetcdr (placeholder, XCDR (obj));

		struct Lisp_Hash_Table *h2
		  = XHASH_TABLE (read_objects_completed);
		hash_hash_t hash;
		ptrdiff_t i = hash_lookup_get_hash (h2, placeholder, &hash);
		eassert (i < 0);
		hash_put (h2, placeholder, Qnil, hash);
		obj = placeholder;
	      }
	    else
	      {
		/* If it can be recursive, remember it for future
		   substitutions.  */
		if (!SYMBOLP (obj) && !NUMBERP (obj)
		    && !(STRINGP (obj) && !string_intervals (obj)))
		  {
		    struct Lisp_Hash_Table *h2
		      = XHASH_TABLE (read_objects_completed);
		    hash_hash_t hash;
		    ptrdiff_t i = hash_lookup_get_hash (h2, obj, &hash);
		    eassert (i < 0);
		    hash_put (h2, obj, Qnil, hash);
		  }

		/* Now put it everywhere the placeholder was...  */
		Flread__substitute_object_in_subtree (obj, placeholder,
						      read_objects_completed);

		/* ...and #n# will use the real value from now on.  */
		struct Lisp_Hash_Table *h = XHASH_TABLE (read_objects_map);
		hash_hash_t hash;
		ptrdiff_t i = hash_lookup_get_hash (h, e->u.numbered.number,
						    &hash);
		eassert (i >= 0);
		set_hash_value_slot (h, i, obj);
	      }
	    break;
	  }
	default:
	  emacs_abort ();
	  break;
	}
    }

  return unbind_to (base_pdl, obj);
}

DEFUN ("lread--substitute-object-in-subtree",
       Flread__substitute_object_in_subtree,
       Slread__substitute_object_in_subtree, 3, 3, 0,
       doc: /* In OBJECT, replace every occurrence of PLACEHOLDER with OBJECT.
COMPLETED is a hash table of objects that might be circular, or is t
if any object might be circular.  */)
  (Lisp_Object object, Lisp_Object placeholder, Lisp_Object completed)
{
  struct subst subst = { object, placeholder, completed, Qnil };
  Lisp_Object check_object = substitute_object_recurse (&subst, object);

  /* The returned object here is expected to always eq the
     original.  */
  if (!EQ (check_object, object))
    error ("Unexpected mutation error in reader");
  return Qnil;
}

static Lisp_Object
substitute_object_recurse (struct subst *subst, Lisp_Object subtree)
{
  /* If we find the placeholder, return the target object.  */
  if (EQ (subst->placeholder, subtree))
    return subst->object;

  /* For common object types that can't contain other objects, don't
     bother looking them up; we're done.  */
  if (SYMBOLP (subtree)
      || (STRINGP (subtree) && !string_intervals (subtree))
      || NUMBERP (subtree))
    return subtree;

  /* If we've been to this node before, don't explore it again.  */
  if (!NILP (Fmemq (subtree, subst->seen)))
    return subtree;

  /* If this node can be the entry point to a cycle, remember that
     we've seen it.  It can only be such an entry point if it was made
     by #n=, which means that we can find it as a value in
     COMPLETED.  */
  if (EQ (subst->completed, Qt)
      || hash_lookup (XHASH_TABLE (subst->completed), subtree) >= 0)
    subst->seen = Fcons (subtree, subst->seen);

  /* Recurse according to subtree's type.
     Every branch must return a Lisp_Object.  */
  switch (XTYPE (subtree))
    {
    case Lisp_Vectorlike:
      {
	ptrdiff_t i = 0, length = 0;
	if (BOOL_VECTOR_P (subtree))
	  return subtree;		/* No sub-objects anyway.  */
	else if (CHAR_TABLE_P (subtree) || SUB_CHAR_TABLE_P (subtree)
		 || CLOSUREP (subtree) || HASH_TABLE_P (subtree)
		 || RECORDP (subtree) || VECTORP (subtree))
	  length = PVSIZE (subtree);
	else
	  /* An unknown pseudovector may contain non-Lisp fields, so we
	     can't just blindly traverse all its fields.  We used to call
	     `Flength' which signaled `sequencep', so I just preserved this
	     behavior.  */
	  wrong_type_argument (Qsequencep, subtree);

	if (SUB_CHAR_TABLE_P (subtree))
	  i = 2;
	for ( ; i < length; i++)
	  ASET (subtree, i,
		substitute_object_recurse (subst, AREF (subtree, i)));
	return subtree;
      }

    case Lisp_Cons:
      XSETCAR (subtree, substitute_object_recurse (subst, XCAR (subtree)));
      XSETCDR (subtree, substitute_object_recurse (subst, XCDR (subtree)));
      return subtree;

    case Lisp_String:
      {
	/* Check for text properties in each interval.
	   substitute_in_interval contains part of the logic.  */

	INTERVAL root_interval = string_intervals (subtree);
	traverse_intervals_noorder (&root_interval,
				    substitute_in_interval, subst);
	return subtree;
      }

      /* Other types don't recurse any further.  */
    default:
      return subtree;
    }
}

/*  Helper function for substitute_object_recurse.  */
static void
substitute_in_interval (INTERVAL *interval, void *arg)
{
  set_interval_plist (*interval,
		      substitute_object_recurse (arg, (*interval)->plist));
}


/* Convert the initial prefix of STRING to a number, assuming base BASE.
   If the prefix has floating point syntax and BASE is 10, return a
   nearest float; otherwise, if the prefix has integer syntax, return
   the integer; otherwise, return nil.  (On antique platforms that lack
   support for NaNs, if the prefix has NaN syntax return a Lisp object that
   will provoke an error if used as a number.)  If PLEN, set *PLEN to the
   length of the numeric prefix if there is one, otherwise *PLEN is
   unspecified.  */

Lisp_Object
string_to_number (char const *string, int base, ptrdiff_t *plen)
{
  char const *cp = string;
  bool float_syntax = false;
  double value = 0;

  /* Negate the value ourselves.  This treats 0, NaNs, and infinity properly on
     IEEE floating point hosts, and works around a formerly-common bug where
     atof ("-0.0") drops the sign.  */
  bool negative = *cp == '-';
  bool positive = *cp == '+';

  bool signedp = negative | positive;
  cp += signedp;

  enum { INTOVERFLOW = 1, LEAD_INT = 2, TRAIL_INT = 4, E_EXP = 16 };
  int state = 0;
  int leading_digit = digit_to_number (*cp, base);
  uintmax_t n = leading_digit;
  if (leading_digit >= 0)
    {
      state |= LEAD_INT;
      for (int digit; 0 <= (digit = digit_to_number (*++cp, base)); )
	{
	  if (INT_MULTIPLY_OVERFLOW (n, base))
	    state |= INTOVERFLOW;
	  n *= base;
	  if (INT_ADD_OVERFLOW (n, digit))
	    state |= INTOVERFLOW;
	  n += digit;
	}
    }
  char const *after_digits = cp;
  if (*cp == '.')
    {
      cp++;
    }

  if (base == 10)
    {
      if ('0' <= *cp && *cp <= '9')
	{
	  state |= TRAIL_INT;
	  do
	    cp++;
	  while ('0' <= *cp && *cp <= '9');
	}
      if (*cp == 'e' || *cp == 'E')
	{
	  char const *ecp = cp;
	  cp++;
	  if (*cp == '+' || *cp == '-')
	    cp++;
	  if ('0' <= *cp && *cp <= '9')
	    {
	      state |= E_EXP;
	      do
		cp++;
	      while ('0' <= *cp && *cp <= '9');
	    }
	  else if (cp[-1] == '+'
		   && cp[0] == 'I' && cp[1] == 'N' && cp[2] == 'F')
	    {
	      state |= E_EXP;
	      cp += 3;
	      value = INFINITY;
	    }
	  else if (cp[-1] == '+'
		   && cp[0] == 'N' && cp[1] == 'a' && cp[2] == 'N')
	    {
	      state |= E_EXP;
	      cp += 3;
#if IEEE_FLOATING_POINT
	      union ieee754_double u
		= { .ieee_nan = { .exponent = 0x7ff, .quiet_nan = 1,
				  .mantissa0 = n >> 31 >> 1, .mantissa1 = n }};
	      value = u.d;
#else
	      if (plen)
		*plen = cp - string;
	      return not_a_number[negative];
#endif
	    }
	  else
	    cp = ecp;
	}

      /* A float has digits after the dot or an exponent.
	 This excludes numbers like "1." which are lexed as integers. */
      float_syntax = ((state & TRAIL_INT)
		      || ((state & LEAD_INT) && (state & E_EXP)));
    }

  if (plen)
    *plen = cp - string;

  /* Return a float if the number uses float syntax.  */
  if (float_syntax)
    {
      /* Convert to floating point, unless the value is already known
	 because it is infinite or a NaN.  */
      if (!value)
	value = atof (string + signedp);
      return make_float (negative ? -value : value);
    }

  /* Return nil if the number uses invalid syntax.  */
  if (!(state & LEAD_INT))
    return Qnil;

  /* Fast path if the integer (san sign) fits in uintmax_t.  */
  if (!(state & INTOVERFLOW))
    {
      if (!negative)
	return make_uint (n);
      if (-MOST_NEGATIVE_FIXNUM < n)
	return make_neg_biguint (n);
      EMACS_INT signed_n = n;
      return make_fixnum (-signed_n);
    }

  /* Trim any leading "+" and trailing nondigits, then return a bignum.  */
  string += positive;
  if (!*after_digits)
    return make_bignum_str (string, base);
  ptrdiff_t trimmed_len = after_digits - string;
  USE_SAFE_ALLOCA;
  char *trimmed = SAFE_ALLOCA (trimmed_len + 1);
  memcpy (trimmed, string, trimmed_len);
  trimmed[trimmed_len] = '\0';
  Lisp_Object result = make_bignum_str (trimmed, base);
  SAFE_FREE ();
  return result;
}

static Lisp_Object initial_obarray;

/* `oblookup' stores the bucket number here, for the sake of Funintern.  */

static size_t oblookup_last_bucket_number;

/* Slow path obarray check: return the obarray to use or signal an error.  */
Lisp_Object
check_obarray_slow (Lisp_Object obarray)
{
  /* For compatibility, we accept vectors whose first element is 0,
     and store an obarray object there.  */
  if (VECTORP (obarray) && ASIZE (obarray) > 0)
    {
      Lisp_Object obj = AREF (obarray, 0);
      if (OBARRAYP (obj))
	return obj;
      if (EQ (obj, make_fixnum (0)))
	{
	  /* Put an actual obarray object in the first slot.
	     The rest of the vector remains unused.  */
	  obj = make_obarray (0);
	  ASET (obarray, 0, obj);
	  return obj;
	}
    }
  /* Reset Vobarray to the standard obarray for nicer error handling. */
  if (EQ (Vobarray, obarray)) Vobarray = initial_obarray;

  wrong_type_argument (Qobarrayp, obarray);
}

static void grow_obarray (struct Lisp_Obarray *o);

/* Intern symbol SYM in OBARRAY using bucket INDEX.  */

/* FIXME: retype arguments as pure C types */
static Lisp_Object
intern_sym (Lisp_Object sym, Lisp_Object obarray, Lisp_Object index)
{
  Lisp_Object *ptr;
  XSYMBOL (sym)->u.s.interned = (EQ (obarray, initial_obarray)
				 ? SYMBOL_INTERNED_IN_INITIAL_OBARRAY
				 : SYMBOL_INTERNED);

  if (SREF (SYMBOL_NAME (sym), 0) == ':' && EQ (obarray, initial_obarray))
    {
      make_symbol_constant (sym);
      XSYMBOL (sym)->u.s.type = SYMBOL_PLAINVAL;
      /* Mark keywords as special.  This makes (let ((:key 'foo)) ...)
	 in lexically bound elisp signal an error, as documented.  */
      XSYMBOL (sym)->u.s.declared_special = true;
      SET_SYMBOL_VAL (XSYMBOL (sym), sym);
    }

  struct Lisp_Obarray *o = XOBARRAY (obarray);
  ptr = o->buckets + XFIXNUM (index);
  set_symbol_next (sym, SYMBOLP (*ptr) ? XSYMBOL (*ptr) : NULL);
  *ptr = sym;
  o->count++;
  if (o->count > obarray_size (o))
    grow_obarray (o);
  return sym;
}

/* Intern a symbol with name STRING in OBARRAY using bucket INDEX.  */

Lisp_Object
intern_driver (Lisp_Object string, Lisp_Object obarray, Lisp_Object index)
{
  SET_SYMBOL_VAL (XSYMBOL (Qobarray_cache), Qnil);
  return intern_sym (Fmake_symbol (string), obarray, index);
}

/* Intern the C string STR: return a symbol with that name,
   interned in the current obarray.  */

Lisp_Object
intern (const char *str)
{
  const ptrdiff_t len = strlen (str);
  Lisp_Object obarray = check_obarray (Vobarray);
  Lisp_Object tem = oblookup (obarray, str, len, len);

  return (SYMBOLP (tem) ? tem
	  /* The above `oblookup' was done on the basis of nchars==nbytes, so
	     the string has to be unibyte.  */
	  : intern_driver (make_unibyte_string (str, len),
			   obarray, tem));
}

Lisp_Object
intern_c_string (const char *str)
{
  const ptrdiff_t len = strlen (str);
  Lisp_Object obarray = check_obarray (Vobarray);
  Lisp_Object val = oblookup (obarray, str, len, len);
  return SYMBOLP (val)
    ? val
    : intern_driver (!NILP (Vpdumper__pure_pool)
		     ? make_pure_c_string (str, len)
		     : make_string (str, len),
		     obarray, val);
}

/* Intern STR of NBYTES bytes and NCHARS characters in the default obarray.  */
Lisp_Object
intern_c_multibyte (const char *str, ptrdiff_t nchars, ptrdiff_t nbytes)
{
  Lisp_Object obarray = check_obarray (Vobarray);
  Lisp_Object sym = oblookup (obarray, str, nchars, nbytes);
  return SYMBOLP (sym)
    ? sym
    : intern_driver (make_multibyte_string (str, nchars, nbytes),
		     obarray, sym);
}

static void
define_symbol (Lisp_Object sym, char const *str)
{
  ptrdiff_t len = strlen (str);
  Lisp_Object string = make_pure_c_string (str, len);
  init_symbol (sym, string);

  /* Qunbound is uninterned, thus distinct from the symbol 'unbound.  */
  if (!EQ (sym, Qunbound))
    {
      Lisp_Object bucket = oblookup (initial_obarray, str, len, len);
      eassert (FIXNUMP (bucket));
      intern_sym (sym, initial_obarray, bucket);
    }
}

DEFUN ("intern", Fintern, Sintern, 1, 2, 0,
       doc: /* Return the canonical symbol whose name is STRING.
If there is none, one is created by this function and returned.
A second optional argument specifies the obarray to use;
it defaults to the value of `obarray'.  */)
  (Lisp_Object string, Lisp_Object obarray)
{
  Lisp_Object tem;

  obarray = check_obarray (NILP (obarray) ? Vobarray : obarray);
  CHECK_STRING (string);

  char* longhand = NULL;
  ptrdiff_t longhand_chars = 0, longhand_bytes = 0;
  tem = oblookup_considering_shorthand (obarray, SSDATA (string),
					SCHARS (string), SBYTES (string),
					&longhand, &longhand_chars,
					&longhand_bytes);

  if (!SYMBOLP (tem))
    {
      if (longhand)
	{
	  eassert (longhand_chars >= 0);
	  tem = intern_driver (make_multibyte_string
			       (longhand, longhand_chars, longhand_bytes),
			       obarray, tem);
	  xfree (longhand);
	}
      else
	tem = intern_driver (Fpurecopy_maybe (string), obarray, tem);
    }
  return tem;
}

DEFUN ("intern-soft", Fintern_soft, Sintern_soft, 1, 2, 0,
       doc: /* Return the canonical symbol named NAME, or nil if none exists.
NAME may be a string or a symbol.  If it is a symbol, that exact
symbol is searched for.
A second optional argument specifies the obarray to use;
it defaults to the value of `obarray'.  */)
  (Lisp_Object name, Lisp_Object obarray)
{
  register Lisp_Object tem, string;

  if (NILP (obarray)) obarray = Vobarray;
  obarray = check_obarray (obarray);

  if (!SYMBOLP (name))
    {
      char *longhand = NULL;
      ptrdiff_t longhand_chars = 0, longhand_bytes = 0;

      CHECK_STRING (name);
      string = name;
      tem = oblookup_considering_shorthand (obarray, SSDATA (string),
					    SCHARS (string), SBYTES (string),
					    &longhand, &longhand_chars,
					    &longhand_bytes);
      if (longhand)
	xfree (longhand);
      return FIXNUMP (tem) ? Qnil : tem;
    }
  else
    {
      /* If already a symbol, we don't do shorthand-longhand translation,
	 as promised in the docstring.  */
      string = SYMBOL_NAME (name);
      tem = oblookup (obarray, SSDATA (string), SCHARS (string), SBYTES (string));
      return EQ (name, tem) ? name : Qnil;
    }
}

DEFUN ("unintern", Funintern, Sunintern, 1, 2, 0,
       doc: /* Delete the symbol named NAME, if any, from OBARRAY.
The value is t if a symbol was found and deleted, nil otherwise.
NAME may be a string or a symbol.  If it is a symbol, that symbol
is deleted, if it belongs to OBARRAY--no other symbol is deleted.
OBARRAY, if nil, defaults to the value of the variable `obarray'.
usage: (unintern NAME OBARRAY)  */)
  (Lisp_Object name, Lisp_Object obarray)
{
  register Lisp_Object tem;
  Lisp_Object string;

  if (NILP (obarray)) obarray = Vobarray;
  obarray = check_obarray (obarray);

  if (SYMBOLP (name))
    string = SYMBOL_NAME (name);
  else
    {
      CHECK_STRING (name);
      string = name;
    }

  char *longhand = NULL;
  ptrdiff_t longhand_chars = 0;
  ptrdiff_t longhand_bytes = 0;
  tem = oblookup_considering_shorthand (obarray, SSDATA (string),
					SCHARS (string), SBYTES (string),
					&longhand, &longhand_chars,
					&longhand_bytes);
  if (longhand)
    xfree(longhand);

  if (FIXNUMP (tem))
    return Qnil;
  /* If arg was a symbol, don't delete anything but that symbol itself.  */
  if (SYMBOLP (name) && !EQ (name, tem))
    return Qnil;

  /* There are plenty of other symbols which will screw up the Emacs
     session if we unintern them, as well as even more ways to use
     `setq' or `fset' or whatnot to make the Emacs session
     unusable.  Let's not go down this silly road.  --Stef  */
  /* if (NILP (tem) || EQ (tem, Qt))
       error ("Attempt to unintern t or nil"); */

  struct Lisp_Symbol *sym = XSYMBOL (tem);
  sym->u.s.interned = SYMBOL_UNINTERNED;

  ptrdiff_t idx = oblookup_last_bucket_number;
  Lisp_Object *loc = &XOBARRAY (obarray)->buckets[idx];

  struct Lisp_Symbol *prev = XSYMBOL (*loc);
  if (sym == prev)
    *loc = sym->u.s.next ? make_lisp_ptr (sym->u.s.next, Lisp_Symbol) : make_fixnum (0);
  else
    while (1)
      {
	struct Lisp_Symbol *next = prev->u.s.next;
	if (next == sym)
	  {
	    prev->u.s.next = next->u.s.next;
	    break;
	  }
	prev = next;
      }

  XOBARRAY (obarray)->count--;

  return Qt;
}

/* Bucket index of the string STR of length SIZE_BYTE bytes in obarray OA.  */
static ptrdiff_t
obarray_index (struct Lisp_Obarray *oa, const char *str, ptrdiff_t size_byte)
{
  EMACS_UINT hash = hash_string (str, size_byte);
  return knuth_hash (reduce_emacs_uint_to_hash_hash (hash), oa->size_bits);
}

/* Return the symbol in OBARRAY whose names matches the string
   of SIZE characters (SIZE_BYTE bytes) at PTR.
   If there is no such symbol, return the integer bucket number of
   where the symbol would be if it were present.

   Also store the bucket number in oblookup_last_bucket_number.  */

Lisp_Object
oblookup (Lisp_Object obarray, register const char *ptr, ptrdiff_t size, ptrdiff_t size_byte)
{
  struct Lisp_Obarray *o = XOBARRAY (obarray);
  ptrdiff_t idx = obarray_index (o, ptr, size_byte);
  Lisp_Object bucket = o->buckets[idx];

  oblookup_last_bucket_number = idx;
  if (!EQ (bucket, make_fixnum (0)))
    {
      Lisp_Object sym = bucket;
      while (1)
	{
	  struct Lisp_Symbol *s = XSYMBOL (sym);
	  Lisp_Object name = s->u.s.name;
	  if (SBYTES (name) == size_byte && SCHARS (name) == size
	      && memcmp (SDATA (name), ptr, size_byte) == 0)
	    return sym;
	  if (s->u.s.next == NULL)
	    break;
	  sym = make_lisp_ptr (s->u.s.next, Lisp_Symbol);
	}
    }
  return make_fixnum (idx);
}

/* Like 'oblookup', but considers 'Vread_symbol_shorthands',
   potentially recognizing that IN is shorthand for some other
   longhand name, which is then placed in OUT.  In that case,
   memory is malloc'ed for OUT (which the caller must free) while
   SIZE_OUT and SIZE_BYTE_OUT respectively hold the character and byte
   sizes of the transformed symbol name.  If IN is not recognized
   shorthand for any other symbol, OUT is set to point to NULL and
   'oblookup' is called.  */

Lisp_Object
oblookup_considering_shorthand (Lisp_Object obarray, const char *in,
				ptrdiff_t size, ptrdiff_t size_byte, char **out,
				ptrdiff_t *size_out, ptrdiff_t *size_byte_out)
{
  Lisp_Object tail = Vread_symbol_shorthands;

  /* First, assume no transformation will take place.  */
  *out = NULL;
  /* Then, iterate each pair in Vread_symbol_shorthands.  */
  FOR_EACH_TAIL_SAFE (tail)
    {
      Lisp_Object pair = XCAR (tail);
      /* Be lenient to 'read-symbol-shorthands': if some element isn't a
	 cons, or some member of that cons isn't a string, just skip
	 to the next element.  */
      if (!CONSP (pair))
	continue;
      Lisp_Object sh_prefix = XCAR (pair);
      Lisp_Object lh_prefix = XCDR (pair);
      if (!STRINGP (sh_prefix) || !STRINGP (lh_prefix))
	continue;
      ptrdiff_t sh_prefix_size = SBYTES (sh_prefix);

      /* Compare the prefix of the transformation pair to the symbol
	 name.  If a match occurs, do the renaming and exit the loop.
	 In other words, only one such transformation may take place.
	 Calculate the amount of memory to allocate for the longhand
	 version of the symbol name with xrealloc.  This isn't
	 strictly needed, but it could later be used as a way for
	 multiple transformations on a single symbol name.  */
      if (sh_prefix_size <= size_byte
	  && memcmp (SSDATA (sh_prefix), in, sh_prefix_size) == 0)
	{
	  ptrdiff_t lh_prefix_size = SBYTES (lh_prefix);
	  ptrdiff_t suffix_size = size_byte - sh_prefix_size;
	  *out = xrealloc (*out, lh_prefix_size + suffix_size);
	  memcpy (*out, SSDATA(lh_prefix), lh_prefix_size);
	  memcpy (*out + lh_prefix_size, in + sh_prefix_size, suffix_size);
	  *size_out = SCHARS (lh_prefix) - SCHARS (sh_prefix) + size;
	  *size_byte_out = lh_prefix_size + suffix_size;
	  break;
	}
    }
  /* Now, as promised, call oblookup with the "final" symbol name to
     lookup.  That function remains oblivious to whether a
     transformation happened here or not, but the caller of this
     function can tell by inspecting the OUT parameter.  */
  if (*out)
    return oblookup (obarray, *out, *size_out, *size_byte_out);
  else
    return oblookup (obarray, in, size, size_byte);
}

static struct Lisp_Obarray *
allocate_obarray (void)
{
  return ALLOCATE_PLAIN_PSEUDOVECTOR (struct Lisp_Obarray, PVEC_OBARRAY);
}

Lisp_Object
make_obarray (unsigned bits)
{
  struct Lisp_Obarray *o = allocate_obarray ();
  o->count = 0;
  o->size_bits = bits;
  ptrdiff_t size = (ptrdiff_t)1 << bits;
  o->buckets = hash_table_alloc_bytes (size * sizeof *o->buckets);
  for (ptrdiff_t i = 0; i < size; i++)
    o->buckets[i] = make_fixnum (0);
  return make_lisp_obarray (o);
}

enum {
  obarray_default_bits = 3,
  word_size_log2 = word_size < 8 ? 5 : 6,  /* good enough */
  obarray_max_bits = min (8 * sizeof (int),
			  8 * sizeof (ptrdiff_t) - word_size_log2) - 1,
};

static void
grow_obarray (struct Lisp_Obarray *o)
{
  ptrdiff_t old_size = obarray_size (o);
  eassert (o->count > old_size);
  Lisp_Object *old_buckets = o->buckets;

  int new_bits = o->size_bits + 1;
  if (new_bits > obarray_max_bits)
    error ("Obarray too big");
  ptrdiff_t new_size = (ptrdiff_t)1 << new_bits;
  o->buckets = hash_table_alloc_bytes (new_size * sizeof *o->buckets);
  for (ptrdiff_t i = 0; i < new_size; i++)
    o->buckets[i] = make_fixnum (0);
  o->size_bits = new_bits;

  /* Rehash symbols.
     FIXME: this is expensive since we need to recompute the hash for every
     symbol name.  Would it be reasonable to store it in the symbol?  */
  for (ptrdiff_t i = 0; i < old_size; i++)
    {
      Lisp_Object obj = old_buckets[i];
      if (SYMBOLP (obj))
	{
	  struct Lisp_Symbol *s = XSYMBOL (obj);
	  while (1)
	    {
	      Lisp_Object name = s->u.s.name;
	      ptrdiff_t idx = obarray_index (o, SSDATA (name), SBYTES (name));
	      Lisp_Object *loc = o->buckets + idx;
	      struct Lisp_Symbol *next = s->u.s.next;
	      s->u.s.next = SYMBOLP (*loc) ? XSYMBOL (*loc) : NULL;
	      *loc = make_lisp_ptr (s, Lisp_Symbol);
	      if (next == NULL)
		break;
	      s = next;
	    }
	}
    }

  hash_table_free_bytes (old_buckets, old_size * sizeof *old_buckets);
}

DEFUN ("obarray-make", Fobarray_make, Sobarray_make, 0, 1, 0,
       doc: /* Return a new obarray of size SIZE.
The obarray will grow to accommodate any number of symbols; the size, if
given, is only a hint for the expected number.  */)
  (Lisp_Object size)
{
  int bits;
  if (NILP (size))
    bits = obarray_default_bits;
  else
    {
      CHECK_FIXNAT (size);
      EMACS_UINT n = XFIXNUM (size);
      bits = elogb (n) + 1;
      if (bits > obarray_max_bits)
	xsignal (Qargs_out_of_range, size);
    }
  return make_obarray (bits);
}

DEFUN ("obarrayp", Fobarrayp, Sobarrayp, 1, 1, 0,
       doc: /* Return t iff OBJECT is an obarray.  */)
  (Lisp_Object object)
{
  return OBARRAYP (object) ? Qt : Qnil;
}

DEFUN ("obarray-clear", Fobarray_clear, Sobarray_clear, 1, 1, 0,
       doc: /* Remove all symbols from OBARRAY.  */)
  (Lisp_Object obarray)
{
  CHECK_OBARRAY (obarray);
  struct Lisp_Obarray *o = XOBARRAY (obarray);

  /* This function does not bother setting the status of its contained symbols
     to uninterned.  It doesn't matter very much.  */
  int new_bits = obarray_default_bits;
  int new_size = (ptrdiff_t)1 << new_bits;
  Lisp_Object *new_buckets
    = hash_table_alloc_bytes (new_size * sizeof *new_buckets);
  for (ptrdiff_t i = 0; i < new_size; i++)
    new_buckets[i] = make_fixnum (0);

  int old_size = obarray_size (o);
  hash_table_free_bytes (o->buckets, old_size * sizeof *o->buckets);
  o->buckets = new_buckets;
  o->size_bits = new_bits;
  o->count = 0;

  return Qnil;
}

void
map_obarray (Lisp_Object obarray,
	     void (*fn) (Lisp_Object, Lisp_Object), Lisp_Object arg)
{
  CHECK_OBARRAY (obarray);
  DOOBARRAY (XOBARRAY (obarray), it)
    (*fn) (obarray_iter_symbol (&it), arg);
}

static void
mapatoms_1 (Lisp_Object sym, Lisp_Object function)
{
  call1 (function, sym);
}

DEFUN ("mapatoms", Fmapatoms, Smapatoms, 1, 2, 0,
       doc: /* Call FUNCTION on every symbol in OBARRAY.
OBARRAY defaults to the value of `obarray'.  */)
  (Lisp_Object function, Lisp_Object obarray)
{
  if (NILP (obarray)) obarray = Vobarray;
  obarray = check_obarray (obarray);

  map_obarray (obarray, mapatoms_1, function);
  return Qnil;
}

DEFUN ("internal--obarray-buckets",
       Finternal__obarray_buckets, Sinternal__obarray_buckets, 1, 1, 0,
       doc: /* Symbols in each bucket of OBARRAY.  Internal use only.  */)
    (Lisp_Object obarray)
{
  obarray = check_obarray (obarray);
  ptrdiff_t size = obarray_size (XOBARRAY (obarray));

  Lisp_Object ret = Qnil;
  for (ptrdiff_t i = 0; i < size; i++)
    {
      Lisp_Object bucket = Qnil;
      Lisp_Object sym = XOBARRAY (obarray)->buckets[i];
      if (SYMBOLP (sym))
	while (1)
	  {
	    bucket = Fcons (sym, bucket);
	    struct Lisp_Symbol *s = XSYMBOL (sym)->u.s.next;
	    if (!s)
	      break;
	    XSETSYMBOL(sym, s);
	  }
      ret = Fcons (Fnreverse (bucket), ret);
    }
  return Fnreverse (ret);
}

void
init_obarray_once (void)
{
  Vobarray = make_obarray (15);
  initial_obarray = Vobarray;
  staticpro (&initial_obarray);

  for (int i = 0; i < ARRAYELTS (lispsym); ++i)
    define_symbol (builtin_lisp_symbol (i), defsym_name[i]);

  DEFSYM (Qunbound, "unbound");

  DEFSYM (Qnil, "nil");
  SET_SYMBOL_VAL (XSYMBOL (Qnil), Qnil);
  make_symbol_constant (Qnil);
  XSYMBOL (Qnil)->u.s.declared_special = true;

  DEFSYM (Qt, "t");
  SET_SYMBOL_VAL (XSYMBOL (Qt), Qt);
  make_symbol_constant (Qt);
  XSYMBOL (Qt)->u.s.declared_special = true;

  DEFSYM (Qvariable_documentation, "variable-documentation");
}


void
defsubr (union Aligned_Lisp_Subr *aname)
{
  struct Lisp_Subr *sname = &aname->s;
  Lisp_Object sym, tem;
  sym = intern_c_string (sname->symbol_name);
  XSETPVECTYPE (sname, PVEC_SUBR);
  XSETSUBR (tem, sname);
  SET_SYMBOL_FUNC (XSYMBOL (sym), tem);
#ifdef HAVE_NATIVE_COMP
  eassert (NILP (Vcomp_native_version_dir));
  Vcomp_subr_list = Fpurecopy_maybe (Fcons (tem, Vcomp_subr_list));
#endif
}

#ifdef NOTDEF /* Use fset in subr.el now!  */
void
defalias (struct Lisp_Subr *sname, char *string)
{
  Lisp_Object sym;
  sym = intern (string);
  XSETSUBR (XSYMBOL (sym)->u.s.function, sname);
}
#endif /* NOTDEF */

/* Define a global Lisp symbol whose value is forwarded to a C
   variable of type intmax_t.  */
void
defvar_int (struct Lisp_Intfwd const *i_fwd, char const *namestring)
{
  Lisp_Object sym = intern_c_string (namestring);
  XSYMBOL (sym)->u.s.declared_special = true;
  XSYMBOL (sym)->u.s.type = SYMBOL_FORWARDED;
  SET_SYMBOL_FWD (XSYMBOL (sym), i_fwd);
  XSYMBOL (sym)->u.s.c_variable.fwdptr = i_fwd;
}

/* Similar but define a variable whose value is t if 1, nil if 0.  */
void
defvar_bool (struct Lisp_Boolfwd const *b_fwd, char const *namestring)
{
  Lisp_Object sym = intern_c_string (namestring);
  XSYMBOL (sym)->u.s.declared_special = true;
  XSYMBOL (sym)->u.s.type = SYMBOL_FORWARDED;
  SET_SYMBOL_FWD (XSYMBOL (sym), b_fwd);
  XSYMBOL (sym)->u.s.c_variable.fwdptr = b_fwd;
  Vbyte_boolean_vars = Fcons (sym, Vbyte_boolean_vars);
}

/* Marking the same slot twice "can cause trouble with strings," so
   for those variables known to be exogenously marked, don't
   staticpro.  */
void
defvar_lisp_nopro (struct Lisp_Objfwd const *o_fwd, char const *namestring)
{
  Lisp_Object sym = intern_c_string (namestring);
  XSYMBOL (sym)->u.s.declared_special = true;
  XSYMBOL (sym)->u.s.type = SYMBOL_FORWARDED;
  SET_SYMBOL_FWD (XSYMBOL (sym), o_fwd);
  XSYMBOL (sym)->u.s.c_variable.fwdptr = o_fwd;
}

void
defvar_lisp (struct Lisp_Objfwd const *o_fwd, char const *namestring)
{
  defvar_lisp_nopro (o_fwd, namestring);
  staticpro (o_fwd->objvar);
}

/* Similar but define a variable whose value is the Lisp Object stored
   at a particular offset in the current kboard object.  */

void
defvar_kboard (struct Lisp_Kboard_Objfwd const *ko_fwd, char const *namestring)
{
  Lisp_Object sym = intern_c_string (namestring);
  XSYMBOL (sym)->u.s.declared_special = true;
  XSYMBOL (sym)->u.s.type = SYMBOL_KBOARD;
  SET_SYMBOL_FWD (XSYMBOL (sym), ko_fwd);
  XSYMBOL (sym)->u.s.c_variable.fwdptr = ko_fwd;
}

/* Check that the elements of lpath exist.  */

static void
load_path_check (Lisp_Object lpath)
{
  Lisp_Object path_tail;

  /* The only elements that might not exist are those from
     PATH_LOADSEARCH, EMACSLOADPATH.  Anything else is only added if
     it exists.  */
  for (path_tail = lpath; !NILP (path_tail); path_tail = XCDR (path_tail))
    {
      Lisp_Object dirfile;
      dirfile = Fcar (path_tail);
      if (STRINGP (dirfile))
        {
          dirfile = Fdirectory_file_name (dirfile);
          if (!file_accessible_directory_p (dirfile))
            dir_warning ("Lisp directory", XCAR (path_tail));
        }
    }
}

/* Dig toplevel LOAD-PATH out of epaths.h.  */

static Lisp_Object
load_path_default (void)
{
  if (will_dump_p ())
    /* PATH_DUMPLOADSEARCH is the lisp dir in the source directory.  */
    return decode_env_path (0, PATH_DUMPLOADSEARCH, 0);

  Lisp_Object lpath = decode_env_path (0, PATH_LOADSEARCH, 0);

  /* Counter-intuitively Vinstallation_directory is nil for
     invocations of the `make install` executable, and is
     Vsource_directory for invocations of the within-repo `make`
     executable.
  */
  if (!NILP (Vinstallation_directory))
    {
      Lisp_Object tem = Fexpand_file_name (build_string ("lisp"),
					   Vinstallation_directory),
	tem1 = Ffile_accessible_directory_p (tem);

      if (NILP (tem1))
	/* Use build-time dirs instead.  */
	lpath = nconc2 (lpath, decode_env_path (0, PATH_DUMPLOADSEARCH, 0));
      else if (NILP (Fmember (tem, lpath)))
	/* Override the inchoate LOAD-PATH.  */
	lpath = list1 (tem);

      /* Add the within-repo site-lisp (unusual).  */
      if (!no_site_lisp)
        {
          tem = Fexpand_file_name (build_string ("site-lisp"),
                                   Vinstallation_directory);
          tem1 = Ffile_accessible_directory_p (tem);
          if (!NILP (tem1) && (NILP (Fmember (tem, lpath))))
	    lpath = Fcons (tem, lpath);
        }

      if (NILP (Fequal (Vinstallation_directory, Vsource_directory)))
        {
	  /* An out-of-tree build (unusual).  */
          tem = Fexpand_file_name (build_string ("src/Makefile"),
                                   Vinstallation_directory);
          tem1 = Fexpand_file_name (build_string ("src/Makefile.in"),
				    Vinstallation_directory);

          /* Don't be fooled if they moved the entire source tree
             AFTER dumping Emacs.  If the build directory is indeed
             different from the source dir, src/Makefile.in and
             src/Makefile will not be found together.  */
          if (!NILP (Ffile_exists_p (tem)) && NILP (Ffile_exists_p (tem1)))
            {
              tem = Fexpand_file_name (build_string ("lisp"),
                                       Vsource_directory);
              if (NILP (Fmember (tem, lpath)))
                lpath = Fcons (tem, lpath);
              if (!no_site_lisp)
                {
                  tem = Fexpand_file_name (build_string ("site-lisp"),
                                           Vsource_directory);
                  if (!NILP (tem) && (NILP (Fmember (tem, lpath))))
		    lpath = Fcons (tem, lpath);
                }
            }
        }
    }

  return lpath;
}

void
init_lread (void)
{
  /* First, set Vload_path.  */

  /* Ignore EMACSLOADPATH when dumping.  */
  bool use_loadpath = !will_dump_p ();

  if (use_loadpath && egetenv ("EMACSLOADPATH"))
    {
      Vload_path = decode_env_path ("EMACSLOADPATH", 0, 1);

      /* Check (non-nil) user-supplied elements.  */
      load_path_check (Vload_path);

      /* If no nils in the environment variable, use as-is.
         Otherwise, replace any nils with the default.  */
      if (!NILP (Fmemq (Qnil, Vload_path)))
        {
          Lisp_Object elem, elpath = Vload_path;
          Lisp_Object default_lpath = load_path_default ();

          /* Check defaults, before adding site-lisp.  */
          load_path_check (default_lpath);

          /* Add the site-lisp directories to the front of the default.  */
          if (!no_site_lisp && PATH_SITELOADSEARCH[0] != '\0')
            {
              Lisp_Object sitelisp = decode_env_path (0, PATH_SITELOADSEARCH, 0);
              if (!NILP (sitelisp))
                default_lpath = nconc2 (sitelisp, default_lpath);
            }

          Vload_path = Qnil;

          /* Replace nils from EMACSLOADPATH by default.  */
          while (CONSP (elpath))
            {
              elem = XCAR (elpath);
              elpath = XCDR (elpath);
              Vload_path = CALLN (Fappend, Vload_path,
				  NILP (elem) ? default_lpath : list1 (elem));
            }
        }
    }
  else
    {
      Vload_path = load_path_default ();

      /* Check before adding site-lisp directories.
         The install should have created them, but they are not
         required, so no need to warn if they are absent.
         Or we might be running before installation.  */
      load_path_check (Vload_path);

      /* Add the site-lisp directories at the front.  */
      if (!will_dump_p () && !no_site_lisp && PATH_SITELOADSEARCH[0] != '\0')
        {
          Lisp_Object sitelisp = decode_env_path (0, PATH_SITELOADSEARCH, 0);
          if (!NILP (sitelisp))
	    Vload_path = nconc2 (sitelisp, Vload_path);
        }
    }

  Vvalues = Qnil;

  load_in_progress = 0;
  Vload_file_name = Qnil;
  Vstandard_input = Qt;
  Vloads_in_progress = Qnil;
}

/* Print a warning that directory intended for use USE and with name
   DIRNAME cannot be accessed.  On entry, errno should correspond to
   the access failure.  Print the warning on stderr and put it in
   *Messages*.  */

void
dir_warning (char const *use, Lisp_Object dirname)
{
  static char const format[] = "Warning: %s '%s': %s\n";
  char *diagnostic = emacs_strerror (errno);
  fprintf (stderr, format, use, SSDATA (ENCODE_SYSTEM (dirname)), diagnostic);

  /* Don't log the warning before we've initialized!!  */
  if (initialized)
    {
      ptrdiff_t diaglen = strlen (diagnostic);
      AUTO_STRING_WITH_LEN (diag, diagnostic, diaglen);
      if (!NILP (Vlocale_coding_system))
	{
	  Lisp_Object s
	    = code_convert_string_norecord (diag, Vlocale_coding_system, false);
	  diagnostic = SSDATA (s);
	  diaglen = SBYTES (s);
	}
      USE_SAFE_ALLOCA;
      char *buffer = SAFE_ALLOCA (sizeof format - 3 * (sizeof "%s" - 1)
				  + strlen (use) + SBYTES (dirname) + diaglen);
      ptrdiff_t message_len = esprintf (buffer, format, use, SSDATA (dirname),
					diagnostic);
      message_dolog (buffer, message_len, 0, STRING_MULTIBYTE (dirname));
      SAFE_FREE ();
    }
}

void
syms_of_lread (void)
{
  defsubr (&Sread);
  defsubr (&Sread_from_string);
  defsubr (&Sread_annotated);
  defsubr (&Slread__substitute_object_in_subtree);
  defsubr (&Sintern);
  defsubr (&Sintern_soft);
  defsubr (&Sunintern);
  defsubr (&Sget_load_suffixes);
  defsubr (&Sload);
  defsubr (&Seval_buffer);
  defsubr (&Seval_region);
  defsubr (&Sread_char);
  defsubr (&Sread_char_exclusive);
  defsubr (&Sread_event);
  defsubr (&Smapatoms);
  defsubr (&Slocate_file_internal);
  defsubr (&Sinternal__obarray_buckets);
  defsubr (&Sobarray_make);
  defsubr (&Sobarrayp);
  defsubr (&Sobarray_clear);

  DEFVAR_LISP ("obarray", Vobarray,
	       doc: /* Symbol table for use by `intern' and `read'.
It is a vector whose length ought to be prime for best results.
The vector's contents don't make sense if examined from Lisp programs;
to find all the symbols in an obarray, use `mapatoms'.  */);

  DEFVAR_LISP ("values", Vvalues,
	       doc: /* List of values of all expressions which were read, evaluated and printed.
Order is reverse chronological.
This variable is obsolete as of Emacs 28.1 and should not be used.  */);
  XSYMBOL (intern ("values"))->u.s.declared_special = false;

  DEFVAR_LISP ("standard-input", Vstandard_input,
	       doc: /* Stream for read to get input from.
See documentation of `read' for possible values.  */);
  Vstandard_input = Qt;

  DEFVAR_LISP ("read-circle", Vread_circle,
	       doc: /* Non-nil means read recursive structures using #N= and #N# syntax.  */);
  Vread_circle = Qt;

  DEFVAR_LISP ("load-path", Vload_path,
	       doc: /* List of directories to search for files to load.
Each element is a string (directory file name) or nil (meaning
`default-directory').
This list is consulted by the `require' function.
Initialized during startup as described in Info node `(elisp)Library Search'.
Use `directory-file-name' when adding items to this path.  However, Lisp
programs that process this list should tolerate directories both with
and without trailing slashes.  */);

  DEFVAR_LISP ("load-suffixes", Vload_suffixes,
	       doc: /* List of suffixes for Emacs Lisp files and dynamic modules.
This list includes suffixes for both compiled and source Emacs Lisp files.
This list should not include the empty string.
`load' and related functions try to append these suffixes, in order,
to the specified file name if a suffix is allowed or required.  */);
  Vload_suffixes = list2 (build_pure_c_string (".elc"),
			  build_pure_c_string (".el"));
#ifdef HAVE_NATIVE_COMP
  Vload_suffixes = Fcons (build_pure_c_string (NATIVE_SUFFIX), Vload_suffixes);
#endif
#ifdef HAVE_MODULES
  Vload_suffixes = Fcons (build_pure_c_string (MODULES_SUFFIX), Vload_suffixes);
#ifdef MODULES_SECONDARY_SUFFIX
  Vload_suffixes =
    Fcons (build_pure_c_string (MODULES_SECONDARY_SUFFIX), Vload_suffixes);
#endif
#endif

  DEFVAR_LISP ("module-file-suffix", Vmodule_file_suffix,
	       doc: /* Suffix of loadable module file, or nil if modules are not supported.  */);
#ifdef HAVE_MODULES
  Vmodule_file_suffix = build_pure_c_string (MODULES_SUFFIX);
#else
  Vmodule_file_suffix = Qnil;
#endif

  DEFVAR_LISP ("dynamic-library-suffixes", Vdynamic_library_suffixes,
	       doc: /* A list of suffixes for loadable dynamic libraries.  */);

#ifndef MSDOS
  Vdynamic_library_suffixes
    = Fcons (build_pure_c_string (DYNAMIC_LIB_SECONDARY_SUFFIX), Qnil);
  Vdynamic_library_suffixes
    = Fcons (build_pure_c_string (DYNAMIC_LIB_SUFFIX),
	     Vdynamic_library_suffixes);
#else
  Vdynamic_library_suffixes = Qnil;
#endif

  DEFVAR_LISP ("load-file-rep-suffixes", Vload_file_rep_suffixes,
	       doc: /* List of suffixes that indicate representations of \
the same file.
This list should normally start with the empty string.

Enabling Auto Compression mode appends the suffixes in
`jka-compr-load-suffixes' to this list and disabling Auto Compression
mode removes them again.  `load' and related functions use this list to
determine whether they should look for compressed versions of a file
and, if so, which suffixes they should try to append to the file name
in order to do so.  However, if you want to customize which suffixes
the loading functions recognize as compression suffixes, you should
customize `jka-compr-load-suffixes' rather than the present variable.  */);
  Vload_file_rep_suffixes = list1 (empty_unibyte_string);

  DEFVAR_BOOL ("load-in-progress", load_in_progress,
	       doc: /* Non-nil if inside of `load'.  */);
  DEFSYM (Qload_in_progress, "load-in-progress");

  DEFVAR_LISP ("after-load-alist", Vafter_load_alist,
	       doc: /* An alist of functions to be evalled when particular files are loaded.
Each element looks like (REGEXP-OR-FEATURE FUNCS...).

REGEXP-OR-FEATURE is either a regular expression to match file names, or
a symbol (a feature name).

When `load' is run and the file-name argument matches an element's
REGEXP-OR-FEATURE, or when `provide' is run and provides the symbol
REGEXP-OR-FEATURE, the FUNCS in the element are called.

An error in FUNCS does not undo the load, but does prevent calling
the rest of the FUNCS.  */);
  Vafter_load_alist = Qnil;

  DEFVAR_LISP ("load-history", Vload_history,
	       doc: /* Alist mapping loaded file names to symbols and features.
Each alist element should be a list (FILE-NAME ENTRIES...), where
FILE-NAME is the name of a file that has been loaded into Emacs.
The file name is absolute and true (i.e. it doesn't contain symlinks).
As an exception, one of the alist elements may have FILE-NAME nil,
for symbols and features not associated with any file.

The remaining ENTRIES in the alist element describe the functions and
variables defined in that file, the features provided, and the
features required.  Each entry has the form `(provide . FEATURE)',
`(require . FEATURE)', `(defun . FUNCTION)', `(defface . SYMBOL)',
 `(define-type . SYMBOL)', or `(cl-defmethod METHOD SPECIALIZERS)'.
In addition, entries may also be single symbols,
which means that symbol was defined by `defvar' or `defconst'.

During preloading, the file name recorded is relative to the main Lisp
directory.  These file names are converted to absolute at startup.  */);
  Vload_history = Qnil;

  DEFVAR_LISP ("load-file-name", Vload_file_name,
	       doc: /* File being loaded by `load'.  */);
  Vload_file_name = Qnil;

  DEFVAR_LISP ("user-init-file", Vuser_init_file,
	       doc: /* File name, including directory, of user's initialization file.
If the file loaded had extension `.elc', and the corresponding source file
exists, this variable contains the name of source file, suitable for use
by functions like `custom-save-all' which edit the init file.
While Emacs loads and evaluates any init file, value is the real name
of the file, regardless of whether or not it has the `.elc' extension.  */);
  Vuser_init_file = Qnil;

  DEFVAR_LISP ("current-load-list", Vcurrent_load_list,
	       doc: /* Used for internal purposes by `load'.  */);
  Vcurrent_load_list = Qnil;

  DEFVAR_LISP ("load-read-function", Vload_read_function,
	       doc: /* Function used for reading expressions.
It is used by `load' and `eval-region'.

Called with a single argument (the stream from which to read).
The default is to use the function `read'.  */);
  DEFSYM (Qread, "read");
  Vload_read_function = Qread;

  DEFVAR_LISP ("load-source-file-function", Vload_source_file_function,
	       doc: /* Because RMS begged off on writing this in C. */);
  Vload_source_file_function = Qnil;

  DEFVAR_BOOL ("load-force-doc-strings", load_force_doc_strings,
	       doc: /* Non-nil means `load' should force-load all dynamic doc strings.
This is useful when the file being loaded is a temporary copy.  */);
  load_force_doc_strings = 0;

  DEFVAR_BOOL ("load-convert-to-unibyte", load_convert_to_unibyte,
	       doc: /* Non-nil means `read' converts strings to unibyte whenever possible.
This is normally bound by `load' and `eval-buffer' to control `read',
and is not meant for users to change.  */);
  load_convert_to_unibyte = 0;

  DEFVAR_LISP ("source-directory", Vsource_directory,
	       doc: /* Directory in which Emacs sources were found when Emacs was built.
You cannot count on them to still be there!  */);
  Vsource_directory
    = Fexpand_file_name (build_string ("../"),
			 Fcar (decode_env_path (0, PATH_DUMPLOADSEARCH, 0)));

  DEFVAR_LISP ("installed-directory", Vinstalled_directory,
	       doc: /* Install path of built-in lisp libraries.
This directory contains the `etc`, `lisp`, and `site-lisp`
installables, and is determined at configure time in the epaths-force
make target.  Not to be confused with the legacy
`installation-directory' nor `invocation-directory'.  */);
  Vinstalled_directory
    = Fexpand_file_name (build_string ("../"),
			 Fcar (decode_env_path (0, PATH_LOADSEARCH, 0)));

  DEFVAR_LISP ("preloaded-file-list", Vpreloaded_file_list,
	       doc: /* Legacy variable.  */);
  Vpreloaded_file_list = Qnil;

  DEFVAR_LISP ("byte-boolean-vars", Vbyte_boolean_vars,
	       doc: /* List of all DEFVAR_BOOL variables, used by the byte code optimizer.  */);
  Vbyte_boolean_vars = Qnil;

  DEFVAR_BOOL ("load-dangerous-libraries", load_dangerous_libraries,
	       doc: /* Non-nil means load dangerous compiled Lisp files.
Some versions of XEmacs use different byte codes than Emacs.  These
incompatible byte codes can make Emacs crash when it tries to execute
them.  */);
  load_dangerous_libraries = 0;

  DEFVAR_BOOL ("force-load-messages", force_load_messages,
	       doc: /* Non-nil means force printing messages when loading Lisp files.
This overrides the value of the NOMESSAGE argument to `load'.  */);
  force_load_messages = 0;

  DEFVAR_LISP ("bytecomp-version-regexp", Vbytecomp_version_regexp,
	       doc: /* Regular expression matching safe to load compiled Lisp files.
When Emacs loads a compiled Lisp file, it reads the first 512 bytes
from the file, and matches them against this regular expression.
When the regular expression matches, the file is considered to be safe
to load.  */);
  Vbytecomp_version_regexp
    = build_pure_c_string
        ("^;;;.\\(?:in Emacs version\\|bytecomp version FSF\\)");

  DEFSYM (Qlexical_binding, "lexical-binding");
  DEFVAR_LISP ("lexical-binding", Vlexical_binding,
	       doc: /* Whether to use lexical binding when evaluating code.
Non-nil means that the code in the current buffer should be evaluated
with lexical binding.
This variable is automatically set from the file variables of an
interpreted Lisp file read using `load'.  Unlike other file local
variables, this must be set in the first line of a file.  */);
  Vlexical_binding = Qnil;
  Fmake_variable_buffer_local (Qlexical_binding);

  DEFVAR_LISP ("eval-buffer-list", Veval_buffer_list,
	       doc: /* List of buffers being read from by calls to `eval-buffer' and `eval-region'.  */);
  Veval_buffer_list = Qnil;

  DEFVAR_LISP ("lread--unescaped-character-literals",
               Vlread_unescaped_character_literals,
               doc: /* List of deprecated unescaped character literals encountered by `read'.
For internal use only.  */);
  Vlread_unescaped_character_literals = Qnil;
  DEFSYM (Qlread_unescaped_character_literals,
          "lread--unescaped-character-literals");

  /* Vsource_directory was initialized in init_lread.  */

  DEFSYM (Qcurrent_load_list, "current-load-list");
  DEFSYM (Qstandard_input, "standard-input");
  DEFSYM (Qread_char, "read-char");
  DEFSYM (Qget_file_char, "get-file-char");

  /* Used instead of Qget_file_char while loading *.elc files compiled
     by Emacs 21 or older.  */
  DEFSYM (Qget_emacs_mule_file_char, "get-emacs-mule-file-char");

  DEFSYM (Qload_force_doc_strings, "load-force-doc-strings");

  DEFSYM (Qbackquote, "`");
  DEFSYM (Qcomma, ",");
  DEFSYM (Qcomma_at, ",@");

#if !IEEE_FLOATING_POINT
  for (int negative = 0; negative < 2; negative++)
    {
      not_a_number[negative] = build_pure_c_string (&"-0.0e+NaN"[!negative]);
      staticpro (&not_a_number[negative]);
    }
#endif

  DEFSYM (Qinhibit_file_name_operation, "inhibit-file-name-operation");
  DEFSYM (Qascii_character, "ascii-character");
  DEFSYM (Qfunction, "function");
  DEFSYM (Qload, "load");
  DEFSYM (Qload_file_name, "load-file-name");
  DEFSYM (Qeval_buffer_list, "eval-buffer-list");
  DEFSYM (Qdir_ok, "dir-ok");
  DEFSYM (Qdo_after_load_evaluation, "do-after-load-evaluation");

  staticpro (&read_objects_map);
  read_objects_map = Qnil;
  staticpro (&read_objects_completed);
  read_objects_completed = Qnil;

  Vloads_in_progress = Qnil;
  staticpro (&Vloads_in_progress);

  DEFSYM (Qhash_table, "hash-table");
  DEFSYM (Qdata, "data");
  DEFSYM (Qtest, "test");
  DEFSYM (Qsize, "size");
  DEFSYM (Qpurecopy, "purecopy");
  DEFSYM (Qweakness, "weakness");

  DEFSYM (Qchar_from_name, "char-from-name");

  DEFVAR_LISP ("read-symbol-shorthands", Vread_symbol_shorthands,
          doc: /* Alist of known symbol-name shorthands.
This variable's value can only be set via file-local variables.
See Info node `(elisp)Shorthands' for more details.  */);
  Vread_symbol_shorthands = Qnil;
  DEFSYM (Qobarray_cache, "obarray-cache");
  DEFSYM (Qobarrayp, "obarrayp");

  DEFSYM (Qmacroexp__dynvars, "macroexp--dynvars");
  DEFVAR_LISP ("macroexp--dynvars", Vmacroexp__dynvars,
        doc:   /* List of variables declared dynamic in the current scope.
Only valid during macro-expansion.  Internal use only. */);
  Vmacroexp__dynvars = Qnil;
}
