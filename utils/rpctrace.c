/* Trace RPCs sent to selected ports

   Copyright (C) 1998,1999,2001 Free Software Foundation, Inc.
   Written by Jose M. Moya <josem@gnu.org>

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA. */

#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/ihash.h>
#include <mach/message.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <argp.h>
#include <error.h>
#include <string.h>
#include <version.h>
#include <sys/wait.h>
#include <inttypes.h>

const char *argp_program_version = STANDARD_HURD_VERSION (rpctrace);

static const struct argp_option options[] = {
  {"output", 'o', "FILE", 0, "Send trace output to FILE instead of stderr."},
  {0}
};

static const char *args_doc = "COMMAND [ARG...]";
static const char *doc =
"Trace Mach Remote Procedure Calls."
"\v.";

/* We keep one of these structures for each port right we are tracing.  */
struct traced_info
{
  struct port_info pi;

  mach_port_t forward;		/* real port */
  mach_msg_type_name_t type;

  char *name;			/* null or a string describing this */

  union
  {
    struct traced_info *nextfree; /* Link when on free list.  */

    struct			/* For a send right wrapper.  */
    {
      void **locp;		/* position in the traced_names hash table */
    } send;

    struct			/* For a send-once right wrapper.  */
    {
      /* We keep track of the send right to which the message containing
	 this send-once right as its reply port was sent, and the msgid of
	 that request.  We don't hold a reference to the send right; it is
	 just a hint to indicate a match with a send right on which we just
	 forwarded a message.  */
      mach_port_t sent_to;
      mach_msg_id_t sent_msgid;
    } send_once;
  } u;
};
#define INFO_SEND_ONCE(info) ((info)->type == MACH_MSG_TYPE_MOVE_SEND_ONCE)

static struct traced_info *freelist;

ihash_t traced_names;
struct port_class *traced_class;
struct port_bucket *traced_bucket;
FILE *ostream;

/* These are the calls made from the tracing engine into
   the output formatting code.  */

/* Called for a message that does not look like an RPC reply.
   The header has already been swapped into the sender's view
   with interposed ports.  */
static void print_request_header (struct traced_info *info,
				  mach_msg_header_t *header);

/* Called for a message that looks like an RPC reply.  */
static void print_reply_header (struct traced_info *info,
				mig_reply_header_t *header);

/* Called for each data item (which might be an array).
   Always called after one of the above two.  */
static void print_data (mach_msg_type_name_t type,
			const void *data,
			mach_msg_type_number_t nelt,
			mach_msg_type_number_t eltsize);

/*** Mechanics of tracing messages and interposing on ports ***/


/* Create a new wrapper port and do `ports_get_right' on it.  */
static struct traced_info *
new_send_wrapper (mach_port_t right, mach_port_t *wrapper_right)
{
  error_t err;
  struct traced_info *info;

  /* Use a free send-once wrapper port if we have one.  */
  if (freelist)
    {
      info = freelist;
      freelist = info->u.nextfree;
    }
  else
    {
      /* Create a new wrapper port that forwards to *RIGHT.  */
      err = ports_create_port (traced_class, traced_bucket,
			       sizeof *info, &info);
      assert_perror (err);
      info->name = 0;
    }

  info->forward = right;
  info->type = MACH_MSG_TYPE_MOVE_SEND;

  /* Store it in the reverse-lookup hash table, so we can
     look up this same right again to find the wrapper port.
     The entry in the hash table holds a weak ref on INFO.  */
  err = ihash_add (traced_names, info->forward, info, &info->u.send.locp);
  assert_perror (err);
  ports_port_ref_weak (info);
  assert (info->u.send.locp != 0);

  *wrapper_right = ports_get_right (info);
  ports_port_deref (info);

  return info;
}

/* Create a new wrapper port and do `ports_get_right' on it.  */
static struct traced_info *
new_send_once_wrapper (mach_port_t right, mach_port_t *wrapper_right)
{
  error_t err;
  struct traced_info *info;

  /* Use a free send-once wrapper port if we have one.  */
  if (freelist)
    {
      info = freelist;
      freelist = info->u.nextfree;
    }
  else
    {
      /* Create a new wrapper port that forwards to *RIGHT.  */
      err = ports_create_port (traced_class, traced_bucket,
			       sizeof *info, &info);
      assert_perror (err);
      info->name = 0;
    }

  info->forward = right;
  info->type = MACH_MSG_TYPE_MOVE_SEND_ONCE;

  /* Send-once rights never compare equal to any other right (even
     another send-once right), so there is no point in putting them
     in the reverse-lookup table.

     Since we never make send rights to this port, we don't want to
     use the normal libports mechanisms (ports_get_right) that are
     designed for send rights and no-senders notifications.
     Instead, we hold on to the initial hard ref to INFO until we
     receive a message on it.  The kernel automatically sends a
     MACH_NOTIFY_SEND_ONCE message if the send-once right dies.  */

  *wrapper_right = info->pi.port_right;
  memset (&info->u.send_once, 0, sizeof info->u.send_once);

  return info;
}


/* This gets called when a wrapper port has no hard refs (send rights),
   only weak refs.  The only weak ref is the one held in the reverse-lookup
   hash table.  */
static void
traced_dropweak (void *pi)
{
  struct traced_info *const info = pi;

  assert (info->type == MACH_MSG_TYPE_MOVE_SEND);
  assert (info->u.send.locp);

  /* Remove INFO from the hash table.  */
  ihash_locp_remove (traced_names, info->u.send.locp);
  ports_port_deref_weak (info);

  /* Deallocate the forward port, so the real port also sees no-senders.  */
  mach_port_deallocate (mach_task_self (), info->forward);

  /* There are no rights to this port, so we can reuse it.
     Add a hard ref and put INFO on the free list.  */
  ports_port_ref (info);

  free (info->name);
  info->name = 0;

  info->u.nextfree = freelist;
  freelist = info;
}


/* Rewrite a port right in a message with an appropriate wrapper port.  */
static struct traced_info *
rewrite_right (mach_port_t *right, mach_msg_type_name_t *type)
{
  error_t err;
  struct traced_info *info;

  /* We can never do anything special with a null or dead port right.  */
  if (!MACH_PORT_VALID (*right))
    return 0;

  switch (*type)
    {
    case MACH_MSG_TYPE_PORT_SEND:
      /* See if we are already tracing this port.  */
      info = ihash_find (traced_names, *right);
      if (info)
	{
	  /* We are already tracing this port.  We will pass on a right
	     to our existing wrapper port.  */
	  *right = ports_get_right (info);
	  *type = MACH_MSG_TYPE_MAKE_SEND;
	  return info;
	}

      /* See if this is already one of our own wrapper ports.  */
      info = ports_lookup_port (traced_bucket, *right, 0);
      if (info)
	{
	  /* This is a send right to one of our own wrapper ports.
	     Instead, send along the original send right.  */
	  mach_port_deallocate (mach_task_self (), *right); /* eat msg ref */
	  *right = info->forward;
	  err = mach_port_mod_refs (mach_task_self (), *right,
				    MACH_PORT_RIGHT_SEND, +1);
	  assert_perror (err);
	  ports_port_deref (info);
	  return info;
	}

      /* We have never seen this port before.  Create a new wrapper port
	 and replace the right in the message with a right to it.  */
      *type = MACH_MSG_TYPE_MAKE_SEND;
      return new_send_wrapper (*right, right);

    case MACH_MSG_TYPE_PORT_SEND_ONCE:
      /* There is no way to know if this send-once right is to the same
	 receive right as any other send-once or send right we have seen.
	 Fortunately, it doesn't matter, since the recipient of the
	 send-once right we pass along can't tell either.  We always just
	 make a new send-once wrapper object, that will trace the one
	 message it receives, and then die.  */
      *type = MACH_MSG_TYPE_MAKE_SEND_ONCE;
      return new_send_once_wrapper (*right, right);

    case MACH_MSG_TYPE_PORT_RECEIVE:
      /* We have got a receive right, call it A.  We will pass along a
	 different receive right of our own, call it B.  We ourselves will
	 receive messages on A, trace them, and forward them on to B.

	 If A is the receive right to a send right that we have wrapped,
	 then B must be that wrapper receive right, moved from us to the
	 intended receiver of A--that way it matches previous send rights
	 to A that were sent through and replaced with our wrapper (B).
	 If not, we create a new receive right.  */
      {
	mach_port_t rr;		/* B */
	char *name;

	info = ihash_find (traced_names, *right);
	if (info)
	  {
	    /* This is a receive right that we have been tracing sends to.  */
	    name = info->name;
	    rr = ports_claim_right (info);
	    /* That released the refs on INFO, so it's been freed now.  */
	  }
	else
	  {
	    /* This is a port we know nothing about.  */
	    rr = mach_reply_port ();
	    name = 0;
	  }

	/* Create a new wrapper object that receives on this port.  */
	err = ports_import_port (traced_class, traced_bucket,
				 *right, sizeof *info, &info);
	assert_perror (err);
	info->name = name;
	info->type = MACH_MSG_TYPE_MOVE_SEND; /* XXX ? */

	/* Get us a send right that we will forward on.  */
	err = mach_port_insert_right (mach_task_self (), rr, rr,
				      MACH_MSG_TYPE_MAKE_SEND);
	assert_perror (err);
	info->forward = rr;

	err = ihash_add (traced_names, info->forward, info,
			 &info->u.send.locp);
	assert_perror (err);
	ports_port_ref_weak (info);

	/* If there are no extant send rights to this port, then INFO will
	   die right here and release its send right to RR.
	   XXX what to do?
	*/
	ports_port_deref (info);

	*right = rr;
	return info;
      }

    default:
      assert (!"??? bogus port type from kernel!");
    }
  return 0;
}

static void
print_contents (mach_msg_header_t *inp,
		void *msg_buf_ptr)
{
  error_t err;

  int first = 1;

  /* Process the message data, wrapping ports and printing data.  */
  while (msg_buf_ptr < (void *) inp + inp->msgh_size)
    {
      mach_msg_type_t *const type = msg_buf_ptr;
      mach_msg_type_long_t *const lt = (void *) type;
      void *data;
      mach_msg_type_number_t nelt; /* Number of data items.  */
      mach_msg_type_size_t eltsize; /* Bytes per item.  */
      mach_msg_type_name_t name; /* MACH_MSG_TYPE_* code */

      if (!type->msgt_longform)
	{
	  name = type->msgt_name;
	  nelt = type->msgt_number;
	  eltsize = type->msgt_size / 8;
	  data = msg_buf_ptr = type + 1;
	}
      else
	{
	  name = lt->msgtl_name;
	  nelt = lt->msgtl_number;
	  eltsize = lt->msgtl_size / 8;
	  data = msg_buf_ptr = lt + 1;
	}

      if (!type->msgt_inline)
	{
	  /* This datum is out-of-line, meaning the message actually
	     contains a pointer to a vm_allocate'd region of data.  */
	  data = *(void **) data;
	  msg_buf_ptr += sizeof (void *);
	}
      else
	msg_buf_ptr += ((nelt * eltsize + sizeof(natural_t) - 1)
			& ~(sizeof(natural_t) - 1));

      if (first)
	first = 0;
      else
	putc (' ', ostream);

      /* Note that MACH_MSG_TYPE_PORT_NAME does not indicate a port right.
	 It indicates a port name, i.e. just an integer--and we don't know
	 what task that port name is meaningful in.  If it's meaningful in
	 a traced task, then it refers to our intercepting port rather than
	 the original port anyway.  */
      if (MACH_MSG_TYPE_PORT_ANY_RIGHT (name))
	{
	  /* These are port rights.  Translate them into wrappers.  */
	  mach_port_t *const portnames = data;
	  mach_msg_type_number_t i;
	  mach_msg_type_name_t newtypes[nelt];
	  int poly;
	  struct traced_info *ti;

	  assert (inp->msgh_bits & MACH_MSGH_BITS_COMPLEX);
	  assert (eltsize == sizeof (mach_port_t));

	  poly = 0;
	  for (i = 0; i < nelt; ++i)
	    {
	      newtypes[i] = name;

	      if (inp->msgh_id == 3215) /* mach_port_insert_right */
		{
		  /* XXX
		   */
		  fprintf (ostream,
			   "\t\t[%d] = pass through port %d, type %d\n",
			   i, portnames[i], name);
		  continue;
		}

	      ti = rewrite_right (&portnames[i], &newtypes[i]);

	      putc ((i == 0 && nelt > 1) ? '{' : ' ', ostream);

	      if (portnames[i] == MACH_PORT_NULL)
		fprintf (ostream, "(null)");
	      else if (portnames[i] == MACH_PORT_DEAD)
		fprintf (ostream, "(dead)");
	      else
		{
		  assert (ti);
		  if (ti->name != 0)
		    fprintf (ostream, "%s", ti->name);
		  else
		    fprintf (ostream, "%3u", (unsigned int) portnames[i]);
		}
	      if (i > 0 && newtypes[i] != newtypes[0])
		poly = 1;
	    }
	  if (nelt > 1)
	    putc ('}', ostream);

	  if (poly)
	    {
	      if (name == MACH_MSG_TYPE_MOVE_SEND_ONCE)
		{
		  /* Some of the new rights are MAKE_SEND_ONCE.
		     Turn them all into MOVE_SEND_ONCE.  */
		  for (i = 0; i < nelt; ++i)
		    if (newtypes[i] == MACH_MSG_TYPE_MAKE_SEND_ONCE)
		      {
			err = mach_port_insert_right (mach_task_self (),
						      portnames[i],
						      portnames[i],
						      newtypes[i]);
			assert_perror (err);
		      }
		    else
		      assert (newtypes[i] == MACH_MSG_TYPE_MOVE_SEND_ONCE);
		}
	      else
		{
		  for (i = 0; i < nelt; ++i)
		    switch (newtypes[i])
		      {
		      case MACH_MSG_TYPE_COPY_SEND:
			err = mach_port_mod_refs (mach_task_self (),
						  portnames[i],
						  MACH_PORT_RIGHT_SEND, +1);
			assert_perror (err);
			break;
		      case MACH_MSG_TYPE_MAKE_SEND:
			err = mach_port_insert_right (mach_task_self (),
						      portnames[i],
						      portnames[i],
						      newtypes[i]);
			assert_perror (err);
			break;
		      default:
			assert (newtypes[i] == MACH_MSG_TYPE_MOVE_SEND);
			break;
		      }

		  name = MACH_MSG_TYPE_MOVE_SEND;
		}
	      (type->msgt_longform ? lt->msgtl_name : type->msgt_name) = name;
	    }
	  else if (newtypes[0] != name)
	    (type->msgt_longform ? lt->msgtl_name : type->msgt_name)
	      = newtypes[0];
	}
      else
	print_data (name, data, nelt, eltsize);
    }
}

int
trace_and_forward (mach_msg_header_t *inp, mach_msg_header_t *outp)
{
  const mach_msg_type_t RetCodeType =
  {
    MACH_MSG_TYPE_INTEGER_32,	/* msgt_name = */
    32,				/* msgt_size = */
    1,				/* msgt_number = */
    TRUE,			/* msgt_inline = */
    FALSE,			/* msgt_longform = */
    FALSE,			/* msgt_deallocate = */
    0				/* msgt_unused = */
  };

  error_t err;
  struct traced_info *info;
  mach_msg_bits_t complex;

  /* Look up our record for the receiving port.  There is no need to check
     the class, because our port bucket only ever contains one class of
     ports (traced_class).  */
  info = ports_lookup_port (traced_bucket, inp->msgh_local_port, 0);
  assert (info);

  if (MACH_MSGH_BITS_LOCAL (inp->msgh_bits) == MACH_MSG_TYPE_MOVE_SEND_ONCE)
    {
      if (inp->msgh_id == MACH_NOTIFY_DEAD_NAME)
	{
	  /* If INFO is a send-once wrapper, this could be a forged
	     notification; oh well.  XXX */

	  const mach_dead_name_notification_t *const n = (void *) inp;

	  assert (n->not_port == info->forward);
	  /* Deallocate extra ref allocated by the notification.  */
	  mach_port_deallocate (mach_task_self (), n->not_port);
	  ports_destroy_right (info);
	  ports_port_deref (info);
	  ((mig_reply_header_t *) outp)->RetCode = MIG_NO_REPLY;
	  return 1;
	}
      else if (inp->msgh_id == MACH_NOTIFY_NO_SENDERS
	       && !INFO_SEND_ONCE (info))
	{
	  /* No more senders for a send right we are tracing.  Now INFO
	     will die, and we will release the tracee send right so it too
	     can see a no-senders notification.  */
	  mach_no_senders_notification_t *n = (void *) inp;
	  ports_no_senders (info, n->not_count);
	  ports_port_deref (info);
	  ((mig_reply_header_t *) outp)->RetCode = MIG_NO_REPLY;
	  return 1;
	}
    }

  complex = inp->msgh_bits & MACH_MSGH_BITS_COMPLEX;

  /* Swap the header data like a crossover cable. */
  {
    mach_msg_type_name_t this_type = MACH_MSGH_BITS_LOCAL (inp->msgh_bits);
    mach_msg_type_name_t reply_type = MACH_MSGH_BITS_REMOTE (inp->msgh_bits);

    inp->msgh_local_port = inp->msgh_remote_port;
    if (reply_type)
      {
	struct traced_info *info;
	info = rewrite_right (&inp->msgh_local_port, &reply_type);
	assert (info);
	if (info->name == 0)
	  asprintf (&info->name, "reply(%u:%u)",
		    (unsigned int) info->pi.port_right,
		    (unsigned int) inp->msgh_id);
	if (info->type == MACH_MSG_TYPE_MOVE_SEND_ONCE)
	  {
	    info->u.send_once.sent_to = info->pi.port_right;
	    info->u.send_once.sent_msgid = inp->msgh_id;
	  }
      }

    inp->msgh_remote_port = info->forward;
    if (this_type == MACH_MSG_TYPE_MOVE_SEND_ONCE)
      {
	/* We have a message to forward for a send-once wrapper object.
	   Since each wrapper object only lives for a single message, this
	   one can be reclaimed now.  We continue to hold a hard ref to the
	   ports object, but we know that nothing else refers to it now, and
	   we are consuming its `forward' right in the message we send.  */
	free (info->name);
	info->name = 0;
	info->u.nextfree = freelist;
	freelist = info;
      }
    else
      this_type = MACH_MSG_TYPE_COPY_SEND;

    inp->msgh_bits = complex | MACH_MSGH_BITS (this_type, reply_type);
  }

  /* The message now appears as it would if we were the sender.
     It is ready to be resent.  */

  if (inp->msgh_local_port == MACH_PORT_NULL
      && inp->msgh_size >= sizeof (mig_reply_header_t)
      && (*(int *) &((mig_reply_header_t *) inp)->RetCodeType
	  == *(int *)&RetCodeType))
    {
      /* This sure looks like an RPC reply message.  */
      mig_reply_header_t *rh = (void *) inp;
      print_reply_header (info, rh);
      putc (' ', ostream);
      print_contents (&rh->Head, rh + 1);
      putc ('\n', ostream);
    }
  else
    {
      /* Print something about the message header.  */
      print_request_header (info, inp);
      print_contents (inp, inp + 1);
      if (inp->msgh_local_port == MACH_PORT_NULL) /* simpleroutine */
	fprintf (ostream, ");\n");
      else
	/* Leave a partial line that will be finished later.  */
	fprintf (ostream, ")");
    }

  /* Resend the message to the tracee.  */
  err = mach_msg (inp, MACH_SEND_MSG, inp->msgh_size, 0,
		  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  if (err == MACH_SEND_INVALID_DEST)
    {
      /* The tracee port died.  No doubt we are about to receive the dead-name
	 notification.  */
      /* XXX MAKE_SEND, MAKE_SEND_ONCE rights in msg not handled */
      mach_msg_destroy (inp);
    }
  else
    assert_perror (err);

  ports_port_deref (info);

  /* We already sent the message, so the server loop shouldn't do it again.  */
  ((mig_reply_header_t *) outp)->RetCode = MIG_NO_REPLY;

  return 1;
}

/* This function runs in the tracing thread and drives all the tracing.  */
static any_t
trace_thread_function (void *arg)
{
  struct port_bucket *const bucket = arg;
  ports_manage_port_operations_one_thread (bucket, trace_and_forward, 0);
  return 0;
}

/*** Output formatting ***/

#if 0
struct msg_type
{
  const char *name;
  const char *letter;
};

static const char *const msg_types[] =
{
  [MACH_MSG_TYPE_BIT]		= {"bool", "b"},
  [MACH_MSG_TYPE_INTEGER_16]	= {"int16", "h"},
  [MACH_MSG_TYPE_INTEGER_32]	= {"int32", "i"},
  [MACH_MSG_TYPE_CHAR]		= {"char", "c"},
  [MACH_MSG_TYPE_INTEGER_8]	= {"int8", "B"},
  [MACH_MSG_TYPE_REAL]		= {"float", "f"},
  [MACH_MSG_TYPE_INTEGER_64]	= {"int64", "q"},
  [MACH_MSG_TYPE_STRING]	= {"string", "s"},
  [MACH_MSG_TYPE_MOVE_RECEIVE]	= {"move-receive", "R"},
  [MACH_MSG_TYPE_MOVE_SEND]	= {"move-send", "S"},
  [MACH_MSG_TYPE_MOVE_SEND_ONCE]= {"move-send-once", "O"},
  [MACH_MSG_TYPE_COPY_SEND]	= {"copy-send", "s"},
  [MACH_MSG_TYPE_MAKE_SEND]	= {"make-send", ""},
  [MACH_MSG_TYPE_MAKE_SEND_ONCE]= {"make-send-once", ""},
  [MACH_MSG_TYPE_PORT_NAME]	= {"port-name", "n"},
};
#endif

static mach_port_t expected_reply_port;

static void
print_request_header (struct traced_info *receiver, mach_msg_header_t *msg)
{
  expected_reply_port = msg->msgh_local_port;

  if (receiver->name != 0)
    fprintf (ostream, "%4s->%5u (", receiver->name, msg->msgh_id);
  else
    fprintf (ostream, "%4u->%5u (",
	     (unsigned int) receiver->pi.port_right, msg->msgh_id);
}

static void
unfinished_line (void)
{
  /* A partial line was printed by print_request_header, but
     cannot be finished before we print something else.
     Finish this line with the name of the reply port that
     will appear in the disconnected reply later on.  */
  fprintf (ostream, " > %4u ...\n", expected_reply_port);
}

static void
print_reply_header (struct traced_info *info, mig_reply_header_t *reply)
{
  if (info->pi.port_right == expected_reply_port)
    {
      /* We have printed a partial line for the request message,
	 and now we have the corresponding reply.  */
      if (reply->Head.msgh_id == info->u.send_once.sent_msgid + 100)
	fprintf (ostream, " = "); /* normal case */
      else
	/* This is not the proper reply message ID.  */
	fprintf (ostream, " =(%u != %u) ",
		 reply->Head.msgh_id,
		 info->u.send_once.sent_msgid + 100);
    }
  else
    {
      /* This does not match up with the last thing printed.  */
      unfinished_line ();
      if (info->name == 0)
	/* This was not a reply port in previous message sent
	   through our wrappers.  */
	fprintf (ostream, "reply?%4u",
		 (unsigned int) info->pi.port_right);
      else
	fprintf (ostream, "%s%4u",
		 info->name, (unsigned int) info->pi.port_right);
      if (reply->Head.msgh_id == info->u.send_once.sent_msgid + 100)
	/* This is a normal reply to a previous request.  */
	fprintf (ostream, " > ");
      else
	/* Weirdo.  */
	fprintf (ostream, " >(%u) ", reply->Head.msgh_id);
    }

  if (reply->RetCode == 0)
    fprintf (ostream, "0");
  else
    {
      const char *str = strerror (reply->RetCode);
      if (str == 0)
	fprintf (ostream, "%#x", reply->RetCode);
      else
	fprintf (ostream, "%#x (%s)", reply->RetCode, str);
    }

  expected_reply_port = MACH_PORT_NULL;
}


static void
print_data (mach_msg_type_name_t type,
	    const void *data,
	    mach_msg_type_number_t nelt,
	    mach_msg_type_number_t eltsize)
{
  switch (type)
    {
    case MACH_MSG_TYPE_PORT_NAME:
      assert (eltsize == sizeof (mach_port_t));
      {
	mach_msg_type_number_t i;
	fprintf (ostream, "pn{");
	for (i = 0; i < nelt; ++i)
	  {
	    fprintf (ostream, "%*u", (i > 0) ? 4 : 3,
		     (unsigned int) ((mach_port_t *) data)[i]);
	  }
	fprintf (ostream, "}");
	return;
      }

    case MACH_MSG_TYPE_STRING:
    case MACH_MSG_TYPE_CHAR:
      fprintf (ostream, "\"%.*s\"",
	       (int) (nelt * eltsize), (const char *) data);
      return;

#if 0
    case MACH_MSG_TYPE_CHAR:
      if (eltsize == 1)
	FMT ("'%c'", unsigned char);
      break;
#endif

#define FMT(fmt, ctype) do {						      \
	mach_msg_type_number_t i;					      \
	for (i = 0; i < nelt; ++i)					      \
	  {								      \
	    fprintf (ostream, "%s" fmt,					      \
		     (i == 0 && nelt > 1) ? "{" : i > 0 ? " " : "",	      \
		     *(const ctype *) data);				      \
	    data += eltsize;						      \
	  }								      \
	if (nelt > 1)							      \
	  putc ('}', ostream);						      \
        return;								      \
      } while (0)

    case MACH_MSG_TYPE_BIT:
    case MACH_MSG_TYPE_INTEGER_8:
    case MACH_MSG_TYPE_INTEGER_16:
    case MACH_MSG_TYPE_INTEGER_32:
    case MACH_MSG_TYPE_INTEGER_64:
      switch (eltsize)
	{
	case 1:				FMT ("%"PRId8, int8_t);
	case 2:				FMT ("%"PRId16, int16_t);
	case 4:				FMT ("%"PRId32, int32_t);
	case 8:				FMT ("%"PRId64, int64_t);
	}
      break;

    case MACH_MSG_TYPE_REAL:
      switch (eltsize)
	{
	case sizeof (float):		FMT ("%g", float);
	case sizeof (double):		FMT ("%g", double);
	case sizeof (long double):	FMT ("%Lg", long double);
	}
      break;
    }

  /* XXX */
  fprintf (ostream, "\t%#x (type %d, %d*%d)\n", *(const int *)data, type,
	   nelt, eltsize);
}


/*** Main program and child startup ***/

task_t traced_task;


/* Run a child and have it do more or else `execvpe (argv, envp);'.  */
pid_t
traced_spawn (char **argv, char **envp)
{
  error_t err;
  pid_t pid;
  mach_port_t task_wrapper;
  struct traced_info *ti;
  file_t file = file_name_path_lookup (argv[0], getenv ("PATH"),
				       O_EXEC, 0, 0);

  if (file == MACH_PORT_NULL)
    error (1, errno, "command not found: %s", argv[0]);

  err = task_create (mach_task_self (), 0, &traced_task);
  assert_perror (err);

  /* Declare the new task to be our child.  This is what a fork does.  */
  err = proc_child (getproc (), traced_task);
  if (err)
    error (2, err, "proc_child");
  pid = task2pid (traced_task);
  if (pid < 0)
    error (2, errno, "task2pid");

  /* Create a trace wrapper for the task port.  */
  ti = new_send_wrapper (traced_task, &task_wrapper);/* consumes ref */
  asprintf (&ti->name, "task%d", (int) pid);

  /* Replace the task's kernel port with the wrapper.  When this task calls
     `mach_task_self ()', it will get our wrapper send right instead of its
     own real task port.  */
  err = mach_port_insert_right (mach_task_self (), task_wrapper,
				task_wrapper, MACH_MSG_TYPE_MAKE_SEND);
  assert_perror (err);
  err = task_set_special_port (traced_task, TASK_KERNEL_PORT, task_wrapper);
  assert_perror (err);

  /* Now actually run the command they told us to trace.  We do the exec on
     the actual task, so the RPCs to map in the program itself do not get
     traced.  Could have an option to use TASK_WRAPPER here instead.  */
  err = _hurd_exec (traced_task, file, argv, envp);
  if (err)
    error (2, err, "cannot exec `%s'", argv[0]);

  /* We were keeping this send right alive so that the wrapper object
     cannot die and hence our TRACED_TASK ref cannot have been released.  */
  mach_port_deallocate (mach_task_self (), task_wrapper);

  return pid;
}


int
main (int argc, char **argv, char **envp)
{
  const char *outfile = 0;
  char **cmd_argv = 0;
  error_t err;

  /* Parse our options...  */
  error_t parse_opt (int key, char *arg, struct argp_state *state)
    {
      switch (key)
	{
	case 'o':
	  outfile = arg;
	  break;

	case ARGP_KEY_NO_ARGS:
	  argp_usage (state);
	  return EINVAL;

	case ARGP_KEY_ARG:
	  cmd_argv = &state->argv[state->next - 1];
	  state->next = state->argc;
	  break;

	default:
	  return ARGP_ERR_UNKNOWN;
	}
      return 0;
    }
  const struct argp argp = { options, parse_opt, args_doc, doc };

  /* Parse our arguments.  */
  argp_parse (&argp, argc, argv, ARGP_IN_ORDER, 0, 0);

  if (outfile)
    {
      ostream = fopen (outfile, "w");
      if (!ostream)
	error (1, errno, "%s", outfile);
    }
  else
    ostream = stderr;
  setlinebuf (ostream);

  traced_bucket = ports_create_bucket ();
  traced_class = ports_create_class (0, &traced_dropweak);

  err = ihash_create (&traced_names);
  assert_perror (err);

  /* Spawn a single thread that will receive intercepted messages, print
     them, and interpose on the ports they carry.  The access to the
     `traced_info' and ihash data structures is all single-threaded,
     happening only in this new thread.  */
  cthread_detach (cthread_fork (trace_thread_function, traced_bucket));

  /* Run the program on the command line and wait for it to die.
     The other thread does all the tracing and interposing.  */
  {
    pid_t child, pid;
    int status;
    child = traced_spawn (cmd_argv, envp);
    pid = waitpid (child, &status, 0);
    sleep (1);			/* XXX gives other thread time to print */
    if (pid != child)
      error (1, errno, "waitpid");
    if (WIFEXITED (status))
      fprintf (ostream, "Child %d exited with %d\n",
	       pid, WEXITSTATUS (status));
    else
      fprintf (ostream, "Child %d %s\n", pid, strsignal (WTERMSIG (status)));
  }

  return 0;
}
