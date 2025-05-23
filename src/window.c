/* Window creation, deletion and examination for GNU Emacs.
   Does not include redisplay.
   Copyright (C) 1985-1987, 1993-1998, 2000-2024 Free Software
   Foundation, Inc.

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

#include <config.h>

/* Work around GCC bug 102671.  */
#if 10 <= __GNUC__
# pragma GCC diagnostic ignored "-Wanalyzer-null-dereference"
#endif

#include "lisp.h"
#include "buffer.h"
#include "keyboard.h"
#include "keymap.h"
#include "frame.h"
#include "window.h"
#include "commands.h"
#include "indent.h"
#include "termchar.h"
#include "disptab.h"
#include "dispextern.h"
#include "blockinput.h"
#include "termhooks.h"		/* For FRAME_TERMINAL.  */
#include "xwidget.h"
#ifdef HAVE_WINDOW_SYSTEM
#include TERM_HEADER
#endif /* HAVE_WINDOW_SYSTEM */
#ifdef MSDOS
#include "msdos.h"
#endif
#include "pdumper.h"

static ptrdiff_t count_windows (struct window *);
static ptrdiff_t get_leaf_windows (struct window *, struct window **,
				   ptrdiff_t);
static void window_scroll_pixel_based (Lisp_Object, int, bool, bool);
static void window_scroll_line_based (Lisp_Object, int, bool, bool);
static void foreach_window (struct frame *,
			    bool (* fn) (struct window *, void *),
                            void *);
static bool foreach_window_1 (struct window *,
			      bool (* fn) (struct window *, void *),
			      void *);
static bool window_resize_check (struct window *, bool);
static void window_resize_apply (struct window *, bool);
static void select_window_1 (Lisp_Object, bool);
static void run_window_configuration_change_hook (struct frame *);

static struct window *set_window_fringes (struct window *, Lisp_Object,
					  Lisp_Object, Lisp_Object,
					  Lisp_Object);
static struct window *set_window_margins (struct window *, Lisp_Object,
					  Lisp_Object);
static struct window *set_window_scroll_bars (struct window *, Lisp_Object,
					      Lisp_Object, Lisp_Object,
					      Lisp_Object, Lisp_Object);
static void apply_window_adjustment (struct window *);

/* This is the window in which the terminal's cursor should
   be left when nothing is being done with it.  This must
   always be a leaf window, and its buffer is selected by
   the top level editing loop at the end of each command.

   This value is always the same as
   FRAME_SELECTED_WINDOW (selected_frame).  */
Lisp_Object selected_window;

/* The value of selected_window at the last time window change
   functions were run.  This is always the same as
   FRAME_OLD_SELECTED_WINDOW (old_selected_frame).  */
static Lisp_Object old_selected_window;

/* A list of all windows for use by next_window and Fwindow_list.
   Functions creating or deleting windows should invalidate this cache
   by setting it to nil.  */
Lisp_Object Vwindow_list;

/* True mean window_change_record has to record all live frames.  */
static bool window_change_record_frames;

/* The mini-buffer window of the selected frame.
   Note that you cannot test for mini-bufferness of an arbitrary window
   by comparing against this; but you can test for mini-bufferness of
   the selected window.  */
Lisp_Object minibuf_window;

/* Non-nil means it is the window whose mode line should be
   shown as the selected window when the minibuffer is selected.  */
Lisp_Object minibuf_selected_window;

/* Incremented for each window created.  */
static EMACS_INT sequence_number;

/* Used by the function window_scroll_pixel_based.  */
static int window_scroll_pixel_based_preserve_x;
static int window_scroll_pixel_based_preserve_y;

/* Same for window_scroll_line_based.  */
static EMACS_INT window_scroll_preserve_hpos;
static EMACS_INT window_scroll_preserve_vpos;

static void
CHECK_WINDOW_CONFIGURATION (Lisp_Object x)
{
  CHECK_TYPE (WINDOW_CONFIGURATIONP (x), Qwindow_configuration_p, x);
}

/* These setters are used only in this file, so they can be private.  */
static void
wset_combination_limit (struct window *w, Lisp_Object val)
{
  w->combination_limit = val;
}

static void
wset_dedicated (struct window *w, Lisp_Object val)
{
  w->dedicated = val;
}

static void
wset_display_table (struct window *w, Lisp_Object val)
{
  w->display_table = val;
}

static void
wset_new_normal (struct window *w, Lisp_Object val)
{
  w->new_normal = val;
}

static void
wset_new_total (struct window *w, Lisp_Object val)
{
  w->new_total = val;
}

static void
wset_normal_cols (struct window *w, Lisp_Object val)
{
  w->normal_cols = val;
}

static void
wset_normal_lines (struct window *w, Lisp_Object val)
{
  w->normal_lines = val;
}

static void
wset_parent (struct window *w, Lisp_Object val)
{
  w->parent = val;
}

static void
wset_pointm (struct window *w, Lisp_Object val)
{
  w->pointm = val;
}

static void
wset_old_pointm (struct window *w, Lisp_Object val)
{
  w->old_pointm = val;
}

static void
wset_start (struct window *w, Lisp_Object val)
{
  w->start = val;
}

static void
wset_temslot (struct window *w, Lisp_Object val)
{
  w->temslot = val;
}

static void
wset_vertical_scroll_bar_type (struct window *w, Lisp_Object val)
{
  w->vertical_scroll_bar_type = val;
}

static void
wset_window_parameters (struct window *w, Lisp_Object val)
{
  w->window_parameters = val;
}

static void
wset_combination (struct window *w, bool horflag, Lisp_Object val)
{
  /* Since leaf windows never becomes non-leaf, there should
     be no buffer and markers in start and pointm fields of W.  */
  eassert (!BUFFERP (w->contents) && NILP (w->start) && NILP (w->pointm));
  w->contents = val;
  /* When an internal window is deleted and VAL is nil, HORFLAG
     is meaningless.  */
  if (!NILP (val))
    w->horizontal = horflag;
}

/* True if leaf window W doesn't reflect the actual state
   of displayed buffer due to its text or overlays change.  */

bool
window_outdated (struct window *w)
{
  struct buffer *b = XBUFFER (w->contents);
  return (w->last_modified < BUF_MODIFF (b)
	  || w->last_overlay_modified < BUF_OVERLAY_MODIFF (b));
}

struct window *
decode_live_window (register Lisp_Object window)
{
  if (NILP (window))
    return XWINDOW (selected_window);

  CHECK_LIVE_WINDOW (window);
  return XWINDOW (window);
}

struct window *
decode_any_window (register Lisp_Object window)
{
  struct window *w;

  if (NILP (window))
    return XWINDOW (selected_window);

  CHECK_WINDOW (window);
  w = XWINDOW (window);
  return w;
}

static struct window *
decode_valid_window (register Lisp_Object window)
{
  struct window *w;

  if (NILP (window))
    return XWINDOW (selected_window);

  CHECK_VALID_WINDOW (window);
  w = XWINDOW (window);
  return w;
}

/* Called when W's buffer slot is changed.  ARG -1 means that W is about to
   cease its buffer, and 1 means that W is about to set up the new one.  */

static void
adjust_window_count (struct window *w, int arg)
{
  eassert (eabs (arg) == 1);
  if (BUFFERP (w->contents))
    {
      struct buffer *b = XBUFFER (w->contents);

      if (b->base_buffer)
	b = b->base_buffer;
      b->window_count += arg;
      eassert (b->window_count >= 0);
      /* These should be recalculated by redisplay code.  */
      w->window_end_valid = false;
      w->base_line_pos = 0;
    }
}

/* Set W's buffer slot to VAL and recompute number
   of windows showing VAL if it is a buffer.  */

void
wset_buffer (struct window *w, Lisp_Object val)
{
  adjust_window_count (w, -1);
  if (BUFFERP (val))
    /* Make sure that we do not assign the buffer
       to an internal window.  */
    eassert (MARKERP (w->start) && MARKERP (w->pointm));
  w->contents = val;
  adjust_window_count (w, 1);
}

static void
wset_old_buffer (struct window *w, Lisp_Object val)
{
  w->old_buffer = val;
}

DEFUN ("windowp", Fwindowp, Swindowp, 1, 1, 0,
       doc: /* Return t if OBJECT is a window and nil otherwise.  */)
  (Lisp_Object object)
{
  return WINDOWP (object) ? Qt : Qnil;
}

DEFUN ("window-valid-p", Fwindow_valid_p, Swindow_valid_p, 1, 1, 0,
       doc: /* Return t if OBJECT is a valid window and nil otherwise.
A valid window is either a window that displays a buffer or an internal
window.  Windows that have been deleted are not valid.  */)
  (Lisp_Object object)
{
  return WINDOW_VALID_P (object) ? Qt : Qnil;
}

DEFUN ("window-live-p", Fwindow_live_p, Swindow_live_p, 1, 1, 0,
       doc: /* Return t if OBJECT is a live window and nil otherwise.
A live window is a window that displays a buffer.
Internal windows and deleted windows are not live.  */)
  (Lisp_Object object)
{
  return WINDOW_LIVE_P (object) ? Qt : Qnil;
}

/* Frames and windows.  */
DEFUN ("window-frame", Fwindow_frame, Swindow_frame, 0, 1, 0,
       doc: /* Return the frame that window WINDOW is on.
WINDOW must be a valid window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return decode_valid_window (window)->frame;
}

DEFUN ("frame-root-window", Fframe_root_window, Sframe_root_window, 0, 1, 0,
       doc: /* Return the root window of FRAME-OR-WINDOW.
If omitted, FRAME-OR-WINDOW defaults to the currently selected frame.
With a frame argument, return that frame's root window.
With a window argument, return the root window of that window's frame.  */)
  (Lisp_Object frame_or_window)
{
  Lisp_Object window;

  if (NILP (frame_or_window))
    window = SELECTED_FRAME ()->root_window;
  else if (WINDOW_VALID_P (frame_or_window))
      window = XFRAME (XWINDOW (frame_or_window)->frame)->root_window;
  else
    {
      CHECK_LIVE_FRAME (frame_or_window);
      window = XFRAME (frame_or_window)->root_window;
    }

  return window;
}

DEFUN ("minibuffer-window", Fminibuffer_window, Sminibuffer_window, 0, 1, 0,
       doc: /* Return the minibuffer window for frame FRAME.
If FRAME is omitted or nil, it defaults to the selected frame.  */)
  (Lisp_Object frame)
{
  return FRAME_MINIBUF_WINDOW (decode_live_frame (frame));
}

DEFUN ("window-minibuffer-p", Fwindow_minibuffer_p,
       Swindow_minibuffer_p, 0, 1, 0,
       doc: /* Return t if WINDOW is a minibuffer window.
WINDOW must be a valid window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return MINI_WINDOW_P (decode_valid_window (window)) ? Qt : Qnil;
}

/* Don't move this to window.el - this must be a safe routine.  */
DEFUN ("frame-first-window", Fframe_first_window, Sframe_first_window, 0, 1, 0,
       doc: /* Return the topmost, leftmost live window on FRAME-OR-WINDOW.
If omitted, FRAME-OR-WINDOW defaults to the currently selected frame.
Else if FRAME-OR-WINDOW denotes a valid window, return the first window
of that window's frame.  If FRAME-OR-WINDOW denotes a live frame, return
the first window of that frame.  */)
  (Lisp_Object frame_or_window)
{
  Lisp_Object window;

  if (NILP (frame_or_window))
    window = SELECTED_FRAME ()->root_window;
  else if (WINDOW_VALID_P (frame_or_window))
    window = XFRAME (WINDOW_FRAME (XWINDOW (frame_or_window)))->root_window;
  else
    {
      CHECK_LIVE_FRAME (frame_or_window);
      window = XFRAME (frame_or_window)->root_window;
    }

  while (WINDOWP (XWINDOW (window)->contents))
    window = XWINDOW (window)->contents;

  return window;
}

DEFUN ("frame-selected-window", Fframe_selected_window,
       Sframe_selected_window, 0, 1, 0,
       doc: /* Return the selected window of FRAME-OR-WINDOW.
If omitted, FRAME-OR-WINDOW defaults to the currently selected frame.
Else if FRAME-OR-WINDOW denotes a valid window, return the selected
window of that window's frame.  If FRAME-OR-WINDOW denotes a live frame,
return the selected window of that frame.  */)
  (Lisp_Object frame_or_window)
{
  Lisp_Object window;

  if (NILP (frame_or_window))
    window = SELECTED_FRAME ()->selected_window;
  else if (WINDOW_VALID_P (frame_or_window))
    window = XFRAME (WINDOW_FRAME (XWINDOW (frame_or_window)))->selected_window;
  else
    {
      CHECK_LIVE_FRAME (frame_or_window);
      window = XFRAME (frame_or_window)->selected_window;
    }

  return window;
}

DEFUN ("frame-old-selected-window", Fframe_old_selected_window,
       Sframe_old_selected_window, 0, 1, 0,
       doc: /* Return old selected window of FRAME.
FRAME must be a live frame and defaults to the selected one.

The return value is the window selected on FRAME the last time window
change functions were run for FRAME.  */)
  (Lisp_Object frame)
{
  if (NILP (frame))
    frame = selected_frame;
  CHECK_LIVE_FRAME (frame);

  return XFRAME (frame)->old_selected_window;
}

DEFUN ("set-frame-selected-window", Fset_frame_selected_window,
       Sset_frame_selected_window, 2, 3, 0,
       doc: /* Set selected window of FRAME to WINDOW.
FRAME must be a live frame and defaults to the selected one.  If FRAME
is the selected frame, this makes WINDOW the selected window.  Optional
argument NORECORD non-nil means to neither change the order of recently
selected windows nor the buffer list.  WINDOW must denote a live window.
Return WINDOW.  */)
  (Lisp_Object frame, Lisp_Object window, Lisp_Object norecord)
{
  if (NILP (frame))
    frame = selected_frame;

  CHECK_LIVE_FRAME (frame);
  CHECK_LIVE_WINDOW (window);

  if (!EQ (frame, WINDOW_FRAME (XWINDOW (window))))
    error ("In `set-frame-selected-window', WINDOW is not on FRAME");

  if (EQ (frame, selected_frame))
    return Fselect_window (window, norecord);
  else
    {
      fset_selected_window (XFRAME (frame), window);
      /* Don't clear FRAME's select_mini_window_flag here.  */
      return window;
    }
}

DEFUN ("selected-window", Fselected_window, Sselected_window, 0, 0, 0,
       doc: /* Return the selected window.
The selected window is the window in which the standard cursor for
selected windows appears and to which many commands apply.

Also see `old-selected-window' and `minibuffer-selected-window'.  */)
  (void)
{
  return selected_window;
}

DEFUN ("old-selected-window", Fold_selected_window,
       Sold_selected_window, 0, 0, 0,
       doc: /* Return the old selected window.
The return value is the window selected the last time window change
functions were run.  */)
  (void)
{
  return old_selected_window;
}

EMACS_INT window_select_count;

/* Fset_window_configuration sets inhibit_point_swap to true to
   circumvent the degenerate case when selected_window is still Qnil. */
static Lisp_Object
select_window (Lisp_Object window, Lisp_Object norecord,
	       bool inhibit_point_swap)
{
  struct window *w;
  struct frame *sf;
  Lisp_Object frame;
  struct frame *f;

  CHECK_LIVE_WINDOW (window);

  w = XWINDOW (window);
  frame = WINDOW_FRAME (w);
  f = XFRAME (frame);

  if (FRAME_TOOLTIP_P (f))
    /* Do not select a tooltip window (Bug#47207).  */
    error ("Cannot select a tooltip window");

  /* We definitely want to select WINDOW, not the mini-window.  */
  f->select_mini_window_flag = false;

  /* Make the selected window's buffer current.  */
  Fset_buffer (w->contents);

  if (EQ (window, selected_window) && !inhibit_point_swap)
    /* `switch-to-buffer' uses (select-window (selected-window)) as a "clever"
       way to call record_buffer from Elisp, so it's important that we call
       record_buffer before returning here.  */
    goto record_and_return;

  if (NILP (norecord) || EQ (norecord, Qmark_for_redisplay))
    { /* Mark the window for redisplay since the selected-window has
	 a different mode-line.  */
      wset_redisplay (XWINDOW (selected_window));
      wset_redisplay (w);
    }
  else
    redisplay_other_windows ();

  sf = SELECTED_FRAME ();
  if (f != sf)
    {
      fset_selected_window (f, window);
      /* Use this rather than Fhandle_switch_frame
	 so that FRAME_FOCUS_FRAME is moved appropriately as we
	 move around in the state where a minibuffer in a separate
	 frame is active.  */
      Fselect_frame (frame, norecord);
      /* Fselect_frame called us back so we've done all the work already.  */
      eassert (EQ (window, selected_window)
	       || (EQ (window, f->minibuffer_window)
		   && NILP (Fminibufferp (XWINDOW (window)->contents, Qt))));
      return window;
    }
  else
    fset_selected_window (sf, window);

  select_window_1 (window, inhibit_point_swap);
  bset_last_selected_window (XBUFFER (w->contents), window);

 record_and_return:
  /* record_buffer can call maybe_quit, so make sure it is run only
     after we have re-established the invariant between
     selected_window and selected_frame, otherwise the temporary
     broken invariant might "escape" (Bug#14161).  */
  if (NILP (norecord))
    {
      w->use_time = ++window_select_count;
      record_buffer (w->contents);
    }

  return window;
}

/* Select window with a minimum of fuss, i.e. don't record the change anywhere
   (not even for redisplay's benefit), and assume that the window's frame is
   already selected.  */
static void
select_window_1 (Lisp_Object window, bool inhibit_point_swap)
{
  /* Store the old selected window's buffer's point in pointm of the old
     selected window.  It belongs to that window, and when the window is
     not selected, must be in the window.  */
  if (!inhibit_point_swap)
    {
      struct window *ow = XWINDOW (selected_window);
      if (BUFFERP (ow->contents))
	set_marker_both (ow->pointm, ow->contents,
			 BUF_PT (XBUFFER (ow->contents)),
			 BUF_PT_BYTE (XBUFFER (ow->contents)));
    }

  selected_window = window;

  /* Go to the point recorded in the window.
     This is important when the buffer is in more
     than one window.  It also matters when
     redisplay_window has altered point after scrolling,
     because it makes the change only in the window.  */
  set_point_from_marker (XWINDOW (window)->pointm);
}

DEFUN ("select-window", Fselect_window, Sselect_window, 1, 2, 0,
       doc: /* Select WINDOW which must be a live window.
Also make WINDOW's frame the selected frame and WINDOW that frame's
selected window.  In addition, make WINDOW's buffer current and set its
buffer's value of `point' to the value of WINDOW's `window-point'.
Return WINDOW.

Optional second arg NORECORD non-nil means do not put this buffer at the
front of the buffer list and do not make this window the most recently
selected one.  Also, do not mark WINDOW for redisplay unless NORECORD
equals the special symbol `mark-for-redisplay'.

Run `buffer-list-update-hook' unless NORECORD is non-nil.  Note that
applications and internal routines often select a window temporarily for
various purposes; mostly, to simplify coding.  As a rule, such
selections should not be recorded and therefore will not pollute
`buffer-list-update-hook'.  Selections that "really count" are those
causing a visible change in the next redisplay of WINDOW's frame and
should always be recorded.  So if you think of running a function each
time a window gets selected, put it on `buffer-list-update-hook' or
`window-selection-change-functions'.

Also note that the main editor command loop sets the current buffer to
the buffer of the selected window before each command.  */)
  (Lisp_Object window, Lisp_Object norecord)
{
  return select_window (window, norecord, false);
}

DEFUN ("window-buffer", Fwindow_buffer, Swindow_buffer, 0, 1, 0,
       doc: /* Return the buffer displayed in window WINDOW.
If WINDOW is omitted or nil, it defaults to the selected window.
Return nil for an internal window or a deleted window.  */)
  (Lisp_Object window)
{
  struct window *w = decode_any_window (window);

  return WINDOW_LEAF_P (w) ? w->contents : Qnil;
}

DEFUN ("window-old-buffer", Fwindow_old_buffer, Swindow_old_buffer, 0, 1, 0,
       doc: /* Return the old buffer displayed by WINDOW.
WINDOW must be a live window and defaults to the selected one.

The return value is the buffer shown in WINDOW at the last time window
change functions were run.  It is nil if WINDOW was created after
that.  It is t if WINDOW has been restored from a window configuration
after that.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);

  return (NILP (w->old_buffer)
	  /* A new window.  */
	  ? Qnil
	  : (w->change_stamp != WINDOW_XFRAME (w)->change_stamp)
	  /* A window restored from a configuration.  */
	  ? Qt
	  /* A window that was live the last time seen by window
	     change functions.  */
	  : w->old_buffer);
}

DEFUN ("window-parent", Fwindow_parent, Swindow_parent, 0, 1, 0,
       doc: /* Return the parent window of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.
Return nil for a window with no parent (e.g. a root window).  */)
  (Lisp_Object window)
{
  return decode_valid_window (window)->parent;
}

DEFUN ("window-top-child", Fwindow_top_child, Swindow_top_child, 0, 1, 0,
       doc: /* Return the topmost child window of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.
Return nil if WINDOW is a live window (live windows have no children).
Return nil if WINDOW is an internal window whose children form a
horizontal combination.  */)
  (Lisp_Object window)
{
  struct window *w = decode_valid_window (window);
  return WINDOW_VERTICAL_COMBINATION_P (w) ? w->contents : Qnil;
}

DEFUN ("window-left-child", Fwindow_left_child, Swindow_left_child, 0, 1, 0,
       doc: /* Return the leftmost child window of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.
Return nil if WINDOW is a live window (live windows have no children).
Return nil if WINDOW is an internal window whose children form a
vertical combination.  */)
  (Lisp_Object window)
{
  struct window *w = decode_valid_window (window);
  return WINDOW_HORIZONTAL_COMBINATION_P (w) ? w->contents : Qnil;
}

DEFUN ("window-next-sibling", Fwindow_next_sibling, Swindow_next_sibling, 0, 1, 0,
       doc: /* Return the next sibling window of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.
Return nil if WINDOW has no next sibling.  */)
  (Lisp_Object window)
{
  return decode_valid_window (window)->next;
}

DEFUN ("window-prev-sibling", Fwindow_prev_sibling, Swindow_prev_sibling, 0, 1, 0,
       doc: /* Return the previous sibling window of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.
Return nil if WINDOW has no previous sibling.  */)
  (Lisp_Object window)
{
  return decode_valid_window (window)->prev;
}

DEFUN ("window-combination-limit", Fwindow_combination_limit, Swindow_combination_limit, 1, 1, 0,
       doc: /* Return combination limit of window WINDOW.
WINDOW must be a valid window used in horizontal or vertical combination.
If the return value is nil, child windows of WINDOW can be recombined with
WINDOW's siblings.  A return value of t means that child windows of
WINDOW are never (re-)combined with WINDOW's siblings.  */)
  (Lisp_Object window)
{
  struct window *w;

  CHECK_VALID_WINDOW (window);
  w = XWINDOW (window);
  if (WINDOW_LEAF_P (w))
    error ("Combination limit is meaningful for internal windows only");
  return w->combination_limit;
}

DEFUN ("set-window-combination-limit", Fset_window_combination_limit, Sset_window_combination_limit, 2, 2, 0,
       doc: /* Set combination limit of window WINDOW to LIMIT; return LIMIT.
WINDOW must be a valid window used in horizontal or vertical combination.
If LIMIT is nil, child windows of WINDOW can be recombined with WINDOW's
siblings.  LIMIT t means that child windows of WINDOW are never
\(re-)combined with WINDOW's siblings.  Other values are reserved for
future use.  */)
  (Lisp_Object window, Lisp_Object limit)
{
  struct window *w;

  CHECK_VALID_WINDOW (window);
  w = XWINDOW (window);
  if (WINDOW_LEAF_P (w))
    error ("Combination limit is meaningful for internal windows only");
  wset_combination_limit (w, limit);
  return limit;
}

DEFUN ("window-use-time", Fwindow_use_time, Swindow_use_time, 0, 1, 0,
       doc: /* Return the use time of window WINDOW.
WINDOW must specify a live window and defaults to the selected one.

The window with the highest use time is usually the one most recently
selected by calling `select-window' with NORECORD nil.  The window with
the lowest use time is usually the least recently selected one chosen in
such a way.

Note that the use time of a window can be also changed by calling
`window-bump-use-time' for that window.  */)
  (Lisp_Object window)
{
  return make_fixnum (decode_live_window (window)->use_time);
}

DEFUN ("window-bump-use-time", Fwindow_bump_use_time,
       Swindow_bump_use_time, 0, 1, 0,
       doc: /* Mark WINDOW as second most recently used.
WINDOW must specify a live window.

If WINDOW is not selected and the selected window has the highest use
time of all windows, set the use time of WINDOW to that of the selected
window, increase the use time of the selected window by one and return
the new use time of WINDOW.  Otherwise, do nothing and return nil.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);
  struct window *sw = XWINDOW (selected_window);

  if (w != sw && sw->use_time == window_select_count)
    {
      w->use_time = window_select_count;
      sw->use_time = ++window_select_count;

      return make_fixnum (w->use_time);
    }
  else
    return Qnil;
}

DEFUN ("window-pixel-width", Fwindow_pixel_width, Swindow_pixel_width, 0, 1, 0,
       doc: /* Return the width of window WINDOW in pixels.
WINDOW must be a valid window and defaults to the selected one.

The return value includes the fringes and margins of WINDOW as well as
any vertical dividers or scroll bars belonging to WINDOW.  If WINDOW is
an internal window, its pixel width is the width of the screen areas
spanned by its children.  */)
     (Lisp_Object window)
{
  return make_fixnum (decode_valid_window (window)->pixel_width);
}

DEFUN ("window-pixel-height", Fwindow_pixel_height, Swindow_pixel_height, 0, 1, 0,
       doc: /* Return the height of window WINDOW in pixels.
WINDOW must be a valid window and defaults to the selected one.

The return value includes the mode line and header line and the bottom
divider, if any.  If WINDOW is an internal window, its pixel height is
the height of the screen areas spanned by its children.  */)
  (Lisp_Object window)
{
  return make_fixnum (decode_valid_window (window)->pixel_height);
}

DEFUN ("window-old-pixel-width", Fwindow_old_pixel_width,
       Swindow_old_pixel_width, 0, 1, 0,
       doc: /* Return old total pixel width of WINDOW.
WINDOW must be a valid window and defaults to the selected one.

The return value is the total pixel width of WINDOW after the last
time window change functions found WINDOW live on its frame.  It is
zero if WINDOW was created after that.  */)
  (Lisp_Object window)
{
  return (make_fixnum
	  (decode_valid_window (window)->old_pixel_width));
}

DEFUN ("window-old-pixel-height", Fwindow_old_pixel_height,
       Swindow_old_pixel_height, 0, 1, 0,
       doc: /* Return old total pixel height of WINDOW.
WINDOW must be a valid window and defaults to the selected one.

The return value is the total pixel height of WINDOW after the last
time window change functions found WINDOW live on its frame.  It is
zero if WINDOW was created after that.  */)
  (Lisp_Object window)
{
  return (make_fixnum
	  (decode_valid_window (window)->old_pixel_height));
}

DEFUN ("window-total-height", Fwindow_total_height, Swindow_total_height, 0, 2, 0,
       doc: /* Return the height of window WINDOW in lines.
WINDOW must be a valid window and defaults to the selected one.

The return value includes the heights of WINDOW's mode and header line
and its bottom divider, if any.  If WINDOW is an internal window, the
total height is the height of the screen areas spanned by its children.

If WINDOW's pixel height is not an integral multiple of its frame's
character height, the number of lines occupied by WINDOW is rounded
internally.  This is done in a way such that, if WINDOW is a parent
window, the sum of the total heights of all its children internally
equals the total height of WINDOW.

If the optional argument ROUND is `ceiling', return the smallest integer
larger than WINDOW's pixel height divided by the character height of
WINDOW's frame.  ROUND `floor' means to return the largest integer
smaller than WINDOW's pixel height divided by the character height of
WINDOW's frame.  Any other value of ROUND means to return the internal
total height of WINDOW.  */)
  (Lisp_Object window, Lisp_Object round)
{
  struct window *w = decode_valid_window (window);

  if (!EQ (round, Qfloor) && !EQ (round, Qceiling))
    return make_fixnum (w->total_lines);
  else
    {
      int unit = FRAME_LINE_HEIGHT (WINDOW_XFRAME (w));

      return make_fixnum (EQ (round, Qceiling)
			  ? ((w->pixel_height + unit - 1) /unit)
			  : (w->pixel_height / unit));
    }
}

DEFUN ("window-total-width", Fwindow_total_width, Swindow_total_width, 0, 2, 0,
       doc: /* Return the total width of window WINDOW in columns.
WINDOW must be a valid window and defaults to the selected one.

The return value includes the widths of WINDOW's fringes, margins,
scroll bars and its right divider, if any.  If WINDOW is an internal
window, the total width is the width of the screen areas spanned by its
children.

If WINDOW's pixel width is not an integral multiple of its frame's
character width, the number of lines occupied by WINDOW is rounded
internally.  This is done in a way such that, if WINDOW is a parent
window, the sum of the total widths of all its children internally
equals the total width of WINDOW.

If the optional argument ROUND is `ceiling', return the smallest integer
larger than WINDOW's pixel width divided by the character width of
WINDOW's frame.  ROUND `floor' means to return the largest integer
smaller than WINDOW's pixel width divided by the character width of
WINDOW's frame.  Any other value of ROUND means to return the internal
total width of WINDOW.  */)
  (Lisp_Object window, Lisp_Object round)
{
  struct window *w = decode_valid_window (window);

  if (!EQ (round, Qfloor) && !EQ (round, Qceiling))
    return make_fixnum (w->total_cols);
  else
    {
      int unit = FRAME_COLUMN_WIDTH (WINDOW_XFRAME (w));

      return make_fixnum (EQ (round, Qceiling)
			  ? ((w->pixel_width + unit - 1) /unit)
			  : (w->pixel_width / unit));
    }
}

DEFUN ("window-new-total", Fwindow_new_total, Swindow_new_total, 0, 1, 0,
       doc: /* Return the new total size of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.

The new total size of WINDOW is the value set by the last call of
`set-window-new-total' for WINDOW.  If it is valid, it will be shortly
installed as WINDOW's total height (see `window-total-height') or total
width (see `window-total-width').  */)
  (Lisp_Object window)
{
  return decode_valid_window (window)->new_total;
}

DEFUN ("window-normal-size", Fwindow_normal_size, Swindow_normal_size, 0, 2, 0,
       doc: /* Return the normal height of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.
If HORIZONTAL is non-nil, return the normal width of WINDOW.

The normal height of a frame's root window or a window that is
horizontally combined (a window that has a left or right sibling) is
1.0.  The normal height of a window that is vertically combined (has a
sibling above or below) is the fraction of the window's height with
respect to its parent.  The sum of the normal heights of all windows in a
vertical combination equals 1.0.

Similarly, the normal width of a frame's root window or a window that is
vertically combined equals 1.0.  The normal width of a window that is
horizontally combined is the fraction of the window's width with respect
to its parent.  The sum of the normal widths of all windows in a
horizontal combination equals 1.0.

The normal sizes of windows are used to restore the proportional sizes
of windows after they have been shrunk to their minimum sizes; for
example when a frame is temporarily made very small and afterwards gets
re-enlarged to its previous size.  */)
  (Lisp_Object window, Lisp_Object horizontal)
{
  struct window *w = decode_valid_window (window);

  return NILP (horizontal) ? w->normal_lines : w->normal_cols;
}

DEFUN ("window-new-normal", Fwindow_new_normal, Swindow_new_normal, 0, 1, 0,
       doc: /* Return new normal size of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.

The new normal size of WINDOW is the value set by the last call of
`set-window-new-normal' for WINDOW.  If valid, it will be shortly
installed as WINDOW's normal size (see `window-normal-size').  */)
  (Lisp_Object window)
{
  return decode_valid_window (window)->new_normal;
}

DEFUN ("window-new-pixel", Fwindow_new_pixel, Swindow_new_pixel, 0, 1, 0,
       doc: /* Return new pixel size of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.

The new pixel size of WINDOW is the value set by the last call of
`set-window-new-pixel' for WINDOW.  If it is valid, it will be shortly
installed as WINDOW's pixel height (see `window-pixel-height') or pixel
width (see `window-pixel-width').  */)
  (Lisp_Object window)
{
  return decode_valid_window (window)->new_pixel;
}

DEFUN ("window-pixel-left", Fwindow_pixel_left, Swindow_pixel_left, 0, 1, 0,
       doc: /* Return left pixel edge of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (decode_valid_window (window)->pixel_left);
}

DEFUN ("window-pixel-top", Fwindow_pixel_top, Swindow_pixel_top, 0, 1, 0,
       doc: /* Return top pixel edge of window WINDOW.
WINDOW must be a valid window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (decode_valid_window (window)->pixel_top);
}

DEFUN ("window-left-column", Fwindow_left_column, Swindow_left_column, 0, 1, 0,
       doc: /* Return left column of window WINDOW.
This is the distance, in columns, between the left edge of WINDOW and
the left edge of the frame's window area.  For instance, the return
value is 0 if there is no window to the left of WINDOW.

WINDOW must be a valid window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (decode_valid_window (window)->left_col);
}

DEFUN ("window-top-line", Fwindow_top_line, Swindow_top_line, 0, 1, 0,
       doc: /* Return top line of window WINDOW.
This is the distance, in lines, between the top of WINDOW and the top
of the frame's window area.  For instance, the return value is 0 if
there is no window above WINDOW.

WINDOW must be a valid window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (decode_valid_window (window)->top_line);
}

static enum window_body_unit
window_body_unit_from_symbol (Lisp_Object unit)
{
  return
    EQ (unit, Qremap)
    ? WINDOW_BODY_IN_REMAPPED_CHARS
    : (NILP (unit)
       ? WINDOW_BODY_IN_CANONICAL_CHARS
       : WINDOW_BODY_IN_PIXELS);
}

/* Return the number of lines/pixels of W's body.  Don't count any mode
   or header line or horizontal divider of W.  Rounds down to nearest
   integer when not working pixelwise. */
static int
window_body_height (struct window *w, enum window_body_unit pixelwise)
{
  int height = (w->pixel_height
		- WINDOW_TAB_LINE_HEIGHT (w)
		- WINDOW_HEADER_LINE_HEIGHT (w)
		- (WINDOW_HAS_HORIZONTAL_SCROLL_BAR (w)
		   ? WINDOW_SCROLL_BAR_AREA_HEIGHT (w)
		   : 0)
		- WINDOW_MODE_LINE_HEIGHT (w)
		- WINDOW_BOTTOM_DIVIDER_WIDTH (w));

  int denom = 1;
  if (pixelwise == WINDOW_BODY_IN_REMAPPED_CHARS)
    {
      if (!NILP (Vface_remapping_alist))
	{
	  struct frame *f = XFRAME (WINDOW_FRAME (w));
	  int face_id = lookup_named_face (NULL, f, Qdefault, true);
	  struct face *face = FACE_FROM_ID_OR_NULL (f, face_id);
	  if (face && face->font && face->font->height)
	    denom = face->font->height;
	}
      /* For performance, use canonical chars if no face remapping.  */
      else
	pixelwise = WINDOW_BODY_IN_CANONICAL_CHARS;
    }

  if (pixelwise == WINDOW_BODY_IN_CANONICAL_CHARS)
    denom = FRAME_LINE_HEIGHT (WINDOW_XFRAME (w));

  /* Don't return a negative value.  */
  return max (height / denom, 0);
}

/* Return the number of columns/pixels of W's body.  Don't count columns
   occupied by the scroll bar or the divider/vertical bar separating W
   from its right sibling or margins.  On window-systems don't count
   fringes either.  Round down to nearest integer when not working
   pixelwise.  */
int
window_body_width (struct window *w, enum window_body_unit pixelwise)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));

  int width = (w->pixel_width
	       - WINDOW_RIGHT_DIVIDER_WIDTH (w)
	       - (WINDOW_HAS_VERTICAL_SCROLL_BAR (w)
		  ? WINDOW_SCROLL_BAR_AREA_WIDTH (w)
		  : (/* A vertical bar is either 1 or 0.  */
		     !FRAME_WINDOW_P (f)
		     && !WINDOW_RIGHTMOST_P (w)
		     && !WINDOW_RIGHT_DIVIDER_WIDTH (w)))
		- WINDOW_MARGINS_WIDTH (w)
		- (FRAME_WINDOW_P (f)
		   ? WINDOW_FRINGES_WIDTH (w)
		   : 0));

  int denom = 1;
  if (pixelwise == WINDOW_BODY_IN_REMAPPED_CHARS)
    {
      if (!NILP (Vface_remapping_alist))
	{
	  int face_id = lookup_named_face (NULL, f, Qdefault, true);
	  struct face *face = FACE_FROM_ID_OR_NULL (f, face_id);
	  if (face && face->font)
	    {
	      if (face->font->average_width)
		denom = face->font->average_width;
	      else if (face->font->space_width)
		denom = face->font->space_width;
	    }
	}
      /* For performance, use canonical chars if no face remapping.  */
      else
	pixelwise = WINDOW_BODY_IN_CANONICAL_CHARS;
    }

  if (pixelwise == WINDOW_BODY_IN_CANONICAL_CHARS)
    denom = FRAME_COLUMN_WIDTH (WINDOW_XFRAME (w));

  /* Don't return a negative value.  */
  return max (width / denom, 0);
}

DEFUN ("window-body-width", Fwindow_body_width, Swindow_body_width, 0, 2, 0,
       doc: /* Return the width of WINDOW's text area.
WINDOW must be a live window and defaults to the selected one.  The
return value does not include any vertical dividers, fringes or
marginal areas, or scroll bars.

The optional argument PIXELWISE defines the units to use for the
width.  If nil, return the largest integer smaller than WINDOW's pixel
width in units of the character width of WINDOW's frame.  If PIXELWISE
is `remap' and the default face is remapped (see
`face-remapping-alist'), use the remapped face to determine the
character width.  For any other non-nil value, return the width in
pixels.

Note that the returned value includes the column reserved for the
continuation glyph.

Also see `window-max-chars-per-line'.  */)
  (Lisp_Object window, Lisp_Object pixelwise)
{
  return (make_fixnum
	  (window_body_width (decode_live_window (window),
			      window_body_unit_from_symbol (pixelwise))));
}

DEFUN ("window-body-height", Fwindow_body_height, Swindow_body_height, 0, 2, 0,
       doc: /* Return the height of WINDOW's text area.
WINDOW must be a live window and defaults to the selected one.  The
return value does not include the mode line or header line or any
horizontal divider.

The optional argument PIXELWISE defines the units to use for the
height.  If nil, return the largest integer smaller than WINDOW's
pixel height in units of the character height of WINDOW's frame.  If
PIXELWISE is `remap' and the default face is remapped (see
`face-remapping-alist'), use the remapped face to determine the
character height.  For any other non-nil value, return the height in
pixels.  */)
  (Lisp_Object window, Lisp_Object pixelwise)
{
  return (make_fixnum
	  (window_body_height (decode_live_window (window),
			       window_body_unit_from_symbol (pixelwise))));
}

DEFUN ("window-old-body-pixel-width",
       Fwindow_old_body_pixel_width,
       Swindow_old_body_pixel_width, 0, 1, 0,
       doc: /* Return old width of WINDOW's text area in pixels.
WINDOW must be a live window and defaults to the selected one.

The return value is the pixel width of WINDOW's text area after the
last time window change functions found WINDOW live on its frame.  It
is zero if WINDOW was created after that.  */)
  (Lisp_Object window)
{
  return (make_fixnum
	  (decode_live_window (window)->old_body_pixel_width));
}

DEFUN ("window-old-body-pixel-height",
       Fwindow_old_body_pixel_height,
       Swindow_old_body_pixel_height, 0, 1, 0,
       doc: /* Return old height of WINDOW's text area in pixels.
WINDOW must be a live window and defaults to the selected one.

The return value is the pixel height of WINDOW's text area after the
last time window change functions found WINDOW live on its frame.  It
is zero if WINDOW was created after that.  */)
  (Lisp_Object window)
{
  return (make_fixnum
	  (decode_live_window (window)->old_body_pixel_height));
}

DEFUN ("window-mode-line-height", Fwindow_mode_line_height,
       Swindow_mode_line_height, 0, 1, 0,
       doc: /* Return the height in pixels of WINDOW's mode-line.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (WINDOW_MODE_LINE_HEIGHT (decode_live_window (window)));
}

DEFUN ("window-header-line-height", Fwindow_header_line_height,
       Swindow_header_line_height, 0, 1, 0,
       doc: /* Return the height in pixels of WINDOW's header-line.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (WINDOW_HEADER_LINE_HEIGHT (decode_live_window (window)));
}

DEFUN ("window-tab-line-height", Fwindow_tab_line_height,
       Swindow_tab_line_height, 0, 1, 0,
       doc: /* Return the height in pixels of WINDOW's tab-line.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (WINDOW_TAB_LINE_HEIGHT (decode_live_window (window)));
}

DEFUN ("window-right-divider-width", Fwindow_right_divider_width,
       Swindow_right_divider_width, 0, 1, 0,
       doc: /* Return the width in pixels of WINDOW's right divider.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (WINDOW_RIGHT_DIVIDER_WIDTH (decode_live_window (window)));
}

DEFUN ("window-bottom-divider-width", Fwindow_bottom_divider_width,
       Swindow_bottom_divider_width, 0, 1, 0,
       doc: /* Return the width in pixels of WINDOW's bottom divider.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (WINDOW_BOTTOM_DIVIDER_WIDTH (decode_live_window (window)));
}

DEFUN ("window-scroll-bar-width", Fwindow_scroll_bar_width,
       Swindow_scroll_bar_width, 0, 1, 0,
       doc: /* Return the width in pixels of WINDOW's vertical scrollbar.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (WINDOW_SCROLL_BAR_AREA_WIDTH (decode_live_window (window)));
}

DEFUN ("window-scroll-bar-height", Fwindow_scroll_bar_height,
       Swindow_scroll_bar_height, 0, 1, 0,
       doc: /* Return the height in pixels of WINDOW's horizontal scrollbar.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (WINDOW_SCROLL_BAR_AREA_HEIGHT (decode_live_window (window)));
}

DEFUN ("window-hscroll", Fwindow_hscroll, Swindow_hscroll, 0, 1, 0,
       doc: /* Return the number of columns by which WINDOW is scrolled from left margin.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return make_fixnum (decode_live_window (window)->hscroll);
}

/* Set W's horizontal scroll amount to HSCROLL clipped to a reasonable
   range, returning the new amount as a fixnum.  */
static Lisp_Object
set_window_hscroll (struct window *w, EMACS_INT hscroll)
{
  /* Horizontal scrolling has problems with large scroll amounts.
     It's too slow with long lines, and even with small lines the
     display can be messed up.  For now, though, impose only the limits
     required by the internal representation: horizontal scrolling must
     fit in fixnum (since it's visible to Elisp) and into ptrdiff_t
     (since it's stored in a ptrdiff_t).  */
  ptrdiff_t hscroll_max = min (MOST_POSITIVE_FIXNUM, PTRDIFF_MAX);
  ptrdiff_t new_hscroll = clip_to_bounds (0, hscroll, hscroll_max);

  /* Prevent redisplay shortcuts when changing the hscroll.  */
  if (w->hscroll != new_hscroll)
    {
      XBUFFER (w->contents)->prevent_redisplay_optimizations_p = true;
      wset_redisplay (w);
    }

  w->hscroll = new_hscroll;
  w->suspend_auto_hscroll = true;

  return make_fixnum (new_hscroll);
}

DEFUN ("set-window-hscroll", Fset_window_hscroll, Sset_window_hscroll, 2, 2, 0,
       doc: /* Set number of columns WINDOW is scrolled from left margin to NCOL.
WINDOW must be a live window and defaults to the selected one.
Clip the number to a reasonable value if out of range.
Return the new number.  NCOL should be zero or positive.

Note that if `auto-hscroll-mode' is non-nil, you cannot scroll the
window so that the location of point moves off-window.  */)
  (Lisp_Object window, Lisp_Object ncol)
{
  CHECK_FIXNUM (ncol);
  return set_window_hscroll (decode_live_window (window), XFIXNUM (ncol));
}

/* Test if the character at column X, row Y is within window W.
   If it is not, return ON_NOTHING;
   if it is on the window's vertical divider, return
      ON_RIGHT_DIVIDER;
   if it is on the window's horizontal divider, return
      ON_BOTTOM_DIVIDER;
   if it is in the window's text area, return ON_TEXT;
   if it is on the window's modeline, return ON_MODE_LINE;
   if it is on the border between the window and its right sibling,
      return ON_VERTICAL_BORDER;
   if it is on a scroll bar, return ON_SCROLL_BAR;
   if it is on the window's top line, return ON_TAB_LINE;
   if it is on the window's header line, return ON_HEADER_LINE;
   if it is in left or right fringe of the window,
      return ON_LEFT_FRINGE or ON_RIGHT_FRINGE;
   if it is in the marginal area to the left/right of the window,
      return ON_LEFT_MARGIN or ON_RIGHT_MARGIN.

   X and Y are frame relative pixel coordinates.  */

static enum window_part
coordinates_in_window (register struct window *w, int x, int y)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  enum window_part part;
  int ux = FRAME_COLUMN_WIDTH (f);
  int left_x = WINDOW_LEFT_EDGE_X (w);
  int right_x = WINDOW_RIGHT_EDGE_X (w);
  int top_y = WINDOW_TOP_EDGE_Y (w);
  int bottom_y = WINDOW_BOTTOM_EDGE_Y (w);
  /* The width of the area where the vertical line can be dragged.
     (Between mode lines for instance.  */
  int grabbable_width = ux;
  int lmargin_width, rmargin_width, text_left, text_right;

  /* Outside any interesting row or column?  */
  if (y < top_y || y >= bottom_y || x < left_x || x >= right_x)
    return ON_NOTHING;

  /* On the horizontal window divider (which prevails the vertical
     divider)?  */
  if (WINDOW_BOTTOM_DIVIDER_WIDTH (w) > 0
      && y >= (bottom_y - WINDOW_BOTTOM_DIVIDER_WIDTH (w))
      && y <= bottom_y)
    return ON_BOTTOM_DIVIDER;
  /* On vertical window divider?  */
  else if (!WINDOW_RIGHTMOST_P (w)
	   && WINDOW_RIGHT_DIVIDER_WIDTH (w) > 0
	   && x >= right_x - WINDOW_RIGHT_DIVIDER_WIDTH (w)
	   && x <= right_x)
    return ON_RIGHT_DIVIDER;
  /* On the horizontal scroll bar?  (Including the empty space at its
     right!)  */
  else if ((WINDOW_HAS_HORIZONTAL_SCROLL_BAR (w)
	    && y >= (bottom_y
		     - WINDOW_SCROLL_BAR_AREA_HEIGHT (w)
		     - CURRENT_MODE_LINE_HEIGHT (w)
		     - WINDOW_BOTTOM_DIVIDER_WIDTH (w))
	    && y <= (bottom_y
		     - CURRENT_MODE_LINE_HEIGHT (w)
		     - WINDOW_BOTTOM_DIVIDER_WIDTH (w))))
    return ON_HORIZONTAL_SCROLL_BAR;
  /* On the mode or header/tab line?   */
  else if ((window_wants_mode_line (w)
	    && y >= (bottom_y
		     - CURRENT_MODE_LINE_HEIGHT (w)
		     - WINDOW_BOTTOM_DIVIDER_WIDTH (w))
	    && y <= bottom_y - WINDOW_BOTTOM_DIVIDER_WIDTH (w)
	    && (part = ON_MODE_LINE))
	   || (window_wants_tab_line (w)
	       && y < top_y + CURRENT_TAB_LINE_HEIGHT (w)
	       && (part = ON_TAB_LINE))
	   || (window_wants_header_line (w)
	       && y < top_y + CURRENT_HEADER_LINE_HEIGHT (w)
	       + (window_wants_tab_line (w)
		  ? CURRENT_TAB_LINE_HEIGHT (w)
		  : 0)
	       && (part = ON_HEADER_LINE)))
    {
      /* If it's under/over the scroll bar portion of the mode/header
	 line, say it's on the vertical line.  That's to be able to
	 resize windows horizontally in case we're using toolkit scroll
	 bars.  Note: If scrollbars are on the left, the window that
	 must be eventually resized is that on the left of WINDOW.  */
      if ((WINDOW_RIGHT_DIVIDER_WIDTH (w) == 0)
	  && ((WINDOW_HAS_VERTICAL_SCROLL_BAR_ON_LEFT (w)
	       && !WINDOW_LEFTMOST_P (w)
	       && eabs (x - left_x) < grabbable_width)
	      || (!WINDOW_HAS_VERTICAL_SCROLL_BAR_ON_LEFT (w)
		  && !WINDOW_RIGHTMOST_P (w)
		  && eabs (x - right_x) < grabbable_width)))
	return ON_VERTICAL_BORDER;
      else
	return part;
    }

  /* In what's below, we subtract 1 when computing right_x because we
     want the rightmost pixel, which is given by left_pixel+width-1.  */
  if (w->pseudo_window_p)
    {
      left_x = 0;
      right_x = WINDOW_PIXEL_WIDTH (w) - 1;
    }
  else
    {
      left_x = WINDOW_BOX_LEFT_EDGE_X (w);
      right_x = WINDOW_BOX_RIGHT_EDGE_X (w) - 1;
    }

  /* Outside any interesting column?  */
  if (x < left_x || x > right_x)
    return ON_VERTICAL_SCROLL_BAR;

  lmargin_width = window_box_width (w, LEFT_MARGIN_AREA);
  rmargin_width = window_box_width (w, RIGHT_MARGIN_AREA);

  text_left = window_box_left (w, TEXT_AREA);
  text_right = text_left + window_box_width (w, TEXT_AREA);

  if (FRAME_WINDOW_P (f))
    {
      if (!w->pseudo_window_p
	  && WINDOW_RIGHT_DIVIDER_WIDTH (w) == 0
	  && !WINDOW_HAS_VERTICAL_SCROLL_BAR (w)
	  && !WINDOW_RIGHTMOST_P (w)
	  && (eabs (x - right_x) < grabbable_width))
	return ON_VERTICAL_BORDER;
    }
  /* Need to say "x > right_x" rather than >=, since on character
     terminals, the vertical line's x coordinate is right_x.  */
  else if (!w->pseudo_window_p
	   && WINDOW_RIGHT_DIVIDER_WIDTH (w) == 0
	   && !WINDOW_RIGHTMOST_P (w)
	   /* Why check ux if we are not the rightmost window?  Also
	      shouldn't a pseudo window always be rightmost?  */
	   && x > right_x - ux)
    return ON_VERTICAL_BORDER;

  if (x < text_left)
    {
      if (lmargin_width > 0
	  && (WINDOW_HAS_FRINGES_OUTSIDE_MARGINS (w)
	      ? (x >= left_x + WINDOW_LEFT_FRINGE_WIDTH (w))
	      : (x < left_x + lmargin_width)))
	return ON_LEFT_MARGIN;
      else
	return ON_LEFT_FRINGE;
    }

  if (x >= text_right)
    {
      if (rmargin_width > 0
	  && (WINDOW_HAS_FRINGES_OUTSIDE_MARGINS (w)
	      ? (x < right_x - WINDOW_RIGHT_FRINGE_WIDTH (w))
	      : (x >= right_x - rmargin_width)))
	return ON_RIGHT_MARGIN;
      else
	return ON_RIGHT_FRINGE;
    }

  /* Everything special ruled out - must be on text area */
  return ON_TEXT;
}

/* Take X is the frame-relative pixel x-coordinate, and return the
   x-coordinate relative to part PART of window W. */
int
window_relative_x_coord (struct window *w, enum window_part part, int x)
{
  int left_x = (w->pseudo_window_p) ? 0 : WINDOW_BOX_LEFT_EDGE_X (w);

  switch (part)
    {
    case ON_TEXT:
      return x - window_box_left (w, TEXT_AREA);

    case ON_TAB_LINE:
    case ON_HEADER_LINE:
    case ON_MODE_LINE:
    case ON_LEFT_FRINGE:
      return x - left_x;

    case ON_RIGHT_FRINGE:
      return x - left_x - WINDOW_LEFT_FRINGE_WIDTH (w);

    case ON_LEFT_MARGIN:
      return (x - left_x
	      - ((WINDOW_HAS_FRINGES_OUTSIDE_MARGINS (w))
		 ? WINDOW_LEFT_FRINGE_WIDTH (w) : 0));

    case ON_RIGHT_MARGIN:
      return (x + 1
	      - ((w->pseudo_window_p)
		 ? WINDOW_PIXEL_WIDTH (w)
		 : WINDOW_BOX_RIGHT_EDGE_X (w))
	      + window_box_width (w, RIGHT_MARGIN_AREA)
	      + ((WINDOW_HAS_FRINGES_OUTSIDE_MARGINS (w))
		 ? WINDOW_RIGHT_FRINGE_WIDTH (w) : 0));

    case ON_NOTHING:
    case ON_VERTICAL_BORDER:
    case ON_VERTICAL_SCROLL_BAR:
    case ON_HORIZONTAL_SCROLL_BAR:
    case ON_RIGHT_DIVIDER:
    case ON_BOTTOM_DIVIDER:
      return 0;

    default:
      emacs_abort ();
    }
}


DEFUN ("coordinates-in-window-p", Fcoordinates_in_window_p,
       Scoordinates_in_window_p, 2, 2, 0,
       doc: /* Return non-nil if COORDINATES are in WINDOW.
WINDOW must be a live window and defaults to the selected one.
COORDINATES is a cons of the form (X . Y), X and Y being distances
measured in characters from the upper-left corner of the frame.
\(0 . 0) denotes the character in the upper left corner of the
frame.
If COORDINATES are in the text portion of WINDOW,
   the coordinates relative to the window are returned.
If they are in the bottom divider of WINDOW, `bottom-divider' is returned.
If they are in the right divider of WINDOW, `right-divider' is returned.
If they are in the mode line of WINDOW, `mode-line' is returned.
If they are in the header line of WINDOW, `header-line' is returned.
If they are in the tab line of WINDOW, `tab-line' is returned.
If they are in the left fringe of WINDOW, `left-fringe' is returned.
If they are in the right fringe of WINDOW, `right-fringe' is returned.
If they are on the border between WINDOW and its right sibling,
  `vertical-line' is returned.
If they are in the windows's left or right marginal areas, `left-margin'\n\
  or `right-margin' is returned.  */)
  (register Lisp_Object coordinates, Lisp_Object window)
{
  struct window *w;
  struct frame *f;
  int x, y;
  Lisp_Object lx, ly;

  w = decode_live_window (window);
  f = XFRAME (w->frame);
  CHECK_CONS (coordinates);
  lx = Fcar (coordinates);
  ly = Fcdr (coordinates);
  CHECK_NUMBER (lx);
  CHECK_NUMBER (ly);
  x = FRAME_PIXEL_X_FROM_CANON_X (f, lx) + FRAME_INTERNAL_BORDER_WIDTH (f);
  y = FRAME_PIXEL_Y_FROM_CANON_Y (f, ly) + FRAME_INTERNAL_BORDER_WIDTH (f);

  switch (coordinates_in_window (w, x, y))
    {
    case ON_NOTHING:
      return Qnil;

    case ON_TEXT:
      /* Convert X and Y to window relative pixel coordinates, and
	 return the canonical char units.  */
      x -= window_box_left (w, TEXT_AREA);
      y -= WINDOW_TOP_EDGE_Y (w);
      return Fcons (FRAME_CANON_X_FROM_PIXEL_X (f, x),
		    FRAME_CANON_Y_FROM_PIXEL_Y (f, y));

    case ON_MODE_LINE:
      return Qmode_line;

    case ON_VERTICAL_BORDER:
      return Qvertical_line;

    case ON_HEADER_LINE:
      return Qheader_line;

    case ON_TAB_LINE:
      return Qtab_line;

    case ON_LEFT_FRINGE:
      return Qleft_fringe;

    case ON_RIGHT_FRINGE:
      return Qright_fringe;

    case ON_LEFT_MARGIN:
      return Qleft_margin;

    case ON_RIGHT_MARGIN:
      return Qright_margin;

    case ON_VERTICAL_SCROLL_BAR:
      /* Historically we are supposed to return nil in this case.  */
      return Qnil;

    case ON_HORIZONTAL_SCROLL_BAR:
      return Qnil;

    case ON_RIGHT_DIVIDER:
      return Qright_divider;

    case ON_BOTTOM_DIVIDER:
      return Qbottom_divider;

    default:
      emacs_abort ();
    }
}


/* Callback for foreach_window, used in window_from_coordinates.
   Check if window W contains coordinates specified by USER_DATA which
   is actually a pointer to a struct check_window_data CW.

   Check if window W contains coordinates *CW->x and *CW->y.  If it
   does, return W in *CW->window, as Lisp_Object, and return in
   *CW->part the part of the window under coordinates *X,*Y.  Return
   false from this function to stop iterating over windows.  */

struct check_window_data
{
  Lisp_Object *window;
  int x, y;
  enum window_part *part;
};

static bool
check_window_containing (struct window *w, void *user_data)
{
  struct check_window_data *cw = user_data;
  enum window_part found = coordinates_in_window (w, cw->x, cw->y);
  if (found == ON_NOTHING)
    return true;
  else
    {
      *cw->part = found;
      XSETWINDOW (*cw->window, w);
      return false;
    }
}


/* Find the window containing frame-relative pixel position X/Y and
   return it as a Lisp_Object.

   If X, Y is on one of the window's special `window_part' elements,
   set *PART to the id of that element.

   If there is no window under X, Y return nil and leave *PART
   unmodified.  TOOL_BAR_P means detect tool-bar windows, and
   TAB_BAR_P means detect tab-bar windows.

   This function was previously implemented with a loop cycling over
   windows with Fnext_window, and starting with the frame's selected
   window.  It turned out that this doesn't work with an
   implementation of next_window using Vwindow_list, because
   FRAME_SELECTED_WINDOW (F) is not always contained in the window
   tree of F when this function is called asynchronously from
   note_mouse_highlight.  The original loop didn't terminate in this
   case.  */

Lisp_Object
window_from_coordinates (struct frame *f, int x, int y,
			 enum window_part *part, bool menu_bar_p,
			 bool tab_bar_p, bool tool_bar_p)
{
  Lisp_Object window;
  struct check_window_data cw;
  enum window_part dummy;

  if (part == 0)
    part = &dummy;

  window = Qnil;
  cw.window = &window, cw.x = x, cw.y = y; cw.part = part;
  foreach_window (f, check_window_containing, &cw);

#if defined (HAVE_WINDOW_SYSTEM) && ! defined (HAVE_EXT_MENU_BAR)
  /* If not found above, see if it's in the menu bar window, if a menu
     bar exists.  */
  if (NILP (window)
      && menu_bar_p
      && WINDOWP (f->menu_bar_window)
      && WINDOW_TOTAL_LINES (XWINDOW (f->menu_bar_window)) > 0
      && (coordinates_in_window (XWINDOW (f->menu_bar_window), x, y)
	  != ON_NOTHING))
    {
      *part = ON_TEXT;
      window = f->menu_bar_window;
    }
#endif

#if defined (HAVE_WINDOW_SYSTEM)
  /* If not found above, see if it's in the tab bar window, if a tab
     bar exists.  */
  if (NILP (window)
      && tab_bar_p
      && WINDOWP (f->tab_bar_window)
      && WINDOW_TOTAL_LINES (XWINDOW (f->tab_bar_window)) > 0
      && (coordinates_in_window (XWINDOW (f->tab_bar_window), x, y)
	  != ON_NOTHING))
    {
      *part = ON_TEXT;
      window = f->tab_bar_window;
    }
#endif

#if defined (HAVE_WINDOW_SYSTEM) && ! defined (HAVE_EXT_TOOL_BAR)
  /* If not found above, see if it's in the tool bar window, if a tool
     bar exists.  */
  if (NILP (window)
      && tool_bar_p
      && WINDOWP (f->tool_bar_window)
      && WINDOW_TOTAL_LINES (XWINDOW (f->tool_bar_window)) > 0
      && (coordinates_in_window (XWINDOW (f->tool_bar_window), x, y)
	  != ON_NOTHING))
    {
      *part = ON_TEXT;
      window = f->tool_bar_window;
    }
#endif

  return window;
}

DEFUN ("window-at", Fwindow_at, Swindow_at, 2, 3, 0,
       doc: /* Return window containing coordinates X and Y on FRAME.
FRAME must be a live frame and defaults to the selected one.
X and Y are measured in units of canonical columns and rows.
The top left corner of the frame is considered to be column 0, row 0.
Tool-bar and tab-bar pseudo-windows are ignored by this function: if
the specified coordinates are in any of these two windows, this
function returns nil.  */)
  (Lisp_Object x, Lisp_Object y, Lisp_Object frame)
{
  struct frame *f = decode_live_frame (frame);

  CHECK_NUMBER (x);
  CHECK_NUMBER (y);

  return window_from_coordinates (f,
				  (FRAME_PIXEL_X_FROM_CANON_X (f, x)
				   + FRAME_INTERNAL_BORDER_WIDTH (f)),
				  (FRAME_PIXEL_Y_FROM_CANON_Y (f, y)
				   + FRAME_INTERNAL_BORDER_WIDTH (f)),
				  0, false, false, false);
}

ptrdiff_t
window_point (struct window *w)
{
  return (w == XWINDOW (selected_window)
          ? BUF_PT (XBUFFER (w->contents))
          : XMARKER (w->pointm)->charpos);
}

DEFUN ("window-point", Fwindow_point, Swindow_point, 0, 1, 0,
       doc: /* Return current value of point in WINDOW.
WINDOW must be a live window and defaults to the selected one.

For a nonselected window, this is the value point would have if that
window were selected.

Note that, when WINDOW is selected, the value returned is the same as
that returned by `point' for WINDOW's buffer.  It would be more strictly
correct to return the top-level value of `point', outside of any
`save-excursion' forms.  But that is hard to define.  */)
  (Lisp_Object window)
{
  return make_fixnum (window_point (decode_live_window (window)));
}

DEFUN ("window-old-point", Fwindow_old_point, Swindow_old_point, 0, 1, 0,
       doc: /* Return old value of point in WINDOW.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return Fmarker_position (decode_live_window (window)->old_pointm);
}

DEFUN ("window-start", Fwindow_start, Swindow_start, 0, 1, 0,
       doc: /* Return position at which display currently starts in WINDOW.
WINDOW must be a live window and defaults to the selected one.
This is updated by redisplay or by calling `set-window-start'.  */)
  (Lisp_Object window)
{
  return Fmarker_position (decode_live_window (window)->start);
}

/* This is text temporarily removed from the doc string below.

This function returns nil if the position is not currently known.
That happens when redisplay is preempted and doesn't finish.
If in that case you want to compute where the end of the window would
have been if redisplay had finished, do this:
    (save-excursion
      (goto-char (window-start window))
      (vertical-motion (1- (window-height window)) window)
      (point))")  */

DEFUN ("window-end", Fwindow_end, Swindow_end, 0, 2, 0,
       doc: /* Return position after final character in WINDOW.
If UPDATE, recompute that position.  */)
  (Lisp_Object window, Lisp_Object update)
{
  Lisp_Object value;
  struct buffer *b;
  struct window *w = decode_live_window (window);

  CHECK_BUFFER (w->contents);
  b = XBUFFER (w->contents);

  if (!NILP (update)
      && !noninteractive
      && (windows_or_buffers_changed
	  || !w->window_end_valid
	  || b->clip_changed
	  || b->prevent_redisplay_optimizations_p
	  || window_outdated (w))
      /* i.e., not daemon (Bug#20565).  */
      && !FRAME_INITIAL_P (WINDOW_XFRAME (w)))
    {
      struct text_pos startp;
      struct it it;
      struct buffer *restore_current = NULL;
      void *itdata = NULL;

      if (b != current_buffer)
	{
	  restore_current = current_buffer;
	  set_buffer_internal (b);
	}

      CLIP_TEXT_POS_FROM_MARKER (startp, w->start);

      itdata = bidi_shelve_cache ();
      start_move_it (&it, w, startp);
      move_it_dy (&it, window_box_height (w));
      move_it_dvpos (&it, 1); /* formerly move_it_past_eol.  */
      value = make_fixnum (IT_CHARPOS (it));
      bidi_unshelve_cache (itdata, false);

      if (restore_current)
	set_buffer_internal (restore_current);
    }
  else
    XSETINT (value, BUF_Z (b) - w->window_end_pos);

  return value;
}

DEFUN ("set-window-point", Fset_window_point, Sset_window_point, 2, 2, 0,
       doc: /* Make point value in WINDOW be at position POS in WINDOW's buffer.
WINDOW must be a live window and defaults to the selected one.
Return POS.  */)
  (Lisp_Object window, Lisp_Object pos)
{
  register struct window *w = decode_live_window (window);

  /* Type of POS is checked by Fgoto_char or set_marker_restricted ...  */

  if (w == XWINDOW (selected_window))
    {
      if (XBUFFER (w->contents) == current_buffer)
	Fgoto_char (pos);
      else
	{
	  struct buffer *old_buffer = current_buffer;

	  /* ... but here we want to catch type error before buffer change.  */
	  CHECK_FIXNUM_COERCE_MARKER (pos);
	  set_buffer_internal (XBUFFER (w->contents));
	  Fgoto_char (pos);
	  set_buffer_internal (old_buffer);
	}
    }
  else
    {
      set_marker_restricted (w->pointm, pos, w->contents);
      /* We have to make sure that redisplay updates the window to show
	 the new value of point.  */
      wset_redisplay (w);
    }

  return pos;
}

DEFUN ("set-window-start", Fset_window_start, Sset_window_start, 2, 3, 0,
       doc: /* Make display in WINDOW start at position POS in WINDOW's buffer.
WINDOW must be a live window and defaults to the selected one.  Return
POS.

Optional third arg NOFORCE non-nil prevents next redisplay from
moving point if displaying the window at POS makes point invisible;
redisplay will then choose the WINDOW's start position by itself in
that case, i.e. it will disregard POS if adhering to it will make
point not visible in the window.

For reliable setting of WINDOW start position, make sure point is
at a position that will be visible when that start is in effect,
otherwise there's a chance POS will be disregarded, e.g., if point
winds up in a partially-visible line.

The setting of the WINDOW's start position takes effect during the
next redisplay cycle, not immediately.  If NOFORCE is nil or
omitted, forcing the display of WINDOW to start at POS cancels
any setting of WINDOW's vertical scroll (\"vscroll\") amount
set by `set-window-vscroll' and by scrolling functions.  */)
  (Lisp_Object window, Lisp_Object pos, Lisp_Object noforce)
{
  register struct window *w = decode_live_window (window);

  set_marker_restricted (w->start, pos, w->contents);
  /* This is not right, but much easier than doing what is right.  */
  w->start_at_line_beg = false;
  if (NILP (noforce))
    w->force_start = true;
  wset_update_mode_line (w);
  /* Bug#15957.  */
  w->window_end_valid = false;
  wset_redisplay (w);

  return pos;
}

DEFUN ("pos-visible-in-window-p", Fpos_visible_in_window_p,
       Spos_visible_in_window_p, 0, 3, 0,
       doc: /* Return non-nil if position POS is currently on the frame in WINDOW.
WINDOW must be a live window and defaults to the selected one.

Return nil if that position is scrolled vertically out of view.  If a
character is only partially visible, nil is returned, unless the
optional argument PARTIALLY is non-nil.  If POS is only out of view
because of horizontal scrolling, return non-nil.  If POS is t, it
specifies either the first position displayed on the last visible
screen line in WINDOW, or the end-of-buffer position, whichever comes
first.  POS defaults to point in WINDOW; WINDOW defaults to the
selected window.

If POS is visible, return t if PARTIALLY is nil; if PARTIALLY is non-nil,
the return value is a list of 2 or 6 elements (X Y [RTOP RBOT ROWH VPOS]),
where X and Y are the pixel coordinates relative to the top left corner
of the window.  The remaining elements are omitted if the character after
POS is fully visible; otherwise, RTOP and RBOT are the number of pixels
off-window at the top and bottom of the screen line ("row") containing
POS, ROWH is the visible height of that row, and VPOS is the row number
\(zero-based).  */)
  (Lisp_Object pos, Lisp_Object window, Lisp_Object partially)
{
  struct window *w;
  EMACS_INT posint;
  struct buffer *buf;
  struct text_pos top;
  Lisp_Object in_window = Qnil;
  int rtop, rbot, rowh, vpos;
  bool fully_p = true;
  int x, y;

  w = decode_live_window (window);
  buf = XBUFFER (w->contents);
  SET_TEXT_POS_FROM_MARKER (top, w->start);

  if (EQ (pos, Qt))
    posint = -1;
  else if (!NILP (pos))
    posint = fix_position (pos);
  else if (w == XWINDOW (selected_window))
    posint = PT;
  else
    posint = marker_position (w->pointm);

  /* If position is above window start or outside buffer boundaries,
     or if window start is out of range, position is not visible.  */
  if ((EQ (pos, Qt)
       || (posint >= CHARPOS (top) && posint <= BUF_ZV (buf)))
      && CHARPOS (top) >= BUF_BEGV (buf)
      && CHARPOS (top) <= BUF_ZV (buf)
      && window_start_coordinates (w, posint, &x, &y, &rtop, &rbot, &rowh, &vpos))
    {
      fully_p = !rtop && !rbot;
      if (!NILP (partially) || fully_p)
	in_window = Qt;
    }

  if (!NILP (in_window) && !NILP (partially))
    {
      Lisp_Object part = Qnil;
      if (!fully_p)
	part = list4i (rtop, rbot, rowh, vpos);
      in_window = Fcons (make_fixnum (x),
			 Fcons (make_fixnum (y), part));
    }

  return in_window;
}

DEFUN ("window-line-height", Fwindow_line_height,
       Swindow_line_height, 0, 2, 0,
       doc: /* Return height in pixels of text line LINE in window WINDOW.
WINDOW must be a live window and defaults to the selected one.

Return height of current line if LINE is omitted or nil.  Return height of
header or mode line if LINE is `header-line' or `mode-line'.
Otherwise, LINE is a text line number starting from 0.  A negative number
counts from the end of the window.

Value is a list (HEIGHT VPOS YPOS OFFBOT), where HEIGHT is the height
in pixels of the visible part of the line, VPOS and YPOS are the
vertical position in lines and pixels of the line, relative to the top
of the first text line, and OFFBOT is the number of off-window pixels at
the bottom of the text line.  If there are off-window pixels at the top
of the (first) text line, YPOS is negative.

Return nil if window display is not up-to-date.  In that case, use
`pos-visible-in-window-p' to obtain the information.  */)
  (Lisp_Object line, Lisp_Object window)
{
  register struct window *w;
  register struct buffer *b;
  struct glyph_row *row, *end_row;
  int max_y, crop, i;
  EMACS_INT n;

  w = decode_live_window (window);

  if (noninteractive || w->pseudo_window_p)
    return Qnil;

  CHECK_BUFFER (w->contents);
  b = XBUFFER (w->contents);

  /* Fail if current matrix is not up-to-date.  */
  if (!w->window_end_valid
      || windows_or_buffers_changed
      || b->clip_changed
      || b->prevent_redisplay_optimizations_p
      || window_outdated (w))
    return Qnil;

  if (NILP (line))
    {
      i = w->cursor.vpos;
      if (i < 0 || i >= w->current_matrix->nrows
	  || (row = MATRIX_ROW (w->current_matrix, i), !row->enabled_p))
	return Qnil;
      max_y = window_text_bottom_y (w);
      goto found_row;
    }

  if (EQ (line, Qtab_line))
    {
      if (!window_wants_tab_line (w))
	return Qnil;
      row = MATRIX_TAB_LINE_ROW (w->current_matrix);
      return row->enabled_p ? list4i (row->height, 0, 0, 0) : Qnil;
    }

  if (EQ (line, Qheader_line))
    {
      if (!window_wants_header_line (w))
	return Qnil;
      row = MATRIX_HEADER_LINE_ROW (w->current_matrix);
      return row->enabled_p ? list4i (row->height, 0, 0, 0) : Qnil;
    }

  if (EQ (line, Qmode_line))
    {
      row = MATRIX_MODE_LINE_ROW (w->current_matrix);
      return (row->enabled_p ?
	      list4i (row->height,
		      0, /* not accurate */
		      (WINDOW_TAB_LINE_HEIGHT (w)
		       + WINDOW_HEADER_LINE_HEIGHT (w)
		       + window_text_bottom_y (w)),
		      0)
	      : Qnil);
    }

  CHECK_FIXNUM (line);
  n = XFIXNUM (line);

  row = MATRIX_FIRST_TEXT_ROW (w->current_matrix);
  end_row = MATRIX_BOTTOM_TEXT_ROW (w->current_matrix, w);
  max_y = window_text_bottom_y (w);
  i = 0;

  while ((n < 0 || i < n)
	 && row <= end_row && row->enabled_p
	 && row->y + row->height < max_y)
    row++, i++;

  if (row > end_row || !row->enabled_p)
    return Qnil;

  if (++n < 0)
    {
      if (-n > i)
	return Qnil;
      row += n;
      i += n;
    }

 found_row:
  crop = max (0, (row->y + row->height) - max_y);
  return list4i (row->height + min (0, row->y) - crop, i, row->y, crop);
}

DEFUN ("window-lines-pixel-dimensions", Fwindow_lines_pixel_dimensions, Swindow_lines_pixel_dimensions, 0, 6, 0,
       doc: /* Return pixel dimensions of WINDOW's lines.
The return value is a list of the x- and y-coordinates of the lower
right corner of the last character of each line.  Return nil if the
current glyph matrix of WINDOW is not up-to-date.

Optional argument WINDOW specifies the window whose lines' dimensions
shall be returned.  Nil or omitted means to return the dimensions for
the selected window.

FIRST, if non-nil, specifies the index of the first line whose
dimensions shall be returned.  If FIRST is nil and BODY is non-nil,
start with the first text line of WINDOW.  Otherwise, start with the
first line of WINDOW.

LAST, if non-nil, specifies the last line whose dimensions shall be
returned.  If LAST is nil and BODY is non-nil, the last line is the last
line of the body (text area) of WINDOW.  Otherwise, last is the last
line of WINDOW.

INVERSE, if nil, means that the y-pixel value returned for a specific
line specifies the distance in pixels from the left edge (body edge if
BODY is non-nil) of WINDOW to the right edge of the last glyph of that
line.  INVERSE non-nil means that the y-pixel value returned for a
specific line specifies the distance in pixels from the right edge of
the last glyph of that line to the right edge (body edge if BODY is
non-nil) of WINDOW.

LEFT non-nil means to return the x- and y-coordinates of the lower left
corner of the leftmost character on each line.  This is the value that
should be used for buffers that mostly display text from right to left.

If LEFT is non-nil and INVERSE is nil, this means that the y-pixel value
returned for a specific line specifies the distance in pixels from the
left edge of the last (leftmost) glyph of that line to the right edge
(body edge if BODY is non-nil) of WINDOW.  If LEFT and INVERSE are both
non-nil, the y-pixel value returned for a specific line specifies the
distance in pixels from the left edge (body edge if BODY is non-nil) of
WINDOW to the left edge of the last (leftmost) glyph of that line.

Normally, the value of this function is not available while Emacs is
busy, for example, when processing a command.  It should be retrievable
though when run from an idle timer with a delay of zero seconds.  */)
  (Lisp_Object window, Lisp_Object first, Lisp_Object last, Lisp_Object body, Lisp_Object inverse, Lisp_Object left)
{
  struct window *w = decode_live_window (window);
  struct buffer *b;
  struct glyph_row *row, *end_row;
  int max_y = NILP (body) ? WINDOW_PIXEL_HEIGHT (w) : window_text_bottom_y (w);
  Lisp_Object rows = Qnil;
  int window_width = NILP (body)
    ? w->pixel_width : window_body_width (w, WINDOW_BODY_IN_PIXELS);
  int tab_line_height = WINDOW_TAB_LINE_HEIGHT (w);
  int header_line_height = WINDOW_HEADER_LINE_HEIGHT (w);
  int subtract = NILP (body) ? 0 : (tab_line_height + header_line_height);
  bool invert = !NILP (inverse);
  bool left_flag = !NILP (left);

  if (noninteractive || w->pseudo_window_p)
    return Qnil;

  CHECK_BUFFER (w->contents);
  b = XBUFFER (w->contents);

  /* Fail if current matrix is not up-to-date.  */
  if (!w->window_end_valid
      || windows_or_buffers_changed
      || b->clip_changed
      || b->prevent_redisplay_optimizations_p
      || window_outdated (w))
    return Qnil;

  row = (!NILP (first)
	 ? MATRIX_ROW (w->current_matrix,
		       check_integer_range (first, 0,
					    w->current_matrix->nrows))
	 : NILP (body)
	 ? MATRIX_ROW (w->current_matrix, 0)
	 : MATRIX_FIRST_TEXT_ROW (w->current_matrix));
  end_row = (!NILP (last)
	     ? MATRIX_ROW (w->current_matrix,
			   check_integer_range (last, 0,
						w->current_matrix->nrows))
	     : NILP (body)
	     ? MATRIX_ROW (w->current_matrix, w->current_matrix->nrows)
	     : MATRIX_BOTTOM_TEXT_ROW (w->current_matrix, w));

  while (row <= end_row && row->enabled_p
	 && row->y + row->height < max_y)
    {

      if (left_flag)
	{
	  struct glyph *glyph = row->glyphs[TEXT_AREA];

	  rows = Fcons (Fcons (make_fixnum
			       (invert
				? glyph->pixel_width
				: window_width - glyph->pixel_width),
			       make_fixnum (row->y + row->height - subtract)),
			rows);
	}
      else
	rows = Fcons (Fcons (make_fixnum
			     (invert
			      ? window_width - row->pixel_width
			      : row->pixel_width),
			     make_fixnum (row->y + row->height - subtract)),
		      rows);
      row++;
    }

  return Fnreverse (rows);
}

DEFUN ("window-dedicated-p", Fwindow_dedicated_p, Swindow_dedicated_p,
       0, 1, 0,
       doc: /* Return non-nil when WINDOW is dedicated to its buffer.
More precisely, return the value assigned by the last call of
`set-window-dedicated-p' for WINDOW.  Return nil if that function was
never called with WINDOW as its argument, or the value set by that
function was internally reset since its last call.  WINDOW must be a
live window and defaults to the selected one.

When a window is dedicated to its buffer, `display-buffer' will refrain
from displaying another buffer in it.  `get-lru-window' and
`get-largest-window' treat dedicated windows specially.
`delete-windows-on', `replace-buffer-in-windows', `quit-window' and
`kill-buffer' can delete a dedicated window and the containing frame.

Functions like `set-window-buffer' may change the buffer displayed by a
window, unless that window is "strongly" dedicated to its buffer, that
is the value returned by `window-dedicated-p' is t.  */)
  (Lisp_Object window)
{
  return decode_live_window (window)->dedicated;
}

DEFUN ("set-window-dedicated-p", Fset_window_dedicated_p,
       Sset_window_dedicated_p, 2, 2, 0,
       doc: /* Mark WINDOW as dedicated according to FLAG.
WINDOW must be a live window and defaults to the selected one.  FLAG
non-nil means mark WINDOW as dedicated to its buffer.  FLAG nil means
mark WINDOW as non-dedicated.  Return FLAG.

When a window is dedicated to its buffer, `display-buffer' will refrain
from displaying another buffer in it.  `get-lru-window' and
`get-largest-window' treat dedicated windows specially.
`delete-windows-on', `replace-buffer-in-windows', `quit-window',
`quit-restore-window' and `kill-buffer' can delete a dedicated window
and the containing frame.

As a special case, if FLAG is t, mark WINDOW as "strongly" dedicated to
its buffer.  Functions like `set-window-buffer' may change the buffer
displayed by a window, unless that window is strongly dedicated to its
buffer.  If and when `set-window-buffer' displays another buffer in a
window, it also makes sure that the window is no more dedicated.  */)
  (Lisp_Object window, Lisp_Object flag)
{
  wset_dedicated (decode_live_window (window), flag);
  return flag;
}

DEFUN ("window-prev-buffers", Fwindow_prev_buffers, Swindow_prev_buffers,
       0, 1, 0,
       doc:  /* Return buffers previously shown in WINDOW.
WINDOW must be a live window and defaults to the selected one.

The return value is a list of elements (BUFFER WINDOW-START POS),
where BUFFER is a buffer, WINDOW-START is the start position of the
window for that buffer, and POS is a window-specific point value.  */)
  (Lisp_Object window)
{
  return decode_live_window (window)->prev_buffers;
}

DEFUN ("set-window-prev-buffers", Fset_window_prev_buffers,
       Sset_window_prev_buffers, 2, 2, 0,
       doc: /* Set WINDOW's previous buffers to PREV-BUFFERS.
WINDOW must be a live window and defaults to the selected one.

PREV-BUFFERS should be a list of elements (BUFFER WINDOW-START POS),
where BUFFER is a buffer, WINDOW-START is the start position of the
window for that buffer, and POS is a window-specific point value.  */)
     (Lisp_Object window, Lisp_Object prev_buffers)
{
  wset_prev_buffers (decode_live_window (window), prev_buffers);
  return prev_buffers;
}

DEFUN ("window-next-buffers", Fwindow_next_buffers, Swindow_next_buffers,
       0, 1, 0,
       doc:  /* Return list of buffers recently re-shown in WINDOW.
WINDOW must be a live window and defaults to the selected one.  */)
     (Lisp_Object window)
{
  return decode_live_window (window)->next_buffers;
}

DEFUN ("set-window-next-buffers", Fset_window_next_buffers,
       Sset_window_next_buffers, 2, 2, 0,
       doc: /* Set WINDOW's next buffers to NEXT-BUFFERS.
WINDOW must be a live window and defaults to the selected one.
NEXT-BUFFERS should be a list of buffers.  */)
     (Lisp_Object window, Lisp_Object next_buffers)
{
  wset_next_buffers (decode_live_window (window), next_buffers);
  return next_buffers;
}

DEFUN ("window-parameters", Fwindow_parameters, Swindow_parameters,
       0, 1, 0,
       doc: /* Return the parameters of WINDOW and their values.
WINDOW must be a valid window and defaults to the selected one.  The
return value is a list of elements of the form (PARAMETER . VALUE).  */)
  (Lisp_Object window)
{
  return Fcopy_alist (decode_valid_window (window)->window_parameters);
}

Lisp_Object
window_parameter (struct window *w, Lisp_Object parameter)
{
  Lisp_Object result = assq_no_quit (parameter, w->window_parameters);

  return CDR_SAFE (result);
}


DEFUN ("window-parameter", Fwindow_parameter, Swindow_parameter,
       2, 2, 0,
       doc:  /* Return WINDOW's value for PARAMETER.
WINDOW can be any window and defaults to the selected one.  */)
  (Lisp_Object window, Lisp_Object parameter)
{
  struct window *w = decode_any_window (window);

  return window_parameter (w, parameter);
}

DEFUN ("set-window-parameter", Fset_window_parameter,
       Sset_window_parameter, 3, 3, 0,
       doc: /* Set WINDOW's value of PARAMETER to VALUE.
WINDOW can be any window and defaults to the selected one.
Return VALUE.  */)
  (Lisp_Object window, Lisp_Object parameter, Lisp_Object value)
{
  register struct window *w = decode_any_window (window);
  Lisp_Object old_alist_elt;

  old_alist_elt = Fassq (parameter, w->window_parameters);
  if (NILP (old_alist_elt))
    wset_window_parameters
      (w, Fcons (Fcons (parameter, value), w->window_parameters));
  else
    Fsetcdr (old_alist_elt, value);
  return value;
}

DEFUN ("window-display-table", Fwindow_display_table, Swindow_display_table,
       0, 1, 0,
       doc: /* Return the display-table that WINDOW is using.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return decode_live_window (window)->display_table;
}

/* Get the display table for use on window W.  This is either W's
   display table or W's buffer's display table.  Ignore the specified
   tables if they are not valid; if no valid table is specified,
   return 0.  */

struct Lisp_Char_Table *
window_display_table (struct window *w)
{
  struct Lisp_Char_Table *dp = NULL;

  if (DISP_TABLE_P (w->display_table))
    dp = XCHAR_TABLE (w->display_table);
  else if (BUFFERP (w->contents))
    {
      struct buffer *b = XBUFFER (w->contents);

      if (DISP_TABLE_P (BVAR (b, display_table)))
	dp = XCHAR_TABLE (BVAR (b, display_table));
      else if (DISP_TABLE_P (Vstandard_display_table))
	dp = XCHAR_TABLE (Vstandard_display_table);
    }

  return dp;
}

DEFUN ("set-window-display-table", Fset_window_display_table, Sset_window_display_table, 2, 2, 0,
       doc: /* Set WINDOW's display-table to TABLE.
WINDOW must be a live window and defaults to the selected one.  */)
  (register Lisp_Object window, Lisp_Object table)
{
  wset_display_table (decode_live_window (window), table);
  return table;
}

/* Record info on buffer window W is displaying
   when it is about to cease to display that buffer.  */
static void
unshow_buffer (register struct window *w)
{
  Lisp_Object buf = w->contents;
  struct buffer *b = XBUFFER (buf);

  eassert (b == XMARKER (w->pointm)->buffer);

#if false
  if (w == XWINDOW (selected_window)
      || ! EQ (buf, XWINDOW (selected_window)->contents))
    /* Do this except when the selected window's buffer
       is being removed from some other window.  */
#endif
    /* last_window_start records the start position that this buffer
       had in the last window to be disconnected from it.
       Now that this statement is unconditional,
       it is possible for the buffer to be displayed in the
       selected window, while last_window_start reflects another
       window which was recently showing the same buffer.
       Some people might say that might be a good thing.  Let's see.  */
    b->last_window_start = marker_position (w->start);

  /* Point in the selected window's buffer
     is actually stored in that buffer, and the window's pointm isn't used.
     So don't clobber point in that buffer.  */
  if (!EQ (buf, XWINDOW (selected_window)->contents)
      /* Don't clobber point in current buffer either (this could be
	 useful in connection with bug#12208).
      && XBUFFER (buf) != current_buffer  */
      /* This line helps to fix Horsley's testbug.el bug.  */
      && !(WINDOWP (BVAR (b, last_selected_window))
	   && w != XWINDOW (BVAR (b, last_selected_window))
	   && EQ (buf, XWINDOW (BVAR (b, last_selected_window))->contents)))
    temp_set_point_both (b,
			 clip_to_bounds (BUF_BEGV (b),
					 marker_position (w->pointm),
					 BUF_ZV (b)),
			 clip_to_bounds (BUF_BEGV_BYTE (b),
					 marker_byte_position (w->pointm),
					 BUF_ZV_BYTE (b)));

  if (WINDOWP (BVAR (b, last_selected_window))
      && w == XWINDOW (BVAR (b, last_selected_window)))
    bset_last_selected_window (b, Qnil);
}

/* Put NEW into the window structure in place of OLD.  SETFLAG false
   means change window structure only.  Otherwise store geometry and
   other settings as well.  */
static void
replace_window (Lisp_Object old, Lisp_Object new, bool setflag)
{
  Lisp_Object tem;
  struct window *o = XWINDOW (old), *n = XWINDOW (new);

  /* If OLD is its frame's root window, then NEW is the new
     root window for that frame.  */
  if (EQ (old, FRAME_ROOT_WINDOW (XFRAME (o->frame))))
    fset_root_window (XFRAME (o->frame), new);

  if (setflag)
    {
      n->pixel_left = o->pixel_left;
      n->pixel_top = o->pixel_top;
      n->pixel_width = o->pixel_width;
      n->pixel_height = o->pixel_height;
      n->left_col = o->left_col;
      n->top_line = o->top_line;
      n->total_cols = o->total_cols;
      n->total_lines = o->total_lines;
      wset_normal_cols (n, o->normal_cols);
      wset_normal_cols (o, make_float (1.0));
      wset_normal_lines (n, o->normal_lines);
      wset_normal_lines (o, make_float (1.0));
      n->desired_matrix = n->current_matrix = 0;
      n->vscroll = 0;
      memset (&n->cursor, 0, sizeof (n->cursor));
      memset (&n->phys_cursor, 0, sizeof (n->phys_cursor));
      n->last_cursor_vpos = 0;
#ifdef HAVE_WINDOW_SYSTEM
      n->phys_cursor_type = NO_CURSOR;
      n->phys_cursor_width = -1;
#endif
      n->must_be_updated_p = false;
      n->pseudo_window_p = false;
      n->window_end_vpos = 0;
      n->window_end_pos = 0;
      n->window_end_valid = false;
    }

  tem = o->next;
  wset_next (n, tem);
  if (!NILP (tem))
    wset_prev (XWINDOW (tem), new);

  tem = o->prev;
  wset_prev (n, tem);
  if (!NILP (tem))
    wset_next (XWINDOW (tem), new);

  tem = o->parent;
  wset_parent (n, tem);
  if (!NILP (tem) && EQ (XWINDOW (tem)->contents, old))
    wset_combination (XWINDOW (tem), XWINDOW (tem)->horizontal, new);
}

/* If window WINDOW and its parent window are iso-combined, merge
   WINDOW's children into those of its parent window and mark WINDOW as
   deleted.  */

static void
recombine_windows (Lisp_Object window)
{
  struct window *w, *p, *c;
  Lisp_Object parent, child;
  bool horflag;

  w = XWINDOW (window);
  parent = w->parent;
  if (!NILP (parent) && NILP (w->combination_limit))
    {
      p = XWINDOW (parent);
      if (WINDOWP (p->contents) && WINDOWP (w->contents)
	  && p->horizontal == w->horizontal)
	/* WINDOW and PARENT are both either a vertical or a horizontal
	   combination.  */
	{
	  horflag = WINDOW_HORIZONTAL_COMBINATION_P (w);
	  child = w->contents;
	  c = XWINDOW (child);

	  /* Splice WINDOW's children into its parent's children and
	     assign new normal sizes.  */
	  if (NILP (w->prev))
	    wset_combination (p, horflag, child);
	  else
	    {
	      wset_prev (c, w->prev);
	      wset_next (XWINDOW (w->prev), child);
	    }

	  while (c)
	    {
	      wset_parent (c, parent);

	      if (horflag)
		wset_normal_cols
		  (c, make_float ((double) c->pixel_width
				  / (double) p->pixel_width));
	      else
		wset_normal_lines
		  (c, make_float ((double) c->pixel_height
				  / (double) p->pixel_height));

	      if (NILP (c->next))
		{
		  if (!NILP (w->next))
		    {
		      wset_next (c, w->next);
		      wset_prev (XWINDOW (c->next), child);
		    }

		  c = 0;
		}
	      else
		{
		  child = c->next;
		  c = XWINDOW (child);
		}
	    }

	  /* WINDOW can be deleted now.  */
	  wset_combination (w, false, Qnil);
	}
    }
}

/* If WINDOW can be deleted, delete it.  */
static void
delete_deletable_window (Lisp_Object window)
{
  if (!NILP (call1 (Qwindow_deletable_p, window)))
    call1 (Qdelete_window, window);
}

/***********************************************************************
			     Window List
 ***********************************************************************/

/* Add window W to *USER_DATA.  USER_DATA is actually a Lisp_Object
   pointer.  This is a callback function for foreach_window, used in
   the window_list function.  */

static bool
add_window_to_list (struct window *w, void *user_data)
{
  Lisp_Object *list = user_data;
  Lisp_Object window;
  XSETWINDOW (window, w);
  *list = Fcons (window, *list);
  return true;
}


/* Return a list of all windows, for use by next_window.  If
   Vwindow_list is a list, return that list.  Otherwise, build a new
   list, cache it in Vwindow_list, and return that.  */

Lisp_Object
window_list (void)
{
  if (!CONSP (Vwindow_list))
    {
      Lisp_Object tail, frame;
      specpdl_ref count = SPECPDL_INDEX ();

      Vwindow_list = Qnil;
      /*  Don't allow quitting in Fnconc.  Otherwise we might end up
	  with a too short Vwindow_list and Fkill_buffer not being able
	  to replace a buffer in all windows showing it (Bug#47244).  */
      specbind (Qinhibit_quit, Qt);
      FOR_EACH_FRAME (tail, frame)
	{
	  Lisp_Object arglist = Qnil;

	  /* We are visiting windows in canonical order, and add
	     new windows at the front of arglist, which means we
	     have to reverse this list at the end.  */
	  foreach_window (XFRAME (frame), add_window_to_list, &arglist);
	  arglist = Fnreverse (arglist);
	  Vwindow_list = nconc2 (Vwindow_list, arglist);
	}

      unbind_to (count, Qnil);
    }

  return Vwindow_list;
}


/* Value is true if WINDOW satisfies the constraints given by
   OWINDOW, MINIBUF and ALL_FRAMES.

   MINIBUF	t means WINDOW may be minibuffer windows.
		`lambda' means WINDOW may not be a minibuffer window.
		a window means a specific minibuffer window

   ALL_FRAMES	t means search all frames,
		nil means search just current frame,
		`visible' means search just visible frames on the
                current terminal,
		0 means search visible and iconified frames on the
                current terminal,
		a window means search the frame that window belongs to,
		a frame means consider windows on that frame, only.  */

static bool
candidate_window_p (Lisp_Object window, Lisp_Object owindow,
		    Lisp_Object minibuf, Lisp_Object all_frames)
{
  struct window *w = XWINDOW (window);
  struct frame *f = XFRAME (w->frame);
  bool candidate_p = true;

  if (!BUFFERP (w->contents))
    candidate_p = false;
  else if (MINI_WINDOW_P (w)
           && (EQ (minibuf, Qlambda)
	       || (WINDOW_LIVE_P (minibuf) && !EQ (minibuf, window))))
    {
      /* If MINIBUF is `lambda' don't consider any mini-windows.
         If it is a window, consider only that one.  */
      candidate_p = false;
    }
  else if (EQ (all_frames, Qt))
    candidate_p = true;
  else if (NILP (all_frames))
    {
      eassert (WINDOWP (owindow));
      candidate_p = EQ (w->frame, XWINDOW (owindow)->frame);
    }
  else if (EQ (all_frames, Qvisible))
    {
      candidate_p = FRAME_VISIBLE_P (f)
	&& (FRAME_TERMINAL (XFRAME (w->frame))
	    == FRAME_TERMINAL (XFRAME (selected_frame)));

    }
  else if (FIXNUMP (all_frames) && XFIXNUM (all_frames) == 0)
    {
      candidate_p = (FRAME_VISIBLE_P (f) || FRAME_ICONIFIED_P (f)
#ifdef HAVE_X_WINDOWS
		     /* Yuck!!  If we've just created the frame and the
			window-manager requested the user to place it
			manually, the window may still not be considered
			`visible'.  I'd argue it should be at least
			something like `iconified', but don't know how to do
			that yet.  --Stef  */
		     || (FRAME_X_P (f) && f->output_data.x->asked_for_visible
			 && !f->output_data.x->has_been_visible)
#endif
		     )
	&& (FRAME_TERMINAL (XFRAME (w->frame))
	    == FRAME_TERMINAL (XFRAME (selected_frame)));
    }
  else if (WINDOWP (all_frames))
    /* 	To qualify as candidate, it's not sufficient for WINDOW's frame
	to just share the minibuffer window - it must be active as well
	(see Bug#24500).  */
    candidate_p = ((EQ (XWINDOW (all_frames)->frame, w->frame)
                    || (EQ (f->minibuffer_window, all_frames)
                        && EQ (XWINDOW (all_frames)->frame, FRAME_FOCUS_FRAME (f))))
                   && (EQ (minibuf, Qt)
		       || !is_minibuffer (0, XWINDOW (all_frames)->contents)));
  else if (FRAMEP (all_frames))
    candidate_p = EQ (all_frames, w->frame);

  return candidate_p;
}


/* Decode arguments as allowed by Fnext_window, Fprevious_window, and
   Fwindow_list.  See candidate_window_p for the meaning of WINDOW,
   MINIBUF, and ALL_FRAMES.  */

static void
decode_next_window_args (Lisp_Object *window, Lisp_Object *minibuf, Lisp_Object *all_frames)
{
  struct window *w = decode_live_window (*window);
  Lisp_Object miniwin = XFRAME (w->frame)->minibuffer_window;

  XSETWINDOW (*window, w);
  /* MINIBUF nil may or may not include minibuffer windows.  Decide if
     it does.  But first make sure that this frame's minibuffer window
     is live (Bug#47207).  */
  if (WINDOW_LIVE_P (miniwin) && NILP (*minibuf))
    *minibuf = (this_minibuffer_depth (XWINDOW (miniwin)->contents)
		? miniwin : Qlambda);
  else if (!EQ (*minibuf, Qt))
    *minibuf = Qlambda;

  /* Now *MINIBUF can be t => count all minibuffer windows, `lambda'
     => count none of them, or a specific minibuffer window (the
     active one) to count.  */

  /* ALL_FRAMES nil doesn't specify which frames to include.  */
  if (NILP (*all_frames))
    *all_frames
      /* Once more make sure that this frame's minibuffer window is live
	 before including it (Bug#47207).  */
      = ((WINDOW_LIVE_P (miniwin) && !EQ (*minibuf, Qlambda))
	 ? miniwin : Qnil);
  else if (EQ (*all_frames, Qvisible))
    ;
  else if (EQ (*all_frames, make_fixnum (0)))
    ;
  else if (FRAMEP (*all_frames))
    ;
  else if (!EQ (*all_frames, Qt))
    *all_frames = Qnil;
}


/* Return the next or previous window of WINDOW in cyclic ordering
   of windows.  NEXT_P means return the next window.  See the
   documentation string of next-window for the meaning of MINIBUF and
   ALL_FRAMES.  */

static Lisp_Object
next_window (Lisp_Object window, Lisp_Object minibuf, Lisp_Object all_frames,
	     bool next_p)
{
  specpdl_ref count = SPECPDL_INDEX ();

  decode_next_window_args (&window, &minibuf, &all_frames);

  /* If ALL_FRAMES is a frame, and WINDOW isn't on that frame, just
     return the first window on the frame.  */
  if (FRAMEP (all_frames)
      && !EQ (all_frames, XWINDOW (window)->frame))
    return Fframe_first_window (all_frames);

  /*  Don't allow quitting in Fmemq.  */
  specbind (Qinhibit_quit, Qt);

  if (next_p)
    {
      Lisp_Object list;

      /* Find WINDOW in the list of all windows.  */
      list = Fmemq (window, window_list ());

      /* Scan forward from WINDOW to the end of the window list.  */
      if (CONSP (list))
	for (list = XCDR (list); CONSP (list); list = XCDR (list))
	  if (candidate_window_p (XCAR (list), window, minibuf, all_frames))
	    break;

      /* Scan from the start of the window list up to WINDOW.  */
      if (!CONSP (list))
	for (list = Vwindow_list;
	     CONSP (list) && !EQ (XCAR (list), window);
	     list = XCDR (list))
	  if (candidate_window_p (XCAR (list), window, minibuf, all_frames))
	    break;

      if (CONSP (list))
	window = XCAR (list);
    }
  else
    {
      Lisp_Object candidate, list;

      /* Scan through the list of windows for candidates.  If there are
	 candidate windows in front of WINDOW, the last one of these
	 is the one we want.  If there are candidates following WINDOW
	 in the list, again the last one of these is the one we want.  */
      candidate = Qnil;
      for (list = window_list (); CONSP (list); list = XCDR (list))
	{
	  if (EQ (XCAR (list), window))
	    {
	      if (WINDOWP (candidate))
		break;
	    }
	  else if (candidate_window_p (XCAR (list), window, minibuf,
				       all_frames))
	    candidate = XCAR (list);
	}

      if (WINDOWP (candidate))
	window = candidate;
    }

  unbind_to (count, Qnil);

  return window;
}


DEFUN ("next-window", Fnext_window, Snext_window, 0, 3, 0,
       doc: /* Return live window after WINDOW in the cyclic ordering of windows.
WINDOW must be a live window and defaults to the selected one.  The
optional arguments MINIBUF and ALL-FRAMES specify the set of windows to
consider.

MINIBUF nil or omitted means consider the minibuffer window only if the
minibuffer is active.  MINIBUF t means consider the minibuffer window
even if the minibuffer is not active.  Any other value means do not
consider the minibuffer window even if the minibuffer is active.

ALL-FRAMES nil or omitted means consider all windows on WINDOW's frame,
plus the minibuffer window if specified by the MINIBUF argument.  If the
minibuffer counts, consider all windows on all frames that share that
minibuffer too.  The following non-nil values of ALL-FRAMES have special
meanings:

- t means consider all windows on all existing frames.

- `visible' means consider all windows on all visible frames.

- 0 (the number zero) means consider all windows on all visible and
  iconified frames.

- A frame means consider all windows on that frame only.

Anything else means consider all windows on WINDOW's frame and no
others.

If you use consistent values for MINIBUF and ALL-FRAMES, you can use
`next-window' to iterate through the entire cycle of acceptable
windows, eventually ending up back at the window you started with.
`previous-window' traverses the same cycle, in the reverse order.  */)
  (Lisp_Object window, Lisp_Object minibuf, Lisp_Object all_frames)
{
  return next_window (window, minibuf, all_frames, true);
}


DEFUN ("previous-window", Fprevious_window, Sprevious_window, 0, 3, 0,
       doc: /* Return live window before WINDOW in the cyclic ordering of windows.
WINDOW must be a live window and defaults to the selected one.  The
optional arguments MINIBUF and ALL-FRAMES specify the set of windows to
consider.

MINIBUF nil or omitted means consider the minibuffer window only if the
minibuffer is active.  MINIBUF t means consider the minibuffer window
even if the minibuffer is not active.  Any other value means do not
consider the minibuffer window even if the minibuffer is active.

ALL-FRAMES nil or omitted means consider all windows on WINDOW's frame,
plus the minibuffer window if specified by the MINIBUF argument.  If the
minibuffer counts, consider all windows on all frames that share that
minibuffer too.  The following non-nil values of ALL-FRAMES have special
meanings:

- t means consider all windows on all existing frames.

- `visible' means consider all windows on all visible frames.

- 0 (the number zero) means consider all windows on all visible and
  iconified frames.

- A frame means consider all windows on that frame only.

Anything else means consider all windows on WINDOW's frame and no
others.

If you use consistent values for MINIBUF and ALL-FRAMES, you can
use `previous-window' to iterate through the entire cycle of
acceptable windows, eventually ending up back at the window you
started with.  `next-window' traverses the same cycle, in the
reverse order.  */)
  (Lisp_Object window, Lisp_Object minibuf, Lisp_Object all_frames)
{
  return next_window (window, minibuf, all_frames, false);
}


/* Return a list of windows in cyclic ordering.  Arguments are like
   for `next-window'.  */

static Lisp_Object
window_list_1 (Lisp_Object window, Lisp_Object minibuf, Lisp_Object all_frames)
{
  Lisp_Object tail, list, rest;
  specpdl_ref count = SPECPDL_INDEX ();

  decode_next_window_args (&window, &minibuf, &all_frames);
  list = Qnil;

  /*  Don't allow quitting in Fmemq and Fnconc.  */
  specbind (Qinhibit_quit, Qt);

  for (tail = window_list (); CONSP (tail); tail = XCDR (tail))
    if (candidate_window_p (XCAR (tail), window, minibuf, all_frames))
      list = Fcons (XCAR (tail), list);

  /* Rotate the list to start with WINDOW.  */
  list = Fnreverse (list);
  rest = Fmemq (window, list);
  if (!NILP (rest) && !EQ (rest, list))
    {
      for (tail = list; !EQ (XCDR (tail), rest); tail = XCDR (tail))
	;
      XSETCDR (tail, Qnil);
      list = nconc2 (rest, list);
    }

  unbind_to (count, Qnil);

  return list;
}


DEFUN ("window-list", Fwindow_list, Swindow_list, 0, 3, 0,
       doc: /* Return a list of windows on FRAME, starting with WINDOW.
FRAME nil or omitted means use the selected frame.
WINDOW nil or omitted means use the window selected within FRAME.
MINIBUF t means include the minibuffer window, even if it isn't active.
MINIBUF nil or omitted means include the minibuffer window only
if it's active.
MINIBUF neither nil nor t means never include the minibuffer window.  */)
  (Lisp_Object frame, Lisp_Object minibuf, Lisp_Object window)
{
  if (NILP (window))
    window = FRAMEP (frame) ? XFRAME (frame)->selected_window : selected_window;
  CHECK_WINDOW (window);
  if (NILP (frame))
    frame = selected_frame;

  if (!EQ (frame, XWINDOW (window)->frame))
    error ("Window is on a different frame");

  return window_list_1 (window, minibuf, frame);
}


DEFUN ("window-list-1", Fwindow_list_1, Swindow_list_1, 0, 3, 0,
       doc: /* Return a list of all live windows.
WINDOW specifies the first window to list and defaults to the selected
window.

Optional argument MINIBUF nil or omitted means consider the minibuffer
window only if the minibuffer is active.  MINIBUF t means consider the
minibuffer window even if the minibuffer is not active.  Any other value
means do not consider the minibuffer window even if the minibuffer is
active.

Optional argument ALL-FRAMES nil or omitted means consider all windows
on WINDOW's frame, plus the minibuffer window if specified by the
MINIBUF argument.  If the minibuffer counts, consider all windows on all
frames that share that minibuffer too.  The following non-nil values of
ALL-FRAMES have special meanings:

- t means consider all windows on all existing frames.

- `visible' means consider all windows on all visible frames.

- 0 (the number zero) means consider all windows on all visible and
  iconified frames.

- A frame means consider all windows on that frame only.

Anything else means consider all windows on WINDOW's frame and no
others.

If WINDOW is not on the list of windows returned, some other window will
be listed first but no error is signaled.  */)
  (Lisp_Object window, Lisp_Object minibuf, Lisp_Object all_frames)
{
  return window_list_1 (window, minibuf, all_frames);
}

/* Look at all windows, performing an operation specified by TYPE
   with argument OBJ.
   If FRAMES is Qt, look at all frames;
                Qnil, look at just the selected frame;
		Qvisible, look at visible frames;
	        a frame, just look at windows on that frame.
   If MINI, perform the operation on minibuffer windows too.  */

enum window_loop
{
  WINDOW_LOOP_UNUSED,
  GET_BUFFER_WINDOW,		    /* Arg is buffer */
  REPLACE_BUFFER_IN_WINDOWS_SAFELY, /* Arg is buffer */
  REDISPLAY_BUFFER_WINDOWS,	    /* Arg is buffer */
  CHECK_ALL_WINDOWS                 /* Arg is ignored */
};

static Lisp_Object
window_loop (enum window_loop type, Lisp_Object obj, bool mini,
	     Lisp_Object frames)
{
  Lisp_Object window, windows, best_window, frame_arg;
  bool frame_best_window_flag = false;
  struct frame *f;

  /* If we're only looping through windows on a particular frame,
     frame points to that frame.  If we're looping through windows
     on all frames, frame is 0.  */
  if (FRAMEP (frames))
    f = XFRAME (frames);
  else if (NILP (frames))
    f = SELECTED_FRAME ();
  else
    f = NULL;

  if (f)
    frame_arg = Qlambda;
  else if (EQ (frames, make_fixnum (0)))
    frame_arg = frames;
  else if (EQ (frames, Qvisible))
    frame_arg = frames;
  else
    frame_arg = Qt;

  /* frame_arg is Qlambda to stick to one frame,
     Qvisible to consider all visible frames,
     or Qt otherwise.  */

  /* Pick a window to start with.  */
  if (WINDOWP (obj))
    window = obj;
  else if (f)
    window = FRAME_SELECTED_WINDOW (f);
  else
    window = FRAME_SELECTED_WINDOW (SELECTED_FRAME ());

  windows = window_list_1 (window, mini ? Qt : Qnil, frame_arg);
  best_window = Qnil;

  for (; CONSP (windows); windows = XCDR (windows))
    {
      struct window *w;

      window = XCAR (windows);
      w = XWINDOW (window);

      /* Note that we do not pay attention here to whether the frame
	 is visible, since Fwindow_list skips non-visible frames if
	 that is desired, under the control of frame_arg.  */
      if (!MINI_WINDOW_P (w)
	  /* For REPLACE_BUFFER_IN_WINDOWS_SAFELY, we must always
	     consider all windows.  */
	  || type == REPLACE_BUFFER_IN_WINDOWS_SAFELY
	  || (mini && minibuf_level > 0))
	switch (type)
	  {
	  case GET_BUFFER_WINDOW:
	    if (EQ (w->contents, obj)
		/* Don't find any minibuffer window except the one that
		   is currently in use.  */
		&& (!MINI_WINDOW_P (w) || EQ (window, minibuf_window)))
	      {
		if (EQ (window, selected_window))
		  /* Preferably return the selected window.  */
		  return window;
		else if (EQ (XWINDOW (window)->frame, selected_frame)
			 && !frame_best_window_flag)
		  /* Prefer windows on the current frame (but don't
		     choose another one if we have one already).  */
		  {
		    best_window = window;
		    frame_best_window_flag = true;
		  }
		else if (NILP (best_window))
		  best_window = window;
	      }
	    break;

	  case REPLACE_BUFFER_IN_WINDOWS_SAFELY:
	    /* We could simply check whether the buffer shown by window
	       is live, and show another buffer in case it isn't.  */
	    if (EQ (w->contents, obj))
	      {
		/* Undedicate WINDOW.  */
		wset_dedicated (w, Qnil);
		/* Make WINDOW show the buffer returned by
		   other_buffer_safely, don't run any hooks.  */
		set_window_buffer
		  (window, other_buffer_safely (w->contents), false, false);
		/* If WINDOW is the selected window, make its buffer
		   current.  But do so only if the window shows the
		   current buffer (Bug#6454).  */
		if (EQ (window, selected_window)
		    && XBUFFER (w->contents) == current_buffer)
		  Fset_buffer (w->contents);
	      }
	    break;

	  case REDISPLAY_BUFFER_WINDOWS:
	    if (EQ (w->contents, obj))
	      {
		mark_window_display_accurate (window, false);
		w->update_mode_line = true;
		XBUFFER (obj)->prevent_redisplay_optimizations_p = true;
		update_mode_lines = 27;
		best_window = window;
	      }
	    break;

	    /* Check for a leaf window that has a killed buffer
	       or broken markers.  */
	  case CHECK_ALL_WINDOWS:
	    if (BUFFERP (w->contents))
	      {
		struct buffer *b = XBUFFER (w->contents);

		if (!BUFFER_LIVE_P (b))
		  emacs_abort ();
		if (!MARKERP (w->start) || XMARKER (w->start)->buffer != b)
		  emacs_abort ();
		if (!MARKERP (w->pointm) || XMARKER (w->pointm)->buffer != b)
		  emacs_abort ();
	      }
	    break;

	  case WINDOW_LOOP_UNUSED:
	    break;
	  }
    }

  return best_window;
}

/* Used for debugging.  Abort if any window has a dead buffer.  */

extern void check_all_windows (void) EXTERNALLY_VISIBLE;
void
check_all_windows (void)
{
  window_loop (CHECK_ALL_WINDOWS, Qnil, true, Qt);
}

DEFUN ("get-buffer-window", Fget_buffer_window, Sget_buffer_window, 0, 2, 0,
       doc: /* Return a window currently displaying BUFFER-OR-NAME, or nil if none.
BUFFER-OR-NAME may be a buffer or a buffer name and defaults to
the current buffer.

The optional argument ALL-FRAMES specifies the frames to consider:

- t means consider all windows on all existing frames.

- `visible' means consider all windows on all visible frames.

- 0 (the number zero) means consider all windows on all visible
    and iconified frames.

- A frame means consider all windows on that frame only.

Any other value of ALL-FRAMES means consider all windows on the
selected frame and no others.  */)
     (Lisp_Object buffer_or_name, Lisp_Object all_frames)
{
  Lisp_Object buffer;

  if (NILP (buffer_or_name))
    buffer = Fcurrent_buffer ();
  else
    buffer = Fget_buffer (buffer_or_name);

  if (BUFFERP (buffer))
    return window_loop (GET_BUFFER_WINDOW, buffer, true, all_frames);
  else
    return Qnil;
}


static Lisp_Object
resize_root_window (Lisp_Object window, Lisp_Object delta,
		    Lisp_Object horizontal, Lisp_Object ignore,
		    Lisp_Object pixelwise)
{
  return call5 (Qwindow__resize_root_window, window, delta,
		horizontal, ignore, pixelwise);
}


static Lisp_Object
window_pixel_to_total (Lisp_Object frame, Lisp_Object horizontal)
{
  return call2 (Qwindow__pixel_to_total, frame, horizontal);
}


/** Remove all occurrences of element whose car is BUFFER from ALIST.
    Return changed ALIST.  */
static Lisp_Object
window_discard_buffer_from_alist (Lisp_Object buffer, Lisp_Object alist)
{
  Lisp_Object tail, *prev = &alist;

  for (tail = alist; CONSP (tail); tail = XCDR (tail))
    {
      Lisp_Object tem = XCAR (tail);

      tem = XCAR (tem);

      if (EQ (tem, buffer))
	*prev = XCDR (tail);
      else
	prev = xcdr_addr (tail);
    }

  return alist;
}

/** Remove all occurrences of BUFFER from LIST.  Return changed
    LIST.  */
static Lisp_Object
window_discard_buffer_from_list (Lisp_Object buffer, Lisp_Object list)
{
  Lisp_Object tail, *prev = &list;

  for (tail = list; CONSP (tail); tail = XCDR (tail))
    if (EQ (XCAR (tail), buffer))
      *prev = XCDR (tail);
    else
      prev = xcdr_addr (tail);

  return list;
}

/** Remove BUFFER from the lists of previous and next buffers of object
    WINDOW.  ALL true means remove any `quit-restore' and
    `quit-restore-prev' parameter of WINDOW referencing BUFFER too.  */
static void
window_discard_buffer_from_window (Lisp_Object buffer, Lisp_Object window, bool all)
{
  struct window *w = XWINDOW (window);

  wset_prev_buffers
    (w, window_discard_buffer_from_alist (buffer, w->prev_buffers));
  wset_next_buffers
    (w, window_discard_buffer_from_list (buffer, w->next_buffers));

  if (all)
    {
      Lisp_Object quit_restore = window_parameter (w, Qquit_restore);
      Lisp_Object quit_restore_prev = window_parameter (w, Qquit_restore_prev);
      Lisp_Object quad;

      if (EQ (buffer, Fnth (make_fixnum (3), quit_restore_prev))
	  || (CONSP (quad = Fcar (Fcdr (quit_restore_prev)))
	      && EQ (Fcar (quad), buffer)))
	Fset_window_parameter (window, Qquit_restore_prev, Qnil);

      if (EQ (buffer, Fnth (make_fixnum (3), quit_restore))
	  || (CONSP (quad = Fcar (Fcdr (quit_restore)))
	      && EQ (Fcar (quad), buffer)))
	{
	  Fset_window_parameter (window, Qquit_restore,
				 window_parameter (w, Qquit_restore_prev));
	  Fset_window_parameter (window, Qquit_restore_prev, Qnil);
	}
    }
}

/** Remove BUFFER from the lists of previous and next buffers and the
    `quit-restore' and `quit-restore-prev' parameters of any dead
    WINDOW.  */
void
window_discard_buffer_from_dead_windows (Lisp_Object buffer)
{
  struct Lisp_Hash_Table *h = XHASH_TABLE (window_dead_windows_table);

  DOHASH (h, k, v)
    window_discard_buffer_from_window (buffer, v, true);
}

DEFUN ("window-discard-buffer-from-window", Fwindow_discard_buffer,
       Swindow_discard_buffer, 2, 3, 0,
       doc: /* Discard BUFFER from WINDOW.
Discard specified live BUFFER from the lists of previous and next
buffers of specified live WINDOW.

Optional argument ALL non-nil means discard any `quit-restore' and
`quit-restore-prev' parameters of WINDOW referencing BUFFER too.  */)
  (Lisp_Object buffer, Lisp_Object window, Lisp_Object all)
{
  if (!BUFFER_LIVE_P (XBUFFER (buffer)))
    error ("Not a live buffer");

  if (!WINDOW_LIVE_P (window))
    error ("Not a live window");

  window_discard_buffer_from_window (buffer, window, !NILP (all));

  return Qnil;
}


DEFUN ("delete-other-windows-internal", Fdelete_other_windows_internal,
       Sdelete_other_windows_internal, 0, 2, "",
       doc: /* Make WINDOW fill its frame.
Only the frame WINDOW is on is affected.  WINDOW must be a valid window
and defaults to the selected one.

Optional argument ROOT, if non-nil, must specify an internal window such
that WINDOW is in its window subtree.  If this is the case, replace ROOT
by WINDOW and leave alone any windows not part of ROOT's subtree.

When WINDOW is live try to reduce display jumps by keeping the text
previously visible in WINDOW in the same place on the frame.  Doing this
depends on the value of (window-start WINDOW), so if calling this
function in a program gives strange scrolling, make sure the
window-start value is reasonable when this function is called.  */)
     (Lisp_Object window, Lisp_Object root)
{
  struct window *w = decode_valid_window (window);
  struct window *r, *s;
  Lisp_Object frame = w->frame;
  struct frame *f = XFRAME (frame);
  Lisp_Object sibling, pwindow, delta;
  Lisp_Object swindow UNINIT;
  ptrdiff_t startpos UNINIT, startbyte UNINIT;
  int top UNINIT;
  int new_top;
  bool resize_failed = false;

  XSETWINDOW (window, w);

  if (NILP (root))
    /* ROOT is the frame's root window.  */
    {
      root = FRAME_ROOT_WINDOW (f);
      r = XWINDOW (root);
    }
  else
    /* ROOT must be an ancestor of WINDOW.  */
    {
      r = decode_valid_window (root);
      pwindow = XWINDOW (window)->parent;
      while (!NILP (pwindow))
	if (EQ (pwindow, root))
	  break;
	else
	  pwindow = XWINDOW (pwindow)->parent;
      if (!EQ (pwindow, root))
	error ("Specified root is not an ancestor of specified window");
    }

  if (EQ (window, root))
    /* A noop.  */
    return Qnil;
  /* I don't understand the "top > 0" part below.  If we deal with a
     standalone minibuffer it would have been caught by the preceding
     test.  */
  else if (MINI_WINDOW_P (w)) /* && top > 0) */
    error ("Can't expand minibuffer to full frame");

  if (BUFFERP (w->contents))
    {
      startpos = marker_position (w->start);
      startbyte = marker_byte_position (w->start);
      top = (WINDOW_TOP_EDGE_LINE (w)
	     - FRAME_TOP_MARGIN (XFRAME (WINDOW_FRAME (w))));
      /* Make sure WINDOW is the frame's selected window.  */
      if (!EQ (window, FRAME_SELECTED_WINDOW (f)))
	{
	  if (EQ (selected_frame, frame))
	    Fselect_window (window, Qnil);
	  else
	    /* Do not clear f->select_mini_window_flag here.  If the
	       last selected window on F was an active minibuffer, we
	       want to return to it on a later Fselect_frame.  */
	    fset_selected_window (f, window);
	}
    }
  else
    {
      /* See if the frame's selected window is a part of the window
	 subtree rooted at WINDOW, by finding all the selected window's
	 parents and comparing each one with WINDOW.  If it isn't we
	 need a new selected window for this frame.  */
      swindow = FRAME_SELECTED_WINDOW (f);
      while (true)
	{
	  pwindow = swindow;
	  while (!NILP (pwindow) && !EQ (window, pwindow))
	    pwindow = XWINDOW (pwindow)->parent;

	  if (EQ (window, pwindow))
	    /* If WINDOW is an ancestor of SWINDOW, then SWINDOW is ok
	       as the new selected window.  */
	    break;
	  else
	    /* Else try the previous window of SWINDOW.  */
	    swindow = Fprevious_window (swindow, Qlambda, Qnil);
	}

      if (!EQ (swindow, FRAME_SELECTED_WINDOW (f)))
	{
	  if (EQ (selected_frame, frame))
	    Fselect_window (swindow, Qnil);
	  else
	    fset_selected_window (f, swindow);
	}
    }

  block_input ();
  if (!FRAME_INITIAL_P (f))
    {
      Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (f);

      /* We are going to free the glyph matrices of WINDOW, and with
	 that we might lose any information about glyph rows that have
	 some of their glyphs highlighted in mouse face.  (These rows
	 are marked with a mouse_face_p flag.)  If WINDOW
	 indeed has some glyphs highlighted in mouse face, signal to
	 frame's up-to-date hook that mouse highlight was overwritten,
	 so that it will arrange for redisplaying the highlight.  */
      if (EQ (hlinfo->mouse_face_window, window))
	reset_mouse_highlight (hlinfo);
    }
  free_window_matrices (r);

  fset_redisplay (f);
  Vwindow_list = Qnil;

  if (!WINDOW_LEAF_P (w))
    {
      /* Resize child windows vertically.  */
      XSETINT (delta, r->pixel_height - w->pixel_height);
      w->pixel_top = r->pixel_top;
      w->top_line = r->top_line;
      resize_root_window (window, delta, Qnil, Qnil, Qt);
      if (window_resize_check (w, false))
	window_resize_apply (w, false);
      else
	{
	  resize_root_window (window, delta, Qnil, Qt, Qt);
	  if (window_resize_check (w, false))
	    window_resize_apply (w, false);
	  else
	    resize_failed = true;
	}

      /* Resize child windows horizontally.  */
      if (!resize_failed)
	{
	  w->left_col = r->left_col;
	  w->pixel_left = r->pixel_left;
	  XSETINT (delta, r->pixel_width - w->pixel_width);
	  resize_root_window (window, delta, Qt, Qnil, Qt);
	  if (window_resize_check (w, true))
	    window_resize_apply (w, true);
	  else
	    {
	      resize_root_window (window, delta, Qt, Qt, Qt);
	      if (window_resize_check (w, true))
		window_resize_apply (w, true);
	      else
		resize_failed = true;
	    }
	}

      if (resize_failed)
	/* Play safe, if we still can ...  */
	{
	  window = swindow;
	  w = XWINDOW (window);
	}
    }

  /* Cleanly unlink WINDOW from window-tree.  */
  if (!NILP (w->prev))
    /* Get SIBLING above (on the left of) WINDOW.  */
    {
      sibling = w->prev;
      s = XWINDOW (sibling);
      wset_next (s, w->next);
      if (!NILP (s->next))
	wset_prev (XWINDOW (s->next), sibling);
    }
  else
    /* Get SIBLING below (on the right of) WINDOW.  */
    {
      sibling = w->next;
      s = XWINDOW (sibling);
      wset_prev (s, Qnil);
      wset_combination (XWINDOW (w->parent),
			XWINDOW (w->parent)->horizontal, sibling);
    }

  /* Delete ROOT and all child windows of ROOT.  */
  if (WINDOWP (r->contents))
    {
      delete_all_child_windows (r->contents);
      wset_combination (r, false, Qnil);
    }

  replace_window (root, window, true);
  /* Assign new total sizes to all windows on FRAME.  We can't do that
     _before_ WINDOW replaces ROOT since 'window--pixel-to-total' works
     on the whole frame and thus would work on the frame's old window
     configuration (Bug#51007).  */
  window_pixel_to_total (frame, Qnil);
  window_pixel_to_total (frame, Qt);

  /* This must become SWINDOW anyway .......  */
  if (BUFFERP (w->contents) && !resize_failed)
    {
      /* Try to minimize scrolling, by setting the window start to the
	 point will cause the text at the old window start to be at the
	 same place on the frame.  But don't try to do this if the
	 window start is outside the visible portion (as might happen
	 when the display is not current, due to typeahead).  */
      new_top = WINDOW_TOP_EDGE_LINE (w) - FRAME_TOP_MARGIN (XFRAME (WINDOW_FRAME (w)));
      if (new_top != top
	  && startpos >= BUF_BEGV (XBUFFER (w->contents))
	  && startpos <= BUF_ZV (XBUFFER (w->contents)))
	{
	  struct position pos;
	  struct buffer *obuf = current_buffer;

	  Fset_buffer (w->contents);
	  /* This computation used to temporarily move point, but that
	     can have unwanted side effects due to text properties.  */
	  pos = *vmotion (startpos, startbyte, -top, w);

	  set_marker_both (w->start, w->contents, pos.bufpos, pos.bytepos);
	  w->window_end_valid = false;
	  w->start_at_line_beg = (pos.bytepos == BEGV_BYTE
				    || FETCH_BYTE (pos.bytepos - 1) == '\n');
	  /* We need to do this, so that the window-scroll-functions
	     get called.  */
	  w->optional_new_start = true;

	  /* Reset the vscroll, as redisplay will not.  */
	  w->vscroll = 0;
	  w->preserve_vscroll_p = false;

	  set_buffer_internal (obuf);
	}
    }

  adjust_frame_glyphs (f);
  unblock_input ();

  FRAME_WINDOW_CHANGE (f) = true;

  return Qnil;
}


void
replace_buffer_in_windows (Lisp_Object buffer)
{
  call1 (Qreplace_buffer_in_windows, buffer);
}

/** If BUFFER is shown in any window, safely replace it with some other
    buffer in all windows of all frames, even those on other keyboards.
    Do not delete any window.

    This function is called by Fkill_buffer when it detects that
    replacing BUFFER in some window showing BUFFER has failed.  It
    assumes that ‘replace-buffer-in-windows’ has removed any entry
    referencing BUFFER from any window's lists of previous and next
    buffers and that window's ‘quit-restore’ and 'quit-restore-prev'
    parameters.
*/
void
replace_buffer_in_windows_safely (Lisp_Object buffer)
{
  if (buffer_window_count (XBUFFER (buffer)))
    {
      Lisp_Object tail, frame;

      /* A single call to window_loop won't do the job because it only
	 considers frames on the current keyboard.  So loop manually over
	 frames, and handle each one.  */
      FOR_EACH_FRAME (tail, frame)
	window_loop (REPLACE_BUFFER_IN_WINDOWS_SAFELY, buffer, true, frame);
    }
}

/* The following three routines are needed for running a window's
   configuration change hook.  */
static void
run_funs (Lisp_Object funs)
{
  for (; CONSP (funs); funs = XCDR (funs))
    if (!EQ (XCAR (funs), Qt))
      call0 (XCAR (funs));
}

static void
select_window_norecord (Lisp_Object window)
{
  if (WINDOW_LIVE_P (window))
    Fselect_window (window, Qt);
}

static void
select_frame_norecord (Lisp_Object frame)
{
  if (FRAME_LIVE_P (XFRAME (frame)))
    Fselect_frame (frame, Qt);
}

/**
 * run_window_configuration_change_hook:
 *
 * Run any functions on 'window-configuration-change-hook' for the
 * frame specified by F.  The buffer-local values are run with the
 * window showing the buffer selected.  The default value is run with
 * the frame specified by F selected.  All functions are called with
 * the selected window's buffer current.
 */
static void
run_window_configuration_change_hook (struct frame *f)
{
  specpdl_ref count = SPECPDL_INDEX ();
  Lisp_Object frame, global_wcch
    = Fdefault_value (Qwindow_configuration_change_hook);
  XSETFRAME (frame, f);

  if (!f->can_set_window_size || !f->after_make_frame)
    return;

  /* Use the right buffer.  Matters when running the local hooks.  */
  if (current_buffer != XBUFFER (Fwindow_buffer (Qnil)))
    {
      record_unwind_current_buffer ();
      Fset_buffer (Fwindow_buffer (Qnil));
    }

  if (SELECTED_FRAME () != f)
    {
      record_unwind_protect (select_frame_norecord, selected_frame);
      select_frame_norecord (frame);
    }

  /* Look for buffer-local values.  */
  {
    Lisp_Object windows = Fwindow_list (frame, Qlambda, Qnil);
    for (; CONSP (windows); windows = XCDR (windows))
      {
	Lisp_Object window = XCAR (windows);
	Lisp_Object buffer = Fwindow_buffer (window);
	if (!NILP (Flocal_variable_p (Qwindow_configuration_change_hook,
				      buffer)))
	  {
	    specpdl_ref inner_count = SPECPDL_INDEX ();
	    record_unwind_protect (select_window_norecord, selected_window);
	    select_window_norecord (window);
	    run_funs (Fbuffer_local_value (Qwindow_configuration_change_hook,
					   buffer));
	    unbind_to (inner_count, Qnil);
	  }
      }
  }

  run_funs (global_wcch);
  unbind_to (count, Qnil);
}

DEFUN ("run-window-configuration-change-hook", Frun_window_configuration_change_hook,
       Srun_window_configuration_change_hook, 0, 1, 0,
       doc: /* Run `window-configuration-change-hook' for FRAME.
If FRAME is omitted or nil, it defaults to the selected frame.

This function should not be needed any more and will be therefore
considered obsolete.  */)
  (Lisp_Object frame)
{
  run_window_configuration_change_hook (decode_live_frame (frame));
  return Qnil;
}

DEFUN ("run-window-scroll-functions", Frun_window_scroll_functions,
       Srun_window_scroll_functions, 0, 1, 0,
       doc: /* Run `window-scroll-functions' for WINDOW.
If WINDOW is omitted or nil, it defaults to the selected window.

This function is called by `split-window' for the new window, after it
has established the size of the new window.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);
  specpdl_ref count = SPECPDL_INDEX ();

  record_unwind_current_buffer ();
  Fset_buffer (w->contents);
  if (!NILP (Vwindow_scroll_functions))
    run_hook_with_args_2 (Qwindow_scroll_functions, window,
			  Fmarker_position (w->start));
  unbind_to (count, Qnil);

  return Qnil;
}


/**
 * window_sub_list:
 *
 * Return list of live windows constructed by traversing any window
 * sub-tree rooted at WINDOW in preorder followed by right siblings of
 * WINDOW.  Called from outside with second argument WINDOWS nil.  The
 * returned list is in reverse order.
 */
static Lisp_Object
window_sub_list (Lisp_Object window, Lisp_Object windows)
{

  struct window *w = XWINDOW (window);

  while (w)
    {
      if (WINDOW_INTERNAL_P (w))
	windows = window_sub_list (w->contents, windows);
      else
	windows = Fcons (window, windows);

      window = w->next;
      w = NILP (window) ? 0 : XWINDOW (window);
    }

  return windows;
}


/**
 * window_change_record_windows:
 *
 * Record changes for all live windows found by traversing any window
 * sub-tree rooted at WINDOW in preorder followed by any right
 * siblings of WINDOW.  This sets the old buffer, old pixel and old
 * body pixel sizes of each live window found to the respective
 * current values.  It also sets the change stamp of each window found
 * to STAMP.  Return the number of live windows found.
 *
 * When not called by itself recursively, WINDOW is its frame's root
 * window, STAMP is the current change stamp of WINDOW's frame and
 * NUMBER is 0.
 */
static ptrdiff_t
window_change_record_windows (Lisp_Object window, int stamp, ptrdiff_t number)
{
  struct window *w = XWINDOW (window);

  while (w)
    {
      if (WINDOW_INTERNAL_P (w))
	number = window_change_record_windows (w->contents, stamp, number);
      else
	{
	  number += 1;
	  w->change_stamp = stamp;
	  wset_old_buffer (w, w->contents);
	  w->old_pixel_width = w->pixel_width;
	  w->old_pixel_height = w->pixel_height;
	  w->old_body_pixel_width
	    = window_body_width (w, WINDOW_BODY_IN_PIXELS);
	  w->old_body_pixel_height
	    = window_body_height (w, WINDOW_BODY_IN_PIXELS);
	}

      w = NILP (w->next) ? 0 : XWINDOW (w->next);
    }

  return number;
}


/**
 * window_change_record:
 *
 * For each frame that has recorded changes, record its selected
 * window, update Fchange stamp, record the states of all its live
 * windows via window_change_record_windows and reset its
 * window_change and window_state_change flags.
 *
 * Record selected window in old_selected_window and selected frame in
 * old_selected_frame.
 */
static void
window_change_record (void)
{
  if (window_change_record_frames)
    {
      Lisp_Object tail, frame;

      FOR_EACH_FRAME (tail, frame)
	{
	  struct frame *f = XFRAME (frame);

	  /* Record FRAME's selected window.  */
	  fset_old_selected_window (f, FRAME_SELECTED_WINDOW (f));

	  /* Bump up FRAME's change stamp.  If this wraps, make it 1 to avoid
	     that a new window (whose change stamp is always set to 0) gets
	     reported as "existing before".  */
	  f->change_stamp += 1;
	  if (f->change_stamp == 0)
	    f->change_stamp = 1;

	  /* Bump up the change stamps of all live windows on this frame so
	     the next call of this function can tell whether any of them
	     "existed before" and record state for each of these windows.  */
	  f->number_of_windows
	    = window_change_record_windows (f->root_window, f->change_stamp, 0);

	  /* Reset our flags.  */
	  FRAME_WINDOW_CHANGE (f) = false;
	  FRAME_WINDOW_STATE_CHANGE (f) = false;
	}
    }

  /* Strictly spoken we don't need old_selected_window at all - its
     value is the old selected window of old_selected_frame.  */
  old_selected_window = selected_window;
  old_selected_frame = selected_frame;
}


/**
 * run_window_change_functions_1:
 *
 * Run window change functions specified by SYMBOL with argument
 * WINDOW_OR_FRAME.  If BUFFER is nil, WINDOW_OR_FRAME specifies a
 * frame.  In this case, run the default value of SYMBOL.  Otherwise,
 * WINDOW_OR_FRAME denotes a window showing BUFFER.  In this case, run
 * the buffer local value of SYMBOL in BUFFER, if any.
 */
static void
run_window_change_functions_1 (Lisp_Object symbol, Lisp_Object buffer,
			       Lisp_Object window_or_frame)
{
  Lisp_Object funs = Qnil;

  if (NILP (buffer))
    funs = Fdefault_value (symbol);
  else if (!NILP (Fassoc (symbol, BVAR (XBUFFER (buffer), local_val_alist),
			  Qnil)))
    /* Don't run global value buffer-locally.  */
    funs = find_symbol_value (XSYMBOL (symbol), NULL);

  while (CONSP (funs))
    {
      if (!EQ (XCAR (funs), Qt)
	  && (NILP (buffer)
	      ? FRAME_LIVE_P (XFRAME (window_or_frame))
	      : WINDOW_LIVE_P (window_or_frame)))
	{
	  /* Any function called here may change the state of any
	     frame.  Make sure to record changes for each live frame
	     in window_change_record later.  */
	  window_change_record_frames = true;
	  safe_calln (XCAR (funs), window_or_frame);
	}

      funs = XCDR (funs);
    }
}


/**
 * run_window_change_functions:
 *
 * Run window change functions for each live frame.  This function
 * must be called from a "safe" position in redisplay_internal.
 *
 * Do not run any functions for a frame whose window_change flag is
 * nil, where no window selection happened and whose window state
 * change flag was not set since the last time this function was
 * called.  Never run any functions for tooltip frames.
 *
 * The change functions run are, in this order:
 *
 * 'window-buffer-change-functions' which are run for a window that
 * changed its buffer or that was not shown the last time window
 * change functions were run.  The default value is also run when a
 * window was deleted since the last time window change functions were
 * run.
 *
 * `window-size-change-functions' run for a window that changed its
 * body or total size, a window that changed its buffer or a window
 * that was not shown the last time window change functions were run.
 *
 * `window-selected-change-functions' run for a window that was
 * (de-)selected since the last time window change functions were run.
 *
 * `window-state-change-functions' run for a window for which any of
 * the above three changes occurred.
 *
 * A buffer-local value of these functions is run if and only if the
 * window for which the functions are run currently shows the buffer.
 * Each call gets one argument - the window showing the buffer.  This
 * means that the buffer-local value of these functions may be called
 * as many times as the buffer is shown on the frame.
 *
 * The default values of these functions are called only after all
 * buffer-local values for all of these functions have been run.  Each
 * such call receives one argument - the frame for which a change
 * occurred.  Functions on `window-state-change-functions' are run
 * also if the corresponding frame's window state change flag has been
 * set.
 *
 * After the four change functions cited above have been run in the
 * indicated way, functions on 'window-configuration-change-hook' are
 * run.  A buffer-local value is run if a window shows that buffer and
 * has either changed its buffer or its body or total size or did not
 * appear on this frame since the last time window change functions
 * were run.  The functions are called without argument and with the
 * buffer's window selected.  The default value is run without
 * argument and with the frame for which the function is run selected.
 *
 * In a final step, functions on `window-state-change-hook' are run
 * provided a window state change has occurred or the window state
 * change flag has been set on at least one frame.  Each of these
 * functions is called without argument.
 *
 * This function does not save and restore match data.  Any functions
 * it calls are responsible for doing that themselves.
 *
 * Additionally, report changes to each frame's selected window to the
 * input method in textconv.c.
 */
void
run_window_change_functions (void)
{
  Lisp_Object tail, frame;
  bool selected_frame_change = !EQ (selected_frame, old_selected_frame);
  bool run_window_state_change_hook = false;
  specpdl_ref count = SPECPDL_INDEX ();

  window_change_record_frames = false;
  record_unwind_protect_void (window_change_record);
  specbind (Qinhibit_redisplay, Qt);

  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      Lisp_Object root = FRAME_ROOT_WINDOW (f);
      bool frame_window_change = FRAME_WINDOW_CHANGE (f);
      bool window_buffer_change, window_size_change;
      bool frame_buffer_change = false, frame_size_change = false;
      bool frame_selected_change
	= (selected_frame_change
	   && (EQ (frame, old_selected_frame)
	       || EQ (frame, selected_frame)));
      bool frame_selected_window_change
	= !EQ (FRAME_OLD_SELECTED_WINDOW (f), FRAME_SELECTED_WINDOW (f));
      bool frame_window_state_change = FRAME_WINDOW_STATE_CHANGE (f);
      bool window_deleted = false;
      Lisp_Object windows;
      ptrdiff_t number_of_windows;

      if (!FRAME_LIVE_P (f)
	  || !f->can_set_window_size
	  || !f->after_make_frame
	  || FRAME_TOOLTIP_P (f)
	  || !(frame_window_change
	       || frame_selected_change
	       || frame_selected_window_change
	       || frame_window_state_change))
	/* Either we are not allowed to run hooks for this frame or no
	   window change has been reported for it since the last time
	   we ran window change functions on it.  */
	continue;

      /* Analyze windows and run buffer locals hooks in pre-order.  */
      windows = Fnreverse (window_sub_list (root, Qnil));
      number_of_windows = 0;

      /* The following loop collects all data needed to tell whether
	 the default value of a hook shall be run and runs any buffer
	 local hooks right away.  */
      for (; CONSP (windows); windows = XCDR (windows))
	{
	  Lisp_Object window = XCAR (windows);
	  struct window *w = XWINDOW (window);
	  Lisp_Object buffer = WINDOW_BUFFER (w);

	  /* Count this window even if it has been deleted while
	     running a hook.  */
	  number_of_windows += 1;

	  if (!WINDOW_LIVE_P (window))
	    continue;

	  /* A "buffer change" means either the window's buffer
	     changed or the window was not part of this frame the last
	     time window change functions were run for it.  */
	  window_buffer_change =
	    (frame_window_change
	     && (!EQ (buffer, w->old_buffer)
		 || w->change_stamp != f->change_stamp));
	  /* A "size change" means either a buffer change or that the
	     total or body size of the window has changed.

	     Note: A buffer change implies a size change because either
	     this window didn't show the buffer before or this window
	     didn't show the buffer the last time the window change
	     functions were run.  In either case, an application
	     tracing size changes in a buffer-locally fashion might
	     want to be informed about that change.  */
	  window_size_change =
	    window_buffer_change
	    || (frame_window_change
		&& (w->pixel_width != w->old_pixel_width
		    || w->pixel_height != w->old_pixel_height
		    || (window_body_width (w, WINDOW_BODY_IN_PIXELS)
			!= w->old_body_pixel_width)
		    || (window_body_height (w, WINDOW_BODY_IN_PIXELS)
			!= w->old_body_pixel_height)));

	  /* The following two are needed when running the default
	     values for this frame below.  */
	  frame_buffer_change = frame_buffer_change || window_buffer_change;
	  frame_size_change = frame_size_change || window_size_change;

	  if (window_buffer_change)
	    run_window_change_functions_1
	      (Qwindow_buffer_change_functions, buffer, window);

	  if (window_size_change && WINDOW_LIVE_P (window))
	    run_window_change_functions_1
	      (Qwindow_size_change_functions, buffer, window);

	  /* This window's selection has changed when it was
	     (de-)selected as its frame's or the globally selected
	     window.  */
	  if (((frame_selected_change
		&& (EQ (window, old_selected_window)
		    || EQ (window, selected_window)))
	       || (frame_selected_window_change
		   && (EQ (window, FRAME_OLD_SELECTED_WINDOW (f))
		       || EQ (window, FRAME_SELECTED_WINDOW (f)))))
	      && WINDOW_LIVE_P (window))
	    run_window_change_functions_1
	      (Qwindow_selection_change_functions, buffer, window);

	  /* This window's state has changed when its buffer or size
	     changed or it was (de-)selected as its frame's or the
	     globally selected window.  */
	  if ((window_buffer_change
	       || window_size_change
	       || ((frame_selected_change
		    && (EQ (window, old_selected_window)
			|| EQ (window, selected_window)))
		   || (frame_selected_window_change
		       && (EQ (window, FRAME_OLD_SELECTED_WINDOW (f))
			   || EQ (window, FRAME_SELECTED_WINDOW (f))))))
	      && WINDOW_LIVE_P (window))
	    run_window_change_functions_1
	      (Qwindow_state_change_functions, buffer, window);
	}

      /* When the number of windows on a frame has decreased, at least
	 one window of that frame was deleted.  In that case, we want
	 to run the default buffer and configuration change hooks.  The
	 default size change hook is not necessarily run in that case,
	 but usually will be unless the deletion was "compensated" by
	 a reduction of the frame size or an increase of a minibuffer
	 window size.  */
      window_deleted = number_of_windows < f->number_of_windows;
      /* A frame changed buffers when one of its windows has changed
	 its buffer or at least one window was deleted.  */
      if ((frame_buffer_change || window_deleted) && FRAME_LIVE_P (f))
	run_window_change_functions_1
	  (Qwindow_buffer_change_functions, Qnil, frame);

      /* A size change occurred when at least one of the frame's
	 windows has changed size.  */
      if (frame_size_change && FRAME_LIVE_P (f))
	run_window_change_functions_1
	  (Qwindow_size_change_functions, Qnil, frame);

      /* A frame has changed its window selection when its selected
	 window has changed or when it was (de-)selected.  */
      if ((frame_selected_change || frame_selected_window_change)
	  && FRAME_LIVE_P (f))
	run_window_change_functions_1
	  (Qwindow_selection_change_functions, Qnil, frame);

#if defined HAVE_TEXT_CONVERSION

      /* If the buffer or selected window has changed, also reset the
	 input method composition state.  */

      if ((frame_selected_window_change || frame_buffer_change)
	  && FRAME_LIVE_P (f)
	  && FRAME_WINDOW_P (f))
	report_selected_window_change (f);

#endif

      /* A frame has changed state when a size or buffer change
	 occurred, its selected window has changed, when it was
	 (de-)selected or its window state change flag was set.  */
      if ((frame_selected_change || frame_selected_window_change
	   || frame_buffer_change || window_deleted
	   || frame_size_change || frame_window_state_change)
	  && FRAME_LIVE_P (f))
	{
	  run_window_change_functions_1
	    (Qwindow_state_change_functions, Qnil, frame);
	  /* Make sure to run 'window-state-change-hook' later.  */
	  run_window_state_change_hook = true;
	  /*  Make sure to record changes for each live frame in
	     window_change_record later.  */
	  window_change_record_frames = true;
	}

      /* A frame's configuration changed when one of its windows has
	 changed buffer or size or at least one window was deleted.  */
      if ((frame_size_change || window_deleted) && FRAME_LIVE_P (f))
	/* This will run any buffer local window configuration change
	   hook as well.  */
	run_window_configuration_change_hook (f);
    }

  /* Run 'window-state-change-hook' if at least one frame has changed
     state.  */
  if (run_window_state_change_hook && !NILP (Vwindow_state_change_hook))
    safe_run_hooks (Qwindow_state_change_hook);

  /* Record changes for all frames (if asked for), selected window and
     frame.  */
  unbind_to (count, Qnil);
}

/* Make WINDOW display BUFFER.  RUN_HOOKS_P means it's allowed
   to run hooks.  See make_frame for a case where it's not allowed.
   KEEP_MARGINS_P means that the current margins, fringes, and
   scroll bar settings of the window are not reset from the buffer's
   local settings.  */

void
set_window_buffer (Lisp_Object window, Lisp_Object buffer,
		   bool run_hooks_p, bool keep_margins_p)
{
  struct window *w = XWINDOW (window);
  struct buffer *b = XBUFFER (buffer);
  specpdl_ref count = SPECPDL_INDEX ();
  bool samebuf = EQ (buffer, w->contents);

  /* It's never OK to assign WINDOW a dead buffer.  */
  eassert (BUFFER_LIVE_P (b));

  wset_buffer (w, buffer);

  if (EQ (window, selected_window))
    bset_last_selected_window (b, window);

  /* Let redisplay errors through.  */
  b->display_error_modiff = 0;

  /* Update time stamps of buffer display.  */
  if (INTEGERP (BVAR (b, display_count)))
    bset_display_count (b, Fadd1 (BVAR (b, display_count)));
  bset_display_time (b, Fcurrent_time ());

  w->window_end_pos = 0;
  w->window_end_vpos = 0;
  w->last_cursor_vpos = 0;

  /* Discard BUFFER from WINDOW's previous and next buffers.  */
  window_discard_buffer_from_window (buffer, window, false);

  if (!(keep_margins_p && samebuf))
    { /* If we're not actually changing the buffer, don't reset hscroll
	 and vscroll.  Resetting hscroll and vscroll here is problematic
	 for things like image-mode and doc-view-mode since it resets
	 the image's position whenever we resize the frame.  */
      w->hscroll = w->min_hscroll = w->hscroll_whole = 0;
      w->suspend_auto_hscroll = false;
      w->vscroll = 0;
      set_marker_both (w->pointm, buffer, BUF_PT (b), BUF_PT_BYTE (b));
      set_marker_both (w->old_pointm, buffer, BUF_PT (b), BUF_PT_BYTE (b));
      set_marker_restricted (w->start,
			     make_fixnum (b->last_window_start),
			     buffer);
      w->start_at_line_beg = false;
      w->force_start = false;
      /* Flush the base_line cache since it applied to another buffer.  */
      w->base_line_number = 0;
    }

  wset_redisplay (w);
  wset_update_mode_line (w);

  /* We must select BUFFER to run the window-scroll-functions and to look up
     the buffer-local value of Vwindow_point_insertion_type.  */
  record_unwind_current_buffer ();
  Fset_buffer (buffer);

  XMARKER (w->pointm)->insertion_type = !NILP (Vwindow_point_insertion_type);
  XMARKER (w->old_pointm)->insertion_type = !NILP (Vwindow_point_insertion_type);

  if (!keep_margins_p)
    {
      /* Set fringes and scroll bars from buffer unless they have been
	 declared as persistent.  */
      if (!w->fringes_persistent)
	set_window_fringes (w, BVAR (b, left_fringe_width),
			    BVAR (b, right_fringe_width),
			    BVAR (b, fringes_outside_margins), Qnil);
      if (!w->scroll_bars_persistent)
	set_window_scroll_bars (w, BVAR (b, scroll_bar_width),
				BVAR (b, vertical_scroll_bar_type),
				BVAR (b, scroll_bar_height),
				BVAR (b, horizontal_scroll_bar_type), Qnil);
      /* Set left and right marginal area width from buffer.  */
      set_window_margins (w, BVAR (b, left_margin_cols),
			  BVAR (b, right_margin_cols));
      apply_window_adjustment (w);
    }

  if (run_hooks_p && !NILP (Vwindow_scroll_functions))
    run_hook_with_args_2 (Qwindow_scroll_functions, window,
			  Fmarker_position (w->start));

  /* Ensure that window change functions are run later if the buffer
     differs and the window is neither a mini nor a pseudo window.

     Note: Running window change functions for the minibuffer is noisy
     and was generally suppressed in the past.  Is there any reason we
     should run them?  */
  if (!samebuf && !MINI_WINDOW_P (w) && !WINDOW_PSEUDO_P (w))
    FRAME_WINDOW_CHANGE (XFRAME (w->frame)) = true;

  unbind_to (count, Qnil);
}

DEFUN ("set-window-buffer", Fset_window_buffer, Sset_window_buffer, 2, 3, 0,
       doc: /* Make WINDOW display BUFFER-OR-NAME.
WINDOW must be a live window and defaults to the selected one.
BUFFER-OR-NAME must be a buffer or the name of an existing buffer.

Optional third argument KEEP-MARGINS non-nil means that WINDOW's current
display margins, fringe widths, and scroll bar settings are preserved;
the default is to reset these from the local settings for BUFFER-OR-NAME
or the frame defaults.  Return nil.

This function throws an error when WINDOW is strongly dedicated to its
buffer (that is `window-dedicated-p' returns t for WINDOW) and does not
already display BUFFER-OR-NAME.

This function runs `window-scroll-functions' before running
`window-configuration-change-hook'.  */)
  (register Lisp_Object window, Lisp_Object buffer_or_name, Lisp_Object keep_margins)
{
  register Lisp_Object tem, buffer;
  register struct window *w = decode_live_window (window);

  XSETWINDOW (window, w);
  buffer = Fget_buffer (buffer_or_name);
  CHECK_BUFFER (buffer);
  if (!BUFFER_LIVE_P (XBUFFER (buffer)))
    error ("Attempt to display deleted buffer");

  tem = w->contents;
  if (NILP (tem))
    error ("Window is deleted");
  else
    {
      if (!EQ (tem, buffer))
	{
	  if (EQ (w->dedicated, Qt))
	    /* WINDOW is strongly dedicated to its buffer, signal an
	       error.  */
	    error ("Window is dedicated to `%s'", SDATA (BVAR (XBUFFER (tem), name)));
	  else
	    /* WINDOW is weakly dedicated to its buffer, reset
	       dedication.  */
	    wset_dedicated (w, Qnil);

	  call1 (Qrecord_window_buffer, window);
	}

      unshow_buffer (w);
    }

  set_window_buffer (window, buffer, true, !NILP (keep_margins));

  return Qnil;
}

static Lisp_Object
display_buffer (Lisp_Object buffer, Lisp_Object not_this_window_p, Lisp_Object override_frame)
{
  return call3 (Qdisplay_buffer, buffer, not_this_window_p, override_frame);
}

DEFUN ("force-window-update", Fforce_window_update, Sforce_window_update,
       0, 1, 0,
       doc: /* Force all windows to be updated on next redisplay.
If optional arg OBJECT is a window, force redisplay of that window only.
If OBJECT is a buffer or buffer name, force redisplay of all windows
displaying that buffer.  */)
  (Lisp_Object object)
{
  if (NILP (object))
    {
      windows_or_buffers_changed = 29;
      update_mode_lines = 28;
      return Qt;
    }

  if (WINDOW_LIVE_P (object))
    {
      struct window *w = XWINDOW (object);
      mark_window_display_accurate (object, false);
      w->update_mode_line = true;
      if (BUFFERP (w->contents))
	XBUFFER (w->contents)->prevent_redisplay_optimizations_p = true;
      update_mode_lines = 29;
      return Qt;
    }

  if (STRINGP (object))
    object = Fget_buffer (object);
  if (BUFFERP (object) && BUFFER_LIVE_P (XBUFFER (object))
      && buffer_window_count (XBUFFER (object)))
    {
      /* If buffer is live and shown in at least one window, find
	 all windows showing this buffer and force update of them.  */
      object = window_loop (REDISPLAY_BUFFER_WINDOWS, object, false, Qvisible);
      return NILP (object) ? Qnil : Qt;
    }

  /* If nothing suitable was found, just return.
     We could signal an error, but this feature will typically be used
     asynchronously in timers or process sentinels, so we don't.  */
  return Qnil;
}

/* Obsolete since 24.3.  */
void
temp_output_buffer_show (register Lisp_Object buf)
{
  register struct buffer *old = current_buffer;
  register Lisp_Object window;
  register struct window *w;

  bset_directory (XBUFFER (buf), BVAR (current_buffer, directory));

  Fset_buffer (buf);
  BUF_SAVE_MODIFF (XBUFFER (buf)) = MODIFF;
  BEGV = BEG;
  ZV = Z;
  SET_PT (BEG);
  set_buffer_internal (old);

  if (!NILP (Vtemp_buffer_show_function))
    call1 (Vtemp_buffer_show_function, buf);
  else if (WINDOW_LIVE_P (window = display_buffer (buf, Qnil, Qnil)))
    {
      if (!EQ (XWINDOW (window)->frame, selected_frame))
	Fmake_frame_visible (WINDOW_FRAME (XWINDOW (window)));
      Vminibuf_scroll_window = window;
      w = XWINDOW (window);
      w->hscroll = w->min_hscroll = w->hscroll_whole = 0;
      w->suspend_auto_hscroll = false;
      set_marker_restricted_both (w->start, buf, BEG, BEG);
      set_marker_restricted_both (w->pointm, buf, BEG, BEG);
      set_marker_restricted_both (w->old_pointm, buf, BEG, BEG);

      /* Run temp-buffer-show-hook, with the chosen window selected
	 and its buffer current.  */
      {
        specpdl_ref count = SPECPDL_INDEX ();
        Lisp_Object prev_window, prev_buffer;
        prev_window = selected_window;
        XSETBUFFER (prev_buffer, old);

        /* Select the window that was chosen, for running the hook.
           Note: Both Fselect_window and select_window_norecord may
           set-buffer to the buffer displayed in the window,
           so we need to save the current buffer.  --stef  */
        record_unwind_protect (restore_buffer, prev_buffer);
        record_unwind_protect (select_window_norecord, prev_window);
        Fselect_window (window, Qt);
        Fset_buffer (w->contents);
        run_hook (Qtemp_buffer_show_hook);
        unbind_to (count, Qnil);
      }
    }
}

/* Allocate basically initialized window.  */

static struct window *
allocate_window (void)
{
  return ALLOCATE_ZEROED_PSEUDOVECTOR (struct window, mode_line_help_echo,
				       PVEC_WINDOW);
}

/* Make new window, have it replace WINDOW in window-tree, and make
   WINDOW its only vertical child (HORFLAG means make WINDOW its only
   horizontal child).   */
static void
make_parent_window (Lisp_Object window, bool horflag)
{
  Lisp_Object parent;
  register struct window *o, *p;

  o = XWINDOW (window);
  p = allocate_window ();
  memcpy ((char *) p + sizeof (union vectorlike_header),
	  (char *) o + sizeof (union vectorlike_header),
	  word_size * VECSIZE (struct window));
  /* P's buffer slot may change from nil to a buffer...  */
  adjust_window_count (p, 1);
  XSETWINDOW (parent, p);

  p->sequence_number = ++sequence_number;

  replace_window (window, parent, true);

  wset_next (o, Qnil);
  wset_prev (o, Qnil);
  wset_parent (o, parent);
  /* ...but now P becomes an internal window.  */
  wset_start (p, Qnil);
  wset_pointm (p, Qnil);
  wset_old_pointm (p, Qnil);
  wset_buffer (p, Qnil);
  wset_combination (p, horflag, window);
  wset_combination_limit (p, Qnil);
  /* Reset any previous and next buffers of p which have been installed
     by the memcpy above.  */
  wset_prev_buffers (p, Qnil);
  wset_next_buffers (p, Qnil);
  wset_window_parameters (p, Qnil);
}

/* Make new window from scratch.  */
Lisp_Object
make_window (void)
{
  Lisp_Object window;
  register struct window *w;

  w = allocate_window ();
  /* Initialize Lisp data.  Note that allocate_window initializes all
     Lisp data to nil, so do it only for slots which should not be nil.  */
  wset_normal_lines (w, make_float (1.0));
  wset_normal_cols (w, make_float (1.0));
  wset_new_total (w, make_fixnum (0));
  wset_new_normal (w, make_fixnum (0));
  wset_new_pixel (w, make_fixnum (0));
  wset_start (w, Fmake_marker ());
  wset_pointm (w, Fmake_marker ());
  wset_old_pointm (w, Fmake_marker ());
  wset_vertical_scroll_bar_type (w, Qt);
  wset_horizontal_scroll_bar_type (w, Qt);
  wset_cursor_type (w, Qt);

  /* Initialize non-Lisp data.  Note that allocate_window zeroes out all
     non-Lisp data, so do it only for slots which should not be zero.  */
  w->nrows_scale_factor = w->ncols_scale_factor = 1;
  w->left_fringe_width = w->right_fringe_width = -1;
  w->mode_line_height = w->tab_line_height = w->header_line_height = -1;
#ifdef HAVE_WINDOW_SYSTEM
  w->phys_cursor_type = NO_CURSOR;
  w->phys_cursor_width = -1;
#endif
  w->sequence_number = ++sequence_number;
  w->scroll_bar_width = -1;
  w->scroll_bar_height = -1;
  w->column_number_displayed = -1;
  /* Reset window_list.  */
  Vwindow_list = Qnil;
  /* Return window.  */
  XSETWINDOW (window, w);
  return window;
}

DEFUN ("set-window-new-pixel", Fset_window_new_pixel, Sset_window_new_pixel, 2, 3, 0,
       doc: /* Set new pixel size of WINDOW to SIZE.
WINDOW must be a valid window and defaults to the selected one.
Return SIZE.

Optional argument ADD non-nil means add SIZE to the new pixel size of
WINDOW and return the sum.

The new pixel size of WINDOW, if valid, will be shortly installed as
WINDOW's pixel height (see `window-pixel-height') or pixel width (see
`window-pixel-width').

Note: This function does not operate on any child windows of WINDOW.  */)
  (Lisp_Object window, Lisp_Object size, Lisp_Object add)
{
  struct window *w = decode_valid_window (window);
  EMACS_INT size_min = NILP (add) ? 0 : - XFIXNUM (w->new_pixel);
  EMACS_INT size_max = size_min + min (INT_MAX, MOST_POSITIVE_FIXNUM);

  int checked_size = check_integer_range (size, size_min, size_max);
  if (NILP (add))
    wset_new_pixel (w, size);
  else
    wset_new_pixel (w, make_fixnum (XFIXNUM (w->new_pixel) + checked_size));

  return w->new_pixel;
}

DEFUN ("set-window-new-total", Fset_window_new_total, Sset_window_new_total, 2, 3, 0,
       doc: /* Set new total size of WINDOW to SIZE.
WINDOW must be a valid window and defaults to the selected one.
Return SIZE.

Optional argument ADD non-nil means add SIZE to the new total size of
WINDOW and return the sum.

The new total size of WINDOW, if valid, will be shortly installed as
WINDOW's total height (see `window-total-height') or total width (see
`window-total-width').

Note: This function does not operate on any child windows of WINDOW.  */)
     (Lisp_Object window, Lisp_Object size, Lisp_Object add)
{
  struct window *w = decode_valid_window (window);

  CHECK_FIXNUM (size);
  if (NILP (add))
    wset_new_total (w, size);
  else
    wset_new_total (w, make_fixnum (XFIXNUM (w->new_total) + XFIXNUM (size)));

  return w->new_total;
}

DEFUN ("set-window-new-normal", Fset_window_new_normal, Sset_window_new_normal, 1, 2, 0,
       doc: /* Set new normal size of WINDOW to SIZE.
WINDOW must be a valid window and defaults to the selected one.
Return SIZE.

The new normal size of WINDOW, if valid, will be shortly installed as
WINDOW's normal size (see `window-normal-size').

Note: This function does not operate on any child windows of WINDOW.  */)
     (Lisp_Object window, Lisp_Object size)
{
  wset_new_normal (decode_valid_window (window), size);
  return size;
}

/* Return true if setting w->pixel_height (w->pixel_width if HORFLAG)
   to w->new_pixel would result in correct heights (widths)
   for window W and recursively all child windows of W.

   Note: This function does not check any of `window-fixed-size-p',
   `window-min-height' or `window-min-width'.  It does check that window
   sizes do not drop below one line (two columns). */
static bool
window_resize_check (struct window *w, bool horflag)
{
  struct frame *f = XFRAME (w->frame);
  struct window *c;

  if (WINDOW_VERTICAL_COMBINATION_P (w))
    /* W is a vertical combination.  */
    {
      c = XWINDOW (w->contents);
      if (horflag)
	/* All child windows of W must have the same width as W.  */
	{
	  while (c)
	    {
	      if (XFIXNUM (c->new_pixel) != XFIXNUM (w->new_pixel)
		  || !window_resize_check (c, horflag))
		return false;

	      c = NILP (c->next) ? 0 : XWINDOW (c->next);
	    }

	  return true;
	}
      else
	/* The sum of the heights of the child windows of W must equal
	   W's height.  */
	{
	  int remaining_pixels = XFIXNUM (w->new_pixel);

	  while (c)
	    {
	      if (!window_resize_check (c, horflag))
		return false;

	      remaining_pixels -= XFIXNUM (c->new_pixel);
	      if (remaining_pixels < 0)
		return false;
	      c = NILP (c->next) ? 0 : XWINDOW (c->next);
	    }

	  return remaining_pixels == 0;
	}
    }
  else if (WINDOW_HORIZONTAL_COMBINATION_P (w))
    /* W is a horizontal combination.  */
    {
      c = XWINDOW (w->contents);
      if (horflag)
	/* The sum of the widths of the child windows of W must equal W's
	   width.  */
	{
	  int remaining_pixels = XFIXNUM (w->new_pixel);

	  while (c)
	    {
	      if (!window_resize_check (c, horflag))
		return false;

	      remaining_pixels -= XFIXNUM (c->new_pixel);
	      if (remaining_pixels < 0)
		return false;
	      c = NILP (c->next) ? 0 : XWINDOW (c->next);
	    }

	  return remaining_pixels == 0;
	}
      else
	/* All child windows of W must have the same height as W.  */
	{
	  while (c)
	    {
	      if (XFIXNUM (c->new_pixel) != XFIXNUM (w->new_pixel)
		  || !window_resize_check (c, horflag))
		return false;

	      c = NILP (c->next) ? 0 : XWINDOW (c->next);
	    }

	  return true;
	}
    }
  else
    /* A leaf window.  Make sure it's not too small.  The following
       hardcodes the values of `window-safe-min-width' (2) and
       `window-safe-min-height' (1) which are defined in window.el.  */
    return (XFIXNUM (w->new_pixel) >= (horflag
				       ? 2 * FRAME_COLUMN_WIDTH (f)
				       : FRAME_LINE_HEIGHT (f)));
}


/* Set w->pixel_height (w->pixel_width if HORFLAG) to
   w->new_pixel for window W and recursively all child windows of W.
   Also calculate and assign the new vertical (horizontal) pixel start
   positions of each of these windows.

   This function does not perform any error checks.  Make sure you have
   run window_resize_check on W before applying this function.  */
static void
window_resize_apply (struct window *w, bool horflag)
{
  struct window *c;
  int edge;
  int unit = (horflag
	      ? FRAME_COLUMN_WIDTH (WINDOW_XFRAME (w))
	      : FRAME_LINE_HEIGHT (WINDOW_XFRAME (w)));

  /* Note: Assigning new_normal requires that the new total size of the
     parent window has been set *before*.  */
  if (horflag)
    {
      w->pixel_width = XFIXNAT (w->new_pixel);
      w->total_cols = w->pixel_width / unit;
      if (NUMBERP (w->new_normal))
	wset_normal_cols (w, w->new_normal);

      edge = w->pixel_left;
    }
  else
    {
      w->pixel_height = XFIXNAT (w->new_pixel);
      w->total_lines = w->pixel_height / unit;
      if (NUMBERP (w->new_normal))
	wset_normal_lines (w, w->new_normal);

      edge = w->pixel_top;
    }

  if (WINDOW_VERTICAL_COMBINATION_P (w))
    /* W is a vertical combination.  */
    {
      c = XWINDOW (w->contents);
      while (c)
	{
	  if (horflag)
	    {
	      c->pixel_left = edge;
	      c->left_col = edge / unit;
	    }
	  else
	    {
	      c->pixel_top = edge;
	      c->top_line = edge / unit;
	    }
	  window_resize_apply (c, horflag);
	  if (!horflag)
	    edge = edge + c->pixel_height;

	  c = NILP (c->next) ? 0 : XWINDOW (c->next);
	}
    }
  else if (WINDOW_HORIZONTAL_COMBINATION_P (w))
    /* W is a horizontal combination.  */
    {
      c = XWINDOW (w->contents);
      while (c)
	{
	  if (horflag)
	    {
	      c->pixel_left = edge;
	      c->left_col = edge / unit;
	    }
	  else
	    {
	      c->pixel_top = edge;
	      c->top_line = edge / unit;
	    }

	  window_resize_apply (c, horflag);
	  if (horflag)
	    edge = edge + c->pixel_width;

	  c = NILP (c->next) ? 0 : XWINDOW (c->next);
	}
    }
  else
    /* Bug#15957.  */
    w->window_end_valid = false;

  if (!WINDOW_PSEUDO_P (w))
    FRAME_WINDOW_CHANGE (WINDOW_XFRAME (w)) = true;
}


/* Set w->total_lines (w->total_cols if HORFLAG) to
   w->new_total for window W and recursively all child windows of W.
   Also calculate and assign the new vertical (horizontal) start
   positions of each of these windows.  */
static void
window_resize_apply_total (struct window *w, bool horflag)
{
  struct window *c;
  int edge;

  /* Note: Assigning new_normal requires that the new total size of the
     parent window has been set *before*.  */
  if (horflag)
    {
      w->total_cols = XFIXNAT (w->new_total);
      edge = w->left_col;
    }
  else
    {
      w->total_lines = XFIXNAT (w->new_total);
      edge = w->top_line;
    }

  if (WINDOW_VERTICAL_COMBINATION_P (w))
    /* W is a vertical combination.  */
    {
      c = XWINDOW (w->contents);
      while (c)
	{
	  if (horflag)
	    c->left_col = edge;
	  else
	    c->top_line = edge;

	  window_resize_apply_total (c, horflag);
	  if (!horflag)
	    edge = edge + c->total_lines;

	  c = NILP (c->next) ? 0 : XWINDOW (c->next);
	}
    }
  else if (WINDOW_HORIZONTAL_COMBINATION_P (w))
    /* W is a horizontal combination.  */
    {
      c = XWINDOW (w->contents);
      while (c)
	{
	  if (horflag)
	    c->left_col = edge;
	  else
	    c->top_line = edge;

	  window_resize_apply_total (c, horflag);
	  if (horflag)
	    edge = edge + c->total_cols;

	  c = NILP (c->next) ? 0 : XWINDOW (c->next);
	}
    }
}

DEFUN ("window-resize-apply", Fwindow_resize_apply, Swindow_resize_apply, 0, 2, 0,
       doc: /* Apply requested size values for window-tree of FRAME.
If FRAME is omitted or nil, it defaults to the selected frame.

Optional argument HORIZONTAL omitted or nil means apply requested
height values.  HORIZONTAL non-nil means apply requested width values.

The requested size values are those set by `set-window-new-pixel' and
`set-window-new-normal'.  This function checks whether the requested
values sum up to a valid window layout, recursively assigns the new
sizes of all child windows and calculates and assigns the new start
positions of these windows.

Return t if the requested values have been applied correctly, nil
otherwise.

Note: This function does not check any of `window-fixed-size-p',
`window-min-height' or `window-min-width'.  All these checks have to
be applied on the Elisp level.  */)
     (Lisp_Object frame, Lisp_Object horizontal)
{
  struct frame *f = decode_live_frame (frame);
  struct window *r = XWINDOW (FRAME_ROOT_WINDOW (f));
  bool horflag = !NILP (horizontal);

  if (!window_resize_check (r, horflag)
      || (XFIXNUM (r->new_pixel)
	  != (horflag ? r->pixel_width : r->pixel_height)))
    return Qnil;

  block_input ();
  window_resize_apply (r, horflag);

  fset_redisplay (f);

  adjust_frame_glyphs (f);
  unblock_input ();

  return Qt;
}


DEFUN ("window-resize-apply-total", Fwindow_resize_apply_total, Swindow_resize_apply_total, 0, 2, 0,
       doc: /* Apply requested total size values for window-tree of FRAME.
If FRAME is omitted or nil, it defaults to the selected frame.

This function does not assign pixel or normal size values.  You should
have run `window-resize-apply' before running this.

Optional argument HORIZONTAL omitted or nil means apply requested
height values.  HORIZONTAL non-nil means apply requested width
values.  */)
     (Lisp_Object frame, Lisp_Object horizontal)
{
  struct frame *f = decode_live_frame (frame);
  struct window *r = XWINDOW (FRAME_ROOT_WINDOW (f));

  block_input ();
  /* Necessary when deleting the top-/or leftmost window.  */
  r->left_col = 0;
  r->top_line = FRAME_TOP_MARGIN (f);
  window_resize_apply_total (r, !NILP (horizontal));
  /* Handle the mini window.  */
  if (FRAME_HAS_MINIBUF_P (f) && !FRAME_MINIBUF_ONLY_P (f))
    {
      struct window *m = XWINDOW (f->minibuffer_window);

      if (NILP (horizontal))
	{
	  m->top_line = r->top_line + r->total_lines;
	  m->total_lines = XFIXNAT (m->new_total);
	}
      else
	m->total_cols = XFIXNAT (m->new_total);
    }

  unblock_input ();

  return Qt;
}

/* Resize frame F's windows when F's inner height (inner width if
   HORFLAG is true) has been set to SIZE pixels.  */

void
resize_frame_windows (struct frame *f, int size, bool horflag)
{
  Lisp_Object root = f->root_window;
  struct window *r = XWINDOW (root);
  int old_pixel_size = horflag ? r->pixel_width : r->pixel_height;
  int new_size, new_pixel_size;
  int unit = horflag ? FRAME_COLUMN_WIDTH (f) : FRAME_LINE_HEIGHT (f);
  Lisp_Object mini = f->minibuffer_window;
  struct window *m = WINDOWP (mini) ? XWINDOW (mini) : NULL;
  int mini_height = ((FRAME_HAS_MINIBUF_P (f) && !FRAME_MINIBUF_ONLY_P (f))
		     ? (unit + m->pixel_height
			- window_body_height (m, WINDOW_BODY_IN_PIXELS))
		     : 0);

  new_pixel_size = max (horflag ? size : size - mini_height, unit);
  new_size = new_pixel_size / unit;

  if (new_pixel_size == old_pixel_size
      && (horflag || r->pixel_top == FRAME_TOP_MARGIN_HEIGHT (f)))
    ;
  else if (WINDOW_LEAF_P (r))
    {
      /* For a leaf root window just set the size.  */
      if (horflag)
	{
	  r->total_cols = new_size;
	  r->pixel_width = new_pixel_size;
	}
      else
	{
	  r->top_line = FRAME_TOP_MARGIN (f);
	  r->pixel_top = FRAME_TOP_MARGIN_HEIGHT (f);

	  r->total_lines = new_size;
	  r->pixel_height = new_pixel_size;
	}

      FRAME_WINDOW_CHANGE (f)
	= !WINDOW_PSEUDO_P (r) && new_pixel_size != old_pixel_size;
    }
  else
    {
      Lisp_Object delta;

      if (!horflag)
	{
	  r->top_line = FRAME_TOP_MARGIN (f);
	  r->pixel_top = FRAME_TOP_MARGIN_HEIGHT (f);
	}

      XSETINT (delta, new_pixel_size - old_pixel_size);

      /* Try a "normal" resize first.  */
      resize_root_window (root, delta, horflag ? Qt : Qnil, Qnil, Qt);
      if (window_resize_check (r, horflag)
	  && new_pixel_size == XFIXNUM (r->new_pixel))
	{
	  window_resize_apply (r, horflag);
	  window_pixel_to_total (r->frame, horflag ? Qt : Qnil);
	}
      else
	{
	  /* Try with "reasonable" minimum sizes next.  */
	  resize_root_window (root, delta, horflag ? Qt : Qnil, Qt, Qt);
	  if (window_resize_check (r, horflag)
	      && new_pixel_size == XFIXNUM (r->new_pixel))
	    {
	      window_resize_apply (r, horflag);
	      window_pixel_to_total (r->frame, horflag ? Qt : Qnil);
	    }
	}
    }

  if (FRAME_HAS_MINIBUF_P (f) && !FRAME_MINIBUF_ONLY_P (f))
    {
      m = XWINDOW (mini);
      if (horflag)
	{
	  m->total_cols = new_size;
	  m->pixel_width = new_pixel_size;
	}
      else
	{
	  m->total_lines = mini_height / unit;
	  m->pixel_height = mini_height;
	  m->top_line = r->top_line + r->total_lines;
	  m->pixel_top = r->pixel_top + r->pixel_height;
	}
    }

  fset_redisplay (f);
}


DEFUN ("split-window-internal", Fsplit_window_internal, Ssplit_window_internal, 4, 4, 0,
       doc: /* Split window OLD.
Second argument PIXEL-SIZE specifies the number of pixels of the
new window.  It must be a positive integer.

Third argument SIDE nil (or `below') specifies that the new window shall
be located below WINDOW.  SIDE `above' means the new window shall be
located above WINDOW.  In both cases PIXEL-SIZE specifies the pixel
height of the new window including space reserved for the mode and/or
header/tab line.

SIDE t (or `right') specifies that the new window shall be located on
the right side of WINDOW.  SIDE `left' means the new window shall be
located on the left of WINDOW.  In both cases PIXEL-SIZE specifies the
width of the new window including space reserved for fringes and the
scrollbar or a divider column.

Fourth argument NORMAL-SIZE specifies the normal size of the new window
according to the SIDE argument.

The new pixel and normal sizes of all involved windows must have been
set correctly.  See the code of `split-window' for how this is done.  */)
  (Lisp_Object old, Lisp_Object pixel_size, Lisp_Object side, Lisp_Object normal_size)
{
  /* OLD (*o) is the window we have to split.  (*p) is either OLD's
     parent window or an internal window we have to install as OLD's new
     parent.  REFERENCE (*r) must denote a live window, or is set to OLD
     provided OLD is a leaf window, or to the frame's selected window.
     NEW (*n) is the new window created with some parameters taken from
     REFERENCE (*r).  */
  Lisp_Object new, frame, reference;
  struct window *o, *p, *n, *r, *c;
  struct frame *f;
  bool horflag
    /* HORFLAG is true when we split side-by-side, false otherwise.  */
    = EQ (side, Qt) || EQ (side, Qleft) || EQ (side, Qright);

  CHECK_WINDOW (old);
  o = XWINDOW (old);
  frame = WINDOW_FRAME (o);
  f = XFRAME (frame);

  CHECK_FIXNUM (pixel_size);
  EMACS_INT total_size
    = XFIXNUM (pixel_size) / (horflag
			   ? FRAME_COLUMN_WIDTH (f)
			   : FRAME_LINE_HEIGHT (f));

  /* Set combination_limit if we have to make a new parent window.
     We do that if either `window-combination-limit' is t, or OLD has no
     parent, or OLD is ortho-combined.  */
  bool combination_limit
    = (EQ (Vwindow_combination_limit, Qt)
       || NILP (o->parent)
       || (horflag
	   ? WINDOW_VERTICAL_COMBINATION_P (XWINDOW (o->parent))
	   : WINDOW_HORIZONTAL_COMBINATION_P (XWINDOW (o->parent))));

  /* We need a live reference window to initialize some parameters.  */
  if (WINDOW_LIVE_P (old))
    /* OLD is live, use it as reference window.  */
    reference = old;
  else
    /* Use the frame's selected window as reference window.  */
    reference = FRAME_SELECTED_WINDOW (f);
  r = XWINDOW (reference);

  /* The following bugs are caught by `split-window'.  */
  if (MINI_WINDOW_P (o))
    error ("Attempt to split minibuffer window");
  else if (total_size < (horflag ? 2 : 1))
    error ("Size of new window too small (after split)");
  else if (!combination_limit && !NILP (Vwindow_combination_resize))
    /* `window-combination-resize' non-nil means try to resize OLD's siblings
       proportionally.  */
    {
      p = XWINDOW (o->parent);
      /* Temporarily pretend we split the parent window.  */
      wset_new_pixel
	(p, make_fixnum ((horflag ? p->pixel_width : p->pixel_height)
			 - XFIXNUM (pixel_size)));
      if (!window_resize_check (p, horflag))
	error ("Window sizes don't fit");
      else
	/* Undo the temporary pretension.  */
	wset_new_pixel (p, make_fixnum (horflag ? p->pixel_width : p->pixel_height));
    }
  else
    {
      if (!window_resize_check (o, horflag))
	error ("Resizing old window failed");
      else if (XFIXNUM (pixel_size) + XFIXNUM (o->new_pixel)
	       != (horflag ? o->pixel_width : o->pixel_height))
	error ("Sum of sizes of old and new window don't fit");
    }

  /* This is our point of no return.  */
  if (combination_limit)
    {
      /* Save the old value of o->normal_cols/lines.  It gets corrupted
	 by make_parent_window and we need it below for assigning it to
	 p->new_normal.  */
      Lisp_Object new_normal
	= horflag ? o->normal_cols : o->normal_lines;

      make_parent_window (old, horflag);
      p = XWINDOW (o->parent);
      if (EQ (Vwindow_combination_limit, Qt))
	/* Store t in the new parent's combination_limit slot to avoid
	   that its children get merged into another window.  */
	wset_combination_limit (p, Qt);
      /* These get applied below.  */
      wset_new_pixel
	(p, make_fixnum (horflag ? o->pixel_width : o->pixel_height));
      wset_new_total
	(p, make_fixnum (horflag ? o->total_cols : o->total_lines));
      wset_new_normal (p, new_normal);
    }
  else
    p = XWINDOW (o->parent);

  fset_redisplay (f);
  new = make_window ();
  n = XWINDOW (new);
  wset_frame (n, frame);
  wset_parent (n, o->parent);

  if (EQ (side, Qabove) || EQ (side, Qleft))
    {
      wset_prev (n, o->prev);
      if (NILP (n->prev))
	wset_combination (p, horflag, new);
      else
	wset_next (XWINDOW (n->prev), new);
      wset_next (n, old);
      wset_prev (o, new);
    }
  else
    {
      wset_next (n, o->next);
      if (!NILP (n->next))
	wset_prev (XWINDOW (n->next), new);
      wset_prev (n, old);
      wset_next (o, new);
    }

  n->window_end_valid = false;
  n->last_cursor_vpos = 0;

  /* Get special geometry settings from reference window.  */
  n->left_margin_cols = r->left_margin_cols;
  n->right_margin_cols = r->right_margin_cols;
  n->left_fringe_width = r->left_fringe_width;
  n->right_fringe_width = r->right_fringe_width;
  n->fringes_outside_margins = r->fringes_outside_margins;
  n->scroll_bar_width = r->scroll_bar_width;
  n->scroll_bar_height = r->scroll_bar_height;
  wset_vertical_scroll_bar_type (n, r->vertical_scroll_bar_type);
  wset_horizontal_scroll_bar_type (n, r->horizontal_scroll_bar_type);

  /* Directly assign orthogonal coordinates and sizes.  */
  if (horflag)
    {
      n->pixel_top = o->pixel_top;
      n->top_line = o->top_line;
      n->pixel_height = o->pixel_height;
      n->total_lines = o->total_lines;
    }
  else
    {
      n->pixel_left = o->pixel_left;
      n->left_col = o->left_col;
      n->pixel_width = o->pixel_width;
      n->total_cols = o->total_cols;
    }

  /* Iso-coordinates and sizes are assigned by window_resize_apply,
     get them ready here.  */
  wset_new_pixel (n, pixel_size);
  EMACS_INT sum = 0;
  c = XWINDOW (p->contents);
  while (c)
    {
      if (c != n)
	sum = sum + XFIXNUM (c->new_total);
      c = NILP (c->next) ? 0 : XWINDOW (c->next);
    }
  wset_new_total (n, make_fixnum ((horflag
				   ? p->total_cols
				   : p->total_lines)
				  - sum));
  wset_new_normal (n, normal_size);

  block_input ();
  window_resize_apply (p, horflag);
  adjust_frame_glyphs (f);
  /* Set buffer of NEW to buffer of reference window.  */
  set_window_buffer (new, r->contents, true, true);
  FRAME_WINDOW_CHANGE (f) = true;
  unblock_input ();

  return new;
}


DEFUN ("delete-window-internal", Fdelete_window_internal, Sdelete_window_internal, 1, 1, 0,
       doc: /* Remove WINDOW from its frame.
WINDOW defaults to the selected window.  Return nil.
Signal an error when WINDOW is the only window on its frame.  */)
     (Lisp_Object window)
{
  Lisp_Object parent, sibling, frame, root;
  struct window *w, *p, *s, *r;
  struct frame *f;
  bool horflag, before_sibling = false;

  w = decode_any_window (window);
  XSETWINDOW (window, w);
  if (NILP (w->contents))
    /* It's a no-op to delete an already deleted window.  */
    return Qnil;

  parent = w->parent;
  if (NILP (parent))
    /* Never delete a minibuffer or frame root window.  */
    error ("Attempt to delete minibuffer or sole ordinary window");
  else if (NILP (w->prev) && NILP (w->next))
    /* Rather bow out here, this case should be handled on the Elisp
       level.  */
    error ("Attempt to delete sole window of parent");

  p = XWINDOW (parent);
  horflag = WINDOW_HORIZONTAL_COMBINATION_P (p);

  frame = WINDOW_FRAME (w);
  f = XFRAME (frame);

  root = FRAME_ROOT_WINDOW (f);
  r = XWINDOW (root);

  /* Unlink WINDOW from window tree.  */
  if (NILP (w->prev))
    /* Get SIBLING below (on the right of) WINDOW.  */
    {
      /* before_sibling means WINDOW is the first child of its
	 parent and thus before the sibling.  */
      before_sibling = true;
      sibling = w->next;
      s = XWINDOW (sibling);
      wset_prev (s, Qnil);
      wset_combination (p, horflag, sibling);
    }
  else
    /* Get SIBLING above (on the left of) WINDOW.  */
    {
      sibling = w->prev;
      s = XWINDOW (sibling);
      wset_next (s, w->next);
      if (!NILP (s->next))
	wset_prev (XWINDOW (s->next), sibling);
    }

  if (window_resize_check (r, horflag)
      && (XFIXNUM (r->new_pixel)
	  == (horflag ? r->pixel_width : r->pixel_height)))
    /* We can delete WINDOW now.  */
    {

      /* Block input.  */
      block_input ();
      xwidget_view_delete_all_in_window (w);
      window_resize_apply (p, horflag);
      /* If this window is referred to by the dpyinfo's mouse
	 highlight, invalidate that slot to be safe (Bug#9904).  */
      if (!FRAME_INITIAL_P (f))
	{
	  Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (f);

	  if (EQ (hlinfo->mouse_face_window, window))
	    hlinfo->mouse_face_window = Qnil;
	}

      fset_redisplay (f);
      Vwindow_list = Qnil;

      wset_next (w, Qnil);  /* Don't delete w->next too.  */
      free_window_matrices (w);

      if (WINDOWP (w->contents))
	{
	  delete_all_child_windows (w->contents);
	  wset_combination (w, false, Qnil);
	}
      else
	{
	  unshow_buffer (w);
	  unchain_marker (XMARKER (w->pointm));
	  unchain_marker (XMARKER (w->old_pointm));
	  unchain_marker (XMARKER (w->start));
	  wset_buffer (w, Qnil);
	  /* Add WINDOW to table of dead windows so when killing a buffer
	     WINDOW mentions, all references to that buffer can be removed
	     and the buffer be collected.  */
	  Fputhash (make_fixnum (w->sequence_number),
		    window, window_dead_windows_table);
	}

      if (NILP (s->prev) && NILP (s->next))
	  /* A matrjoshka where SIBLING has become the only child of
	     PARENT.  */
	{
	  /* Put SIBLING into PARENT's place.  */
	  replace_window (parent, sibling, false);
	  /* Have SIBLING inherit the following three slot values from
	     PARENT (the combination_limit slot is not inherited).  */
	  wset_normal_cols (s, p->normal_cols);
	  wset_normal_lines (s, p->normal_lines);
	  /* Mark PARENT as deleted.  */
	  wset_combination (p, false, Qnil);
	  /* Try to merge SIBLING into its new parent.  */
	  recombine_windows (sibling);
	}

      adjust_frame_glyphs (f);

      if (!WINDOW_LIVE_P (FRAME_SELECTED_WINDOW (f)))
	/* We apparently deleted the frame's selected window; use the
	   frame's first window as substitute but don't record it yet.
	   `delete-window' may have something better up its sleeves.  */
	{
	  /* Use the frame's first window as fallback ...  */
	  Lisp_Object new_selected_window = Fframe_first_window (frame);

	  if (EQ (FRAME_SELECTED_WINDOW (f), selected_window))
	    Fselect_window (new_selected_window, Qt);
	  else
	    /* Do not clear f->select_mini_window_flag here.  If the
	       last selected window on F was an active minibuffer, we
	       want to return to it on a later Fselect_frame.  */
	    fset_selected_window (f, new_selected_window);
	}

      unblock_input ();
      FRAME_WINDOW_CHANGE (f) = true;
    }
  else
    /* We failed: Relink WINDOW into window tree.  */
    {
      if (before_sibling)
	{
	  wset_prev (s, window);
	  wset_combination (p, horflag, window);
	}
      else
	{
	  wset_next (s, window);
	  if (!NILP (w->next))
	    wset_prev (XWINDOW (w->next), window);
	}
      error ("Deletion failed");
    }

  return Qnil;
}

/***********************************************************************
			Resizing Mini-Windows
 ***********************************************************************/

/**
 * resize_mini_window_apply:
 *
 * Assign new window sizes after resizing a mini window W by DELTA
 * pixels.  No error checking performed.
  */
static void
resize_mini_window_apply (struct window *w, int delta)
{
  struct frame *f = XFRAME (w->frame);
  Lisp_Object root = FRAME_ROOT_WINDOW (f);
  struct window *r = XWINDOW (root);

  block_input ();
  w->pixel_height = w->pixel_height + delta;
  w->total_lines = w->pixel_height / FRAME_LINE_HEIGHT (f);

  window_resize_apply (r, false);

  w->pixel_top = r->pixel_top + r->pixel_height;
  w->top_line = r->top_line + r->total_lines;

  /* Enforce full redisplay of the frame.  If f->redisplay is already
     set, which it generally is in the wake of a ConfigureNotify
     (frame resize) event, merely setting f->redisplay is insufficient
     for redisplay_internal to continue redisplaying the frame, as
     redisplay_internal cannot distinguish between f->redisplay set
     before it calls redisplay_window and that after, so garbage the
     frame as well.  */

  if (f->redisplay)
    SET_FRAME_GARBAGED (f);

  /* FIXME: Shouldn't some of the caller do it?  */
  fset_redisplay (f);
  adjust_frame_glyphs (f);
  unblock_input ();
}

/**
 * grow_mini_window:
 *
 * Grow mini-window W by DELTA pixels.  If DELTA is negative, this may
 * shrink the minibuffer window to the minimum height to display one
 * line of text.
 */
void
grow_mini_window (struct window *w, int delta)
{
  struct frame *f = XFRAME (w->frame);
  int old_height = window_body_height (w, WINDOW_BODY_IN_PIXELS);
  int min_height = FRAME_LINE_HEIGHT (f);

  eassert (MINI_WINDOW_P (w));

  /* Never shrink mini-window to less than its minimum height.  */
  if (old_height + delta < min_height)
    delta = old_height > min_height ? min_height - old_height : 0;

  if (delta != 0)
    {
      Lisp_Object root = FRAME_ROOT_WINDOW (f);
      struct window *r = XWINDOW (root);
      Lisp_Object grow;

      grow = call3 (Qwindow__resize_root_window_vertically,
		    root, make_fixnum (- delta), Qt);

      if (FIXNUMP (grow)
	  /* It might be impossible to resize the window, in which case
	     calling resize_mini_window_apply will set off an infinite
	     loop where the redisplay cycle so forced returns to
	     resize_mini_window, making endless attempts to expand the
	     minibuffer window to this impossible size.  (bug#69140) */
	  && XFIXNUM (grow) != 0
	  && window_resize_check (r, false))
	resize_mini_window_apply (w, -XFIXNUM (grow));
    }
  FRAME_WINDOWS_FROZEN (f)
    = window_body_height (w, WINDOW_BODY_IN_PIXELS) > FRAME_LINE_HEIGHT (f);
}

/**
 * shrink_mini_window:
 *
 * Shrink mini-window W to the minimum height needed to display one
 * line of text.
 */
void
shrink_mini_window (struct window *w)
{
  struct frame *f = XFRAME (w->frame);
  int delta = (window_body_height (w, WINDOW_BODY_IN_PIXELS)
	       - FRAME_LINE_HEIGHT (f));

  eassert (MINI_WINDOW_P (w));

  if (delta > 0)
    {
      Lisp_Object root = FRAME_ROOT_WINDOW (f);
      struct window *r = XWINDOW (root);
      Lisp_Object grow;

      grow = call3 (Qwindow__resize_root_window_vertically,
		    root, make_fixnum (delta), Qt);

      if (FIXNUMP (grow) && window_resize_check (r, false))
	resize_mini_window_apply (w, -XFIXNUM (grow));
    }
  else if (delta < 0)
    /* delta can be less than zero after adding horizontal scroll
       bar.  */
    grow_mini_window (w, -delta);

  FRAME_WINDOWS_FROZEN (f)
    = window_body_height (w, WINDOW_BODY_IN_PIXELS) > FRAME_LINE_HEIGHT (f);
}

DEFUN ("resize-mini-window-internal", Fresize_mini_window_internal,
       Sresize_mini_window_internal, 1, 1, 0,
       doc: /* Resize mini window WINDOW.  */)
     (Lisp_Object window)
{
  struct window *w = XWINDOW (window);
  struct window *r;
  struct frame *f;
  int old_height, delta;

  CHECK_LIVE_WINDOW (window);
  f = XFRAME (w->frame);

  if (!EQ (FRAME_MINIBUF_WINDOW (XFRAME (w->frame)), window))
    error ("Not a valid minibuffer window");
  else if (FRAME_MINIBUF_ONLY_P (f))
    error ("Cannot resize a minibuffer-only frame");

  r = XWINDOW (FRAME_ROOT_WINDOW (f));
  old_height = r->pixel_height + w->pixel_height;
  delta = XFIXNUM (w->new_pixel) - w->pixel_height;
  if (window_resize_check (r, false)
      && XFIXNUM (w->new_pixel) > 0
      && old_height == XFIXNUM (r->new_pixel) + XFIXNUM (w->new_pixel))
    {
      resize_mini_window_apply (w, delta);

      return Qt;
    }
  else
    error ("Cannot resize mini window");
}

/* Mark window cursors off for all windows in the window tree rooted
   at W by setting their phys_cursor_on_p flag to zero.  Called from
   xterm.c, e.g. when a frame is cleared and thereby all cursors on
   the frame are cleared.  */

void
mark_window_cursors_off (struct window *w)
{
  while (w)
    {
      if (WINDOWP (w->contents))
	mark_window_cursors_off (XWINDOW (w->contents));
      else
	w->phys_cursor_on_p = false;

      w = NILP (w->next) ? 0 : XWINDOW (w->next);
    }
}


/**
 * window_wants_mode_line:
 *
 * Return 1 if window W wants a mode line and is high enough to
 * accommodate it, 0 otherwise.
 *
 * W wants a mode line if it's a leaf window and neither a minibuffer
 * nor a pseudo window.  Moreover, its 'window-mode-line-format'
 * parameter must not be 'none' and either that parameter or W's
 * buffer's 'mode-line-format' value must be non-nil.  Finally, W must
 * be higher than its frame's canonical character height.
 */
bool
window_wants_mode_line (struct window *w)
{
  Lisp_Object window_mode_line_format =
    window_parameter (w, Qmode_line_format);

  return (WINDOW_LEAF_P (w)
	  && !MINI_WINDOW_P (w)
	  && !WINDOW_PSEUDO_P (w)
	  && !EQ (window_mode_line_format, Qnone)
	  && (!NILP (window_mode_line_format)
	      || !NILP (BVAR (XBUFFER (w->contents), mode_line_format)))
	  && WINDOW_PIXEL_HEIGHT (w) > WINDOW_FRAME_LINE_HEIGHT (w));
}

/*
 * Dispense with header line if FMT is '(:eval nil) or
 * is otherwise degenerate.
 */

static bool
null_header_line_format (Lisp_Object fmt)
{
  Lisp_Object val = fmt;
  if (CONSP (fmt))
    {
      Lisp_Object car = XCAR (fmt);
      if (EQ (car, QCeval))
	{
	  specpdl_ref count = SPECPDL_INDEX ();
	  specbind (Qinhibit_quit, Qt);
	  val = safe_eval (XCAR (XCDR (fmt)));
	  unbind_to (count, Qnil);
	}
      else if (SYMBOLP (car))
	val = find_symbol_value (XSYMBOL (car), NULL);
    }
  return EQ (val, Qunbound) || NILP (val);
}

/**
 * window_wants_header_line:
 *
 * Return 1 if window W wants a header line and is high enough to
 * accommodate it, 0 otherwise.
 *
 * W wants a header line if it's a leaf window and neither a
 * minibuffer nor a pseudo window.  Moreover, its
 * 'window-header-line-format' parameter must not be 'none' and either
 * that parameter or W's buffer's 'header-line-format' value must be
 * non-nil.  Finally, W must be higher than its frame's canonical
 * character height and be able to accommodate a mode line too if
 * necessary (the mode line prevails).
 */
bool
window_wants_header_line (struct window *w)
{
  Lisp_Object window_format = window_parameter (w, Qheader_line_format);
  return (WINDOW_LEAF_P (w)
	  && !MINI_WINDOW_P (w)
	  && !WINDOW_PSEUDO_P (w)
	  && !EQ (window_format, Qnone)
	  && (!null_header_line_format (window_format)
	      || !null_header_line_format (BVAR (XBUFFER (w->contents), header_line_format)))
	  && (WINDOW_PIXEL_HEIGHT (w)
	      > (window_wants_mode_line (w)
		 ? 2 * WINDOW_FRAME_LINE_HEIGHT (w)
		 : WINDOW_FRAME_LINE_HEIGHT (w))));
}


/**
 * window_wants_tab_line:
 *
 * Return 1 if window W wants a tab line and is high enough to
 * accommodate it, 0 otherwise.
 *
 * W wants a tab line if it's a leaf window and neither a minibuffer
 * nor a pseudo window.  Moreover, its 'window-tab-line-format'
 * parameter must not be 'none' and either that parameter or W's
 * buffer's 'tab-line-format' value must be non-nil.  Finally, W must
 * be higher than its frame's canonical character height and be able
 * to accommodate a mode line and a header line too if necessary (the
 * mode line and a header line prevail).
 */

bool
window_wants_tab_line (struct window *w)
{
  Lisp_Object window_tab_line_format =
    window_parameter (w, Qtab_line_format);

  return (WINDOW_LEAF_P (w)
	  && !MINI_WINDOW_P (w)
	  && !WINDOW_PSEUDO_P (w)
	  && !EQ (window_tab_line_format, Qnone)
	  && (!NILP (window_tab_line_format)
	      || !NILP (BVAR (XBUFFER (w->contents), tab_line_format)))
	  && (WINDOW_PIXEL_HEIGHT (w)
	      > (((window_wants_mode_line (w) ? 1 : 0)
		  + (window_wants_header_line (w) ? 1 : 0)
		  + 1) * WINDOW_FRAME_LINE_HEIGHT (w))));
}

/* Return number of lines of text in window W, not counting the mode
   line and header line, if any.  Do NOT use this for windows on GUI
   frames; use window_body_height instead.  This function is only for
   windows on TTY frames, where it is much more efficient.  */

int
window_internal_height (struct window *w)
{
  int ht = w->total_lines;

  if (window_wants_mode_line (w))
    --ht;

  if (window_wants_header_line (w))
    --ht;

  if (window_wants_tab_line (w))
    --ht;

  return ht;
}


/************************************************************************
			   Window Scrolling
 ***********************************************************************/

/* Scroll contents of window WINDOW up.  If WHOLE, scroll
   N screen-fulls, which is defined as the height of the window minus
   next_screen_context_lines.  If WHOLE is zero, scroll up N lines
   instead.  Negative values of N mean scroll down.  NOERROR
   means don't signal an error if we try to move over BEGV or ZV,
   respectively.  */

static void
window_scroll (Lisp_Object window, EMACS_INT n, bool whole, bool noerror)
{
  specpdl_ref count = SPECPDL_INDEX ();

  n = clip_to_bounds (INT_MIN, n, INT_MAX);

  wset_redisplay (XWINDOW (window));

  if (whole && fast_but_imprecise_scrolling)
    specbind (Qfontification_functions, Qnil);

  /* On GUI frames, use the pixel-based version which is much slower
     than the line-based one but can handle varying line heights.  */
  if (FRAME_WINDOW_P (XFRAME (XWINDOW (window)->frame)))
    window_scroll_pixel_based (window, n, whole, noerror);
  else
    window_scroll_line_based (window, n, whole, noerror);

  unbind_to (count, Qnil);

  /* Bug#15957.  */
  XWINDOW (window)->window_end_valid = false;
}

/* Compute scroll margin for WINDOW.
   We scroll when point is within this distance from the top or bottom
   of the window.  The result is measured in lines or in pixels
   depending on the second parameter.  */
int
window_scroll_margin (struct window *window, enum margin_unit unit)
{
  if (scroll_margin > 0)
    {
      int frame_line_height = default_line_height (window);
      int window_lines = window_box_height (window) / frame_line_height;

      double ratio = 0.25;
      if (FLOATP (Vmaximum_scroll_margin))
        {
          ratio = XFLOAT_DATA (Vmaximum_scroll_margin);
          ratio = max (0.0, ratio);
          ratio = min (ratio, 0.5);
        }
      int max_margin = min ((window_lines - 1)/2,
                            (int) (window_lines * ratio));
      int margin = clip_to_bounds (0, scroll_margin, max_margin);
      return (unit == MARGIN_IN_PIXELS)
        ? margin * frame_line_height
        : margin;
    }
  else
    return 0;
}

static int
sanitize_next_screen_context_lines (void)
{
  return clip_to_bounds (0, next_screen_context_lines, 1000000);
}

/* Implementation of window_scroll that works based on pixel line
   heights.  See the comment of window_scroll for parameter
   descriptions.  */

static void
window_scroll_pixel_based (Lisp_Object window, int n, bool whole, bool noerror)
{
  struct it it;
  struct window *w = XWINDOW (window);
  struct text_pos start;
  int this_scroll_margin;
  /* True if we fiddled the window vscroll field without really scrolling.  */
  bool vscrolled = false;
  int x, y, rtop, rbot, rowh, vpos;
  void *itdata = NULL;
  int frame_line_height = default_line_height (w);
  bool adjust_old_pointm = !NILP (Fequal (Fwindow_point (window),
					  Fwindow_old_point (window)));

  SET_TEXT_POS_FROM_MARKER (start, w->start);
  /* Scrolling a minibuffer window via scroll bar when the echo area
     shows long text sometimes resets the minibuffer contents behind
     our backs.  Also, someone might narrow-to-region and immediately
     call a scroll function.  */
  if (CHARPOS (start) > ZV || CHARPOS (start) < BEGV)
    SET_TEXT_POS (start, BEGV, BEGV_BYTE);

  /* If PT is not visible in WINDOW, move back one half of
     the screen.  Allow PT to be partially visible, otherwise
     something like (scroll-down 1) with PT in the line before
     the partially visible one would recenter.  */

  if (!window_start_coordinates (w, PT, &x, &y, &rtop, &rbot, &rowh, &vpos))
    {
      itdata = bidi_shelve_cache ();
      /* Move backward half the height of the window.  Performance note:
	 vmotion used here is about 10% faster, but would give wrong
	 results for variable height lines.  */
      init_iterator (&it, w, PT, PT_BYTE, NULL, DEFAULT_FACE_ID);
      it.current_y = it.last_visible_y;
      move_it_dy (&it, window_box_height (w) / -2);

      /* The function move_iterator_vertically may move over more than
	 the specified y-distance.  If it->w is small, e.g. a
	 mini-buffer window, we may end up in front of the window's
	 display area.  Start displaying at the start of the line
	 containing PT in this case.  */
      if (it.current_y <= 0)
	{
	  init_iterator (&it, w, PT, PT_BYTE, NULL, DEFAULT_FACE_ID);
	  move_it_dy (&it, 0);
	  it.current_y = 0;
	}

      start = it.current.pos;
      bidi_unshelve_cache (itdata, false);
    }
  else if (auto_window_vscroll_p)
    {
      if (rtop || rbot)		/* Partially visible.  */
	{
	  int px;
	  int dy = frame_line_height;
	  /* In the below we divide the window box height by the
	     frame's line height to make the result predictable when
	     the window box is not an integral multiple of the line
	     height.  This is important to ensure we get back to the
	     same position when scrolling up, then down.  */
	  if (whole)
	    {
	      int ht = window_box_height (w);
	      int nscls = sanitize_next_screen_context_lines ();
	      dy = max (dy, (ht / dy - nscls) * dy);
	    }
	  dy *= n;

	  if (n < 0)
	    {
	      /* Only vscroll backwards if already vscrolled forwards.  */
	      if (w->vscroll < 0 && rtop > 0)
		{
		  px = max (0, -w->vscroll - min (rtop, -dy));
		  Fset_window_vscroll (window, make_fixnum (px), Qt,
				       Qnil);
		  return;
		}
	    }
	  if (n > 0)
	    {
	      /* Do vscroll if already vscrolled or only display line.  */
	      if (rbot > 0 && (w->vscroll < 0 || vpos == 0))
		{
		  px = max (0, -w->vscroll + min (rbot, dy));
		  Fset_window_vscroll (window, make_fixnum (px), Qt,
				       Qnil);
		  return;
		}

	      /* Maybe modify window start instead of scrolling.  */
	      if (rbot > 0 || w->vscroll < 0)
		{
		  ptrdiff_t spos;

		  Fset_window_vscroll (window, make_fixnum (0), Qt,
				       Qnil);
		  /* If there are other text lines above the current row,
		     move window start to current row.  Else to next row. */
		  if (rbot > 0)
		    spos = XFIXNUM (Fline_beginning_position (Qnil));
		  else
		    spos = min (XFIXNUM (Fline_end_position (Qnil)) + 1, ZV);
		  set_marker_restricted (w->start, make_fixnum (spos),
					 w->contents);
		  w->start_at_line_beg = true;
		  wset_update_mode_line (w);
		  /* Set force_start so that redisplay_window will run the
		     window-scroll-functions.  */
		  w->force_start = true;
		  return;
		}
	    }
	}
      /* Cancel previous vscroll.  */
      Fset_window_vscroll (window, make_fixnum (0), Qt, Qnil);
    }

  itdata = bidi_shelve_cache ();
  /* If scroll_preserve_screen_position is non-nil, we try to set
     point in the same window line as it is now, so get that line.  */
  if (!NILP (Vscroll_preserve_screen_position))
    {
      /* We preserve the goal pixel coordinate across consecutive
	 calls to scroll-up, scroll-down and other commands that
	 have the `scroll-command' property.  This avoids the
	 possibility of point becoming "stuck" on a tall line when
	 scrolling by one line.  */
      if (window_scroll_pixel_based_preserve_y < 0
	  || !SYMBOLP (KVAR (current_kboard, Vlast_command))
	  || NILP (Fget (KVAR (current_kboard, Vlast_command), Qscroll_command)))
	{
	  start_move_it (&it, w, start);
	  move_it_forward (&it, PT, -1, MOVE_TO_POS, NULL);
	  window_scroll_pixel_based_preserve_y = it.current_y;
	  window_scroll_pixel_based_preserve_x = it.current_x;
	}
    }
  else
    window_scroll_pixel_based_preserve_y
      = window_scroll_pixel_based_preserve_x = -1;

  /* Move iterator it from start the specified distance forward or
     backward.  The result is the new window start.  */
  start_move_it (&it, w, start);
  if (whole)
    {
      ptrdiff_t start_pos = IT_CHARPOS (it);
      int flh = frame_line_height;
      int ht = window_box_height (w);
      int nscls = sanitize_next_screen_context_lines ();
      /* In the below we divide the window box height by the frame's
	 line height to make the result predictable when the window
	 box is not an integral multiple of the line height.  This is
	 important to ensure we get back to the same position when
	 scrolling up, then down.  */
      int dy = n * max (flh, (ht / flh - nscls) * flh);
      int goal_y;
      void *it_data;

      /* Note that move_it_dy always moves the iterator to the
         start of a line.  So, if the last line doesn't have a newline,
	 we would end up at the start of the line ending at ZV.  */
      if (dy <= 0)
	{
	  goal_y = it.current_y + dy;
	  move_it_dy (&it, dy);
	  /* move_it_dy (backward) above always overshoots if DY
	     cannot be reached exactly, i.e. if it falls in the middle
	     of a screen line.  But if that screen line is large
	     (e.g., a tall image), it might make more sense to
	     undershoot instead.  */
	  if (goal_y - it.current_y > 0.5 * flh)
	    {
	      it_data = bidi_shelve_cache ();
	      struct it it1 = it;
	      if (window_line_bottom_y (it1) - goal_y < goal_y - it.current_y)
		move_it_dvpos (&it, 1);
	      bidi_unshelve_cache (it_data, true);
	    }
	  /* Ensure we actually do move, e.g. in case we are currently
	     looking at an image that is taller that the window height.  */
	  while (start_pos == IT_CHARPOS (it)
		 && start_pos > BEGV)
	    move_it_dvpos (&it, -1);
	}
      else if (dy > 0)
	{
	  goal_y = it.current_y + dy;
	  move_it_forward (&it, ZV, goal_y, MOVE_TO_POS | MOVE_TO_Y, NULL);
	  /* Extra precision for people who want us to preserve the
	     screen position of the cursor: effectively round DY to the
	     nearest screen line, instead of rounding to zero; the latter
	     causes point to move by one line after C-v followed by M-v,
	     if the buffer has lines of different height.  */
	  if (!NILP (Vscroll_preserve_screen_position)
	      && goal_y - it.current_y  > 0.5 * flh)
	    {
	      it_data = bidi_shelve_cache ();
	      struct it it2 = it;

	      move_it_dvpos (&it, 1);
	      if (it.current_y > goal_y + 0.5 * flh)
		{
		  it = it2;
		  bidi_unshelve_cache (it_data, false);
		}
	      else
		bidi_unshelve_cache (it_data, true);
	    }
	  /* Ensure we actually do move, e.g. in case we are currently
	     looking at an image that is taller that the window height.  */
	  while (start_pos == IT_CHARPOS (it)
		 && start_pos < ZV)
	    move_it_dvpos (&it, 1);
	}
    }
  else
    move_it_dvpos (&it, n);

  /* We failed if we find ZV is already on the screen (scrolling up,
     means there's nothing past the end), or if we can't start any
     earlier (scrolling down, means there's nothing past the top).  */
  if ((n > 0 && IT_CHARPOS (it) == ZV)
      || (n < 0 && IT_CHARPOS (it) == CHARPOS (start)))
    {
      if (IT_CHARPOS (it) == ZV)
	{
	  if (it.current_y < it.last_visible_y
	      && (it.current_y + it.max_ascent + it.max_descent
		  > it.last_visible_y))
	    {
	      /* The last line was only partially visible, make it fully
		 visible.  */
	      w->vscroll = (it.last_visible_y
			    - it.current_y + it.max_ascent + it.max_descent);
	      adjust_frame_glyphs (it.f);
	    }
	  else
	    {
	      bidi_unshelve_cache (itdata, false);
	      if (noerror)
		return;
	      else if (n < 0)	/* could happen with empty buffers */
		xsignal0 (Qbeginning_of_buffer);
	      else
		xsignal0 (Qend_of_buffer);
	    }
	}
      else
	{
	  if (w->vscroll != 0)
	    /* The first line was only partially visible, make it fully
	       visible. */
	    w->vscroll = 0;
	  else
	    {
	      bidi_unshelve_cache (itdata, false);
	      if (noerror)
		return;
	      else
		xsignal0 (Qbeginning_of_buffer);
	    }
	}

      /* If control gets here, then we vscrolled.  */

      XBUFFER (w->contents)->prevent_redisplay_optimizations_p = true;

      /* Don't try to change the window start below.  */
      vscrolled = true;
    }

  if (!vscrolled)
    {
      ptrdiff_t pos = IT_CHARPOS (it);
      ptrdiff_t bytepos;

      /* If in the middle of a multi-glyph character move forward to
	 the next character.  */
      if (in_display_vector_p (&it))
	{
	  ++pos;
	  move_it_forward (&it, pos, -1, MOVE_TO_POS, NULL);
	}

      /* Set the window start, and set up the window for redisplay.  */
      set_marker_restricted_both (w->start, w->contents, IT_CHARPOS (it),
				  IT_BYTEPOS (it));
      bytepos = marker_byte_position (w->start);
      w->start_at_line_beg = (pos == BEGV || FETCH_BYTE (bytepos - 1) == '\n');
      wset_update_mode_line (w);
      /* Set force_start so that redisplay_window will run the
	 window-scroll-functions.  */
      w->force_start = true;
    }

  /* The rest of this function uses current_y in a nonstandard way,
     not including the height of the header line if any.  */
  it.current_y = it.vpos = 0;

  /* Move PT out of scroll margins.
     This code wants current_y to be zero at the window start position
     even if there is a header line.  */
  this_scroll_margin = window_scroll_margin (w, MARGIN_IN_PIXELS);

  if (n > 0)
    {
      int last_y = it.last_visible_y - this_scroll_margin - 1;

      /* We moved the window start towards ZV, so PT may be now
	 in the scroll margin at the top.  */
      move_it_forward (&it, PT, -1, MOVE_TO_POS, NULL);
      if (IT_CHARPOS (it) == PT
	  && it.current_y >= this_scroll_margin
	  && it.current_y <= last_y - WINDOW_TAB_LINE_HEIGHT (w)
				    - WINDOW_HEADER_LINE_HEIGHT (w)
	  && (NILP (Vscroll_preserve_screen_position)
	      || EQ (Vscroll_preserve_screen_position, Qt)))
	/* We found PT at a legitimate height.  Leave it alone.  */
	;
      else
	{
	  if (window_scroll_pixel_based_preserve_y >= 0)
	    {
	      /* Don't enter the scroll margin at the end of the window.  */
	      int goal_y = min (last_y, window_scroll_pixel_based_preserve_y);

	      /* If we have a header line, take account of it.  This
		 is necessary because we set it.current_y to 0, above.  */
	      move_it_forward (&it, -1,
			       (goal_y - WINDOW_TAB_LINE_HEIGHT (w)
				- WINDOW_HEADER_LINE_HEIGHT (w)),
			       MOVE_TO_Y,
			       NULL);
	    }

	  /* Get out of the scroll margin at the top of the window.  */
	  while (it.current_y < this_scroll_margin)
	    {
	      int prev = it.current_y;
	      move_it_dvpos (&it, 1);
	      if (prev == it.current_y)
		break;
	    }
	  SET_PT_BOTH (IT_CHARPOS (it), IT_BYTEPOS (it));
	  /* Fix up the Y position to preserve, if it is inside the
	     scroll margin at the window top.  */
	  if (window_scroll_pixel_based_preserve_y >= 0
	      && window_scroll_pixel_based_preserve_y < this_scroll_margin)
	    window_scroll_pixel_based_preserve_y = this_scroll_margin;
	}
    }
  else if (n < 0)
    {
      ptrdiff_t charpos, bytepos;
      bool partial_p;

      /* We moved the window start towards BEGV, so PT may be now
	 in the scroll margin at the bottom.  */
      move_it_forward (&it, PT,
		       /* We subtract WINDOW_HEADER_LINE_HEIGHT because
			  it.y is relative to the bottom of the header
			  line, see above.  */
		       (it.last_visible_y - WINDOW_TAB_LINE_HEIGHT (w)
			- WINDOW_HEADER_LINE_HEIGHT (w)
			- partial_line_height (&it) - this_scroll_margin - 1),
		       MOVE_TO_POS | MOVE_TO_Y,
		       NULL);

      /* Save our position, in case it's correct.  */
      charpos = IT_CHARPOS (it);
      bytepos = IT_BYTEPOS (it);

      /* If PT is in the screen line at the last fully visible line,
	 move_it_forward will stop at X = 0 in that line, because the
	 required Y coordinate is reached there.  See if we can get to
	 PT without descending lower in Y, and if we can, it means we
	 reached PT before the scroll margin.  */
      if (charpos != PT)
	{
	  struct it it2;
	  void *it_data;

	  it2 = it;
	  it_data = bidi_shelve_cache ();
	  move_it_forward (&it, PT, -1, MOVE_TO_POS, NULL);
	  if (IT_CHARPOS (it) == PT && it.current_y == it2.current_y)
	    {
	      charpos = IT_CHARPOS (it);
	      bytepos = IT_BYTEPOS (it);
	      bidi_unshelve_cache (it_data, true);
	    }
	  else
	    {
	      it = it2;
	      bidi_unshelve_cache (it_data, false);
	    }
	}

      /* See if point is on a partially visible line at the end.  */
      if (it.what == IT_EOB)
	partial_p =
	  it.current_y + it.ascent + it.descent
	  > it.last_visible_y - this_scroll_margin
	  - WINDOW_TAB_LINE_HEIGHT (w) - WINDOW_HEADER_LINE_HEIGHT (w);
      else
	{
	  move_it_dvpos (&it, 1);
	  partial_p =
	    it.current_y
	    > it.last_visible_y - this_scroll_margin
	      - WINDOW_TAB_LINE_HEIGHT (w) - WINDOW_HEADER_LINE_HEIGHT (w);
	}

      if (charpos == PT && !partial_p
          && (NILP (Vscroll_preserve_screen_position)
	      || EQ (Vscroll_preserve_screen_position, Qt)))
	/* We found PT before we found the display margin, so PT is ok.  */
	;
      else if (window_scroll_pixel_based_preserve_y >= 0)
	{
	  int goal_y = min (it.last_visible_y - this_scroll_margin - 1,
			    window_scroll_pixel_based_preserve_y);

	  /* Don't let the preserved screen Y coordinate put us inside
	     any of the two margins.  */
	  if (goal_y < this_scroll_margin)
	    goal_y = this_scroll_margin;
	  SET_TEXT_POS_FROM_MARKER (start, w->start);
	  start_move_it (&it, w, start);
	  /* It would be wrong to subtract WINDOW_HEADER_LINE_HEIGHT
	     here because we called start_move_it again and did not
	     alter it.current_y this time.  */
	  move_it_forward (&it, -1, goal_y, MOVE_TO_Y, NULL);
	  SET_PT_BOTH (IT_CHARPOS (it), IT_BYTEPOS (it));
	}
      else
	{
	  if (partial_p)
	    /* The last line was only partially visible, so back up two
	       lines to make sure we're on a fully visible line.  */
	    {
	      move_it_dvpos (&it, -2);
	      SET_PT_BOTH (IT_CHARPOS (it), IT_BYTEPOS (it));
	    }
	  else
	    /* No, the position we saved is OK, so use it.  */
	    SET_PT_BOTH (charpos, bytepos);
	}
    }
  bidi_unshelve_cache (itdata, false);

  if (adjust_old_pointm)
    Fset_marker (w->old_pointm,
		 ((w == XWINDOW (selected_window))
		  ? make_fixnum (BUF_PT (XBUFFER (w->contents)))
		  : Fmarker_position (w->pointm)),
		 w->contents);
}


/* Implementation of window_scroll that works based on screen lines.
   See the comment of window_scroll for parameter descriptions.  */

static void
window_scroll_line_based (Lisp_Object window, int n, bool whole, bool noerror)
{
  struct window *w = XWINDOW (window);
  Lisp_Object opoint_marker = Fpoint_marker ();
  register ptrdiff_t pos, pos_byte;
  register int ht = window_internal_height (w);
  register Lisp_Object tem;
  bool lose;
  Lisp_Object bolp;
  ptrdiff_t startpos = marker_position (w->start);
  ptrdiff_t startbyte = marker_byte_position (w->start);
  Lisp_Object original_pos = Qnil;
  bool adjust_old_pointm = !NILP (Fequal (Fwindow_point (window),
					  Fwindow_old_point (window)));

  /* If scrolling screen-fulls, compute the number of lines to
     scroll from the window's height.  */
  if (whole)
    {
      int nscls = sanitize_next_screen_context_lines ();
      n *= max (1, ht - nscls);
    }

  if (!NILP (Vscroll_preserve_screen_position))
    {
      if (window_scroll_preserve_vpos <= 0
	  || !SYMBOLP (KVAR (current_kboard, Vlast_command))
	  || NILP (Fget (KVAR (current_kboard, Vlast_command), Qscroll_command)))
	{
	  struct position posit
	    = *compute_motion (startpos, startbyte, 0, 0, false,
			       PT, ht, 0, -1, w->hscroll, 0, w);

	  window_scroll_preserve_vpos = posit.vpos;
	  window_scroll_preserve_hpos = posit.hpos + w->hscroll;
	}

      original_pos = Fcons (make_fixnum (window_scroll_preserve_hpos),
			    make_fixnum (window_scroll_preserve_vpos));
    }

  XSETFASTINT (tem, PT);
  tem = Fpos_visible_in_window_p (tem, window, Qnil);

  if (NILP (tem))
    {
      Fvertical_motion (make_fixnum (- (ht / 2)), window, Qnil);
      startpos = PT;
      startbyte = PT_BYTE;
    }

  SET_PT_BOTH (startpos, startbyte);
  lose = n < 0 && PT == BEGV;
  Fvertical_motion (make_fixnum (n), window, Qnil);
  pos = PT;
  pos_byte = PT_BYTE;
  bolp = Fbolp ();
  SET_PT_BOTH (marker_position (opoint_marker),
	       marker_byte_position (opoint_marker));

  if (lose)
    {
      if (noerror)
	return;
      else
	xsignal0 (Qbeginning_of_buffer);
    }

  if (pos < ZV)
    {
      int this_scroll_margin = window_scroll_margin (w, MARGIN_IN_LINES);

      set_marker_restricted_both (w->start, w->contents, pos, pos_byte);
      w->start_at_line_beg = !NILP (bolp);
      wset_update_mode_line (w);
      /* Set force_start so that redisplay_window will run
	 the window-scroll-functions.  */
      w->force_start = true;

      if (!NILP (Vscroll_preserve_screen_position)
	  && this_scroll_margin == 0
	  && (whole || !EQ (Vscroll_preserve_screen_position, Qt)))
	{
	  SET_PT_BOTH (pos, pos_byte);
	  Fvertical_motion (original_pos, window, Qnil);
	}
      /* If we scrolled forward, put point enough lines down
	 that it is outside the scroll margin.  */
      else if (n > 0)
	{
	  int top_margin;

	  if (this_scroll_margin > 0)
	    {
	      SET_PT_BOTH (pos, pos_byte);
	      Fvertical_motion (make_fixnum (this_scroll_margin), window, Qnil);
	      top_margin = PT;
	    }
	  else
	    top_margin = pos;

	  if (top_margin <= marker_position (opoint_marker))
	    SET_PT_BOTH (marker_position (opoint_marker),
			 marker_byte_position (opoint_marker));
	  else if (!NILP (Vscroll_preserve_screen_position))
	    {
	      int nlines = window_scroll_preserve_vpos;

	      SET_PT_BOTH (pos, pos_byte);
	      if (window_scroll_preserve_vpos < this_scroll_margin)
		nlines = this_scroll_margin;
	      else if (window_scroll_preserve_vpos
		       >= w->total_lines - this_scroll_margin)
		nlines = w->total_lines - this_scroll_margin - 1;
	      Fvertical_motion (Fcons (make_fixnum (window_scroll_preserve_hpos),
				       make_fixnum (nlines)), window, Qnil);
	    }
	  else
	    SET_PT (top_margin);
	}
      else if (n < 0)
	{
	  int bottom_margin;

	  /* If we scrolled backward, put point near the end of the window
	     but not within the scroll margin.  */
	  SET_PT_BOTH (pos, pos_byte);
	  tem = Fvertical_motion (make_fixnum (ht - this_scroll_margin), window,
				  Qnil);
	  if (XFIXNAT (tem) == ht - this_scroll_margin)
	    bottom_margin = PT;
	  else
	    bottom_margin = PT + 1;

	  if (bottom_margin > marker_position (opoint_marker))
	    SET_PT_BOTH (marker_position (opoint_marker),
			 marker_byte_position (opoint_marker));
	  else
	    {
	      if (!NILP (Vscroll_preserve_screen_position))
		{
		  int nlines = window_scroll_preserve_vpos;

		  SET_PT_BOTH (pos, pos_byte);
		  if (window_scroll_preserve_vpos < this_scroll_margin)
		    nlines = this_scroll_margin;
		  else if (window_scroll_preserve_vpos
			   >= ht - this_scroll_margin)
		    nlines = ht - this_scroll_margin - 1;
		  Fvertical_motion (Fcons (make_fixnum (window_scroll_preserve_hpos),
					   make_fixnum (nlines)), window, Qnil);
		}
	      else
		Fvertical_motion (make_fixnum (-1), window, Qnil);
	    }
	}
    }
  else
    {
      if (noerror)
	return;
      else
	xsignal0 (Qend_of_buffer);
    }

  if (adjust_old_pointm)
    Fset_marker (w->old_pointm,
		 ((w == XWINDOW (selected_window))
		  ? make_fixnum (BUF_PT (XBUFFER (w->contents)))
		  : Fmarker_position (w->pointm)),
		 w->contents);
}


/* Scroll WINDOW up or down.  If N is nil, scroll upward by a
   screen-full which is defined as the height of the window minus
   next_screen_context_lines.  If N is the symbol `-', scroll downward
   by a screen-full.  DIRECTION may be 1 meaning to scroll down, or -1
   meaning to scroll up.  */

static void
scroll_command (Lisp_Object window, Lisp_Object n, int direction)
{
  struct window *w;
  bool other_window;
  specpdl_ref count = SPECPDL_INDEX ();

  eassert (eabs (direction) == 1);

  w = XWINDOW (window);
  other_window = !EQ (window, selected_window);

  /* If given window's buffer isn't current, make it current for the
     moment.  If the window's buffer is the same, but it is not the
     selected window, we need to save-excursion to avoid affecting
     point in the selected window (which would cause the selected
     window to scroll).  Don't screw up if window_scroll gets an
     error.  */
  if (other_window || XBUFFER (w->contents) != current_buffer)
    {
      record_unwind_protect_excursion ();
      if (XBUFFER (w->contents) != current_buffer)
	Fset_buffer (w->contents);
    }

  if (other_window)
    {
      SET_PT_BOTH (marker_position (w->pointm),
                   marker_byte_position (w->pointm));
      SET_PT_BOTH (marker_position (w->old_pointm),
                   marker_byte_position (w->old_pointm));
    }

  if (NILP (n))
    window_scroll (window, direction, true, false);
  else if (EQ (n, Qminus))
    window_scroll (window, -direction, true, false);
  else
    {
      n = Fprefix_numeric_value (n);
      window_scroll (window, XFIXNUM (n) * direction, false, false);
    }

  if (other_window)
    {
      set_marker_both (w->pointm, Qnil, PT, PT_BYTE);
      set_marker_both (w->old_pointm, Qnil, PT, PT_BYTE);
    }

  unbind_to (count, Qnil);
}

DEFUN ("scroll-up", Fscroll_up, Sscroll_up, 0, 1, "^P",
       doc: /* Scroll text of selected window upward ARG lines.
If ARG is omitted or nil, scroll upward by a near full screen.
A near full screen is `next-screen-context-lines' less than a full screen.
Negative ARG means scroll downward.
If ARG is the atom `-', scroll downward by nearly full screen.
When calling from a program, supply as argument a number, nil, or `-'.  */)
  (Lisp_Object arg)
{
  scroll_command (selected_window, arg, 1);
  return Qnil;
}

DEFUN ("scroll-down", Fscroll_down, Sscroll_down, 0, 1, "^P",
       doc: /* Scroll text of selected window down ARG lines.
If ARG is omitted or nil, scroll down by a near full screen.
A near full screen is `next-screen-context-lines' less than a full screen.
Negative ARG means scroll upward.
If ARG is the atom `-', scroll upward by nearly full screen.
When calling from a program, supply as argument a number, nil, or `-'.  */)
  (Lisp_Object arg)
{
  scroll_command (selected_window, arg, -1);
  return Qnil;
}

DEFUN ("other-window-for-scrolling", Fother_window_for_scrolling, Sother_window_for_scrolling, 0, 0, 0,
       doc: /* Return \"the other\" window for \"other window scroll\" commands.
If in the minibuffer, and `minibuffer-scroll-window' is non-nil,
it specifies the window to use.
Otherwise, if `other-window-scroll-buffer' is a buffer, a window
showing that buffer is the window to use, popping it up if necessary.
Otherwise, if `other-window-scroll-default' is a function, call it,
and the window it returns is the window to use.
Finally, the function looks for a neighboring window on the selected
frame, followed by windows on all the visible frames on the current
terminal.  */)
  (void)
{
  Lisp_Object window;

  if (MINI_WINDOW_P (XWINDOW (selected_window))
      && !NILP (Vminibuf_scroll_window))
    window = Vminibuf_scroll_window;
  /* If buffer is specified and live, scroll that buffer.  */
  else if (BUFFERP (Vother_window_scroll_buffer)
	   && BUFFER_LIVE_P (XBUFFER (Vother_window_scroll_buffer)))
    {
      window = Fget_buffer_window (Vother_window_scroll_buffer, Qnil);
      if (NILP (window))
	window = display_buffer (Vother_window_scroll_buffer, Qt, Qnil);
    }
  else if (FUNCTIONP (Vother_window_scroll_default))
    /* Nothing specified; try to get a window from the function.  */
    window = call0 (Vother_window_scroll_default);
  else
    {
      /* Otherwise, look for a neighboring window on the same frame.  */
      window = Fnext_window (selected_window, Qlambda, Qnil);

      if (EQ (window, selected_window))
	/* That didn't get us anywhere; look for a window on another
           visible frame on the current terminal.  */
        window = Fnext_window (window, Qlambda, Qvisible);
    }

  CHECK_LIVE_WINDOW (window);

  if (EQ (window, selected_window))
    error ("There is no other window");

  return window;
}


DEFUN ("scroll-left", Fscroll_left, Sscroll_left, 0, 2, "^P\np",
       doc: /* Scroll selected window display ARG columns left.
Default for ARG is window width minus 2.
Value is the total amount of leftward horizontal scrolling in
effect after the change.
If SET-MINIMUM is non-nil, the new scroll amount becomes the
lower bound for automatic scrolling, i.e. automatic scrolling
will not scroll a window to a column less than the value returned
by this function.  This happens in an interactive call.  */)
  (register Lisp_Object arg, Lisp_Object set_minimum)
{
  struct window *w = XWINDOW (selected_window);
  EMACS_INT requested_arg =
    (NILP (arg)
     ? window_body_width (w, WINDOW_BODY_IN_CANONICAL_CHARS) - 2
     : XFIXNUM (Fprefix_numeric_value (arg)));
  Lisp_Object result = set_window_hscroll (w, w->hscroll + requested_arg);

  if (!NILP (set_minimum))
    w->min_hscroll = w->hscroll;

  w->suspend_auto_hscroll = true;

  return result;
}

DEFUN ("scroll-right", Fscroll_right, Sscroll_right, 0, 2, "^P\np",
       doc: /* Scroll selected window display ARG columns right.
Default for ARG is window width minus 2.
Value is the total amount of leftward horizontal scrolling in
effect after the change.
If SET-MINIMUM is non-nil, the new scroll amount becomes the
lower bound for automatic scrolling, i.e. automatic scrolling
will not scroll a window to a column less than the value returned
by this function.  This happens in an interactive call.  */)
  (register Lisp_Object arg, Lisp_Object set_minimum)
{
  struct window *w = XWINDOW (selected_window);
  EMACS_INT requested_arg =
    (NILP (arg)
     ? window_body_width (w, WINDOW_BODY_IN_CANONICAL_CHARS) - 2
     : XFIXNUM (Fprefix_numeric_value (arg)));
  Lisp_Object result = set_window_hscroll (w, w->hscroll - requested_arg);

  if (!NILP (set_minimum))
    w->min_hscroll = w->hscroll;

  w->suspend_auto_hscroll = true;

  return result;
}

DEFUN ("minibuffer-selected-window", Fminibuffer_selected_window, Sminibuffer_selected_window, 0, 0, 0,
       doc: /* Return window selected just before minibuffer window was selected.
Return nil if the selected window is not a minibuffer window.  */)
  (void)
{
  if (minibuf_level > 0
      && MINI_WINDOW_P (XWINDOW (selected_window))
      && WINDOW_LIVE_P (minibuf_selected_window))
    return minibuf_selected_window;

  return Qnil;
}

/* Value is the number of lines actually displayed in window W,
   as opposed to its height.  */

static int
displayed_window_lines (struct window *w)
{
  struct it it;
  struct text_pos start;
  int height = window_box_height (w);
  struct buffer *old_buffer;
  int bottom_y;
  void *itdata = NULL;

  if (XBUFFER (w->contents) != current_buffer)
    {
      old_buffer = current_buffer;
      set_buffer_internal (XBUFFER (w->contents));
    }
  else
    old_buffer = NULL;

  /* In case W->start is out of the accessible range, do something
     reasonable.  This happens in Info mode when Info-scroll-down
     calls (recenter -1) while W->start is 1.  */
  CLIP_TEXT_POS_FROM_MARKER (start, w->start);

  itdata = bidi_shelve_cache ();
  start_move_it (&it, w, start);
  move_it_dy (&it, height);
  bottom_y = window_line_bottom_y (it);
  bidi_unshelve_cache (itdata, false);

  /* Add in empty lines at the bottom of the window.  */
  if (bottom_y < height)
    {
      int uy = FRAME_LINE_HEIGHT (it.f);
      it.vpos += (height - bottom_y + uy - 1) / uy;
    }
  else if (bottom_y == height)
    it.vpos++;

  if (old_buffer)
    set_buffer_internal (old_buffer);

  return it.vpos;
}


DEFUN ("recenter", Frecenter, Srecenter, 0, 2, "P\np",
       doc: /* Center point in selected window and maybe redisplay frame.
With a numeric prefix argument ARG, recenter putting point on screen line ARG
relative to the selected window.  If ARG is negative, it counts up from the
bottom of the window.  (ARG should be less than the height of the window.)

If ARG is omitted or nil, then recenter with point on the middle line
of the selected window; if REDISPLAY & `recenter-redisplay' are
non-nil, also erase the entire frame and redraw it (when
`auto-resize-tool-bars' is set to `grow-only', this resets the
tool-bar's height to the minimum height needed); if
`recenter-redisplay' has the special value `tty', then only tty frames
are redrawn.  Interactively, REDISPLAY is always non-nil.

Just C-u as prefix means put point in the center of the window
and redisplay normally--don't erase and redraw the frame.  */)
  (Lisp_Object arg, Lisp_Object redisplay)
{
  struct window *w = XWINDOW (selected_window);
  struct buffer *buf = XBUFFER (w->contents);
  bool center_p = false;
  ptrdiff_t charpos, bytepos;
  EMACS_INT iarg UNINIT;
  int this_scroll_margin;

  /* For reasons why we signal an error here, see
     https://lists.gnu.org/r/emacs-devel/2014-06/msg00053.html,
     https://lists.gnu.org/r/emacs-devel/2014-06/msg00094.html.  */
  if (buf != current_buffer)
    error ("`recenter'ing a window that does not display current-buffer");

  /* If redisplay is suppressed due to an error, try again.  */
  buf->display_error_modiff = 0;

  if (NILP (arg))
    {
      if (!NILP (redisplay)
	  && !NILP (Vrecenter_redisplay)
	  && (!EQ (Vrecenter_redisplay, Qtty)
	      || !NILP (Ftty_type (selected_frame))))
	{
	  ptrdiff_t i;

	  /* Invalidate pixel data calculated for all compositions.  */
	  for (i = 0; i < n_compositions; i++)
	    composition_table[i]->font = NULL;
#if defined (HAVE_WINDOW_SYSTEM)
	  WINDOW_XFRAME (w)->minimize_tab_bar_window_p = 1;
#endif
#if defined (HAVE_WINDOW_SYSTEM) && ! defined (HAVE_EXT_TOOL_BAR)
	  WINDOW_XFRAME (w)->minimize_tool_bar_window_p = 1;
#endif
	  Fredraw_frame (WINDOW_FRAME (w));
	  SET_FRAME_GARBAGED (WINDOW_XFRAME (w));
	}

      center_p = true;
    }
  else if (CONSP (arg)) /* Just C-u.  */
    center_p = true;
  else
    {
      arg = Fprefix_numeric_value (arg);
      CHECK_FIXNUM (arg);
      iarg = XFIXNUM (arg);
    }

  /* Do this after making BUF current
     in case scroll_margin is buffer-local.  */
  this_scroll_margin = window_scroll_margin (w, MARGIN_IN_LINES);

  /* Don't use redisplay code for initial frames, as the necessary
     data structures might not be set up yet then.  */
  if (!FRAME_INITIAL_P (XFRAME (w->frame)))
    {
      if (center_p)
	{
	  struct it it;
	  struct text_pos pt;
	  void *itdata = bidi_shelve_cache ();

	  SET_TEXT_POS (pt, PT, PT_BYTE);
	  start_move_it (&it, w, pt);
	  move_it_dy (&it, window_box_height (w) / -2);
	  charpos = IT_CHARPOS (it);
	  bytepos = IT_BYTEPOS (it);
	  bidi_unshelve_cache (itdata, false);
	}
      else if (iarg < 0)
	{
	  struct it it;
	  struct text_pos pt;
	  ptrdiff_t nlines = min (PTRDIFF_MAX, -iarg);
	  int extra_line_spacing;
	  int h = window_box_height (w);
	  int ht = window_internal_height (w);
	  void *itdata = bidi_shelve_cache ();

	  nlines = clip_to_bounds (this_scroll_margin + 1, nlines,
				   ht - this_scroll_margin);

	  SET_TEXT_POS (pt, PT, PT_BYTE);
	  start_move_it (&it, w, pt);

	  /* Be sure we have the exact height of the full line containing PT.  */
	  move_it_dvpos (&it, 0);

	  /* The amount of pixels we have to move back is the window
	     height minus what's displayed in the line containing PT,
	     and the lines below.  */
	  it.current_y = 0;
	  it.vpos = 0;
	  move_it_dvpos (&it, nlines);

	  if (it.vpos == nlines)
	    h -= it.current_y;
	  else
	    {
	      /* Last line has no newline.  */
	      h -= window_line_bottom_y (it);
	      it.vpos++;
	    }

	  /* Don't reserve space for extra line spacing of last line.  */
	  extra_line_spacing = it.max_extra_line_spacing;

	  /* If we can't move down NLINES lines because we hit
	     the end of the buffer, count in some empty lines.  */
	  if (it.vpos < nlines)
	    {
	      nlines -= it.vpos;
	      extra_line_spacing = it.extra_line_spacing;
	      h -= nlines * (FRAME_LINE_HEIGHT (it.f) + extra_line_spacing);
	    }
	  if (h <= 0)
	    {
	      bidi_unshelve_cache (itdata, false);
	      return Qnil;
	    }

	  /* Now find the new top line (starting position) of the window.  */
	  start_move_it (&it, w, pt);
	  it.current_y = 0;
	  move_it_dy (&it, -h);

	  /* If extra line spacing is present, we may move too far
	     back.  This causes the last line to be only partially
	     visible (which triggers redisplay to recenter that line
	     in the middle), so move forward.
	     But ignore extra line spacing on last line, as it is not
	     considered to be part of the visible height of the line.
	  */
	  h += extra_line_spacing;
	  while (-it.current_y > h)
	    move_it_dvpos (&it, 1);

	  charpos = IT_CHARPOS (it);
	  bytepos = IT_BYTEPOS (it);

	  bidi_unshelve_cache (itdata, false);
	}
      else
	{
	  struct it it;
	  struct text_pos pt;
	  ptrdiff_t nlines = min (PTRDIFF_MAX, iarg);
	  int ht = window_internal_height (w);
	  void *itdata = bidi_shelve_cache ();

	  nlines = clip_to_bounds (this_scroll_margin, nlines,
				   ht - this_scroll_margin - 1);

	  SET_TEXT_POS (pt, PT, PT_BYTE);
	  start_move_it (&it, w, pt);

	  /* Move to the beginning of screen line containing PT.  */
	  move_it_dvpos (&it, 0);

	  /* Move back to find the point which is ARG screen lines above PT.  */
	  if (nlines > 0)
	    {
	      it.current_y = 0;
	      it.vpos = 0;
	      move_it_dvpos (&it, -nlines);
	    }

	  charpos = IT_CHARPOS (it);
	  bytepos = IT_BYTEPOS (it);

	  bidi_unshelve_cache (itdata, false);
	}
    }
  else
    {
      struct position pos;
      int ht = window_internal_height (w);

      if (center_p)
	iarg = ht / 2;
      else if (iarg < 0)
	iarg += ht;

      /* Don't let it get into the margin at either top or bottom.  */
      iarg = clip_to_bounds (this_scroll_margin, iarg,
			     ht - this_scroll_margin - 1);

      pos = *vmotion (PT, PT_BYTE, - iarg, w);
      charpos = pos.bufpos;
      bytepos = pos.bytepos;
    }

  /* Set the new window start.  */
  set_marker_both (w->start, w->contents, charpos, bytepos);

  /* The window start was calculated with an iterator already adjusted
     by the existing vscroll, so w->start must not be combined with
     retaining the existing vscroll, which redisplay will not reset if
     w->preserve_vscroll_p is enabled.  (bug#70386) */
  w->vscroll = 0;
  w->preserve_vscroll_p = false;
  w->window_end_valid = false;
  w->optional_new_start = true;

  w->start_at_line_beg = (bytepos == BEGV_BYTE
			  || FETCH_BYTE (bytepos - 1) == '\n');

  wset_redisplay (w);

  return Qnil;
}

DEFUN ("window-text-width", Fwindow_text_width, Swindow_text_width,
       0, 2, 0,
       doc: /* Return the width in columns of the text display area of WINDOW.
WINDOW must be a live window and defaults to the selected one.

The returned width does not include dividers, scrollbars, margins,
fringes, nor any partial-width columns at the right of the text
area.

Optional argument PIXELWISE non-nil, means to return the width in
pixels.  */)
  (Lisp_Object window, Lisp_Object pixelwise)
{
  struct window *w = decode_live_window (window);

  if (NILP (pixelwise))
    return make_fixnum (window_box_width (w, TEXT_AREA)
			/ FRAME_COLUMN_WIDTH (WINDOW_XFRAME (w)));
  else
    return make_fixnum (window_box_width (w, TEXT_AREA));
}

DEFUN ("window-text-height", Fwindow_text_height, Swindow_text_height,
       0, 2, 0,
       doc: /* Return the height in lines of the text display area of WINDOW.
WINDOW must be a live window and defaults to the selected one.

The returned height does not include dividers, the mode line, any header
line, nor any partial-height lines at the bottom of the text area.

Optional argument PIXELWISE non-nil, means to return the height in
pixels.  */)
  (Lisp_Object window, Lisp_Object pixelwise)
{
  struct window *w = decode_live_window (window);

  if (NILP (pixelwise))
    return make_fixnum (window_box_height (w)
			/ FRAME_LINE_HEIGHT (WINDOW_XFRAME (w)));
  else
    return make_fixnum (window_box_height (w));
}

DEFUN ("move-to-window-line", Fmove_to_window_line, Smove_to_window_line,
       1, 1, "P",
       doc: /* Position point relative to window.
ARG nil means position point at center of window.
Else, ARG specifies vertical position within the window;
zero means top of window, negative means relative to bottom
of window, -1 meaning the last fully visible display line
of the window.

Value is the screen line of the window point moved to, counting
from the top of the window.  */)
  (Lisp_Object arg)
{
  struct window *w = XWINDOW (selected_window);
  int lines, start;
  Lisp_Object window;
#if false
  int this_scroll_margin;
#endif

  if (!(BUFFERP (w->contents) && XBUFFER (w->contents) == current_buffer))
    /* This test is needed to make sure PT/PT_BYTE make sense in w->contents
       when passed below to set_marker_both.  */
    error ("move-to-window-line called from unrelated buffer");

  window = selected_window;
  start = marker_position (w->start);
  if (start < BEGV || start > ZV)
    {
      int height = window_internal_height (w);
      Fvertical_motion (make_fixnum (- (height / 2)), window, Qnil);
      set_marker_both (w->start, w->contents, PT, PT_BYTE);
      w->start_at_line_beg = !NILP (Fbolp ());
      w->force_start = true;

      /* Since `Fvertical_motion' computes coordinates after vscroll is
         applied, it is taken into account in POS, and vscroll must be
         reset by `force_start' in `redisplay_internal'.  */
      w->preserve_vscroll_p = false;
    }
  else
    Fgoto_char (w->start);

  lines = displayed_window_lines (w);

  if (NILP (arg))
    XSETFASTINT (arg, lines / 2);
  else
    {
      EMACS_INT iarg = XFIXNUM (Fprefix_numeric_value (arg));

      if (iarg < 0)
	iarg = iarg + lines;

#if false /* This code would prevent move-to-window-line from moving point
	     to a place inside the scroll margins (which would cause the
	     next redisplay to scroll).  I wrote this code, but then concluded
	     it is probably better not to install it.  However, it is here
	     inside #if false so as not to lose it.  -- rms.  */

      this_scroll_margin = window_scroll_margin (w, MARGIN_IN_LINES);

      /* Don't let it get into the margin at either top or bottom.  */
      iarg = max (iarg, this_scroll_margin);
      iarg = min (iarg, lines - this_scroll_margin - 1);
#endif

      arg = make_fixnum (iarg);
    }

  /* Skip past a partially visible first line.  */
  if (w->vscroll)
    XSETINT (arg, XFIXNUM (arg) + 1);

  return Fvertical_motion (arg, window, Qnil);
}



/***********************************************************************
			 Window Configuration
 ***********************************************************************/

struct save_window_data
  {
    union vectorlike_header header;
    Lisp_Object selected_frame;
    Lisp_Object current_window;
    Lisp_Object f_current_buffer;
    Lisp_Object minibuf_scroll_window;
    Lisp_Object minibuf_selected_window;
    Lisp_Object root_window;
    Lisp_Object focus_frame;
    /* A vector, each of whose elements is a struct saved_window
       for one window.  */
    Lisp_Object saved_windows;

    /* All fields above are traced by the GC.
       After saved_windows, the fields are ignored by the GC.  */

    /* We should be able to do without the following two.  */
    int frame_cols, frame_lines;
    /* These three should get eventually replaced by their pixel
       counterparts.  */
    int frame_menu_bar_lines, frame_tab_bar_lines, frame_tool_bar_lines;
    int frame_text_width, frame_text_height;
    /* These are currently unused.  We need them as soon as we convert
       to pixels.  */
    int frame_menu_bar_height, frame_tab_bar_height, frame_tool_bar_height;
  } GCALIGNED_STRUCT;

/* This is saved as a Lisp_Vector.  */
struct saved_window
{
  union vectorlike_header header;

  Lisp_Object window, buffer, start, pointm, old_pointm;
  Lisp_Object pixel_left, pixel_top, pixel_height, pixel_width;
  Lisp_Object left_col, top_line, total_cols, total_lines;
  Lisp_Object normal_cols, normal_lines;
  Lisp_Object hscroll, min_hscroll, hscroll_whole, suspend_auto_hscroll;
  Lisp_Object vscroll;
  Lisp_Object parent, prev;
  Lisp_Object start_at_line_beg;
  Lisp_Object display_table;
  Lisp_Object left_margin_cols, right_margin_cols;
  Lisp_Object left_fringe_width, right_fringe_width;
  Lisp_Object fringes_outside_margins, fringes_persistent;
  Lisp_Object scroll_bar_width, vertical_scroll_bar_type;
  Lisp_Object scroll_bar_height, horizontal_scroll_bar_type;
  Lisp_Object scroll_bars_persistent, dedicated;
  Lisp_Object combination_limit, window_parameters;
};

#define SAVED_WINDOW_N(swv,n) \
  ((struct saved_window *) (XVECTOR ((swv)->contents[(n)])))

DEFUN ("window-configuration-p", Fwindow_configuration_p, Swindow_configuration_p, 1, 1, 0,
       doc: /* Return t if OBJECT is a window-configuration object.  */)
  (Lisp_Object object)
{
  return WINDOW_CONFIGURATIONP (object) ? Qt : Qnil;
}

DEFUN ("window-configuration-frame", Fwindow_configuration_frame, Swindow_configuration_frame, 1, 1, 0,
       doc: /* Return the frame that CONFIG, a window-configuration object, is about.  */)
  (Lisp_Object config)
{
  register struct save_window_data *data;
  struct Lisp_Vector *saved_windows;

  CHECK_WINDOW_CONFIGURATION (config);

  data = (struct save_window_data *) XVECTOR (config);
  saved_windows = XVECTOR (data->saved_windows);
  return XWINDOW (SAVED_WINDOW_N (saved_windows, 0)->window)->frame;
}

DEFUN ("set-window-configuration", Fset_window_configuration,
       Sset_window_configuration, 1, 3, 0,
       doc: /* Set the configuration of windows and buffers as specified by CONFIGURATION.
CONFIGURATION must be a value previously returned
by `current-window-configuration'.

Normally, this function selects the frame of the CONFIGURATION, but if
DONT-SET-FRAME is non-nil, it leaves selected the frame which was
current at the start of the function.  If DONT-SET-MINIWINDOW is non-nil,
the mini-window of the frame doesn't get set to the corresponding element
of CONFIGURATION.

This function consults the variable `window-restore-killed-buffer-windows'
when restoring a window whose buffer was killed after CONFIGURATION was
recorded.

If CONFIGURATION was made from a frame that is now deleted,
only frame-independent values can be restored.  In this case,
the return value is nil.  Otherwise the value is t.  */)
  (Lisp_Object configuration, Lisp_Object dont_set_frame,
   Lisp_Object dont_set_miniwindow)
{
  register struct save_window_data *data;
  struct Lisp_Vector *saved_windows;
  Lisp_Object new_current_buffer;
  Lisp_Object frame;
  Lisp_Object kept_windows = Qnil;
  Lisp_Object old_frame = selected_frame;
  struct frame *f;
  ptrdiff_t old_point = -1;
  USE_SAFE_ALLOCA;

  CHECK_WINDOW_CONFIGURATION (configuration);

  data = (struct save_window_data *) XVECTOR (configuration);
  saved_windows = XVECTOR (data->saved_windows);

  new_current_buffer = data->f_current_buffer;
  if (!BUFFER_LIVE_P (XBUFFER (new_current_buffer)))
    new_current_buffer = Qnil;
  else
    {
      if (XBUFFER (new_current_buffer) == current_buffer)
	/* The code further down "preserves point" by saving here PT in
	   old_point and then setting it later back into PT.  When the
	   current-selected-window and the final-selected-window both show
	   the current buffer, this suffers from the problem that the
	   current PT is the window-point of the current-selected-window,
	   while the final PT is the point of the final-selected-window, so
	   this copy from one PT to the other would end up moving the
	   window-point of the final-selected-window to the window-point of
	   the current-selected-window.  So we have to be careful which
	   point of the current-buffer we copy into old_point.  */
	if (EQ (XWINDOW (data->current_window)->contents, new_current_buffer)
	    && WINDOWP (selected_window)
	    && EQ (XWINDOW (selected_window)->contents, new_current_buffer)
	    && !EQ (selected_window, data->current_window))
	  old_point = marker_position (XWINDOW (data->current_window)->pointm);
	else
	  old_point = PT;
      else
	/* BUF_PT (XBUFFER (new_current_buffer)) gives us the position of
	   point in new_current_buffer as of the last time this buffer was
	   used.  This can be non-deterministic since it can be changed by
	   things like jit-lock by mere temporary selection of some random
	   window that happens to show this buffer.
	   So if possible we want this arbitrary choice of "which point" to
	   be the one from the to-be-selected-window so as to prevent this
	   window's cursor from being copied from another window.  */
	if (EQ (XWINDOW (data->current_window)->contents, new_current_buffer)
	    /* If current_window = selected_window, its point is in BUF_PT.  */
	    && !EQ (selected_window, data->current_window))
	  old_point = marker_position (XWINDOW (data->current_window)->pointm);
	else
	  old_point = BUF_PT (XBUFFER (new_current_buffer));
    }

  frame = XWINDOW (SAVED_WINDOW_N (saved_windows, 0)->window)->frame;
  f = XFRAME (frame);

  /* If f is a dead frame, don't bother rebuilding its window tree.
     However, there is other stuff we should still try to do below.  */
  if (FRAME_LIVE_P (f))
    {
      Lisp_Object window;
      Lisp_Object dead_windows = Qnil;
      Lisp_Object tem, par, pers;
      struct window *w;
      struct saved_window *p;
      struct window *root_window;
      struct window **leaf_windows;
      ptrdiff_t i, k, n_leaf_windows;

      /* Don't do this within the main loop below: This may call Lisp
	 code and is thus potentially unsafe while input is blocked.  */
      for (k = 0; k < saved_windows->header.size; k++)
	{
	  p = SAVED_WINDOW_N (saved_windows, k);
	  window = p->window;
	  w = XWINDOW (window);

	  if (BUFFERP (w->contents)
	      && !EQ (w->contents, p->buffer)
	      && BUFFER_LIVE_P (XBUFFER (p->buffer))
	      && (NILP (Fminibufferp (p->buffer, Qnil))))
	    /* If a window we restore gets another buffer, record the
	       window's old buffer.  */
	    call1 (Qrecord_window_buffer, window);
	}

      /* Disallow set_window_size_hook, temporarily.  */
      f->can_set_window_size = false;
      /* The mouse highlighting code could get screwed up
	 if it runs during this.  */
      block_input ();

      /* "Swap out" point from the selected window's buffer
	 into the window itself.  (Normally the pointm of the selected
	 window holds garbage.)  We do this now, before
	 restoring the window contents, and prevent it from
	 being done later on when we select a new window.  */
      if (!NILP (XWINDOW (selected_window)->contents))
	{
	  w = XWINDOW (selected_window);
	  set_marker_both (w->pointm,
			   w->contents,
			   BUF_PT (XBUFFER (w->contents)),
			   BUF_PT_BYTE (XBUFFER (w->contents)));
	}

      fset_redisplay (f);

      /* Problem: Freeing all matrices and later allocating them again
	 is a serious redisplay flickering problem.  What we would
	 really like to do is to free only those matrices not reused
	 below.  */
      root_window = XWINDOW (FRAME_ROOT_WINDOW (f));
      ptrdiff_t nwindows = count_windows (root_window);
      SAFE_NALLOCA (leaf_windows, 1, nwindows);
      n_leaf_windows = get_leaf_windows (root_window, leaf_windows, 0);

      /* Kludge Alert!
	 Mark all windows now on frame as "deleted".
	 Restoring the new configuration "undeletes" any that are in it.

	 Save their current buffers in their height fields, since we may
	 need it later, if a buffer saved in the configuration is now
	 dead.  */
      delete_all_child_windows (FRAME_ROOT_WINDOW (f));

      for (k = 0; k < saved_windows->header.size; k++)
	{
	  p = SAVED_WINDOW_N (saved_windows, k);
	  window = p->window;
	  w = XWINDOW (window);
	  wset_next (w, Qnil);

	  if (!NILP (p->parent))
	    wset_parent
	      (w, SAVED_WINDOW_N (saved_windows, XFIXNAT (p->parent))->window);
	  else
	    wset_parent (w, Qnil);

	  if (!NILP (p->prev))
	    {
	      wset_prev
		(w, SAVED_WINDOW_N (saved_windows, XFIXNAT (p->prev))->window);
	      wset_next (XWINDOW (w->prev), p->window);
	    }
	  else
	    {
	      wset_prev (w, Qnil);
	      if (!NILP (w->parent))
		wset_combination (XWINDOW (w->parent),
				  (XFIXNUM (p->total_cols)
				   != XWINDOW (w->parent)->total_cols),
				  p->window);
	    }

	  /* If we squirreled away the buffer, restore it now.  */
	  if (BUFFERP (w->combination_limit))
	    wset_buffer (w, w->combination_limit);
	  w->pixel_left = XFIXNAT (p->pixel_left);
	  w->pixel_top = XFIXNAT (p->pixel_top);
	  w->pixel_width = XFIXNAT (p->pixel_width);
	  w->pixel_height = XFIXNAT (p->pixel_height);
	  w->left_col = XFIXNAT (p->left_col);
	  w->top_line = XFIXNAT (p->top_line);
	  w->total_cols = XFIXNAT (p->total_cols);
	  w->total_lines = XFIXNAT (p->total_lines);
	  wset_normal_cols (w, p->normal_cols);
	  wset_normal_lines (w, p->normal_lines);
	  w->hscroll = XFIXNAT (p->hscroll);
	  w->suspend_auto_hscroll = !NILP (p->suspend_auto_hscroll);
	  w->min_hscroll = XFIXNAT (p->min_hscroll);
	  w->hscroll_whole = XFIXNAT (p->hscroll_whole);
	  w->vscroll = -XFIXNAT (p->vscroll);
	  wset_display_table (w, p->display_table);
	  w->left_margin_cols = XFIXNUM (p->left_margin_cols);
	  w->right_margin_cols = XFIXNUM (p->right_margin_cols);
	  w->left_fringe_width = XFIXNUM (p->left_fringe_width);
	  w->right_fringe_width = XFIXNUM (p->right_fringe_width);
	  w->fringes_outside_margins = !NILP (p->fringes_outside_margins);
	  w->fringes_persistent = !NILP (p->fringes_persistent);
	  w->scroll_bar_width = XFIXNUM (p->scroll_bar_width);
	  w->scroll_bar_height = XFIXNUM (p->scroll_bar_height);
	  w->scroll_bars_persistent = !NILP (p->scroll_bars_persistent);
	  wset_vertical_scroll_bar_type (w, p->vertical_scroll_bar_type);
	  wset_horizontal_scroll_bar_type (w, p->horizontal_scroll_bar_type);
	  wset_dedicated (w, p->dedicated);
	  wset_combination_limit (w, p->combination_limit);
	  /* Restore any window parameters that have been saved.
	     Parameters that have not been saved are left alone.  */
	  for (tem = p->window_parameters; CONSP (tem); tem = XCDR (tem))
	    {
	      pers = XCAR (tem);
	      if (CONSP (pers))
		{
		  if (NILP (XCDR (pers)))
		    {
		      par = Fassq (XCAR (pers), w->window_parameters);
		      if (CONSP (par) && !NILP (XCDR (par)))
			/* Reset a parameter to nil if and only if it
			   has a non-nil association.  Don't make new
			   associations.  */
			Fsetcdr (par, Qnil);
		    }
		  else
		    /* Always restore a non-nil value.  */
		    Fset_window_parameter (window, XCAR (pers), XCDR (pers));
		}
	    }

	  /* Remove window from the table of dead windows.  */
	  Fremhash (make_fixnum (w->sequence_number),
		    window_dead_windows_table);

	  if ((NILP (dont_set_miniwindow) || !MINI_WINDOW_P (w))
	      && BUFFERP (p->buffer) && BUFFER_LIVE_P (XBUFFER (p->buffer)))
	    /* If saved buffer is alive, install it, unless it's a
	       minibuffer we explicitly prohibit.  */
	    {
	      if (!EQ (w->contents, p->buffer))
		{
		  wset_buffer (w, p->buffer);
		  window_discard_buffer_from_window (w->contents, window, false);
		}

	      w->start_at_line_beg = !NILP (p->start_at_line_beg);
	      set_marker_restricted (w->start, p->start, w->contents);
	      set_marker_restricted (w->pointm, p->pointm, w->contents);
	      set_marker_restricted (w->old_pointm, p->old_pointm, w->contents);
	      /* As documented in Fcurrent_window_configuration, don't
		 restore the location of point in the buffer which was
		 current when the window configuration was recorded.  */
	      if (!EQ (p->buffer, new_current_buffer)
		  && XBUFFER (p->buffer) == current_buffer)
		Fgoto_char (w->pointm);
	    }
	  else if (BUFFERP (w->contents) && BUFFER_LIVE_P (XBUFFER (w->contents)))
	    /* Keep window's old buffer; make sure the markers are real.  */
	    {
	      /* Set window markers at start of visible range.  */
	      if (XMARKER (w->start)->buffer == 0)
		set_marker_restricted_both (w->start, w->contents, 0, 0);
	      if (XMARKER (w->pointm)->buffer == 0)
		set_marker_restricted_both
		  (w->pointm, w->contents,
		   BUF_PT (XBUFFER (w->contents)),
		   BUF_PT_BYTE (XBUFFER (w->contents)));
	      if (XMARKER (w->old_pointm)->buffer == 0)
		set_marker_restricted_both
		  (w->old_pointm, w->contents,
		   BUF_PT (XBUFFER (w->contents)),
		   BUF_PT_BYTE (XBUFFER (w->contents)));
	      w->start_at_line_beg = true;
	      if (FUNCTIONP (window_restore_killed_buffer_windows)
		  && !MINI_WINDOW_P (w))
		kept_windows = Fcons (listn (6, window, p->buffer,
					     Fmarker_last_position (p->start),
					     Fmarker_last_position (p->pointm),
					     p->dedicated, Qt),
				      kept_windows);
	    }
	  else if (!NILP (w->start))
	    /* Leaf window has no live buffer, get one.  */
	    {
	      /* Get the buffer via other_buffer_safely in order to
		 avoid showing an unimportant buffer and, if necessary, to
		 recreate *scratch* in the course (part of Juanma's bs-show
		 scenario from March 2011).  */
	      wset_buffer (w, other_buffer_safely (Fcurrent_buffer ()));
	      window_discard_buffer_from_window (w->contents, window, false);
	      /* This will set the markers to beginning of visible
		 range.  */
	      set_marker_restricted_both (w->start, w->contents, 0, 0);
	      set_marker_restricted_both (w->pointm, w->contents, 0, 0);
	      set_marker_restricted_both (w->old_pointm, w->contents, 0, 0);
	      w->start_at_line_beg = true;
	      if (!MINI_WINDOW_P (w))
		{
		  if (FUNCTIONP (window_restore_killed_buffer_windows))
		    kept_windows
		      = Fcons (listn (6, window, p->buffer,
				      Fmarker_last_position (p->start),
				      Fmarker_last_position (p->pointm),
				      p->dedicated, Qnil),
			       kept_windows);
		  else if (EQ (window_restore_killed_buffer_windows, Qdelete)
			   || (!NILP (p->dedicated)
			       && (NILP (window_restore_killed_buffer_windows)
				   || EQ (window_restore_killed_buffer_windows,
					  Qdedicated))))
		    /* Try to delete this window later.  */
		    dead_windows = Fcons (window, dead_windows);
		  /* Make sure window is no more dedicated.  */
		  wset_dedicated (w, Qnil);
		}
	    }
	}

      fset_root_window (f, data->root_window);
      /* Arrange *not* to restore point in the buffer that was
	 current when the window configuration was saved.  */
      if (EQ (XWINDOW (data->current_window)->contents, new_current_buffer))
	set_marker_restricted (XWINDOW (data->current_window)->pointm,
			       make_fixnum (old_point),
			       XWINDOW (data->current_window)->contents);

      /* In the following call to select_window, prevent "swapping out
	 point" in the old selected window using the buffer that has
	 been restored into it.  We already swapped out that point
	 from that window's old buffer.

	 Do not record the buffer here.  We do that in a separate call
	 to select_window below.  See also Bug#16207.  */
      select_window (data->current_window, Qt, true);
      BVAR (XBUFFER (XWINDOW (selected_window)->contents),
	    last_selected_window)
	= selected_window;

      /* We may have deleted windows above.  Then again, maybe we
	 haven't: the functions we call to maybe delete windows can
	 decide a window cannot be deleted.  Force recalculation of
	 Vwindow_list next time it is needed, to make sure stale
	 windows with no buffers don't escape into the wild, which
	 will cause crashes elsewhere.  */
      Vwindow_list = Qnil;

      if (NILP (data->focus_frame)
	  || (FRAMEP (data->focus_frame)
	      && FRAME_LIVE_P (XFRAME (data->focus_frame))))
	Fredirect_frame_focus (frame, data->focus_frame);

      /* Now, free glyph matrices in windows that were not reused.  */
      for (i = 0; i < n_leaf_windows; i++)
	if (NILP (leaf_windows[i]->contents))
	  free_window_matrices (leaf_windows[i]);

      /* Allow set_window_size_hook again and resize frame's windows
	 if necessary.  But change frame size only to preserve window
	 minimum sizes.  */
      f->can_set_window_size = true;
      adjust_frame_size (f, -1, -1, 4, false, Qset_window_configuration);

      adjust_frame_glyphs (f);
      unblock_input ();

      /* Scan dead buffer windows.  */
      for (; CONSP (dead_windows); dead_windows = XCDR (dead_windows))
	{
	  window = XCAR (dead_windows);
	  if (WINDOW_LIVE_P (window) && !EQ (window, FRAME_ROOT_WINDOW (f)))
	    delete_deletable_window (window);
	}

      /* Record the selected window's buffer here.  The window should
	 already be the selected one from the call above.  */
      if (WINDOW_LIVE_P (data->current_window))
	select_window (data->current_window, Qnil, false);

      /* select_window will have made f the selected frame, so we
	 reselect the proper frame here.  do_switch_frame will change
	 the selected window too, but that doesn't make the call to
	 select_window above totally superfluous; it still sets f's
	 selected window.  */
      if (FRAME_LIVE_P (XFRAME (data->selected_frame)))
	do_switch_frame (NILP (dont_set_frame)
                         ? data->selected_frame
                         : old_frame
                         , 0, 0, Qnil);
    }

  FRAME_WINDOW_CHANGE (f) = true;

  if (!NILP (new_current_buffer))
    {
      Fset_buffer (new_current_buffer);
      /* If the new current buffer doesn't appear in the selected
	 window, go to its old point (Bug#12208).

	 The original fix used data->current_window below which caused
	 false positives (compare Bug#31695) when data->current_window
	 is not on data->selected_frame.  This happens, for example,
	 when read_minibuf restores the configuration of a stand-alone
	 minibuffer frame: After switching to the previously selected
	 "normal" frame, point of that frame's selected window jumped
	 unexpectedly because new_current_buffer is usually *not*
	 shown in data->current_window - the minibuffer frame's
	 selected window.  Using selected_window instead fixes this
	 because do_switch_frame has set up selected_window already to
	 the "normal" frame's selected window and that window *does*
	 show new_current_buffer.  */
      if (!EQ (XWINDOW (selected_window)->contents, new_current_buffer))
	Fgoto_char (make_fixnum (old_point));
    }

  Vminibuf_scroll_window = data->minibuf_scroll_window;
  minibuf_selected_window = data->minibuf_selected_window;

  SAFE_FREE ();

  if (FUNCTIONP (window_restore_killed_buffer_windows))
    safe_calln (window_restore_killed_buffer_windows,
		frame, kept_windows, Qconfiguration);

  return FRAME_LIVE_P (f) ? Qt : Qnil;
}

void
restore_window_configuration (Lisp_Object configuration)
{
  if (CONSP (configuration))
    Fset_window_configuration (XCAR (configuration),
			       Fcar_safe (XCDR (configuration)),
			       Fcar_safe (Fcdr_safe (XCDR (configuration))));
  else
    Fset_window_configuration (configuration, Qnil, Qnil);
}


/* If WINDOW is an internal window, recursively delete all child windows
   reachable via the next and contents slots of WINDOW.  Otherwise setup
   WINDOW to not show any buffer.  */

void
delete_all_child_windows (Lisp_Object window)
{
  register struct window *w;

  w = XWINDOW (window);

  if (!NILP (w->next))
    /* Delete WINDOW's siblings (we traverse postorderly).  */
    delete_all_child_windows (w->next);

  if (WINDOWP (w->contents))
    {
      delete_all_child_windows (w->contents);
      wset_combination (w, false, Qnil);
    }
  else if (BUFFERP (w->contents))
    {
      unshow_buffer (w);
      unchain_marker (XMARKER (w->pointm));
      unchain_marker (XMARKER (w->old_pointm));
      unchain_marker (XMARKER (w->start));
      /* Since combination limit makes sense for an internal windows
	 only, we use this slot to save the buffer for the sake of
	 possible resurrection in Fset_window_configuration.  */
      wset_combination_limit (w, w->contents);
      wset_buffer (w, Qnil);
      /* Add WINDOW to table of dead windows so when killing a buffer
	 WINDOW mentions, all references to that buffer can be removed
	 and the buffer be collected.  */
      Fputhash (make_fixnum (w->sequence_number),
		window, window_dead_windows_table);
    }

  Vwindow_list = Qnil;
}

static ptrdiff_t
count_windows (struct window *window)
{
  ptrdiff_t count = 1;
  if (!NILP (window->next))
    count += count_windows (XWINDOW (window->next));
  if (WINDOWP (window->contents))
    count += count_windows (XWINDOW (window->contents));
  return count;
}


/* Fill vector FLAT with leaf windows under W, starting at index I.
   Value is last index + 1.  */
static ptrdiff_t
get_leaf_windows (struct window *w, struct window **flat, ptrdiff_t i)
{
  while (w)
    {
      if (WINDOWP (w->contents))
	i = get_leaf_windows (XWINDOW (w->contents), flat, i);
      else
	flat[i++] = w;

      w = NILP (w->next) ? 0 : XWINDOW (w->next);
    }

  return i;
}


/* Return a pointer to the glyph W's physical cursor is on.  Value is
   null if W's current matrix is invalid, so that no meaningful glyph
   can be returned.  */
struct glyph *
get_phys_cursor_glyph (struct window *w)
{
  struct glyph_row *row;
  struct glyph *glyph;
  int hpos = w->phys_cursor.hpos;

  if (!(w->phys_cursor.vpos >= 0
	&& w->phys_cursor.vpos < w->current_matrix->nrows))
    return NULL;

  row = MATRIX_ROW (w->current_matrix, w->phys_cursor.vpos);
  if (!row->enabled_p)
    return NULL;

  if (w->hscroll)
    {
      /* When the window is hscrolled, cursor hpos can legitimately be
	 out of bounds, but we draw the cursor at the corresponding
	 window margin in that case.  */
      if (!row->reversed_p && hpos < 0)
	hpos = 0;
      if (row->reversed_p && hpos >= row->used[TEXT_AREA])
	hpos = row->used[TEXT_AREA] - 1;
    }

  if (0 <= hpos && hpos < row->used[TEXT_AREA])
    glyph = row->glyphs[TEXT_AREA] + hpos;
  else
    glyph = NULL;

  return glyph;
}


static ptrdiff_t
save_window_save (Lisp_Object window, struct Lisp_Vector *vector, ptrdiff_t i)
{
  struct saved_window *p;
  struct window *w;
  Lisp_Object tem, pers, par;

  for (; !NILP (window); window = w->next)
    {
      p = SAVED_WINDOW_N (vector, i);
      w = XWINDOW (window);

      wset_temslot (w, make_fixnum (i)); i++;
      p->window = window;
      p->buffer = (WINDOW_LEAF_P (w) ? w->contents : Qnil);
      p->pixel_left = make_fixnum (w->pixel_left);
      p->pixel_top = make_fixnum (w->pixel_top);
      p->pixel_width = make_fixnum (w->pixel_width);
      p->pixel_height = make_fixnum (w->pixel_height);
      p->left_col = make_fixnum (w->left_col);
      p->top_line = make_fixnum (w->top_line);
      p->total_cols = make_fixnum (w->total_cols);
      p->total_lines = make_fixnum (w->total_lines);
      p->normal_cols = w->normal_cols;
      p->normal_lines = w->normal_lines;
      XSETFASTINT (p->hscroll, w->hscroll);
      p->suspend_auto_hscroll = w->suspend_auto_hscroll ? Qt : Qnil;
      XSETFASTINT (p->min_hscroll, w->min_hscroll);
      XSETFASTINT (p->hscroll_whole, w->hscroll_whole);
      XSETFASTINT (p->vscroll, -w->vscroll);
      p->display_table = w->display_table;
      p->left_margin_cols = make_fixnum (w->left_margin_cols);
      p->right_margin_cols = make_fixnum (w->right_margin_cols);
      p->left_fringe_width = make_fixnum (w->left_fringe_width);
      p->right_fringe_width = make_fixnum (w->right_fringe_width);
      p->fringes_outside_margins = w->fringes_outside_margins ? Qt : Qnil;
      p->fringes_persistent = w->fringes_persistent ? Qt : Qnil;
      p->scroll_bar_width = make_fixnum (w->scroll_bar_width);
      p->scroll_bar_height = make_fixnum (w->scroll_bar_height);
      p->scroll_bars_persistent = w->scroll_bars_persistent ? Qt : Qnil;
      p->vertical_scroll_bar_type = w->vertical_scroll_bar_type;
      p->horizontal_scroll_bar_type = w->horizontal_scroll_bar_type;
      p->dedicated = w->dedicated;
      p->combination_limit = w->combination_limit;
      p->window_parameters = Qnil;

      if (!NILP (Vwindow_persistent_parameters))
	{
	  /* Run cycle detection on Vwindow_persistent_parameters.  */
	  Lisp_Object tortoise, hare;

	  hare = tortoise = Vwindow_persistent_parameters;
	  while (CONSP (hare))
	    {
	      hare = XCDR (hare);
	      if (!CONSP (hare))
		break;

	      hare = XCDR (hare);
	      tortoise = XCDR (tortoise);

	      if (EQ (hare, tortoise))
		/* Reset Vwindow_persistent_parameters to Qnil.  */
		{
		  Vwindow_persistent_parameters = Qnil;
		  break;
		}
	    }

	  for (tem = Vwindow_persistent_parameters; CONSP (tem);
	       tem = XCDR (tem))
	    {
	      pers = XCAR (tem);
	      /* Save values for persistent window parameters. */
	      if (CONSP (pers) && !NILP (XCDR (pers)))
		{
		  par = Fassq (XCAR (pers), w->window_parameters);
		  if (NILP (par))
		    /* If the window has no value for the parameter,
		       make one.  */
		    p->window_parameters = Fcons (Fcons (XCAR (pers), Qnil),
						  p->window_parameters);
		  else
		    /* If the window has a value for the parameter,
		       save it.  */
		    p->window_parameters = Fcons (Fcons (XCAR (par),
							 XCDR (par)),
						  p->window_parameters);
		}
	    }
	}

      if (BUFFERP (w->contents))
	{
	  bool window_point_insertion_type
	    = !NILP (find_symbol_value (XSYMBOL (Qwindow_point_insertion_type),
					 XBUFFER (w->contents)));

	  /* Save w's value of point in the window configuration.  If w
	     is the selected window, then get the value of point from
	     the buffer; pointm is garbage in the selected window.  */
	  if (EQ (window, selected_window))
	    p->pointm = build_marker (XBUFFER (w->contents),
				      BUF_PT (XBUFFER (w->contents)),
				      BUF_PT_BYTE (XBUFFER (w->contents)));
	  else
	    p->pointm = Fcopy_marker (w->pointm, Qnil);
	  p->old_pointm = Fcopy_marker (w->old_pointm, Qnil);
	  XMARKER (p->pointm)->insertion_type = window_point_insertion_type;
	  XMARKER (p->old_pointm)->insertion_type = window_point_insertion_type;

	  p->start = Fcopy_marker (w->start, Qnil);
	  p->start_at_line_beg = w->start_at_line_beg ? Qt : Qnil;
	}
      else
	{
	  p->pointm = Qnil;
	  p->old_pointm = Qnil;
	  p->start = Qnil;
	  p->start_at_line_beg = Qnil;
	}

      p->parent = NILP (w->parent) ? Qnil : XWINDOW (w->parent)->temslot;
      p->prev = NILP (w->prev) ? Qnil : XWINDOW (w->prev)->temslot;

      if (WINDOWP (w->contents))
	i = save_window_save (w->contents, vector, i);
    }

  return i;
}

DEFUN ("current-window-configuration", Fcurrent_window_configuration,
       Scurrent_window_configuration, 0, 1, 0,
       doc: /* Return an object representing the current window configuration of FRAME.
If FRAME is nil or omitted, use the selected frame.
This describes the number of windows, their sizes and current buffers,
and for each displayed buffer, where display starts, and the position of
point.  An exception is made for point in the current buffer:
its value is -not- saved.
This also records the currently selected frame, and FRAME's focus
redirection (see `redirect-frame-focus').  The variable
`window-persistent-parameters' specifies which window parameters are
saved by this function.  */)
  (Lisp_Object frame)
{
  struct frame *f = decode_live_frame (frame);
  ptrdiff_t n_windows = count_windows (XWINDOW (FRAME_ROOT_WINDOW (f)));
  struct save_window_data *data
    = ALLOCATE_PSEUDOVECTOR (struct save_window_data, saved_windows,
			     PVEC_WINDOW_CONFIGURATION);
  data->frame_cols = FRAME_COLS (f);
  data->frame_lines = FRAME_LINES (f);
  data->frame_menu_bar_lines = FRAME_MENU_BAR_LINES (f);
  data->frame_tab_bar_lines = FRAME_TAB_BAR_LINES (f);
  data->frame_tool_bar_lines = FRAME_TOOL_BAR_LINES (f);
  data->frame_text_width = FRAME_TEXT_WIDTH (f);
  data->frame_text_height = FRAME_TEXT_HEIGHT (f);
  data->frame_menu_bar_height = FRAME_MENU_BAR_HEIGHT (f);
  data->frame_tab_bar_height = FRAME_TAB_BAR_HEIGHT (f);
  data->frame_tool_bar_height = FRAME_TOOL_BAR_HEIGHT (f);
  data->selected_frame = selected_frame;
  data->current_window = FRAME_SELECTED_WINDOW (f);
  XSETBUFFER (data->f_current_buffer, current_buffer);
  data->minibuf_scroll_window = minibuf_level > 0 ? Vminibuf_scroll_window : Qnil;
  data->minibuf_selected_window = minibuf_level > 0 ? minibuf_selected_window : Qnil;
  data->root_window = FRAME_ROOT_WINDOW (f);
  data->focus_frame = FRAME_FOCUS_FRAME (f);
  Lisp_Object tem = initialize_vector (n_windows, Qnil);
  data->saved_windows = tem;
  for (ptrdiff_t i = 0; i < n_windows; i++)
    ASET (tem, i, initialize_vector (VECSIZE (struct saved_window), Qnil));
  save_window_save (FRAME_ROOT_WINDOW (f), XVECTOR (tem), 0);
  XSETWINDOW_CONFIGURATION (tem, data);
  return tem;
}

/* Called after W's margins, fringes or scroll bars was adjusted.  */

static void
apply_window_adjustment (struct window *w)
{
  eassert (w);
  clear_glyph_matrix (w->current_matrix);
  w->window_end_valid = false;
  wset_redisplay (w);
  adjust_frame_glyphs (XFRAME (WINDOW_FRAME (w)));
}


/***********************************************************************
			    Marginal Areas
 ***********************************************************************/

static int
extract_dimension (Lisp_Object dimension)
{
  if (NILP (dimension))
    return -1;
  return check_integer_range (dimension, 0, INT_MAX);
}

static struct window *
set_window_margins (struct window *w, Lisp_Object left_width,
		    Lisp_Object right_width)
{
  int unit = WINDOW_FRAME_COLUMN_WIDTH (w);
  int left = NILP (left_width) ? 0 : extract_dimension (left_width);
  int right = NILP (right_width) ? 0 : extract_dimension (right_width);

  if (w->left_margin_cols != left || w->right_margin_cols != right)
    {
      /* Don't change anything if new margins won't fit.  */
      if ((WINDOW_PIXEL_WIDTH (w)
	   - WINDOW_FRINGES_WIDTH (w)
	   - WINDOW_SCROLL_BAR_AREA_WIDTH (w)
	   - (left + right) * unit)
	  >= MIN_SAFE_WINDOW_PIXEL_WIDTH (w))
	{
	  w->left_margin_cols = left;
	  w->right_margin_cols = right;

	  return w;
	}
      else
	return NULL;
    }
  else
    return NULL;
}

DEFUN ("set-window-margins", Fset_window_margins, Sset_window_margins,
       2, 3, 0,
       doc: /* Set width of marginal areas of window WINDOW.
WINDOW must be a live window and defaults to the selected one.

Second arg LEFT-WIDTH specifies the number of character cells to
reserve for the left marginal area.  Optional third arg RIGHT-WIDTH
does the same for the right marginal area.  A nil width parameter
means no margin.

Leave margins unchanged if WINDOW is not large enough to accommodate
margins of the desired width.  Return t if any margin was actually
changed and nil otherwise.

The margins specified by calling this function may be later overridden
by invoking `set-window-buffer' for the same WINDOW, with its
KEEP-MARGINS argument nil or omitted.  */)
  (Lisp_Object window, Lisp_Object left_width, Lisp_Object right_width)
{
  struct window *w = set_window_margins (decode_live_window (window),
					 left_width, right_width);
  return w ? (apply_window_adjustment (w), Qt) : Qnil;
}


DEFUN ("window-margins", Fwindow_margins, Swindow_margins,
       0, 1, 0,
       doc: /* Get width of marginal areas of window WINDOW.
WINDOW must be a live window and defaults to the selected one.

Value is a cons of the form (LEFT-WIDTH . RIGHT-WIDTH).
If a marginal area does not exist, its width will be returned
as nil.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);
  return Fcons (w->left_margin_cols
		? make_fixnum (w->left_margin_cols) : Qnil,
		w->right_margin_cols
		? make_fixnum (w->right_margin_cols) : Qnil);
}



/***********************************************************************
			    Fringes
 ***********************************************************************/

static struct window *
set_window_fringes (struct window *w,
		    Lisp_Object left_width, Lisp_Object right_width,
		    Lisp_Object outside_margins, Lisp_Object persistent)
{
  /* Do nothing on a tty.  */
  if (!FRAME_WINDOW_P (WINDOW_XFRAME (w)))
    return NULL;
  else
    {
      struct frame *f = XFRAME (WINDOW_FRAME (w));
      int old_left = WINDOW_LEFT_FRINGE_WIDTH (w);
      int old_right = WINDOW_RIGHT_FRINGE_WIDTH (w);
      int new_left = extract_dimension (left_width);
      int new_right = extract_dimension (right_width);
      bool outside = !NILP (outside_margins);
      bool changed = false;
      bool failed = false;

      /* Check dimensions of new fringes.  Make changes only if they
	 fit the window's dimensions.  */
      if ((WINDOW_PIXEL_WIDTH (w)
	   - WINDOW_MARGINS_WIDTH (w)
	   - WINDOW_SCROLL_BAR_AREA_WIDTH (w)
	   - WINDOW_RIGHT_DIVIDER_WIDTH (w)
	   - (new_left == -1 ? FRAME_LEFT_FRINGE_WIDTH (f) : new_left)
	   - (new_right == -1 ? FRAME_RIGHT_FRINGE_WIDTH (f) : new_right))
	  >= MIN_SAFE_WINDOW_PIXEL_WIDTH (w))
	{
	  w->left_fringe_width = new_left;
	  w->right_fringe_width = new_right;
	  changed = new_left != old_left || new_right != old_right;
	}
      else
	failed = true;

      /* Placing fringes outside margins.  */
      if (outside != w->fringes_outside_margins)
	{
	  w->fringes_outside_margins = outside;
	  changed = true;
	}

      /* Make settings persistent unless we failed to apply some
	 changes.  */
      if (!failed)
	w->fringes_persistent = !NILP (persistent);

      /* This is needed to trigger immediate redisplay of the window
	 when its fringes are changed, because fringes are redrawn
	 only if update_window is called, so we must trigger that even
	 if the window's glyph matrices did not change at all.  */
      if (changed)
	{
	  windows_or_buffers_changed = 35;
	  return w;
	}
      else
	return NULL;
    }
}

DEFUN ("set-window-fringes", Fset_window_fringes, Sset_window_fringes,
       2, 5, 0,
       doc: /* Set fringes of specified WINDOW.
WINDOW must specify a live window and defaults to the selected one.

Second arg LEFT-WIDTH specifies the number of pixels to reserve for
the left fringe.  Optional third arg RIGHT-WIDTH specifies the right
fringe width.  If a fringe width arg is nil, that means to use the
frame's default fringe width.  Default fringe widths can be set with
the command `set-fringe-style'.

If optional fourth arg OUTSIDE-MARGINS is non-nil, draw the fringes
outside of the display margins.  By default, fringes are drawn between
display marginal areas and the text area.

Optional fifth argument PERSISTENT non-nil means that fringe settings
for WINDOW are persistent, i.e., remain unchanged when another buffer
is shown in WINDOW.  PERSISTENT nil means that fringes are reset from
buffer local values when `set-window-buffer' is called on WINDOW with
the argument KEEP-MARGINS nil.

Leave fringes unchanged if WINDOW is not large enough to accommodate
fringes of the desired width.  Return t if any fringe was actually
changed and nil otherwise.  */)
  (Lisp_Object window, Lisp_Object left_width, Lisp_Object right_width,
   Lisp_Object outside_margins, Lisp_Object persistent)
{
  struct window *w
    = set_window_fringes (decode_live_window (window), left_width,
			  right_width, outside_margins, persistent);
  return w ? (apply_window_adjustment (w), Qt) : Qnil;
}


DEFUN ("window-fringes", Fwindow_fringes, Swindow_fringes,
       0, 1, 0,
       doc: /* Return fringe settings for specified WINDOW.
WINDOW must be a live window and defaults to the selected one.

Value is a list of the form (LEFT-WIDTH RIGHT-WIDTH OUTSIDE-MARGINS
PERSISTENT), see `set-window-fringes'.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);

  return list4 (make_fixnum (WINDOW_LEFT_FRINGE_WIDTH (w)),
		make_fixnum (WINDOW_RIGHT_FRINGE_WIDTH (w)),
		WINDOW_HAS_FRINGES_OUTSIDE_MARGINS (w) ? Qt : Qnil,
		w->fringes_persistent ? Qt : Qnil);
}

DEFUN ("set-window-cursor-type", Fset_window_cursor_type,
       Sset_window_cursor_type, 2, 2, 0,
       doc: /* Set the `cursor-type' of WINDOW to TYPE.

This setting takes precedence over the variable `cursor-type', and TYPE
has the same format as the value of that variable.  The initial value
for new windows is t, which says to respect the buffer-local value of
`cursor-type'.

WINDOW nil means use the selected window.  This setting persists across
buffers shown in WINDOW, so `set-window-buffer' does not reset it.  */)
  (Lisp_Object window, Lisp_Object type)
{
  struct window *w = decode_live_window (window);

  if (!(NILP (type)
	|| EQ (type, Qt)
	|| EQ (type, Qbox)
	|| EQ (type, Qhollow)
	|| EQ (type, Qbar)
	|| EQ (type, Qhbar)
	|| (CONSP (type)
	    && (EQ (XCAR (type), Qbox)
		|| EQ (XCAR (type), Qbar)
		|| EQ (XCAR (type), Qhbar))
	    && INTEGERP (XCDR (type)))))
    error ("Invalid cursor type");

  wset_cursor_type (w, type);

  /* Redisplay with updated cursor type.  */
  wset_redisplay (w);

  return type;
}

/* FIXME: Add a way to get the _effective_ cursor type, possibly by
   extending this function with an additional optional argument.  */
DEFUN ("window-cursor-type", Fwindow_cursor_type, Swindow_cursor_type,
       0, 1, 0,
       doc: /* Return the `cursor-type' of WINDOW.
WINDOW must be a live window and defaults to the selected one.  */)
  (Lisp_Object window)
{
  return decode_live_window (window)->cursor_type;
}


/***********************************************************************
			    Scroll bars
 ***********************************************************************/

static struct window *
set_window_scroll_bars (struct window *w, Lisp_Object width,
			Lisp_Object vertical_type, Lisp_Object height,
			Lisp_Object horizontal_type, Lisp_Object persistent)
{
  /* Do nothing on a tty.  */
  if (!FRAME_WINDOW_P (WINDOW_XFRAME (w)))
    return NULL;
  else
    {
      struct frame *f = XFRAME (WINDOW_FRAME (w));
      int new_width = extract_dimension (width);
      bool changed = false;
      bool failed = false;

      if (new_width == 0)
	vertical_type = Qnil;
      else if (!(NILP (vertical_type)
		 || EQ (vertical_type, Qleft)
		 || EQ (vertical_type, Qright)
		 || EQ (vertical_type, Qt)))
	error ("Invalid type of vertical scroll bar");

      /* Check dimension of new scroll bar.  Make changes only if it
	 fit the window's dimensions.  */
      if ((WINDOW_PIXEL_WIDTH (w)
	   - WINDOW_MARGINS_WIDTH (w)
	   - WINDOW_FRINGES_WIDTH (w)
	   - WINDOW_RIGHT_DIVIDER_WIDTH (w)
	   - (new_width == -1 ? FRAME_SCROLL_BAR_AREA_WIDTH (f) : new_width))
	  >= MIN_SAFE_WINDOW_PIXEL_WIDTH (w))
	{
	  changed = (!EQ (vertical_type, w->vertical_scroll_bar_type)
		     || new_width != WINDOW_SCROLL_BAR_AREA_WIDTH (w));
	  wset_vertical_scroll_bar_type (w, vertical_type);
	  w->scroll_bar_width = new_width;
	}
      else
	failed = true;

#if USE_HORIZONTAL_SCROLL_BARS
      int new_height = extract_dimension (height);

      if ((MINI_WINDOW_P (w) && !EQ (horizontal_type, Qbottom))
	  || new_height == 0)
	horizontal_type = Qnil;

      if (!(NILP (horizontal_type)
	    || EQ (horizontal_type, Qbottom)
	    || EQ (horizontal_type, Qt)))
	error ("Invalid type of horizontal scroll bar");

      /* Don't change anything if new scroll bar won't fit.  */
      if ((WINDOW_PIXEL_HEIGHT (w)
	   - WINDOW_TAB_LINE_HEIGHT (w)
	   - WINDOW_HEADER_LINE_HEIGHT (w)
	   - WINDOW_MODE_LINE_HEIGHT (w)
	   - (new_height == -1 ? FRAME_SCROLL_BAR_AREA_HEIGHT (f) : new_height))
	  >= MIN_SAFE_WINDOW_PIXEL_HEIGHT (w))
	{
	  changed = (changed
		     || !EQ (horizontal_type, w->horizontal_scroll_bar_type)
		     || new_height != WINDOW_SCROLL_BAR_AREA_HEIGHT (w));
	  wset_horizontal_scroll_bar_type (w, horizontal_type);
	  w->scroll_bar_height = new_height;
	}
      else
	failed = true;
#else
      wset_horizontal_scroll_bar_type (w, Qnil);
#endif

      /* Make settings persistent unless we failed to apply some
	 changes.  */
      if (!failed)
	w->scroll_bars_persistent = !NILP (persistent);

      /* This is needed to trigger immediate redisplay of the window when
	 scroll bars are changed, because scroll bars are redisplayed only
	 if more than a single window needs to be considered, see
	 redisplay_internal.  */
      if (changed)
	wset_redisplay (w);

      return changed ? w : NULL;
    }
}

DEFUN ("set-window-scroll-bars", Fset_window_scroll_bars,
       Sset_window_scroll_bars, 1, 6, 0,
       doc: /* Set width and type of scroll bars of specified WINDOW.
WINDOW must specify a live window and defaults to the selected one.

Second argument WIDTH specifies the pixel width for the vertical scroll
bar.  If WIDTH is nil, use the scroll bar width of WINDOW's frame.
Third argument VERTICAL-TYPE specifies the type of the vertical scroll
bar: left, right, nil or t where nil means to not display a vertical
scroll bar on WINDOW and t means to use WINDOW frame's vertical scroll
bar type.

Fourth argument HEIGHT specifies the pixel height for the horizontal
scroll bar.  If HEIGHT is nil, use the scroll bar height of WINDOW's
frame.  Fifth argument HORIZONTAL-TYPE specifies the type of the
horizontal scroll bar: bottom, nil, or t where nil means to not
display a horizontal scroll bar on WINDOW and t means to use WINDOW
frame's horizontal scroll bar type.  If WINDOW is a mini window, t
effectively behaves like nil.  HORIZONTAL-TYPE must equal bottom in
order to show a scroll bar for mini windows.

Optional sixth argument PERSISTENT non-nil means that scroll bar
settings for WINDOW are persistent, i.e., remain unchanged when
another buffer is shown in WINDOW.  PERSISTENT nil means that scroll
bars are reset from buffer local values when `set-window-buffer' is
called on WINDOW with the argument KEEP-MARGINS nil.

If WINDOW is not large enough to accommodate a scroll bar of the
desired dimension, leave the corresponding scroll bar unchanged.
Return t if scroll bars were actually changed and nil otherwise.  */)
  (Lisp_Object window, Lisp_Object width, Lisp_Object vertical_type,
   Lisp_Object height, Lisp_Object horizontal_type, Lisp_Object persistent)
{
  struct window *w
    = set_window_scroll_bars (decode_live_window (window),
			      width, vertical_type, height,
			      horizontal_type, persistent);
  return w ? (apply_window_adjustment (w), Qt) : Qnil;
}


DEFUN ("window-scroll-bars", Fwindow_scroll_bars, Swindow_scroll_bars,
       0, 1, 0,
       doc: /* Get width and type of scroll bars of window WINDOW.
WINDOW must be a live window and defaults to the selected one.

Value is a list of the form (WIDTH COLUMNS VERTICAL-TYPE HEIGHT LINES
HORIZONTAL-TYPE PERSISTENT).  WIDTH reports the pixel width of the
vertical scroll bar; COLUMNS is the equivalent number of columns.
Similarly, HEIGHT and LINES are the height of the horizontal scroll
bar in pixels and the equivalent number of lines.  VERTICAL-TYPE
reports the type of the vertical scroll bar, either left, right, nil,
or t.  HORIZONTAL-TYPE reports the type of the horizontal scroll bar,
either bottom, nil or t.  PERSISTENT reports the value specified by
the last successful call to `set-window-scroll-bars', or nil if there
was none.

If WIDTH or HEIGHT is nil or VERTICAL-TYPE or HORIZONTAL-TYPE is t,
WINDOW is using the corresponding value specified for the frame.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);

  return Fcons (((w->scroll_bar_width >= 0)
		 ? make_fixnum (w->scroll_bar_width)
		 : Qnil),
		Fcons (make_fixnum (WINDOW_SCROLL_BAR_COLS (w)),
		       list5 (w->vertical_scroll_bar_type,
			      ((w->scroll_bar_height >= 0)
			       ? make_fixnum (w->scroll_bar_height)
			       : Qnil),
			      make_fixnum (WINDOW_SCROLL_BAR_LINES (w)),
			      w->horizontal_scroll_bar_type,
			      w->scroll_bars_persistent ? Qt : Qnil)));
}

/***********************************************************************
			   Smooth scrolling
 ***********************************************************************/

DEFUN ("window-vscroll", Fwindow_vscroll, Swindow_vscroll, 0, 2, 0,
       doc: /* Return the amount by which WINDOW is scrolled vertically.
This takes effect when displaying tall lines or images.

If WINDOW is omitted or nil, it defaults to the selected window.
Normally, value is a multiple of the canonical character height of WINDOW;
optional second arg PIXELS-P means value is measured in pixels.  */)
  (Lisp_Object window, Lisp_Object pixels_p)
{
  Lisp_Object result;
  struct window *w = decode_live_window (window);
  struct frame *f = XFRAME (w->frame);

  if (FRAME_WINDOW_P (f))
    result = (NILP (pixels_p)
	      ? FRAME_CANON_Y_FROM_PIXEL_Y (f, -w->vscroll)
	      : make_fixnum (-w->vscroll));
  else
    result = make_fixnum (0);
  return result;
}


DEFUN ("set-window-vscroll", Fset_window_vscroll, Sset_window_vscroll,
       2, 4, 0,
       doc: /* Set amount by which WINDOW should be scrolled vertically to VSCROLL.
This takes effect when displaying tall lines or images.

WINDOW nil means use the selected window.  Normally, VSCROLL is a
non-negative multiple of the canonical character height of WINDOW;
optional third arg PIXELS-P non-nil means that VSCROLL is in pixels.
If PIXELS-P is nil, VSCROLL may have to be rounded so that it
corresponds to an integral number of pixels.  The return value is the
result of this rounding.
If PIXELS-P is non-nil, the return value is VSCROLL.

PRESERVE-VSCROLL-P makes setting the start of WINDOW preserve the
vscroll if its start is "frozen" due to a resized mini-window.  */)
  (Lisp_Object window, Lisp_Object vscroll, Lisp_Object pixels_p,
   Lisp_Object preserve_vscroll_p)
{
  struct window *w = decode_live_window (window);
  struct frame *f = XFRAME (w->frame);

  CHECK_NUMBER (vscroll);

  if (FRAME_WINDOW_P (f))
    {
      int old_dy = w->vscroll;

      w->vscroll = - (NILP (pixels_p)
		      ? FRAME_LINE_HEIGHT (f) * XFLOATINT (vscroll)
		      : XFLOATINT (vscroll));
      w->vscroll = min (w->vscroll, 0);

      if (w->vscroll != old_dy)
	{
	  /* Adjust glyph matrix of the frame if the virtual display
	     area becomes larger than before.  */
	  if (w->vscroll < 0 && w->vscroll < old_dy)
	    adjust_frame_glyphs (f);

	  /* Prevent redisplay shortcuts.  */
	  XBUFFER (w->contents)->prevent_redisplay_optimizations_p = true;

	  /* Mark W for redisplay.  (bug#55299) */
	  wset_redisplay (w);
	}

      w->preserve_vscroll_p = !NILP (preserve_vscroll_p);
    }

  return Fwindow_vscroll (window, pixels_p);
}


/* Call FN for all leaf windows on frame F.  FN is called with the
   first argument being a pointer to the leaf window, and with
   additional argument USER_DATA.  Stops when FN returns 0.  */

static void
foreach_window (struct frame *f, bool (*fn) (struct window *, void *),
		void *user_data)
{
  /* delete_frame may set FRAME_ROOT_WINDOW (f) to Qnil.  */
  if (WINDOWP (FRAME_ROOT_WINDOW (f)))
    foreach_window_1 (XWINDOW (FRAME_ROOT_WINDOW (f)), fn, user_data);
}


/* Helper function for foreach_window.  Call FN for all leaf windows
   reachable from W.  FN is called with the first argument being a
   pointer to the leaf window, and with additional argument USER_DATA.
   Stop when FN returns false.  Value is false if stopped by FN.  */

static bool
foreach_window_1 (struct window *w, bool (*fn) (struct window *, void *),
		  void *user_data)
{
  bool cont;

  for (cont = true; w && cont;)
    {
      if (WINDOWP (w->contents))
 	cont = foreach_window_1 (XWINDOW (w->contents), fn, user_data);
      else
	cont = fn (w, user_data);

      w = NILP (w->next) ? 0 : XWINDOW (w->next);
    }

  return cont;
}

/***********************************************************************
			    Initialization
 ***********************************************************************/

/* Return true if window configurations CONFIGURATION1 and CONFIGURATION2
   describe the same state of affairs.  This is used by Fequal.

   Ignore non-matching scroll positions and the like.

   This ignores a couple of things like the dedication status of
   window, combination_limit and the like.  This might have to be
   fixed.  */

static bool
compare_window_configurations (Lisp_Object configuration1,
			       Lisp_Object configuration2)
{
  struct save_window_data *d1, *d2;
  struct Lisp_Vector *sws1, *sws2;
  ptrdiff_t i;

  CHECK_WINDOW_CONFIGURATION (configuration1);
  CHECK_WINDOW_CONFIGURATION (configuration2);

  d1 = (struct save_window_data *) XVECTOR (configuration1);
  d2 = (struct save_window_data *) XVECTOR (configuration2);
  sws1 = XVECTOR (d1->saved_windows);
  sws2 = XVECTOR (d2->saved_windows);

  /* Frame settings must match.  */
  if (d1->frame_cols != d2->frame_cols
      || d1->frame_lines != d2->frame_lines
      || d1->frame_menu_bar_lines != d2->frame_menu_bar_lines
      || !EQ (d1->selected_frame, d2->selected_frame)
      || !EQ (d1->f_current_buffer, d2->f_current_buffer)
      || !EQ (d1->focus_frame, d2->focus_frame)
      /* Verify that the two configurations have the same number of windows.  */
      || sws1->header.size != sws2->header.size)
    return false;

  for (i = 0; i < sws1->header.size; i++)
    {
      struct saved_window *sw1, *sw2;

      sw1 = SAVED_WINDOW_N (sws1, i);
      sw2 = SAVED_WINDOW_N (sws2, i);

      if (
	   /* The "current" windows in the two configurations must
	      correspond to each other.  */
	  EQ (d1->current_window, sw1->window)
	  != EQ (d2->current_window, sw2->window)
	  /* Windows' buffers must match.  */
	  || !EQ (sw1->buffer, sw2->buffer)
	  || !EQ (sw1->pixel_left, sw2->pixel_left)
	  || !EQ (sw1->pixel_top, sw2->pixel_top)
	  || !EQ (sw1->pixel_height, sw2->pixel_height)
	  || !EQ (sw1->pixel_width, sw2->pixel_width)
	  || !EQ (sw1->left_col, sw2->left_col)
	  || !EQ (sw1->top_line, sw2->top_line)
	  || !EQ (sw1->total_cols, sw2->total_cols)
	  || !EQ (sw1->total_lines, sw2->total_lines)
	  || !EQ (sw1->display_table, sw2->display_table)
	  /* The next two disjuncts check the window structure for
	     equality.  */
	  || !EQ (sw1->parent, sw2->parent)
	  || !EQ (sw1->prev, sw2->prev)
	  || !EQ (sw1->left_margin_cols, sw2->left_margin_cols)
	  || !EQ (sw1->right_margin_cols, sw2->right_margin_cols)
	  || !EQ (sw1->left_fringe_width, sw2->left_fringe_width)
	  || !EQ (sw1->right_fringe_width, sw2->right_fringe_width)
	  || !EQ (sw1->fringes_outside_margins, sw2->fringes_outside_margins)
	  || !EQ (sw1->fringes_persistent, sw2->fringes_persistent)
	  || !EQ (sw1->scroll_bar_width, sw2->scroll_bar_width)
	  || !EQ (sw1->scroll_bar_height, sw2->scroll_bar_height)
	  || !EQ (sw1->vertical_scroll_bar_type, sw2->vertical_scroll_bar_type)
	  || !EQ (sw1->horizontal_scroll_bar_type, sw2->horizontal_scroll_bar_type)
	  || !EQ (sw1->scroll_bars_persistent, sw2->scroll_bars_persistent))
	return false;
    }

  return true;
}

DEFUN ("window-configuration-equal-p", Fwindow_configuration_equal_p,
       Swindow_configuration_equal_p, 2, 2, 0,
       doc: /* Say whether two window configurations have the same window layout.
This function ignores details such as the values of point and
scrolling positions.  */)
  (Lisp_Object x, Lisp_Object y)
{
  if (compare_window_configurations (x, y))
    return Qt;
  return Qnil;
}


static void init_window_once_for_pdumper (void);

void
init_window_once (void)
{
  minibuf_window = Qnil;
  staticpro (&minibuf_window);

  selected_window = Qnil;
  staticpro (&selected_window);

  Vwindow_list = Qnil;
  staticpro (&Vwindow_list);

  minibuf_selected_window = Qnil;
  staticpro (&minibuf_selected_window);
  old_selected_window = Qnil;
  staticpro (&old_selected_window);

  pdumper_do_now_and_after_load (init_window_once_for_pdumper);
}

static void init_window_once_for_pdumper (void)
{
  window_scroll_pixel_based_preserve_x = -1;
  window_scroll_pixel_based_preserve_y = -1;
  window_scroll_preserve_hpos = -1;
  window_scroll_preserve_vpos = -1;
  PDUMPER_IGNORE (sequence_number);

  PDUMPER_RESET_LV (minibuf_window, Qnil);
  PDUMPER_RESET_LV (selected_window, Qnil);
  PDUMPER_RESET_LV (Vwindow_list, Qnil);
  PDUMPER_RESET_LV (minibuf_selected_window, Qnil);

  bool restore = mode_line_in_non_selected_windows;
  mode_line_in_non_selected_windows = false;
  struct frame *f = make_initial_frame ();
  mode_line_in_non_selected_windows = restore;
  XSETFRAME (selected_frame, f);
  old_selected_frame = Vterminal_frame = selected_frame;
  minibuf_window = f->minibuffer_window;
  old_selected_window = selected_window = f->selected_window;
}

void
init_window (void)
{
  Vwindow_list = Qnil;
}

void
syms_of_window (void)
{
  DEFSYM (Qscroll_up, "scroll-up");
  DEFSYM (Qscroll_down, "scroll-down");
  DEFSYM (Qscroll_command, "scroll-command");

  Fput (Qscroll_up, Qscroll_command, Qt);
  Fput (Qscroll_down, Qscroll_command, Qt);

  DEFSYM (Qwindow_configuration_change_hook, "window-configuration-change-hook");
  DEFSYM (Qwindow_state_change_hook, "window-state-change-hook");
  DEFSYM (Qwindow_state_change_functions, "window-state-change-functions");
  DEFSYM (Qwindow_size_change_functions, "window-size-change-functions");
  DEFSYM (Qwindow_buffer_change_functions, "window-buffer-change-functions");
  DEFSYM (Qwindow_selection_change_functions, "window-selection-change-functions");
  DEFSYM (Qwindowp, "windowp");
  DEFSYM (Qwindow_configuration_p, "window-configuration-p");
  DEFSYM (Qwindow_live_p, "window-live-p");
  DEFSYM (Qwindow_valid_p, "window-valid-p");
  DEFSYM (Qwindow_deletable_p, "window-deletable-p");
  DEFSYM (Qdelete_window, "delete-window");
  DEFSYM (Qwindow__resize_root_window, "window--resize-root-window");
  DEFSYM (Qwindow__resize_root_window_vertically,
	  "window--resize-root-window-vertically");
  DEFSYM (Qwindow__resize_mini_frame, "window--resize-mini-frame");
  DEFSYM (Qwindow__pixel_to_total, "window--pixel-to-total");
  DEFSYM (Qsafe, "safe");
  DEFSYM (Qdisplay_buffer, "display-buffer");
  DEFSYM (Qreplace_buffer_in_windows, "replace-buffer-in-windows");
  DEFSYM (Qrecord_window_buffer, "record-window-buffer");
  DEFSYM (Qget_mru_window, "get-mru-window");
  DEFSYM (Qwindow_size, "window-size");
  DEFSYM (Qtemp_buffer_show_hook, "temp-buffer-show-hook");
  DEFSYM (Qabove, "above");
  DEFSYM (Qclone_of, "clone-of");
  DEFSYM (Qfloor, "floor");
  DEFSYM (Qceiling, "ceiling");
  DEFSYM (Qmark_for_redisplay, "mark-for-redisplay");
  DEFSYM (Qmode_line_format, "mode-line-format");
  DEFSYM (Qheader_line_format, "header-line-format");
  DEFSYM (Qtab_line_format, "tab-line-format");
  DEFSYM (Qno_other_window, "no-other-window");
  DEFSYM (Qconfiguration, "configuration");
  DEFSYM (Qdelete, "delete");
  DEFSYM (Qdedicated, "dedicated");
  DEFSYM (Qquit_restore, "quit-restore");
  DEFSYM (Qquit_restore_prev, "quit-restore-prev");

  DEFVAR_LISP ("temp-buffer-show-function", Vtemp_buffer_show_function,
	       doc: /* Non-nil means call as function to display a help buffer.
The function is called with one argument, the buffer to be displayed.
Used by `with-output-to-temp-buffer'.
If this function is used, then it must do the entire job of showing
the buffer; `temp-buffer-show-hook' is not run unless this function runs it.  */);
  Vtemp_buffer_show_function = Qnil;

  DEFVAR_LISP ("minibuffer-scroll-window", Vminibuf_scroll_window,
	       doc: /* Non-nil means it is the window that C-M-v in minibuffer should scroll.  */);
  Vminibuf_scroll_window = Qnil;

  DEFVAR_BOOL ("mode-line-in-non-selected-windows", mode_line_in_non_selected_windows,
	       doc: /* Non-nil means to use `mode-line-inactive' face in non-selected windows.
If the minibuffer is active, the `minibuffer-scroll-window' mode line
is displayed in the `mode-line' face.  */);
  mode_line_in_non_selected_windows = true;

  DEFVAR_LISP ("other-window-scroll-buffer", Vother_window_scroll_buffer,
	       doc: /* If this is a live buffer, \\[scroll-other-window] should scroll its window.  */);
  Vother_window_scroll_buffer = Qnil;

  DEFVAR_LISP ("other-window-scroll-default", Vother_window_scroll_default,
	       doc: /* Function that provides the window to scroll by \\[scroll-other-window].
The function `other-window-for-scrolling' first tries to use
`minibuffer-scroll-window' and `other-window-scroll-buffer'.
But when both are nil, then by default it uses a neighboring window.
This variable is intended to get another default instead of `next-window'.  */);
  Vother_window_scroll_default = Qnil;

  DEFVAR_BOOL ("auto-window-vscroll", auto_window_vscroll_p,
	       doc: /* Non-nil means to automatically adjust `window-vscroll' to view tall lines.  */);
  auto_window_vscroll_p = true;

  DEFVAR_INT ("next-screen-context-lines", next_screen_context_lines,
	      doc: /* Number of lines of continuity when scrolling by screenfuls.  */);
  next_screen_context_lines = 2;

  DEFVAR_LISP ("scroll-preserve-screen-position",
	       Vscroll_preserve_screen_position,
	       doc: /* Controls if scroll commands move point to keep its screen position unchanged.

A value of nil means point does not keep its screen position except
at the scroll margin or window boundary respectively.

A value of t means point keeps its screen position if the scroll
command moved it vertically out of the window, e.g. when scrolling
by full screens.  If point is within `next-screen-context-lines' lines
from the edges of the window, point will typically not keep its screen
position when doing commands like `scroll-up-command'/`scroll-down-command'
and the like.

Any other value means point always keeps its screen position.
Scroll commands should have the `scroll-command' property
on their symbols to be controlled by this variable.  */);
  Vscroll_preserve_screen_position = Qnil;

  DEFVAR_LISP ("window-point-insertion-type", Vwindow_point_insertion_type,
	       doc: /* Insertion type of marker to use for `window-point'.
See `marker-insertion-type' for the meaning of the possible values.  */);
  Vwindow_point_insertion_type = Qnil;
  DEFSYM (Qwindow_point_insertion_type, "window-point-insertion-type");

  DEFVAR_LISP ("window-buffer-change-functions", Vwindow_buffer_change_functions,
	       doc: /* Functions called during redisplay when window buffers have changed.
The value should be a list of functions that take one argument.

Functions specified buffer-locally are called for each window showing
the corresponding buffer if and only if that window has been added or
changed its buffer since the last redisplay.  In this case the window
is passed as argument.

Functions specified by the default value are called for each frame if
at least one window on that frame has been added, deleted or changed
its buffer since the last redisplay.  In this case the frame is passed
as argument.  */);
  Vwindow_buffer_change_functions = Qnil;

  DEFVAR_LISP ("window-size-change-functions", Vwindow_size_change_functions,
	       doc: /* Functions called during redisplay when window sizes have changed.
The value should be a list of functions that take one argument.

Functions specified buffer-locally are called for each window showing
the corresponding buffer if and only if that window has been added or
changed its buffer or its total or body size since the last redisplay.
In this case the window is passed as argument.

Functions specified by the default value are called for each frame if
at least one window on that frame has been added or changed its buffer
or its total or body size since the last redisplay.  In this case the
frame is passed as argument.

For instance, to hide the title bar when the frame is maximized, you
can add `frame-hide-title-bar-when-maximized' to this variable.  */);
  Vwindow_size_change_functions = Qnil;

  DEFVAR_LISP ("window-selection-change-functions", Vwindow_selection_change_functions,
	       doc: /* Functions called during redisplay when the selected window has changed.
The value should be a list of functions that take one argument.

Functions specified buffer-locally are called for each window showing
the corresponding buffer if and only if that window has been selected
or deselected since the last redisplay.  In this case the window is
passed as argument.

Functions specified by the default value are called for each frame if
the frame's selected window has changed since the last redisplay.  In
this case the frame is passed as argument.  */);
  Vwindow_selection_change_functions = Qnil;

  DEFVAR_LISP ("window-state-change-functions", Vwindow_state_change_functions,
	       doc: /* Functions called during redisplay when the window state changed.
The value should be a list of functions that take one argument.

Functions specified buffer-locally are called for each window showing
the corresponding buffer if and only if that window has been added,
resized, changed its buffer or has been (de-)selected since the last
redisplay.  In this case the window is passed as argument.

Functions specified by the default value are called for each frame if
at least one window on that frame has been added, deleted, changed its
buffer or its total or body size or the frame has been (de-)selected,
its selected window has changed or the window state change flag has
been set for this frame since the last redisplay.  In this case the
frame is passed as argument.  */);
  Vwindow_state_change_functions = Qnil;

  DEFVAR_LISP ("window-state-change-hook", Vwindow_state_change_hook,
	       doc: /* Functions called during redisplay when the window state changed.
The value should be a list of functions that take no argument.

This hook is called during redisplay when at least one window has been
added, deleted, (de-)selected, changed its buffer or its total or body
size or the window state change flag has been set for at least one
frame.  This hook is called after all other window change functions
have been run and should be used only if a function should react to
changes that happened on at least two frames since last redisplay or
the function intends to change the window configuration.  */);
  Vwindow_state_change_hook = Qnil;

  DEFVAR_LISP ("window-configuration-change-hook", Vwindow_configuration_change_hook,
	       doc: /* Functions called during redisplay when window configuration has changed.
The value should be a list of functions that take no argument.

Functions specified buffer-locally are called for each window showing
the corresponding buffer if at least one window on that frame has been
added, deleted or changed its buffer or its total or body size since
the last redisplay.  Each call is performed with the window showing
the buffer temporarily selected.

Functions specified by the default value are called for each frame if
at least one window on that frame has been added, deleted or changed
its buffer or its total or body size since the last redisplay.  Each
call is performed with the frame temporarily selected.  */);
  Vwindow_configuration_change_hook = Qnil;

  DEFVAR_LISP ("window-restore-killed-buffer-windows",
	       window_restore_killed_buffer_windows,
	       doc: /* Control restoring windows whose buffer was killed.
This variable specifies how the functions `set-window-configuration' and
`window-state-put' shall handle a window whose buffer has been killed
since the corresponding configuration or state was made.  Any such
window may be live - in which case it shows some other buffer - or dead
at the time one of these functions is called.

As a rule, `set-window-configuration' leaves the window alone if it is
live while `window-state-put' deletes it.  The following values can be
used to override the default behavior for dead windows in the case of
`set-window-configuration' and for dead and live windows in the case of
`window-state-put'.

- t means to restore the window and show some other buffer in it.

- `delete' means to try to delete the window.

- `dedicated' means to try to delete the window if and only if it is
  dedicated to its buffer.

- nil, the default, means that `set-window-configuration' will try to
  delete the window if and only if it is dedicated to its buffer while
  `window-state-put' will unconditionally try to delete it.

- a function means to restore the window, show some other buffer in it
  and add an entry for that window to a list that will be later passed
  as argument to that function.

If a window cannot be deleted (typically, because it is the last window
on its frame), show another buffer in it.

If the value is a function, it should take three arguments.  The first
argument specifies the frame whose windows have been restored.  The
third argument is the symbol `configuration' if the windows are
restored by `set-window-configuration' and the symbol `state' if the
windows are restored by `window-state-put'.

The second argument specifies a list of entries for @emph{any} window
whose previous buffer has been encountered dead at the time
`set-window-configuration' or `window-state-put' tried to restore it in
that window (minibuffer windows are excluded).  This means that the
function specified by this variable may also delete windows encountered
live by `set-window-configuration'.

Each entry is a list of six values - the window whose buffer was found
dead, the dead buffer or its name, the positions of start and point of
the buffer in that window, the dedicated status of the window as
reported by `window-dedicated-p' and a boolean - t if the window was
live when `set-window-configuration' tried to restore it and nil
otherwise.  */);
  window_restore_killed_buffer_windows = Qnil;

  DEFVAR_LISP ("recenter-redisplay", Vrecenter_redisplay,
	       doc: /* Non-nil means `recenter' redraws entire frame.
If this option is non-nil, then the `recenter' command with a nil
argument will redraw the entire frame; the special value `tty' causes
the frame to be redrawn only if it is a tty frame.  */);
  Vrecenter_redisplay = Qtty;

  DEFVAR_LISP ("window-combination-resize", Vwindow_combination_resize,
	       doc: /* If t, resize window combinations proportionally.
If this variable is nil, splitting a window gets the entire screen space
for displaying the new window from the window to split.  Deleting and
resizing a window preferably resizes one adjacent window only.

If this variable is t, splitting a window tries to get the space
proportionally from all windows in the same combination.  This also
allows splitting a window that is otherwise too small or of fixed size.
Resizing and deleting a window proportionally resize all windows in the
same combination.

Other values are reserved for future use.

A specific split operation may ignore the value of this variable if it
is affected by a non-nil value of `window-combination-limit'.  */);
  Vwindow_combination_resize = Qnil;

  DEFVAR_LISP ("window-combination-limit", Vwindow_combination_limit,
	       doc: /* If non-nil, splitting a window makes a new parent window.
The following values are recognized:

nil means splitting a window will create a new parent window only if the
    window has no parent window or the window shall become part of a
    combination orthogonal to the one it is part of.

`window-size' means that splitting a window for displaying a buffer
    makes a new parent window provided `display-buffer' is supposed to
    explicitly set the window's size due to the presence of a
    `window-height' or `window-width' entry in the alist used by
    `display-buffer'.  Otherwise, this value is handled like nil.

`temp-buffer-resize' means that splitting a window for displaying a
    temporary buffer via `with-temp-buffer-window' makes a new parent
    window only if `temp-buffer-resize-mode' is enabled.  Otherwise,
    this value is handled like nil.

`temp-buffer' means that splitting a window for displaying a temporary
    buffer via `with-temp-buffer-window' always makes a new parent
    window.  Otherwise, this value is handled like nil.

`display-buffer' means that splitting a window for displaying a buffer
    always makes a new parent window.  Since temporary buffers are
    displayed by the function `display-buffer', this value is stronger
    than `temp-buffer'.  Splitting a window for other purpose makes a
    new parent window only if needed.

t means that splitting a window always creates a new parent window.  If
    all splits behave this way, each frame's window tree is a binary
    tree and every window but the frame's root window has exactly one
    sibling.

The default value is `window-size'.  Other values are reserved for
future use.  */);
  Vwindow_combination_limit = Qwindow_size;

  DEFVAR_LISP ("window-persistent-parameters", Vwindow_persistent_parameters,
	       doc: /* Alist of persistent window parameters.
This alist specifies which window parameters shall get saved by
`current-window-configuration' and `window-state-get' and subsequently
restored to their previous values by `set-window-configuration' and
`window-state-put'.

The car of each entry of this alist is the symbol specifying the
parameter.  The cdr is one of the following:

nil means the parameter is neither saved by `window-state-get' nor by
`current-window-configuration'.

t means the parameter is saved by `current-window-configuration' and,
provided its WRITABLE argument is nil, by `window-state-get'.

The symbol `writable' means the parameter is saved unconditionally by
both `current-window-configuration' and `window-state-get'.  Do not use
this value for parameters without read syntax (like windows or frames).

Parameters not saved by `current-window-configuration' or
`window-state-get' are left alone by `set-window-configuration'
respectively are not installed by `window-state-put'.  */);
  Vwindow_persistent_parameters = list1 (Fcons (Qclone_of, Qt));

  DEFVAR_BOOL ("window-resize-pixelwise", window_resize_pixelwise,
	       doc: /*  Non-nil means resize windows pixelwise.
This currently affects the functions: `split-window', `maximize-window',
`minimize-window', `fit-window-to-buffer' and `fit-frame-to-buffer', and
all functions that symmetrically resize a parent window.

Note that when a frame's pixel size is not a multiple of the
frame's character size, at least one window may get resized
pixelwise even if this option is nil.  */);
  window_resize_pixelwise = false;

  DEFVAR_BOOL ("fast-but-imprecise-scrolling",
               fast_but_imprecise_scrolling,
               doc: /* When non-nil, accelerate scrolling operations.
This comes into play when scrolling rapidly over previously
unfontified buffer regions.  Only those portions of the buffer which
are actually going to be displayed get fontified.

Note that this optimization can cause the portion of the buffer
displayed after a scrolling operation to be somewhat inaccurate.  */);
  fast_but_imprecise_scrolling = false;

  DEFVAR_LISP ("window-dead-windows-table", window_dead_windows_table,
    doc: /* Hash table of dead windows.
Each entry in this table maps a window number to a window object.
Entries are added by `delete-window-internal' and are removed by the
garbage collector.

This table is maintained by code in window.c and is made visible in
Elisp for testing purposes only.  */);
  window_dead_windows_table
    = CALLN (Fmake_hash_table, QCweakness, Qt);

  defsubr (&Sselected_window);
  defsubr (&Sold_selected_window);
  defsubr (&Sminibuffer_window);
  defsubr (&Swindow_minibuffer_p);
  defsubr (&Swindowp);
  defsubr (&Swindow_valid_p);
  defsubr (&Swindow_live_p);
  defsubr (&Swindow_frame);
  defsubr (&Sframe_root_window);
  defsubr (&Sframe_first_window);
  defsubr (&Sframe_selected_window);
  defsubr (&Sframe_old_selected_window);
  defsubr (&Sset_frame_selected_window);
  defsubr (&Spos_visible_in_window_p);
  defsubr (&Swindow_line_height);
  defsubr (&Swindow_buffer);
  defsubr (&Swindow_old_buffer);
  defsubr (&Swindow_parent);
  defsubr (&Swindow_top_child);
  defsubr (&Swindow_left_child);
  defsubr (&Swindow_next_sibling);
  defsubr (&Swindow_prev_sibling);
  defsubr (&Swindow_combination_limit);
  defsubr (&Sset_window_combination_limit);
  defsubr (&Swindow_use_time);
  defsubr (&Swindow_pixel_width);
  defsubr (&Swindow_pixel_height);
  defsubr (&Swindow_old_pixel_width);
  defsubr (&Swindow_old_pixel_height);
  defsubr (&Swindow_old_body_pixel_width);
  defsubr (&Swindow_old_body_pixel_height);
  defsubr (&Swindow_total_width);
  defsubr (&Swindow_total_height);
  defsubr (&Swindow_normal_size);
  defsubr (&Swindow_new_pixel);
  defsubr (&Swindow_new_total);
  defsubr (&Swindow_new_normal);
  defsubr (&Swindow_pixel_left);
  defsubr (&Swindow_pixel_top);
  defsubr (&Swindow_left_column);
  defsubr (&Swindow_top_line);
  defsubr (&Sset_window_new_pixel);
  defsubr (&Sset_window_new_total);
  defsubr (&Sset_window_new_normal);
  defsubr (&Swindow_resize_apply);
  defsubr (&Swindow_resize_apply_total);
  defsubr (&Swindow_body_height);
  defsubr (&Swindow_body_width);
  defsubr (&Swindow_hscroll);
  defsubr (&Sset_window_hscroll);
  defsubr (&Swindow_mode_line_height);
  defsubr (&Swindow_header_line_height);
  defsubr (&Swindow_tab_line_height);
  defsubr (&Swindow_right_divider_width);
  defsubr (&Swindow_bottom_divider_width);
  defsubr (&Swindow_scroll_bar_width);
  defsubr (&Swindow_scroll_bar_height);
  defsubr (&Scoordinates_in_window_p);
  defsubr (&Swindow_at);
  defsubr (&Swindow_point);
  defsubr (&Swindow_old_point);
  defsubr (&Swindow_start);
  defsubr (&Swindow_end);
  defsubr (&Sset_window_point);
  defsubr (&Sset_window_start);
  defsubr (&Swindow_dedicated_p);
  defsubr (&Swindow_lines_pixel_dimensions);
  defsubr (&Sset_window_dedicated_p);
  defsubr (&Swindow_display_table);
  defsubr (&Sset_window_display_table);
  defsubr (&Snext_window);
  defsubr (&Sprevious_window);
  defsubr (&Sget_buffer_window);
  defsubr (&Sdelete_other_windows_internal);
  defsubr (&Sdelete_window_internal);
  defsubr (&Sresize_mini_window_internal);
  defsubr (&Sset_window_buffer);
  defsubr (&Srun_window_configuration_change_hook);
  defsubr (&Srun_window_scroll_functions);
  defsubr (&Sselect_window);
  defsubr (&Sforce_window_update);
  defsubr (&Ssplit_window_internal);
  defsubr (&Sscroll_up);
  defsubr (&Sscroll_down);
  defsubr (&Sscroll_left);
  defsubr (&Sscroll_right);
  defsubr (&Sother_window_for_scrolling);
  defsubr (&Sminibuffer_selected_window);
  defsubr (&Srecenter);
  defsubr (&Swindow_text_width);
  defsubr (&Swindow_text_height);
  defsubr (&Smove_to_window_line);
  defsubr (&Swindow_configuration_p);
  defsubr (&Swindow_configuration_frame);
  defsubr (&Sset_window_configuration);
  defsubr (&Scurrent_window_configuration);
  defsubr (&Sset_window_margins);
  defsubr (&Swindow_margins);
  defsubr (&Sset_window_fringes);
  defsubr (&Swindow_fringes);
  defsubr (&Sset_window_scroll_bars);
  defsubr (&Swindow_scroll_bars);
  defsubr (&Swindow_vscroll);
  defsubr (&Sset_window_vscroll);
  defsubr (&Swindow_configuration_equal_p);
  defsubr (&Swindow_bump_use_time);
  defsubr (&Swindow_list);
  defsubr (&Swindow_list_1);
  defsubr (&Swindow_prev_buffers);
  defsubr (&Sset_window_prev_buffers);
  defsubr (&Swindow_next_buffers);
  defsubr (&Sset_window_next_buffers);
  defsubr (&Swindow_parameters);
  defsubr (&Swindow_parameter);
  defsubr (&Sset_window_parameter);
  defsubr (&Swindow_discard_buffer);
  defsubr (&Swindow_cursor_type);
  defsubr (&Sset_window_cursor_type);
}
