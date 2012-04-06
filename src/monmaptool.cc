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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "common/config.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "mon/MonMap.h"

void usage()
{
  cout << " usage: [--print] [--create [--clobber][--fsid uuid]] [--add name 1.2.3.4:567] [--rm name] <mapfilename>" << std::endl;
  exit(1);
}

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);

  const char *me = argv[0];

  std::string fn;
  bool print = false;
  bool create = false;
  bool clobber = false;
  bool modified = false;
  map<string,entity_addr_t> add;
  list<string> rm;

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY,
	      CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  std::string val;
  for (std::vector<const char*>::iterator i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      usage();
    } else if (ceph_argparse_flag(args, i, "-p", "--print", (char*)NULL)) {
      print = true;
    } else if (ceph_argparse_flag(args, i, "--create", (char*)NULL)) {
      create = true;
    } else if (ceph_argparse_flag(args, i, "--clobber", (char*)NULL)) {
      clobber = true;
    } else if (ceph_argparse_flag(args, i, "--add", (char*)NULL)) {
      string name = *i;
      i = args.erase(i);
      if (i == args.end())
	usage();
      entity_addr_t addr;
      if (!addr.parse(*i)) {
	cerr << me << ": invalid ip:port '" << *i << "'" << std::endl;
	return -1;
      }
      if (addr.get_port() == 0)
	addr.set_port(CEPH_MON_PORT);
      add[name] = addr;
      modified = true;
      i = args.erase(i);
    } else if (ceph_argparse_witharg(args, i, &val, "--rm", (char*)NULL)) {
      rm.push_back(val);
      modified = true;
    } else {
      ++i;
    }
  }
  if (args.size() < 1) {
    cerr << me << ": must specify monmap filename" << std::endl;
    usage();
  }
  else if (args.size() > 1) {
    cerr << me << ": too many arguments" << std::endl;
    usage();
  }
  fn = args[0];
  
  MonMap monmap;

  cout << me << ": monmap file " << fn << std::endl;

  int r = 0;
  if (!(create && clobber)) {
    try {
      r = monmap.read(fn.c_str());
    } catch (...) {
      cerr << me << ": unable to read monmap file" << std::endl;
      return -1;
    }
  }

  char buf[80];
  if (!create && r < 0) {
    cerr << me << ": couldn't open " << fn << ": " << strerror_r(-r, buf, sizeof(buf)) << std::endl;
    return -1;
  }    
  else if (create && !clobber && r == 0) {
    cerr << me << ": " << fn << " exists, --clobber to overwrite" << std::endl;
    return -1;
  }

  if (create) {
    monmap.epoch = 0;
    monmap.created = ceph_clock_now(g_ceph_context);
    monmap.last_changed = monmap.created;
    srand(getpid() + time(0));
    if (g_conf->fsid.is_zero()) {
      monmap.generate_fsid();
      cout << me << ": generated fsid " << monmap.fsid << std::endl;
    }
    modified = true;
  }
  if (!g_conf->fsid.is_zero()) {
    monmap.fsid = g_conf->fsid;
    cout << me << ": set fsid to " << monmap.fsid << std::endl;
    modified = true;
  }

  for (map<string,entity_addr_t>::iterator p = add.begin(); p != add.end(); p++) {
    if (monmap.contains(p->first)) {
      cerr << me << ": map already contains mon." << p->first << std::endl;
      usage();
    }
    if (monmap.contains(p->second)) {
      cerr << me << ": map already contains " << p->second << std::endl;
      usage();
    }
    monmap.add(p->first, p->second);
  }
  for (list<string>::iterator p = rm.begin(); p != rm.end(); p++) {
    cout << me << ": removing " << *p << std::endl;
    if (!monmap.contains(*p)) {
      cerr << me << ": map does not contain " << *p << std::endl;
      usage();
    }
    monmap.remove(*p);
  }

  if (!print && !modified)
    usage();

  if (modified && !create)
    monmap.epoch++;

  if (print) 
    monmap.print(cout);

  if (modified) {
    // write it out
    cout << me << ": writing epoch " << monmap.epoch
	 << " to " << fn
	 << " (" << monmap.size() << " monitors)" 
	 << std::endl;
    int r = monmap.write(fn.c_str());
    if (r < 0) {
      cerr << "monmaptool: error writing to '" << fn << "': " << strerror_r(-r, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  

  return 0;
}
