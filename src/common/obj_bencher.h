// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_OBJ_BENCHER_H
#define CEPH_OBJ_BENCHER_H

#include "common/config.h"
#include "common/Cond.h"

struct bench_data {
  bool done; //is the benchmark is done
  int object_size; //the size of the objects
  int trans_size; //size of the write/read to perform
  // same as object_size for write tests
  int in_flight; //number of reads/writes being waited on
  int started;
  int finished;
  double min_latency;
  double max_latency;
  double avg_latency;
  utime_t cur_latency; //latency of last completed transaction
  utime_t start_time; //start time for benchmark
  char *object_contents; //pointer to the contents written to each object
};

const int OP_WRITE     = 1;
const int OP_SEQ_READ  = 2;
const int OP_RAND_READ = 3;

class ObjBencher {
protected:
  Mutex lock;

  static void *status_printer(void *bencher);

  struct bench_data data;

  int write_bench(int secondsToRun, int concurrentios);
  int seq_read_bench(int secondsToRun, int concurrentios, int num_objects, int writePid);

  virtual int completions_init(int concurrentios) = 0;
  virtual void completions_done() = 0;

  virtual int create_completion(int i, void (*cb)(void *, void*), void *arg) = 0;
  virtual void release_completion(int slot) = 0;

  virtual bool completion_is_done(int slot) = 0;
  virtual int completion_wait(int slot) = 0;
  virtual int completion_ret(int slot) = 0;

  virtual int aio_read(const std::string& oid, int slot, bufferlist *pbl, size_t len) = 0;
  virtual int aio_write(const std::string& oid, int slot, const bufferlist& bl, size_t len) = 0;
  virtual int sync_read(const std::string& oid, bufferlist& bl, size_t len) = 0;
  virtual int sync_write(const std::string& oid, bufferlist& bl, size_t len) = 0;
public:
  ObjBencher() : lock("ObjBencher::lock") {}
  virtual ~ObjBencher() {}
  int aio_bench(int operation, int secondsToRun, int concurrentios, int op_size);
};


#endif
