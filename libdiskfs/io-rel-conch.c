/* 
   Copyright (C) 1994 Free Software Foundation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include "priv.h"
#include "io_S.h"

/* Implement io_release_conch as described in <hurd/io.defs>. */
error_t
S_io_release_conch (struct protid *cred)
{
  struct node *np;
  int error = 0;
  
  if (!cred)
    return EOPNOTSUPP;
  
  np = cred->po->np;
  mutex_lock (&np->lock);
  if (!cred->mapped)
    {
      mutex_unlock (&np->lock);
      return EINVAL;
    }
  
  np = cred->po->np;
  
  ioserver_handle_io_release_conch (&ip->i_conch, cred);
  
  mutex_unlock (&ip->i_toplock);
  return 0;
}
