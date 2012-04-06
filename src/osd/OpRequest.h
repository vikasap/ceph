// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2012 New Dream Network/Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#ifndef OPREQUEST_H_
#define OPREQUEST_H_
#include <sstream>
#include <stdint.h>
#include <include/utime.h>
#include "common/Mutex.h"
#include "include/xlist.h"
#include "msg/Message.h"
#include <tr1/memory>
#include "common/TrackedOp.h"

class OpRequest;
typedef std::tr1::shared_ptr<OpRequest> OpRequestRef;
class OpTracker {
  class RemoveOnDelete {
    OpTracker *tracker;
  public:
    RemoveOnDelete(OpTracker *tracker) : tracker(tracker) {}
    void operator()(OpRequest *op);
  };
  friend class RemoveOnDelete;
  uint64_t seq;
  Mutex ops_in_flight_lock;
  xlist<OpRequest *> ops_in_flight;

public:
  OpTracker() : seq(0), ops_in_flight_lock("OpTracker mutex") {}
  void dump_ops_in_flight(std::ostream& ss);
  void register_inflight_op(xlist<OpRequest*>::item *i);
  void unregister_inflight_op(xlist<OpRequest*>::item *i);
  bool check_ops_in_flight(std::ostream &out);
  void mark_event(OpRequest *op, const string &evt);
  OpRequestRef create_request(Message *req);
};

/**
 * The OpRequest takes in a Message* and takes over a single reference
 * to it, which it puts() when destroyed.
 * OpRequest is itself ref-counted. The expectation is that you get a Message
 * you want to track, create an OpRequest with it, and then pass around that OpRequest
 * the way you used to pass around the Message.
 */
struct OpRequest : public TrackedOp {
  friend class OpTracker;
  Message *request;
  xlist<OpRequest*>::item xitem;
  utime_t received_time;
  uint8_t warn_interval_multiplier;
private:
  OpTracker *tracker;
  uint8_t hit_flag_points;
  uint8_t latest_flag_point;
  uint64_t seq;
  static const uint8_t flag_queued_for_pg=1 << 0;
  static const uint8_t flag_reached_pg =  1 << 1;
  static const uint8_t flag_delayed =     1 << 2;
  static const uint8_t flag_started =     1 << 3;
  static const uint8_t flag_sub_op_sent = 1 << 4;

  OpRequest(Message *req, OpTracker *tracker) :
    request(req), xitem(this),
    warn_interval_multiplier(1),
    tracker(tracker),
    seq(0) {
    received_time = request->get_recv_stamp();
    tracker->register_inflight_op(&xitem);
  }
public:
  ~OpRequest() {
    assert(request);
    request->put();
  }

  bool been_queued_for_pg() { return hit_flag_points & flag_queued_for_pg; }
  bool been_reached_pg() { return hit_flag_points & flag_reached_pg; }
  bool been_delayed() { return hit_flag_points & flag_delayed; }
  bool been_started() { return hit_flag_points & flag_started; }
  bool been_sub_op_sent() { return hit_flag_points & flag_sub_op_sent; }
  bool currently_queued_for_pg() { return latest_flag_point & flag_queued_for_pg; }
  bool currently_reached_pg() { return latest_flag_point & flag_reached_pg; }
  bool currently_delayed() { return latest_flag_point & flag_delayed; }
  bool currently_started() { return latest_flag_point & flag_started; }
  bool currently_sub_op_sent() { return latest_flag_point & flag_sub_op_sent; }

  const char *state_string() {
    switch(latest_flag_point) {
    case flag_queued_for_pg: return "queued for pg";
    case flag_reached_pg: return "reached pg";
    case flag_delayed: return "delayed";
    case flag_started: return "started";
    case flag_sub_op_sent: return "waiting for sub ops";
    default: break;
    }
    return "no flag points reached";
  }

  void mark_queued_for_pg() {
    mark_event("queued_for_pg");
    hit_flag_points |= flag_queued_for_pg;
    latest_flag_point = flag_queued_for_pg;
  }
  void mark_reached_pg() {
    mark_event("reached_pg");
    hit_flag_points |= flag_reached_pg;
    latest_flag_point = flag_reached_pg;
  }
  void mark_delayed() {
    hit_flag_points |= flag_delayed;
    latest_flag_point = flag_delayed;
  }
  void mark_started() {
    mark_event("started");
    hit_flag_points |= flag_started;
    latest_flag_point = flag_started;
  }
  void mark_sub_op_sent() {
    mark_event("sub_op_sent");
    hit_flag_points |= flag_sub_op_sent;
    latest_flag_point = flag_sub_op_sent;
  }

  void mark_event(const string &event);
};

#endif /* OPREQUEST_H_ */
