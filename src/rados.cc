// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "include/types.h"

#include "include/rados/librados.hpp"
using namespace librados;

#include "osdc/rados_bencher.h"

#include "common/config.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "common/Cond.h"
#include "common/debug.h"
#include "common/Formatter.h"
#include "mds/inode_backtrace.h"
#include "auth/Crypto.h"
#include <iostream>
#include <fstream>

#include <stdlib.h>
#include <time.h>
#include <sstream>
#include <errno.h>
#include <dirent.h>
#include <stdexcept>

#include "common/errno.h"

int rados_tool_sync(const std::map < std::string, std::string > &opts,
                             std::vector<const char*> &args);

#define STR(x) #x

void usage(ostream& out)
{
  out <<					\
"usage: rados [options] [commands]\n"
"POOL COMMANDS\n"
"   lspools                         list pools\n"
"   mkpool <pool-name> [123[ 4]]     create pool <pool-name>'\n"
"                                    [with auid 123[and using crush rule 4]]\n"
"   rmpool <pool-name>               remove pool <pool-name>'\n"
"   mkpool <pool-name>               create the pool <pool-name>\n"
"   df                              show per-pool and total usage\n"
"   ls                               list objects in pool\n\n"
"   chown 123                        change the pool owner to auid 123\n"
"\n"
"OBJECT COMMANDS\n"
"   get <obj-name> [outfile]         fetch object\n"
"   put <obj-name> [infile]          write object\n"
"   create <obj-name> [category]     create object\n"
"   rm <obj-name>                    remove object\n"
"   listxattr <obj-name>\n"
"   getxattr <obj-name> attr\n"
"   setxattr <obj-name> attr val\n"
"   rmxattr <obj-name> attr\n"
"   stat objname                     stat the named object\n"
"   mapext <obj-name>\n"
"   lssnap                           list snaps\n"
"   mksnap <snap-name>               create snap <snap-name>\n"
"   rmsnap <snap-name>               remove snap <snap-name>\n"
"   rollback <obj-name> <snap-name>  roll back object to snap <snap-name>\n\n"
"   bench <seconds> write|seq|rand [-t concurrent_operations]\n"
"                                    default is 16 concurrent IOs and 4 MB ops\n"
"   load-gen [options]               generate load on the cluster\n"
"\n"
"IMPORT AND EXPORT\n"
"   import [options] <local-directory> <rados-pool>\n"
"       Upload <local-directory> to <rados-pool>\n"
"   export [options] rados-pool> <local-directory>\n"
"       Download <rados-pool> to <local-directory>\n"
"   options:\n"
"       -f / --force                 Copy everything, even if it hasn't changed.\n"
"       -d / --delete-after          After synchronizing, delete unreferenced\n"
"                                    files or objects from the target bucket\n"
"                                    or directory.\n"
"       --workers                    Number of worker threads to spawn (default "
STR(DEFAULT_NUM_RADOS_WORKER_THREADS) ")\n"
"\n"
"GLOBAL OPTIONS:\n"
"   --object_locator object_locator\n"
"        set object_locator for operation"
"   -p pool\n"
"   --pool=pool\n"
"        select given pool by name\n"
"   -b op_size\n"
"        set the size of write ops for put or benchmarking"
"   -s name\n"
"   --snap name\n"
"        select given snap name for (read) IO\n"
"   -i infile\n"
"   -o outfile\n"
"        specify input or output file (for certain commands)\n"
"   --create\n"
"        create the pool or directory that was specified\n"
"\n"
"LOAD GEN OPTIONS:\n"
"   --num-objects                    total number of objects\n"
"   --min-object-size                min object size\n"
"   --max-object-size                max object size\n"
"   --min-ops                        min number of operations\n"
"   --max-ops                        max number of operations\n"
"   --max-backlog                    max backlog (in MB)\n"
"   --percent                        percent of operations that are read\n"
"   --target-throughput              target throughput (in MB)\n"
"   --run-length                     total time (in seconds)\n";


}

static void usage_exit()
{
  usage(cerr);
  exit(1);
}

static int do_get(IoCtx& io_ctx, const char *objname, const char *outfile, bool check_stdio)
{
  string oid(objname);
  bufferlist outdata;
  int ret = io_ctx.read(oid, outdata, 0, 0);
  if (ret < 0) {
    return ret;
  }

  if (check_stdio && strcmp(outfile, "-") == 0) {
    fwrite(outdata.c_str(), outdata.length(), 1, stdout);
  } else {
    outdata.write_file(outfile);
    generic_dout(0) << "wrote " << outdata.length() << " byte payload to " << outfile << dendl;
  }

  return 0;
}

static int do_put(IoCtx& io_ctx, const char *objname, const char *infile, int op_size, bool check_stdio)
{
  string oid(objname);
  bufferlist indata;
  bool stdio = false;
  if (check_stdio && strcmp(infile, "-") == 0)
    stdio = true;

  if (stdio) {
    char buf[256];
    while(!cin.eof()) {
      cin.getline(buf, 256);
      indata.append(buf);
      indata.append('\n');
    }
  } else {
    int ret, fd = open(infile, O_RDONLY);
    if (fd < 0) {
      char buf[80];
      cerr << "error reading input file " << infile << ": " << strerror_r(errno, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    char *buf = new char[op_size];
    int count = op_size;
    uint64_t offset = 0;
    while (count == op_size) {
      count = read(fd, buf, op_size);
      if (count == 0) {
        if (!offset) {
          int ret = io_ctx.create(oid, true);
          if (ret < 0)
            cerr << "WARNING: could not create object: " << oid << std::endl;
        }
        continue;
      }
      indata.append(buf, count);
      if (offset == 0)
	ret = io_ctx.write_full(oid, indata);
      else
	ret = io_ctx.write(oid, indata, count, offset);
      indata.clear();

      if (ret < 0) {
        close(fd);
        return ret;
      }
      offset += count;
    }
    close(fd);
  }
  return 0;
}

class RadosWatchCtx : public librados::WatchCtx {
  string name;
public:
  RadosWatchCtx(const char *imgname) : name(imgname) {}
  virtual ~RadosWatchCtx() {}
  virtual void notify(uint8_t opcode, uint64_t ver, bufferlist& bl) {
    string s;
    try {
      bufferlist::iterator iter = bl.begin();
      ::decode(s, iter);
    } catch (buffer::error *err) {
      cout << "could not decode bufferlist, buffer length=" << bl.length() << std::endl;
    }
    cout << name << " got notification opcode=" << (int)opcode << " ver=" << ver << " msg='" << s << "'" << std::endl;
  }
};

static const char alphanum_table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int gen_rand_alphanumeric(char *dest, int size) /* size should be the required string size + 1 */
{
  int ret = get_random_bytes(dest, size);
  if (ret < 0) {
    cerr << "cannot get random bytes: " << cpp_strerror(-ret) << std::endl;
    return -1;
  }

  int i;
  for (i=0; i<size - 1; i++) {
    int pos = (unsigned)dest[i];
    dest[i] = alphanum_table[pos & 63];
  }
  dest[i] = '\0';

  return 0;
}

struct obj_info {
  string name;
  size_t len;
};

uint64_t get_random(uint64_t min_val, uint64_t max_val)
{
  uint64_t r;
  get_random_bytes((char *)&r, sizeof(r));
  r = min_val + r % (max_val - min_val + 1);
  return r;
}

class LoadGen {
  size_t total_sent;
  size_t total_completed;

  IoCtx io_ctx;
  Rados *rados;

  map<int, obj_info> objs;

  utime_t start_time;

  bool going_down;

public:
  int read_percent;
  int num_objs;
  size_t min_obj_len;
  uint64_t max_obj_len;
  size_t min_op_len;
  size_t max_op_len;
  size_t max_ops;
  size_t max_backlog;
  size_t target_throughput;
  int run_length;

  enum {
    OP_READ,
    OP_WRITE,
  };

  struct LoadGenOp {
    int id;
    int type;
    string oid;
    size_t off;
    size_t len;
    bufferlist bl;
    LoadGen *lg;
    librados::AioCompletion *completion;

    LoadGenOp() {}
    LoadGenOp(LoadGen *_lg) : lg(_lg), completion(NULL) {}
  };

  int max_op;

  map<int, LoadGenOp *> pending_ops;

  void gen_op(LoadGenOp *op);
  uint64_t gen_next_op();
  void run_op(LoadGenOp *op);

  uint64_t cur_sent_rate() {
    return total_sent / time_passed();
  }

  uint64_t cur_completed_rate() {
    return total_completed / time_passed();
  }

  uint64_t total_expected() {
    return target_throughput * time_passed();
  }

  float time_passed() {
    utime_t now = ceph_clock_now(g_ceph_context);
    now -= start_time;
    uint64_t ns = now.nsec();
    float total = ns / 1000000000;
    total += now.sec();
    return total;
  }

  Mutex lock;
  Cond cond;

  LoadGen(Rados *_rados) : rados(_rados), going_down(false), lock("LoadGen") {
    read_percent = 80;
    min_obj_len = 1024;
    max_obj_len = 5ull * 1024ull * 1024ull * 1024ull;
    min_op_len = 1024;
    target_throughput = 5 * 1024 * 1024; // B/sec
    max_op_len = 2 * 1024 * 1024;
    max_backlog = target_throughput * 2;
    run_length = 60;

    total_sent = 0;
    total_completed = 0;
    num_objs = 200;
    max_op = 16;
  }
  int bootstrap(const char *pool);
  int run();
  void cleanup();

  void io_cb(completion_t c, LoadGenOp *op) {
    total_completed += op->len;

    Mutex::Locker l(lock);

    double rate = (double)cur_completed_rate() / (1024 * 1024);
    cout.precision(3);
    cout << "op " << op->id << " completed, throughput=" << rate  << "MB/sec" << std::endl;

    map<int, LoadGenOp *>::iterator iter = pending_ops.find(op->id);
    if (iter != pending_ops.end())
      pending_ops.erase(iter);

    if (!going_down)
      op->completion->release();

    delete op;

    cond.Signal();
  }
};

static void _load_gen_cb(completion_t c, void *param)
{
  LoadGen::LoadGenOp *op = (LoadGen::LoadGenOp *)param;
  op->lg->io_cb(c, op);
}

int LoadGen::bootstrap(const char *pool)
{
  char buf[128];
  int i;

  if (!pool) {
    cerr << "ERROR: pool name was not specified" << std::endl;
    return -EINVAL;
  }

  int ret = rados->ioctx_create(pool, io_ctx);
  if (ret < 0) {
    cerr << "error opening pool " << pool << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
    return ret;
  }

  int buf_len = 1;
  bufferptr p = buffer::create(buf_len);
  bufferlist bl;
  memset(p.c_str(), 0, buf_len);
  bl.push_back(p);

  list<librados::AioCompletion *> completions;
  for (i = 0; i < num_objs; i++) {
    obj_info info;
    gen_rand_alphanumeric(buf, 16);
    info.name = "obj-";
    info.name.append(buf);
    info.len = get_random(min_obj_len, max_obj_len);

    // throttle...
    while (completions.size() > max_ops) {
      AioCompletion *c = completions.front();
      c->wait_for_complete();
      ret = c->get_return_value();
      c->release();
      completions.pop_front();
      if (ret < 0) {
	cerr << "aio_write failed" << std::endl;
	return ret;
      }
    }

    librados::AioCompletion *c = rados->aio_create_completion(NULL, NULL, NULL);
    completions.push_back(c);
    // generate object
    ret = io_ctx.aio_write(info.name, c, bl, buf_len, info.len - buf_len);
    if (ret < 0) {
      cerr << "couldn't write obj: " << info.name << " ret=" << ret << std::endl;
      return ret;
    }
    objs[i] = info;
  }

  list<librados::AioCompletion *>::iterator iter;
  for (iter = completions.begin(); iter != completions.end(); ++iter) {
    AioCompletion *c = *iter;
    c->wait_for_complete();
    ret = c->get_return_value();
    c->release();
    if (ret < 0) { // yes, we leak.
      cerr << "aio_write failed" << std::endl;
      return ret;
    }
  }
  return 0;
}

void LoadGen::run_op(LoadGenOp *op)
{
  op->completion = rados->aio_create_completion(op, _load_gen_cb, NULL);

  switch (op->type) {
  case OP_READ:
    io_ctx.aio_read(op->oid, op->completion, &op->bl, op->len, op->off);
    break;
  case OP_WRITE:
    bufferptr p = buffer::create(op->len);
    memset(p.c_str(), 0, op->len);
    op->bl.push_back(p);
    
    io_ctx.aio_write(op->oid, op->completion, op->bl, op->len, op->off);
    break;
  }

  total_sent += op->len;
}

void LoadGen::gen_op(LoadGenOp *op)
{
  int i = get_random(0, objs.size() - 1);
  obj_info& info = objs[i];
  op->oid = info.name;

  size_t len = get_random(min_op_len, max_op_len);
  if (len > info.len)
    len = info.len;
  size_t off = get_random(0, info.len);

  if (off + len > info.len)
    off = info.len - len;

  op->off = off;
  op->len = len;

  i = get_random(1, 100);
  if (i > read_percent)
    op->type = OP_WRITE;
  else
    op->type = OP_READ;

  cout << (op->type == OP_READ ? "READ" : "WRITE") << " : oid=" << op->oid << " off=" << op->off << " len=" << op->len << std::endl;
}

uint64_t LoadGen::gen_next_op()
{
  lock.Lock();

  LoadGenOp *op = new LoadGenOp(this);
  gen_op(op);
  op->id = max_op++;
  pending_ops[op->id] = op;

  lock.Unlock();

  run_op(op);

  return op->len;
}

int LoadGen::run()
{
  start_time = ceph_clock_now(g_ceph_context);
  utime_t end_time = start_time;
  end_time += run_length;
  utime_t stamp_time = start_time;
  uint32_t total_sec = 0;

  while (1) {
    lock.Lock();
    utime_t one_second(1, 0);
    cond.WaitInterval(g_ceph_context, lock, one_second);
    lock.Unlock();
    utime_t now = ceph_clock_now(g_ceph_context);

    if (now > end_time)
      break;

    uint64_t expected = total_expected();  
    lock.Lock();
    uint64_t sent = total_sent;
    uint64_t completed = total_completed;
    lock.Unlock();

    if (now - stamp_time >= utime_t(1, 0)) {
      double rate = (double)cur_completed_rate() / (1024 * 1024);
      ++total_sec;
      cout.precision(3);
      cout << setw(5) << total_sec << ": throughput=" << rate  << "MB/sec" << " pending data=" << sent - completed << std::endl;
      stamp_time = now; 
    }

    while (sent < expected &&
           sent - completed < max_backlog &&
	   pending_ops.size() < max_ops) {
      sent += gen_next_op();
    }
  }

  // get a reference to all pending requests
  vector<librados::AioCompletion *> completions;
  lock.Lock();
  going_down = true;
  map<int, LoadGenOp *>::iterator iter;
  for (iter = pending_ops.begin(); iter != pending_ops.end(); ++iter) {
    LoadGenOp *op = iter->second;
    completions.push_back(op->completion);
  }
  lock.Unlock();

  cout << "waiting for all operations to complete" << std::endl;

  // now wait on all the pending requests
  vector<librados::AioCompletion *>::iterator citer;
  for (citer = completions.begin(); citer != completions.end(); citer++) {
    librados::AioCompletion *c = *citer;
    c->wait_for_complete();
    c->release();
  }

  return 0;
}

void LoadGen::cleanup()
{
  cout << "cleaning up objects" << std::endl;
  map<int, obj_info>::iterator iter;
  for (iter = objs.begin(); iter != objs.end(); ++iter) {
    obj_info& info = iter->second;
    int ret = io_ctx.remove(info.name);
    if (ret < 0)
      cerr << "couldn't remove obj: " << info.name << " ret=" << ret << std::endl;
  }
}

/**********************************************

**********************************************/
static int rados_tool_common(const std::map < std::string, std::string > &opts,
                             std::vector<const char*> &nargs)
{
  int ret;
  bool create_pool = false;
  const char *pool_name = NULL;
  string oloc;
  int concurrent_ios = 16;
  int op_size = 1 << 22;
  const char *snapname = NULL;
  snap_t snapid = CEPH_NOSNAP;
  std::map<std::string, std::string>::const_iterator i;
  std::string category;

  uint64_t min_obj_len = 0;
  uint64_t max_obj_len = 0;
  uint64_t min_op_len = 0;
  uint64_t max_op_len = 0;
  uint64_t max_ops = 0;
  uint64_t max_backlog = 0;
  uint64_t target_throughput = 0;
  int64_t read_percent = -1;
  uint64_t num_objs = 0;
  int run_length = 0;

  Formatter *formatter = NULL;
  bool pretty_format = false;

  i = opts.find("create");
  if (i != opts.end()) {
    create_pool = true;
  }
  i = opts.find("pool");
  if (i != opts.end()) {
    pool_name = i->second.c_str();
  }
  i = opts.find("object_locator");
  if (i != opts.end()) {
    oloc = i->second;
  }
  i = opts.find("category");
  if (i != opts.end()) {
    category = i->second;
  }
  i = opts.find("concurrent-ios");
  if (i != opts.end()) {
    concurrent_ios = strtol(i->second.c_str(), NULL, 10);
  }
  i = opts.find("block-size");
  if (i != opts.end()) {
    op_size = strtol(i->second.c_str(), NULL, 10);
  }
  i = opts.find("snap");
  if (i != opts.end()) {
    snapname = i->second.c_str();
  }
  i = opts.find("snapid");
  if (i != opts.end()) {
    snapid = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("min-object-size");
  if (i != opts.end()) {
    min_obj_len = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("max-object-size");
  if (i != opts.end()) {
    max_obj_len = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("min-op-len");
  if (i != opts.end()) {
    min_op_len = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("max-op-len");
  if (i != opts.end()) {
    max_op_len = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("max-ops");
  if (i != opts.end()) {
    max_ops = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("max-backlog");
  if (i != opts.end()) {
    max_backlog = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("target-throughput");
  if (i != opts.end()) {
    target_throughput = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("read-percent");
  if (i != opts.end()) {
    read_percent = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("num-objects");
  if (i != opts.end()) {
    num_objs = strtoll(i->second.c_str(), NULL, 10);
  }
  i = opts.find("run-length");
  if (i != opts.end()) {
    run_length = strtol(i->second.c_str(), NULL, 10);
  }
  i = opts.find("pretty-format");
  if (i != opts.end()) {
    pretty_format = true;
  }
  i = opts.find("format");
  if (i != opts.end()) {
    const char *format = i->second.c_str();
    if (strcmp(format, "xml") == 0)
      formatter = new XMLFormatter(pretty_format);
    else if (strcmp(format, "json") == 0)
      formatter = new JSONFormatter(pretty_format);
    else {
      cerr << "unrecognized format: " << format << std::endl;
      return -EINVAL;
    }
  }


  // open rados
  Rados rados;
  ret = rados.init_with_context(g_ceph_context);
  if (ret) {
     cerr << "couldn't initialize rados! error " << ret << std::endl;
     return ret;
  }

  ret = rados.connect();
  if (ret) {
     cerr << "couldn't connect to cluster! error " << ret << std::endl;
     return ret;
  }
  char buf[80];

  if (create_pool && !pool_name) {
    cerr << "--create-pool requested but pool_name was not specified!" << std::endl;
    usage_exit();
  }

  if (create_pool) {
    ret = rados.pool_create(pool_name, 0, 0);
    if (ret < 0) {
      cerr << "error creating pool " << pool_name << ": "
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }

  // open io context.
  IoCtx io_ctx;
  if (pool_name) {
    ret = rados.ioctx_create(pool_name, io_ctx);
    if (ret < 0) {
      cerr << "error opening pool " << pool_name << ": "
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }

  // snapname?
  if (snapname) {
    ret = io_ctx.snap_lookup(snapname, &snapid);
    if (ret < 0) {
      cerr << "error looking up snap '" << snapname << "': " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  if (oloc.size()) {
    io_ctx.locator_set_key(oloc);
  }
  if (snapid != CEPH_NOSNAP) {
    string name;
    ret = io_ctx.snap_get_name(snapid, &name);
    if (ret < 0) {
      cerr << "snapid " << snapid << " doesn't exist in pool "
	   << io_ctx.get_pool_name() << std::endl;
      return 1;
    }
    io_ctx.snap_set_read(snapid);
    cout << "selected snap " << snapid << " '" << snapname << "'" << std::endl;
  }

  assert(!nargs.empty());

  // list pools?
  if (strcmp(nargs[0], "lspools") == 0) {
    list<string> vec;
    rados.pool_list(vec);
    for (list<string>::iterator i = vec.begin(); i != vec.end(); ++i)
      cout << *i << std::endl;
  }
  else if (strcmp(nargs[0], "df") == 0) {
    // pools
    list<string> vec;

    if (!pool_name)
      rados.pool_list(vec);
    else
      vec.push_back(pool_name);

    map<string, map<string, pool_stat_t> > stats;
    rados.get_pool_stats(vec, category, stats);

    if (!formatter) {
      printf("%-15s %-15s"
	     "%12s %12s %12s %12s "
	     "%12s %12s %12s %12s %12s\n",
	     "pool name",
	     "category",
	     "KB", "objects", "clones", "degraded",
	     "unfound", "rd", "rd KB", "wr", "wr KB");
    } else {
      formatter->open_object_section("stats");
      formatter->open_array_section("pools");
    }
    for (map<string, librados::stats_map>::iterator c = stats.begin(); c != stats.end(); ++c) {
      const char *pool_name = c->first.c_str();
      stats_map& m = c->second;
      if (formatter) {
        formatter->open_object_section("pool");
        int64_t pool_id = rados.pool_lookup(pool_name);
        formatter->dump_string("name", pool_name);
        if (pool_id >= 0)
          formatter->dump_format("id", "%lld", pool_id);
        else
          cerr << "ERROR: lookup_pg_pool_name for name=" << pool_name << " returned " << pool_id << std::endl;
        formatter->open_array_section("categories");
      }
      for (stats_map::iterator i = m.begin(); i != m.end(); ++i) {
        const char *category = (i->first.size() ? i->first.c_str() : "");
        pool_stat_t& s = i->second;
        if (!formatter) {
          if (!*category)
            category = "-";
          printf("%-15s "
                 "%-15s "
	         "%12lld %12lld %12lld %12lld"
	         "%12lld %12lld %12lld %12lld %12lld\n",
	         pool_name,
                 category,
	         (long long)s.num_kb,
	         (long long)s.num_objects,
	         (long long)s.num_object_clones,
	         (long long)s.num_objects_degraded,
	         (long long)s.num_objects_unfound,
	         (long long)s.num_rd, (long long)s.num_rd_kb,
	         (long long)s.num_wr, (long long)s.num_wr_kb);
        } else {
          formatter->open_object_section("category");
          if (category)
            formatter->dump_string("name", category);
          formatter->dump_format("size_bytes", "%lld", s.num_bytes);
          formatter->dump_format("size_kb", "%lld", s.num_kb);
          formatter->dump_format("num_objects", "%lld", s.num_objects);
          formatter->dump_format("num_object_clones", "%lld", s.num_object_clones);
          formatter->dump_format("num_object_copies", "%lld", s.num_object_copies);
          formatter->dump_format("num_objects_missing_on_primary", "%lld", s.num_objects_missing_on_primary);
          formatter->dump_format("num_objects_unfound", "%lld", s.num_objects_unfound);
          formatter->dump_format("num_objects_degraded", "%lld", s.num_objects_degraded);
          formatter->dump_format("read_bytes", "%lld", s.num_rd);
          formatter->dump_format("read_kb", "%lld", s.num_rd_kb);
          formatter->dump_format("write_bytes", "%lld", s.num_wr);
          formatter->dump_format("write_kb", "%lld", s.num_wr_kb);
          formatter->flush(cout);
        }
        if (formatter) {
          formatter->close_section();
        }
      }
      if (formatter) {
        formatter->close_section();
        formatter->close_section();
        formatter->flush(cout);
      }
    }

    // total
    cluster_stat_t tstats;
    rados.cluster_stat(tstats);
    if (!formatter) {
      printf("  total used    %12lld %12lld\n", (long long unsigned)tstats.kb_used,
	     (long long unsigned)tstats.num_objects);
      printf("  total avail   %12lld\n", (long long unsigned)tstats.kb_avail);
      printf("  total space   %12lld\n", (long long unsigned)tstats.kb);
    } else {
      formatter->close_section();
      formatter->dump_format("total_objects", "%lld", (long long unsigned)tstats.num_objects);
      formatter->dump_format("total_used", "%lld", (long long unsigned)tstats.kb_used);
      formatter->dump_format("total_avail", "%lld", (long long unsigned)tstats.kb_avail);
      formatter->dump_format("total_space", "%lld", (long long unsigned)tstats.kb);
      formatter->close_section();
      formatter->flush(cout);
    }
  }

  else if (strcmp(nargs[0], "ls") == 0) {
    if (!pool_name) {
      cerr << "pool name was not specified" << std::endl;
      return 1;
    }

    bool stdout = (nargs.size() < 2) || (strcmp(nargs[1], "-") == 0);
    ostream *outstream;
    if(stdout)
      outstream = &cout;
    else
      outstream = new ofstream(nargs[1]);

    {
      try {
	librados::ObjectIterator i = io_ctx.objects_begin();
	librados::ObjectIterator i_end = io_ctx.objects_end();
	for (; i != i_end; ++i) {
	  if (i->second.size())
	    *outstream << i->first << "\t" << i->second << std::endl;
	  else
	    *outstream << i->first << std::endl;
	}
      }
      catch (const std::runtime_error& e) {
	cerr << e.what() << std::endl;
	return 1;
      }
    }
    if (!stdout)
      delete outstream;
  }
  else if (strcmp(nargs[0], "chown") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();

    uint64_t new_auid = strtol(nargs[1], 0, 10);
    ret = io_ctx.set_auid(new_auid);
    if (ret < 0) {
      cerr << "error changing auid on pool " << io_ctx.get_pool_name() << ':'
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
    } else cerr << "changed auid on pool " << io_ctx.get_pool_name()
		<< " to " << new_auid << std::endl;
  }
  else if (strcmp(nargs[0], "mapext") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();
    string oid(nargs[1]);
    std::map<uint64_t,uint64_t> m;
    ret = io_ctx.mapext(oid, 0, -1, m);
    if (ret < 0) {
      cerr << "mapext error on " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    std::map<uint64_t,uint64_t>::iterator iter;
    for (iter = m.begin(); iter != m.end(); ++iter) {
      cout << hex << iter->first << "\t" << iter->second << dec << std::endl;
    }
  }
  else if (strcmp(nargs[0], "stat") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();
    string oid(nargs[1]);
    uint64_t size;
    time_t mtime;
    ret = io_ctx.stat(oid, &size, &mtime);
    if (ret < 0) {
      cerr << " error stat-ing " << pool_name << "/" << oid << ": "
           << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    } else {
      cout << pool_name << "/" << oid
           << " mtime " << mtime << ", size " << size << std::endl;
    }
  }
  else if (strcmp(nargs[0], "get") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage_exit();
    ret = do_get(io_ctx, nargs[1], nargs[2], true);
    if (ret < 0) {
      cerr << "error getting " << pool_name << "/" << nargs[1] << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  else if (strcmp(nargs[0], "put") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage_exit();
    ret = do_put(io_ctx, nargs[1], nargs[2], op_size, true);
    if (ret < 0) {
      cerr << "error putting " << pool_name << "/" << nargs[1] << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  else if (strcmp(nargs[0], "setxattr") == 0) {
    if (!pool_name || nargs.size() < 4)
      usage_exit();

    string oid(nargs[1]);
    string attr_name(nargs[2]);
    string attr_val(nargs[3]);

    bufferlist bl;
    bl.append(attr_val.c_str(), attr_val.length());

    ret = io_ctx.setxattr(oid, attr_name.c_str(), bl);
    if (ret < 0) {
      cerr << "error setting xattr " << pool_name << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    else
      ret = 0;
  }
  else if (strcmp(nargs[0], "getxattr") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage_exit();

    string oid(nargs[1]);
    string attr_name(nargs[2]);

    bufferlist bl;
    ret = io_ctx.getxattr(oid, attr_name.c_str(), bl);
    if (ret < 0) {
      cerr << "error getting xattr " << pool_name << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    else
      ret = 0;
    string s(bl.c_str(), bl.length());
    cout << s << std::endl;
  } else if (strcmp(nargs[0], "rmxattr") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage_exit();

    string oid(nargs[1]);
    string attr_name(nargs[2]);

    ret = io_ctx.rmxattr(oid, attr_name.c_str());
    if (ret < 0) {
      cerr << "error removing xattr " << pool_name << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  } else if (strcmp(nargs[0], "listxattr") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();

    string oid(nargs[1]);
    map<std::string, bufferlist> attrset;
    bufferlist bl;
    ret = io_ctx.getxattrs(oid, attrset);
    if (ret < 0) {
      cerr << "error getting xattr set " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }

    for (map<std::string, bufferlist>::iterator iter = attrset.begin();
         iter != attrset.end(); ++iter) {
      cout << iter->first << std::endl;
    }
  }
  else if (strcmp(nargs[0], "rm") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();
    string oid(nargs[1]);
    ret = io_ctx.remove(oid);
    if (ret < 0) {
      cerr << "error removing " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  else if (strcmp(nargs[0], "create") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();
    string oid(nargs[1]);
    if (nargs.size() > 2) {
      string category(nargs[2]);
      ret = io_ctx.create(oid, true, category);
    } else {
      ret = io_ctx.create(oid, true);
    }
    if (ret < 0) {
      cerr << "error creating " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }

  else if (strcmp(nargs[0], "tmap") == 0) {
    if (nargs.size() < 3)
      usage_exit();
    if (strcmp(nargs[1], "dump") == 0) {
      bufferlist outdata;
      string oid(nargs[2]);
      ret = io_ctx.read(oid, outdata, 0, 0);
      if (ret < 0) {
	cerr << "error reading " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
	return 1;
      }
      bufferlist::iterator p = outdata.begin();
      bufferlist header;
      map<string, bufferlist> kv;
      ::decode(header, p);
      ::decode(kv, p);
      cout << "header (" << header.length() << " bytes):\n";
      header.hexdump(cout);
      cout << "\n";
      cout << kv.size() << " keys\n";
      for (map<string,bufferlist>::iterator q = kv.begin(); q != kv.end(); q++) {
	cout << "key '" << q->first << "' (" << q->second.length() << " bytes):\n";
	q->second.hexdump(cout);
	cout << "\n";
      }
    }
    else if (strcmp(nargs[1], "set") == 0 ||
	     strcmp(nargs[1], "create") == 0) {
      if (nargs.size() < 5)
	usage_exit();
      string oid(nargs[2]);
      string k(nargs[3]);
      string v(nargs[4]);
      bufferlist bl;
      char c = (strcmp(nargs[1], "set") == 0) ? CEPH_OSD_TMAP_SET : CEPH_OSD_TMAP_CREATE;
      ::encode(c, bl);
      ::encode(k, bl);
      ::encode(v, bl);
      ret = io_ctx.tmap_update(oid, bl);
    }
  }

  else if (strcmp(nargs[0], "mkpool") == 0) {
    int auid = 0;
    __u8 crush_rule = 0;
    if (nargs.size() < 2)
      usage_exit();
    if (nargs.size() > 2) {
      auid = strtol(nargs[2], 0, 10);
      cerr << "setting auid:" << auid << std::endl;
      if (nargs.size() > 3) {
	crush_rule = (__u8)strtol(nargs[3], 0, 10);
	cerr << "using crush rule " << (int)crush_rule << std::endl;
      }
    }
    ret = rados.pool_create(nargs[1], auid, crush_rule);
    if (ret < 0) {
      cerr << "error creating pool " << nargs[1] << ": "
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "successfully created pool " << nargs[1] << std::endl;
  }
  else if (strcmp(nargs[0], "rmpool") == 0) {
    if (nargs.size() < 2)
      usage_exit();
    ret = rados.pool_delete(nargs[1]);
    if (ret >= 0) {
      cout << "successfully deleted pool " << nargs[1] << std::endl;
    } else { //error
      cerr << "pool " << nargs[1] << " does not exist" << std::endl;
    }
  }
  else if (strcmp(nargs[0], "lssnap") == 0) {
    if (!pool_name || nargs.size() != 1)
      usage_exit();

    vector<snap_t> snaps;
    io_ctx.snap_list(&snaps);
    for (vector<snap_t>::iterator i = snaps.begin();
	 i != snaps.end();
	 i++) {
      string s;
      time_t t;
      if (io_ctx.snap_get_name(*i, &s) < 0)
	continue;
      if (io_ctx.snap_get_stamp(*i, &t) < 0)
	continue;
      struct tm bdt;
      localtime_r(&t, &bdt);
      cout << *i << "\t" << s << "\t";

      cout.setf(std::ios::right);
      cout.fill('0');
      cout << std::setw(4) << (bdt.tm_year+1900)
	   << '.' << std::setw(2) << (bdt.tm_mon+1)
	   << '.' << std::setw(2) << bdt.tm_mday
	   << ' '
	   << std::setw(2) << bdt.tm_hour
	   << ':' << std::setw(2) << bdt.tm_min
	   << ':' << std::setw(2) << bdt.tm_sec
	   << std::endl;
      cout.unsetf(std::ios::right);
    }
    cout << snaps.size() << " snaps" << std::endl;
  }

  else if (strcmp(nargs[0], "mksnap") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();

    ret = io_ctx.snap_create(nargs[1]);
    if (ret < 0) {
      cerr << "error creating pool " << pool_name << " snapshot " << nargs[1]
	   << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "created pool " << pool_name << " snap " << nargs[1] << std::endl;
  }

  else if (strcmp(nargs[0], "rmsnap") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();

    ret = io_ctx.snap_remove(nargs[1]);
    if (ret < 0) {
      cerr << "error removing pool " << pool_name << " snapshot " << nargs[1]
	   << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "removed pool " << pool_name << " snap " << nargs[1] << std::endl;
  }

  else if (strcmp(nargs[0], "rollback") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage_exit();

    ret = io_ctx.rollback(nargs[1], nargs[2]);
    if (ret < 0) {
      cerr << "error rolling back pool " << pool_name << " to snapshot " << nargs[1]
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "rolled back pool " << pool_name
	 << " to snapshot " << nargs[2] << std::endl;
  }
  else if (strcmp(nargs[0], "bench") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage_exit();
    int seconds = atoi(nargs[1]);
    int operation = 0;
    if (strcmp(nargs[2], "write") == 0)
      operation = OP_WRITE;
    else if (strcmp(nargs[2], "seq") == 0)
      operation = OP_SEQ_READ;
    else if (strcmp(nargs[2], "rand") == 0)
      operation = OP_RAND_READ;
    else
      usage_exit();
    RadosBencher bencher(rados, io_ctx);
    ret = bencher.aio_bench(operation, seconds, concurrent_ios, op_size);
    if (ret != 0)
      cerr << "error during benchmark: " << ret << std::endl;
  }
  else if (strcmp(nargs[0], "watch") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage_exit();
    string oid(nargs[1]);
    RadosWatchCtx ctx(oid.c_str());
    uint64_t cookie;
    ret = io_ctx.watch(oid, 0, &cookie, &ctx);
    if (ret != 0)
      cerr << "error calling watch: " << ret << std::endl;
    else {
      cout << "press enter to exit..." << std::endl;
      getchar();
    }
  }
  else if (strcmp(nargs[0], "notify") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage_exit();
    string oid(nargs[1]);
    string msg(nargs[2]);
    bufferlist bl;
    ::encode(msg, bl);
    ret = io_ctx.notify(oid, 0, bl);
    if (ret != 0)
      cerr << "error calling notify: " << ret << std::endl;
  } else if (strcmp(nargs[0], "load-gen") == 0) {
    if (!pool_name) {
      cerr << "error: must specify pool" << std::endl;
      usage_exit();
    }
    LoadGen lg(&rados);
    if (min_obj_len)
      lg.min_obj_len = min_obj_len;
    if (max_obj_len)
      lg.max_obj_len = max_obj_len;
    if (min_op_len)
      lg.min_op_len = min_op_len;
    if (max_op_len)
      lg.max_op_len = max_op_len;
    if (max_ops)
      lg.max_ops = max_ops;
    if (max_backlog)
      lg.max_backlog = max_backlog;
    if (target_throughput)
      lg.target_throughput = target_throughput << 20;
    if (read_percent >= 0)
      lg.read_percent = read_percent;
    if (num_objs)
      lg.num_objs = num_objs;
    if (run_length)
      lg.run_length = run_length;

    cout << "run length " << run_length << " seconds" << std::endl;
    cout << "preparing " << lg.num_objs << " objects" << std::endl;
    ret = lg.bootstrap(pool_name);
    if (ret < 0) {
      cerr << "load-gen bootstrap failed" << std::endl;
      exit(1);
    }
    cout << "load-gen will run " << lg.run_length << " seconds" << std::endl;
    lg.run();
    lg.cleanup();
  }  else {
    cerr << "unrecognized command " << nargs[0] << std::endl;
    usage_exit();
  }

  if (ret)
    cerr << "error " << (-ret) << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
  return (ret < 0) ? 1 : 0;
}

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  std::map < std::string, std::string > opts;
  std::vector<const char*>::iterator i;
  std::string val;
  for (i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      usage(cout);
      exit(0);
    } else if (ceph_argparse_flag(args, i, "-f", "--force", (char*)NULL)) {
      opts["force"] = "true";
    } else if (ceph_argparse_flag(args, i, "-d", "--delete-after", (char*)NULL)) {
      opts["delete-after"] = "true";
    } else if (ceph_argparse_flag(args, i, "-C", "--create", "--create-pool",
				  (char*)NULL)) {
      opts["create"] = "true";
    } else if (ceph_argparse_flag(args, i, "--pretty-format", (char*)NULL)) {
      opts["pretty-format"] = "true";
    } else if (ceph_argparse_witharg(args, i, &val, "-p", "--pool", (char*)NULL)) {
      opts["pool"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--object-locator" , (char *)NULL)) {
      opts["object_locator"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--category", (char*)NULL)) {
      opts["category"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-t", "--concurrent-ios", (char*)NULL)) {
      opts["concurrent-ios"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--block-size", (char*)NULL)) {
      opts["block-size"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-b", (char*)NULL)) {
      opts["block-size"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-s", "--snap", (char*)NULL)) {
      opts["snap"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-S", "--snapid", (char*)NULL)) {
      opts["snapid"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--min-object-size", (char*)NULL)) {
      opts["min-object-size"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--max-object-size", (char*)NULL)) {
      opts["max-object-size"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--min-op-len", (char*)NULL)) {
      opts["min-op-len"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--max-op-len", (char*)NULL)) {
      opts["max-op-len"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--max-ops", (char*)NULL)) {
      opts["max-ops"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--max-backlog", (char*)NULL)) {
      opts["max-backlog"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--target-throughput", (char*)NULL)) {
      opts["target-throughput"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--read-percent", (char*)NULL)) {
      opts["read-percent"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--num-objects", (char*)NULL)) {
      opts["num-objects"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--run-length", (char*)NULL)) {
      opts["run-length"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--workers", (char*)NULL)) {
      opts["workers"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--format", (char*)NULL)) {
      opts["format"] = val;
    } else {
      if (val[0] == '-')
        usage_exit();
      i++;
    }
  }

  if (args.empty()) {
    cerr << "rados: you must give an action. Try --help" << std::endl;
    return 1;
  }
  if ((strcmp(args[0], "import") == 0) || (strcmp(args[0], "export") == 0))
    return rados_tool_sync(opts, args);
  else
    return rados_tool_common(opts, args);
}
