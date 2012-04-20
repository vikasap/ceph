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
 * Series of functions to test your rados installation. Notice
 * that this code is not terribly robust -- for instance, if you
 * try and bench on a pool you don't have permission to access
 * it will just loop forever.
 */
#include "common/Cond.h"
#include "obj_bencher.h"

#include <iostream>
#include <fstream>

#include <stdlib.h>
#include <time.h>
#include <sstream>


const char *BENCH_DATA = "benchmark_write_data";

static void generate_object_name(char *s, size_t size, int objnum, int pid = 0)
{
  char hostname[30];
  gethostname(hostname, sizeof(hostname)-1);
  hostname[sizeof(hostname)-1] = 0;
  if (pid) {
    snprintf(s, size, "%s_%d_object%d", hostname, pid, objnum);
  } else {
    snprintf(s, size, "%s_%d_object%d", hostname, getpid(), objnum);
  }
}

static void sanitize_object_contents (bench_data *data, int length) {
  for (int i = 0; i < length; ++i) {
    data->object_contents[i] = i % sizeof(char);
  }
}

void *ObjBencher::status_printer(void *_bencher) {
  ObjBencher *bencher = (ObjBencher *)_bencher;
  bench_data& data = bencher->data;
  Cond cond;
  int i = 0;
  int previous_writes = 0;
  int cycleSinceChange = 0;
  double avg_bandwidth;
  double bandwidth;
  utime_t ONE_SECOND;
  ONE_SECOND.set_from_double(1.0);
  bencher->lock.Lock();
  while(!data.done) {
    if (i % 20 == 0) {
      if (i > 0)
	cout << "min lat: " << data.min_latency
	     << " max lat: " << data.max_latency
	     << " avg lat: " << data.avg_latency << std::endl;
      //I'm naughty and don't reset the fill
      cout << setfill(' ')
	   << setw(5) << "sec"
	   << setw(8) << "Cur ops"
	   << setw(10) << "started"
	   << setw(10) << "finished"
	   << setw(10) << "avg MB/s"
	   << setw(10) << "cur MB/s"
	   << setw(10) << "last lat"
	   << setw(10) << "avg lat" << std::endl;
    }
    bandwidth = (double)(data.finished - previous_writes)
      * (data.trans_size)
      / (1024*1024)
      / cycleSinceChange;
    avg_bandwidth = (double) (data.trans_size) * (data.finished)
      / (double)(ceph_clock_now(g_ceph_context) - data.start_time) / (1024*1024);
    if (previous_writes != data.finished) {
      previous_writes = data.finished;
      cycleSinceChange = 0;
      cout << setfill(' ')
	   << setw(5) << i
	   << setw(8) << data.in_flight
	   << setw(10) << data.started
	   << setw(10) << data.finished
	   << setw(10) << avg_bandwidth
	   << setw(10) << bandwidth
	   << setw(10) << (double)data.cur_latency
	   << setw(10) << data.avg_latency << std::endl;
    }
    else {
      cout << setfill(' ')
	   << setw(5) << i
	   << setw(8) << data.in_flight
	   << setw(10) << data.started
	   << setw(10) << data.finished
	   << setw(10) << avg_bandwidth
	   << setw(10) << '0'
	   << setw(10) << '-'
	   << setw(10) << data.avg_latency << std::endl;
    }
    ++i;
    ++cycleSinceChange;
    cond.WaitInterval(g_ceph_context, bencher->lock, ONE_SECOND);
  }
  bencher->lock.Unlock();
  return NULL;
}

int ObjBencher::aio_bench(int operation, int secondsToRun, int concurrentios, int op_size) {
  int object_size = op_size;
  int num_objects = 0;
  char* contentsChars = new char[op_size];
  int r = 0;
  int prevPid = 0;

  //get data from previous write run, if available
  if (operation != OP_WRITE) {
    bufferlist object_data;
    r = sync_read(BENCH_DATA, object_data, sizeof(int)*3);
    if (r <= 0) {
      delete[] contentsChars;
      if (r == -2)
	cerr << "Must write data before running a read benchmark!" << std::endl;
      return r;
    }
    bufferlist::iterator p = object_data.begin();
    ::decode(object_size, p);
    ::decode(num_objects, p);
    ::decode(prevPid, p);
  } else {
    object_size = op_size;
  }

  lock.Lock();
  data.done = false;
  data.object_size = object_size;
  data.trans_size = op_size;
  data.in_flight = 0;
  data.started = 0;
  data.finished = num_objects;
  data.min_latency = 9999.0; // this better be higher than initial latency!
  data.max_latency = 0;
  data.avg_latency = 0;
  data.object_contents = contentsChars;
  lock.Unlock();

  //fill in contentsChars deterministically so we can check returns
  sanitize_object_contents(&data, data.object_size);

  if (OP_WRITE == operation) {
    r = write_bench(secondsToRun, concurrentios);
    if (r != 0) goto out;
  }
  else if (OP_SEQ_READ == operation) {
    r = seq_read_bench(secondsToRun, concurrentios, num_objects, prevPid);
    if (r != 0) goto out;
  }
  else if (OP_RAND_READ == operation) {
    cerr << "Random test not implemented yet!" << std::endl;
    r = -1;
  }

 out:
  delete[] contentsChars;
  return r;
}

struct lock_cond {
  lock_cond(Mutex *_lock) : lock(_lock) {}
  Mutex *lock;
  Cond cond;
};

void _aio_cb(void *cb, void *arg) {
  struct lock_cond *lc = (struct lock_cond *)arg;
  lc->lock->Lock();
  lc->cond.Signal();
  lc->lock->Unlock();
}

int ObjBencher::write_bench(int secondsToRun, int concurrentios) {
  cout << "Maintaining " << concurrentios << " concurrent writes of "
       << data.object_size << " bytes for at least "
       << secondsToRun << " seconds." << std::endl;

  char* name[concurrentios];
  bufferlist* contents[concurrentios];
  double total_latency = 0;
  utime_t start_times[concurrentios];
  utime_t stopTime;
  int r = 0;
  bufferlist b_write;
  lock_cond lc(&lock);
  utime_t runtime;
  utime_t timePassed;

  r = completions_init(concurrentios);

  //set up writes so I can start them together
  for (int i = 0; i<concurrentios; ++i) {
    name[i] = new char[128];
    contents[i] = new bufferlist();
    generate_object_name(name[i], 128, i);
    snprintf(data.object_contents, data.object_size, "I'm the %dth object!", i);
    contents[i]->append(data.object_contents, data.object_size);
  }

  pthread_t print_thread;

  pthread_create(&print_thread, NULL, ObjBencher::status_printer, (void *)this);
  lock.Lock();
  data.start_time = ceph_clock_now(g_ceph_context);
  lock.Unlock();
  for (int i = 0; i<concurrentios; ++i) {
    start_times[i] = ceph_clock_now(g_ceph_context);
    r = create_completion(i, _aio_cb, (void *)&lc);
    if (r < 0)
      goto ERR;
    r = aio_write(name[i], i, *contents[i], data.object_size);
    if (r < 0) { //naughty, doesn't clean up heap
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
  }

  //keep on adding new writes as old ones complete until we've passed minimum time
  int slot;
  bufferlist* newContents;
  char* newName;

  //don't need locking for reads because other thread doesn't write

  runtime.set_from_double(secondsToRun);
  stopTime = data.start_time + runtime;
  while( ceph_clock_now(g_ceph_context) < stopTime ) {
    lock.Lock();
    while (1) {
      for (slot = 0; slot < concurrentios; ++slot) {
	if (completion_is_done(slot)) {
	  break;
	}
      }
      if (slot < concurrentios) {
	break;
      }
      lc.cond.Wait(lock);
    }
    lock.Unlock();
    //create new contents and name on the heap, and fill them
    newContents = new bufferlist();
    newName = new char[128];
    generate_object_name(newName, 128, data.started);
    snprintf(data.object_contents, data.object_size, "I'm the %dth object!", data.started);
    newContents->append(data.object_contents, data.object_size);
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0) {
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(g_ceph_context) - start_times[slot];
    total_latency += data.cur_latency;
    if( data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);
    timePassed = ceph_clock_now(g_ceph_context) - data.start_time;

    //write new stuff to backend, then delete old stuff
    //and save locations of new stuff for later deletion
    start_times[slot] = ceph_clock_now(g_ceph_context);
    r = create_completion(slot, _aio_cb, &lc);
    if (r < 0)
      goto ERR;
    r = aio_write(newName, slot, *newContents, data.object_size);
    if (r < 0) {//naughty; doesn't clean up heap space.
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
    delete[] name[slot];
    delete contents[slot];
    name[slot] = newName;
    contents[slot] = newContents;
  }

  while (data.finished < data.started) {
    slot = data.finished % concurrentios;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0) {
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(g_ceph_context) - start_times[slot];
    total_latency += data.cur_latency;
    if (data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);
    delete[] name[slot];
    delete contents[slot];
  }

  timePassed = ceph_clock_now(g_ceph_context) - data.start_time;
  lock.Lock();
  data.done = true;
  lock.Unlock();

  pthread_join(print_thread, NULL);

  double bandwidth;
  bandwidth = ((double)data.finished)*((double)data.object_size)/(double)timePassed;
  bandwidth = bandwidth/(1024*1024); // we want it in MB/sec
  char bw[20];
  snprintf(bw, sizeof(bw), "%.3lf \n", bandwidth);

  cout << "Total time run:        " << timePassed << std::endl
       << "Total writes made:     " << data.finished << std::endl
       << "Write size:            " << data.object_size << std::endl
       << "Bandwidth (MB/sec):    " << bw << std::endl
       << "Average Latency:       " << data.avg_latency << std::endl
       << "Max latency:           " << data.max_latency << std::endl
       << "Min latency:           " << data.min_latency << std::endl;

  //write object size/number data for read benchmarks
  ::encode(data.object_size, b_write);
  ::encode(data.finished, b_write);
  ::encode(getpid(), b_write);
  sync_write(BENCH_DATA, b_write, sizeof(int)*3);

  completions_done();

  return 0;

 ERR:
  lock.Lock();
  data.done = 1;
  lock.Unlock();
  pthread_join(print_thread, NULL);
  return -5;
}

int ObjBencher::seq_read_bench(int seconds_to_run, int num_objects, int concurrentios, int pid) {
  data.finished = 0;

  lock_cond lc(&lock);
  char* name[concurrentios];
  bufferlist* contents[concurrentios];
  int index[concurrentios];
  int errors = 0;
  utime_t start_time;
  utime_t start_times[concurrentios];
  utime_t time_to_run;
  time_to_run.set_from_double(seconds_to_run);
  double total_latency = 0;
  int r = 0;
  utime_t runtime;
  sanitize_object_contents(&data, 128); //clean it up once; subsequent
  //changes will be safe because string length monotonically increases

  r = completions_init(concurrentios);
  if (r < 0)
    return r;

  //set up initial reads
  for (int i = 0; i < concurrentios; ++i) {
    name[i] = new char[128];
    generate_object_name(name[i], 128, i, pid);
    contents[i] = new bufferlist();
  }

  pthread_t print_thread;
  pthread_create(&print_thread, NULL, status_printer, (void *)this);

  lock.Lock();
  data.start_time = ceph_clock_now(g_ceph_context);
  lock.Unlock();
  utime_t finish_time = data.start_time + time_to_run;
  //start initial reads
  for (int i = 0; i < concurrentios; ++i) {
    index[i] = i;
    start_times[i] = ceph_clock_now(g_ceph_context);
    create_completion(i, _aio_cb, (void *)&lc);
    r = aio_read(name[i], i, contents[i], data.object_size);
    if (r < 0) { //naughty, doesn't clean up heap -- oh, or handle the print thread!
      cerr << "r = " << r << std::endl;
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
  }

  //keep on adding new reads as old ones complete
  int slot;
  char* newName;
  bufferlist *cur_contents;

  while (seconds_to_run && (ceph_clock_now(g_ceph_context) < finish_time) &&
      num_objects > data.started) {
    lock.Lock();
    while (1) {
      for (slot = 0; slot < concurrentios; ++slot) {
	if (completion_is_done(slot)) {
	  break;
	}
      }
      if (slot < concurrentios) {
	break;
      }
      lc.cond.Wait(lock);
    }
    lock.Unlock();
    newName = new char[128];
    generate_object_name(newName, 128, data.started, pid);
    int current_index = index[slot];
    index[slot] = data.started;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0) {
      cerr << "read got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(g_ceph_context) - start_times[slot];
    total_latency += data.cur_latency;
    if( data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);
    cur_contents = contents[slot];

    //start new read and check data if requested
    start_times[slot] = ceph_clock_now(g_ceph_context);
    contents[slot] = new bufferlist();
    create_completion(slot, _aio_cb, (void *)&lc);
    r = aio_read(newName, slot, contents[slot], data.object_size);
    if (r < 0) {
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    snprintf(data.object_contents, data.object_size, "I'm the %dth object!", current_index);
    lock.Unlock();
    if (memcmp(data.object_contents, cur_contents->c_str(), data.object_size) != 0) {
      cerr << name[slot] << " is not correct!" << std::endl;
      ++errors;
    }
    delete name[slot];
    name[slot] = newName;
    delete cur_contents;
  }

  //wait for final reads to complete
  while (data.finished < data.started) {
    slot = data.finished % concurrentios;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0) {
      cerr << "read got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(g_ceph_context) - start_times[slot];
    total_latency += data.cur_latency;
    if (data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    release_completion(slot);
    snprintf(data.object_contents, data.object_size, "I'm the %dth object!", index[slot]);
    lock.Unlock();
    if (memcmp(data.object_contents, contents[slot]->c_str(), data.object_size) != 0) {
      cerr << name[slot] << " is not correct!" << std::endl;
      ++errors;
    }
    delete name[slot];
    delete contents[slot];
  }

  runtime = ceph_clock_now(g_ceph_context) - data.start_time;
  lock.Lock();
  data.done = true;
  lock.Unlock();

  pthread_join(print_thread, NULL);

  double bandwidth;
  bandwidth = ((double)data.finished)*((double)data.object_size)/(double)runtime;
  bandwidth = bandwidth/(1024*1024); // we want it in MB/sec
  char bw[20];
  snprintf(bw, sizeof(bw), "%.3lf \n", bandwidth);

  cout << "Total time run:        " << runtime << std::endl
       << "Total reads made:     " << data.finished << std::endl
       << "Read size:            " << data.object_size << std::endl
       << "Bandwidth (MB/sec):    " << bw << std::endl
       << "Average Latency:       " << data.avg_latency << std::endl
       << "Max latency:           " << data.max_latency << std::endl
       << "Min latency:           " << data.min_latency << std::endl;

  completions_done();

  return 0;

 ERR:
  lock.Lock();
  data.done = 1;
  lock.Unlock();
  pthread_join(print_thread, NULL);
  return -5;
}


