// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Portions Copyright (C) 2013 CohortFS, LLC
 *s
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef XIO_PORTAL_H
#define XIO_PORTAL_H

extern "C" {
#include "libxio.h"
}
#include "XioInSeq.h"
#include <boost/lexical_cast.hpp>
#include "msg/SimplePolicyMessenger.h"
#include "XioConnection.h"
#include "XioMsg.h"

#include "include/assert.h"
#include "common/dout.h"

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64 /* XXX arch-specific define */
#endif
#define CACHE_PAD(_n) char __pad ## _n [CACHE_LINE_SIZE]

class XioPortal : public Thread
{
private:

  struct SubmitQueue
  {
    const static int nlanes = 7;

    struct Lane
    {
      uint32_t size;
      XioMsg::Queue q;
      pthread_spinlock_t sp;
      CACHE_PAD(0);
    };

    Lane qlane[nlanes];

    int ix; /* atomicity by portal thread */

    SubmitQueue() : ix(0)
      {
	int ix;
	Lane* lane;

	for (ix = 0; ix < nlanes; ++ix) {
	  lane = &qlane[ix];
	  pthread_spin_init(&lane->sp, PTHREAD_PROCESS_PRIVATE);
	  lane->size = 0;
	}
      }

    inline Lane* get_lane(XioConnection *xcon)
      {
	return &qlane[((uint64_t) xcon) % nlanes];
      }

    void enq(XioConnection *xcon, XioSubmit* xs)
      {
	Lane* lane = get_lane(xcon);
	pthread_spin_lock(&lane->sp);
	lane->q.push_back(*xs);
	++(lane->size);
	pthread_spin_unlock(&lane->sp);
      }

    void enq(XioConnection *xcon, XioSubmit::Queue& requeue_q)
      {
	int size = requeue_q.size();
	Lane* lane = get_lane(xcon);
	pthread_spin_lock(&lane->sp);
	XioSubmit::Queue::const_iterator i1 = lane->q.end();
	lane->q.splice(i1, requeue_q);
	lane->size += size;
	pthread_spin_unlock(&lane->sp);
      }

    void deq(XioSubmit::Queue& send_q)
      {
	Lane* lane;
	int cnt;
	for (cnt = 0; cnt < nlanes; ++cnt, ++ix, ix = ix % nlanes) {
	  lane = &qlane[ix];
	  pthread_spin_lock(&lane->sp);
	  if (lane->size > 0) {
	    XioSubmit::Queue::const_iterator i1 = send_q.end();
	    send_q.splice(i1, lane->q);
	    lane->size = 0;
	    ++ix, ix = ix % nlanes;
	    pthread_spin_unlock(&lane->sp);
	    break;
	  }
	  pthread_spin_unlock(&lane->sp);
	}
      }

  }; /* SubmitQueue */

  Messenger *msgr;
  struct xio_context *ctx;
  struct xio_server *server;
  SubmitQueue submit_q;
  pthread_spinlock_t sp;
  pthread_mutex_t mtx;
  void *ev_loop;
  string xio_uri;
  char *portal_id;
  bool _shutdown;
  bool drained;
  uint32_t magic;
  uint32_t special_handling;

  friend class XioPortals;
  friend class XioMessenger;

public:
  XioPortal(Messenger *_msgr) :
  msgr(_msgr), ctx(NULL), server(NULL), submit_q(), xio_uri(""),
  portal_id(NULL), _shutdown(false), drained(false),
  magic(0),
  special_handling(0)
    {
      pthread_spin_init(&sp, PTHREAD_PROCESS_PRIVATE);
      pthread_mutex_init(&mtx, NULL);

      /* a portal is an xio_context and event loop */
      ctx = xio_context_create(NULL, 0 /* poll timeout */, -1 /* cpu hint */);

      /* associate this XioPortal object with the xio_context handle */
      struct xio_context_attr xca;
      xca.user_context = this;
      xio_modify_context(ctx, &xca, XIO_CONTEXT_ATTR_USER_CTX);

      if (magic & (MSG_MAGIC_XIO)) {
	printf("XioPortal %p created ev_loop %p ctx %p\n",
	       this, ev_loop, ctx);
      }
    }

  int bind(struct xio_session_ops *ops, const string &base_uri,
	   uint16_t port, uint16_t *assigned_port);

  inline void release_xio_rsp(XioRsp* xrsp) {
    struct xio_msg *msg = xrsp->dequeue();
    struct xio_msg *next_msg = NULL;
    int code;
    while (msg) {
      next_msg = static_cast<struct xio_msg *>(msg->user_context);
      if (unlikely(!xrsp->xcon->conn || !xrsp->xcon->is_connected()))
        code = ENOTCONN;
      else
        code = xio_release_msg(msg);
      if (unlikely(code)) {
	/* very unlikely, so log it */
	xrsp->xcon->msg_release_fail(msg, code);
      }
      msg =  next_msg;
    }
    xrsp->finalize(); /* unconditional finalize */
  }

  void enqueue_for_send(XioConnection *xcon, XioSubmit *xs)
    {
      if (! _shutdown) {
	submit_q.enq(xcon, xs);
	xio_context_stop_loop(ctx);
	return;
      }

      /* dispose xs */
      switch(xs->type) {
      case XioSubmit::OUTGOING_MSG: /* it was an outgoing 1-way */
      {
	XioMsg* xmsg = static_cast<XioMsg*>(xs);
	xs->xcon->msg_send_fail(xmsg, -EINVAL);
      }
	break;
      default:
	/* INCOMING_MSG_RELEASE */
	release_xio_rsp(static_cast<XioRsp*>(xs));
      break;
      };
    }

  void requeue(XioConnection* xcon, XioSubmit::Queue& send_q) {
    submit_q.enq(xcon, send_q);
  }

  void requeue_all_xcon(XioConnection* xcon,
			XioSubmit::Queue::iterator& q_iter,
			XioSubmit::Queue& send_q) {
    // XXX gather all already-dequeued outgoing messages for xcon
    // and push them in FIFO order to front of the input queue,
    // and mark the connection as flow-controlled
    XioSubmit::Queue requeue_q;
    XioMsg *xmsg;

    while (q_iter != send_q.end()) {
      XioSubmit *xs = &(*q_iter);
      // skip retires and anything for other connections
      if (xs->xcon != xcon) {
	q_iter++;
	continue;
      }
      xmsg = static_cast<XioMsg*>(xs);
      q_iter = send_q.erase(q_iter);
      requeue_q.push_back(*xmsg);
    }
    pthread_spin_lock(&xcon->sp);
    XioSubmit::Queue::const_iterator i1 = xcon->outgoing.requeue.begin();
    xcon->outgoing.requeue.splice(i1, requeue_q);
    xcon->cstate.state_flow_controlled(XioConnection::CState::OP_FLAG_LOCKED);
    pthread_spin_unlock(&xcon->sp);
  }

  void *entry()
    {
      int size, code = 0;
      uint32_t xio_qdepth_high;
      XioSubmit::Queue send_q;
      XioSubmit::Queue::iterator q_iter;
      struct xio_msg *msg = NULL;
      XioConnection *xcon;
      XioSubmit *xs;
      XioMsg *xmsg;

      do {
	submit_q.deq(send_q);

	/* shutdown() barrier */
	pthread_spin_lock(&sp);

      restart:
	size = send_q.size();

	if (_shutdown) {
	  // XXX XioMsg queues for flow-controlled connections may require
	  // cleanup
	  drained = true;
	}

	if (size > 0) {
	  q_iter = send_q.begin();
	  while (q_iter != send_q.end()) {
	    xs = &(*q_iter);
	    xcon = xs->xcon;

	    switch (xs->type) {
	    case XioSubmit::OUTGOING_MSG: /* it was an outgoing 1-way */
	      xmsg = static_cast<XioMsg*>(xs);
	      if (unlikely(!xcon->conn || !xcon->is_connected()))
		code = ENOTCONN;
	      else {
		/* XXX guard Accelio send queue (should be safe to rely
		 * on Accelio's check on below, but this assures that
		 * all chained xio_msg are accounted) */
		xio_qdepth_high = xcon->xio_qdepth_high_mark();
		if (unlikely((xcon->send_ctr + xmsg->hdr.msg_cnt) >
			     xio_qdepth_high)) {
		  requeue_all_xcon(xcon, q_iter, send_q);
		  goto restart;
		}

		msg = &xmsg->req_0.msg;
		code = xio_send_msg(xcon->conn, msg);
		/* header trace moved here to capture xio serial# */
		if (ldlog_p1(msgr->cct, ceph_subsys_xio, 11)) {
		  print_xio_msg_hdr(msgr->cct, "xio_send_msg", xmsg->hdr, msg);
		  print_ceph_msg(msgr->cct, "xio_send_msg", xmsg->m);
		}
		/* get the right Accelio's errno code */
		if (unlikely(code)) {
		  if ((code == -1) && (xio_errno() == -1)) {
		    /* In case XIO does not have any credits to send,
		     * it would still queue up the message(s) for transmission,
		     * but would return -1 and errno would also be set to -1.
		     * This needs to be treated as a success.
		     */
		    code = 0;
		  }
		  else {
		    code = xio_errno();
		  }
		}
	      } /* !ENOTCONN */
	      if (unlikely(code)) {
		switch (code) {
		case XIO_E_TX_QUEUE_OVERFLOW:
		{
		  requeue_all_xcon(xcon, q_iter, send_q);
		  goto restart;
		}
		  break;
		default:
		  q_iter = send_q.erase(q_iter);
		  xcon->msg_send_fail(xmsg, code);
		  continue;
		  break;
		};
	      } else {
		xcon->send.set(msg->timestamp); // need atomic?
		xcon->send_ctr += xmsg->hdr.msg_cnt; // only inc if cb promised
	      }
	      break;
	    default:
	      /* INCOMING_MSG_RELEASE */
	      q_iter = send_q.erase(q_iter);
	      release_xio_rsp(static_cast<XioRsp*>(xs));
	      continue;
	    } /* switch (xs->type) */
	    q_iter = send_q.erase(q_iter);
	  } /* while */
	} /* size > 0 */

	pthread_spin_unlock(&sp);
	xio_context_run_loop(ctx, 300);

      } while ((!_shutdown) || (!drained));

      /* shutting down */
      if (server) {
	xio_unbind(server);
      }
      xio_context_destroy(ctx);
      return NULL;
    }

  void shutdown()
    {
	pthread_spin_lock(&sp);
	_shutdown = true;
	pthread_spin_unlock(&sp);
    }
};

class XioPortals
{
private:
  vector<XioPortal*> portals;
  char **p_vec;
  int n;
  int last_use;

public:
  XioPortals(Messenger *msgr, int _n) : p_vec(NULL)
  {
    /* portal0 */
    portals.push_back(new XioPortal(msgr));
    last_use = 0;

    /* enforce at least two portals if bind */
    if (_n < 2)
      _n = 2;
    n = _n;

    /* additional portals allocated on bind() */
  }

  vector<XioPortal*>& get() { return portals; }

  const char **get_vec()
  {
    return (const char **) p_vec;
  }

  int get_portals_len()
  {
    return n;
  }

  int get_last_use()
  {
    int pix = last_use;
    if (++last_use >= get_portals_len() - 1)
      last_use = 0;
    return pix;
  }

  XioPortal* get_portal0()
  {
    return portals[0];
  }

  int bind(struct xio_session_ops *ops, const string& base_uri,
	   uint16_t port, uint16_t *port0);

  int accept(struct xio_session *session,
	     struct xio_new_session_req *req,
	     void *cb_user_context)
  {
    const char **portals_vec = get_vec();
    int pix = get_last_use();

    return xio_accept(session,
		      (const char **)&(portals_vec[pix]),
		      1, NULL, 0);
  }

  void start()
  {
    XioPortal *portal;
    int p_ix, nportals = portals.size();

    /* portal_0 is the new-session handler, portal_1+ terminate
     * active sessions */

    p_vec = new char*[(nportals-1)];
    for (p_ix = 1; p_ix < nportals; ++p_ix) {
      portal = portals[p_ix];
      /* shift left */
      p_vec[(p_ix-1)] = (char*) /* portal->xio_uri.c_str() */
			portal->portal_id;
      }

    for (p_ix = 0; p_ix < nportals; ++p_ix) {
      portal = portals[p_ix];
      portal->create();
    }
  }

  void shutdown()
  {
    int nportals = portals.size();
    for (int p_ix = 0; p_ix < nportals; ++p_ix) {
      XioPortal *portal = portals[p_ix];
      portal->shutdown();
    }
  }

  void join()
  {
    int nportals = portals.size();
    for (int p_ix = 0; p_ix < nportals; ++p_ix) {
      XioPortal *portal = portals[p_ix];
      portal->join();
    }
  }

  ~XioPortals()
  {
    int nportals = portals.size();
    for (int ix = 0; ix < nportals; ++ix)
      delete(portals[ix]);
    portals.clear();
    if (p_vec)
      delete[] p_vec;
  }
};

#endif /* XIO_PORTAL_H */
