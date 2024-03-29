/*  xkbtimer.c -- Manage XKB timers.

    Copyright (C) 2003, 2004  Marco Gerards
   
    Written by Marco Gerards <marco@student.han.nl>
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
  
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.  */

/* XKB requires a timer for key repeat and accessibility. Carefully
   stack all compatibility features.  */

#include <mach.h>
#include <errno.h>
#include <assert-backtrace.h>

#include "xkb.h"
#include <timer.h>

/* For key repeat.  */
static int key_delay = 0;
static int key_repeat = 0;

/* SlowKeys.  */
static int slowkeys_delay = 50;
static int slowkeys_active = 0;

/* BounceKeys.  */
static int bouncekeys_delay = 100;
static int bouncekeys_active = 0;

/* The current status of the timer.  */
enum timer_status
  {
    timer_stopped,
    timer_slowkeys,
    timer_repeat_delay,
    timer_repeating
  };

/* Timer used to time key controls.  */
static struct per_key_timer
{
  /* Used for slowkeys and repeat.  */
  struct timer_list enable_timer;
  
  /* Status of the enable timer.  */
  enum timer_status enable_status;
  
  /* Used for bouncekeys.  */
  struct timer_list disable_timer;

  /* The key was disabled for bouncekeys.  */
  unsigned short disabled;
} per_key_timers[255];

/* The last pressed key. Only this key may generate keyrepeat events.  */
static int lastkey = 0;

error_t
xkb_init_repeat (int delay, int repeat)
{
  error_t err;
  err = timer_init ();
  if (err)
    return err;

  key_delay = delay;
  key_repeat = repeat;
  
  return 0;
}

static int
key_enable (void *handle)
{
  int key = (int) handle;
  
  /* Enable the key.  */
  per_key_timers[key].disabled = 0;
  
  return 0;
}

error_t
xkb_generate_event (keycode_t kc)
{
  static keycode_t prevkc = 0;

  keycode_t keycode = kc & 127;
  process_keypress_event (keycode);
  prevkc = keycode;
  return 0;
}

/* Called by key timer. The global variable timer_status determines
   the current control.  */
static int
key_timing (void *handle)
{
  int current_key = (int) handle;

  xkb_generate_event (current_key);

  /* Another key was pressed after this key, stop repeating.  */
  if (lastkey != current_key)
    {
      per_key_timers[current_key].enable_status = timer_stopped;
      return 0;
    }

  switch (per_key_timers[current_key].enable_status)
    {
    case timer_stopped:
      assert_backtrace ("Stopped timer triggered timer event\n");
      break;
    case timer_slowkeys:
      per_key_timers[current_key].enable_timer.expires
	= fetch_jiffies () + key_delay;
      lastkey = current_key;
      
      per_key_timers[current_key].enable_status = timer_repeat_delay;
      break;
    case timer_repeat_delay:
      per_key_timers[current_key].enable_status = timer_repeating;
      /* Fall through.  */
    case timer_repeating:
      per_key_timers[current_key].enable_timer.expires 
	= fetch_jiffies () + key_repeat;
      break;
    }
  return 1;
}

void
xkb_key_released(keycode_t keycode, keycode_t keyc)
{
  /* Stop the timer for this released key.  */
  if (per_key_timers[keyc].enable_status != timer_stopped)
    {
      timer_remove (&per_key_timers[keyc].enable_timer);
      per_key_timers[keyc].enable_status = timer_stopped;
    }

  /* No more last key; it was released.  */
  if (keyc == lastkey)
    lastkey = 0;

  /* If bouncekeys is active, disable the key.  */
  if (bouncekeys_active)
    {
      per_key_timers[keyc].disabled = 1;

      /* Setup a timer to enable the key.  */
      timer_clear (&per_key_timers[keyc].disable_timer);
      per_key_timers[keyc].disable_timer.fnc = key_enable;
      per_key_timers[keyc].disable_timer.fnc_data = (void *) keyc;
      per_key_timers[keyc].disable_timer.expires
        = fetch_jiffies () + bouncekeys_delay;
      timer_add (&per_key_timers[keyc].disable_timer);
    }
}

void
xkb_key_pressed(keycode_t keyc)
{
  /* Setup the timer for slowkeys.  */
  timer_clear (&per_key_timers[keyc].enable_timer);
  lastkey = keyc;
  per_key_timers[keyc].enable_timer.fnc = key_timing;
  per_key_timers[keyc].enable_timer.fnc_data = (void *) keyc;

  if (slowkeys_active)
    {
      per_key_timers[keyc].enable_status = timer_slowkeys;
      per_key_timers[keyc].enable_timer.expires
        = fetch_jiffies () + slowkeys_delay;
    }
  else
    {
      per_key_timers[keyc].enable_status = timer_repeat_delay;
      per_key_timers[keyc].enable_timer.expires
        = fetch_jiffies () + key_delay;
    }
  timer_add (&per_key_timers[keyc].enable_timer);
}

void
xkb_timer_notify_input (keypress_t key)
{
  int keyc = key.keycode & 127;

  /* Filter out any double or disabled keys.  */
  if ((!key.rel && key.keycode == lastkey) || per_key_timers[keyc].disabled)
    return;

  /* Always handle key release events.  */
  if (key.rel)
    xkb_key_released(key.keycode, keyc);
  else
    xkb_key_pressed(keyc);
}
