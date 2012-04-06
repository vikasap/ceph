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

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "common/config.h"
#include "include/ceph_features.h"

#include "mon/MonMap.h"
#include "mon/Monitor.h"
#include "mon/MonitorStore.h"
#include "mon/MonClient.h"

#include "msg/Messenger.h"

#include "include/CompatSet.h"

#include "common/ceph_argparse.h"
#include "common/pick_address.h"
#include "common/Timer.h"
#include "common/errno.h"

#include "global/global_init.h"
#include "global/signal_handler.h"

#define dout_subsys ceph_subsys_mon

extern CompatSet get_ceph_mon_feature_compat_set();

Monitor *mon = NULL;

void handle_mon_signal(int signum)
{
  if (mon)
    mon->handle_signal(signum);
}

void usage()
{
  cerr << "usage: ceph-mon -i monid [--mon-data=pathtodata] [flags]" << std::endl;
  cerr << "  --debug_mon n\n";
  cerr << "        debug monitor level (e.g. 10)\n";
  cerr << "  --mkfs\n";
  cerr << "        build fresh monitor fs\n";
  generic_server_usage();
}

int main(int argc, const char **argv) 
{
  int err;

  bool mkfs = false;
  std::string osdmapfn, inject_monmap;

  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_MON, CODE_ENVIRONMENT_DAEMON, 0);

  uuid_d fsid;
  std::string val;
  for (std::vector<const char*>::iterator i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      usage();
      exit(0);
    } else if (ceph_argparse_flag(args, i, "--mkfs", (char*)NULL)) {
      mkfs = true;
    } else if (ceph_argparse_witharg(args, i, &val, "--osdmap", (char*)NULL)) {
      osdmapfn = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--inject_monmap", (char*)NULL)) {
      inject_monmap = val;
    } else {
      ++i;
    }
  }
  if (!args.empty()) {
    cerr << "too many arguments: " << args << std::endl;
    usage();
  }

  if (g_conf->mon_data.empty()) {
    cerr << "must specify '--mon-data=foo' data path" << std::endl;
    usage();
  }

  // -- mkfs --
  if (mkfs) {
    // resolve public_network -> public_addr
    pick_addresses(g_ceph_context);

    common_init_finish(g_ceph_context);

    bufferlist monmapbl, osdmapbl;
    std::string error;
    MonMap monmap;

    // load or generate monmap
    if (g_conf->monmap.length()) {
      int err = monmapbl.read_file(g_conf->monmap.c_str(), &error);
      if (err < 0) {
	cerr << argv[0] << ": error reading " << g_conf->monmap << ": " << error << std::endl;
	exit(1);
      }
      try {
	monmap.decode(monmapbl);
      }
      catch (const buffer::error& e) {
	cerr << argv[0] << ": error decoding monmap " << g_conf->monmap << ": " << e.what() << std::endl;
	exit(1);
      }      
    } else {
      int err = MonClient::build_initial_monmap(g_ceph_context, monmap);
      if (err < 0) {
	cerr << argv[0] << ": error generating initial monmap: " << cpp_strerror(err) << std::endl;
	usage();
	exit(1);
      }

      // am i part of the initial quorum?
      if (monmap.contains(g_conf->name.get_id())) {
	// hmm, make sure the ip listed exists on the current host?
	// maybe later.
      } else if (!g_conf->public_addr.is_blank_ip()) {
	entity_addr_t a = g_conf->public_addr;
	if (a.get_port() == 0)
	  a.set_port(CEPH_MON_PORT);
	if (monmap.contains(a)) {
	  string name;
	  monmap.get_addr_name(a, name);
	  monmap.rename(name, g_conf->name.get_id());
	  cout << argv[0] << ": renaming mon." << name << " " << a
	       << " to mon." << g_conf->name.get_id() << std::endl;
	}
      } else {
	// is a local address listed without a name?  if so, name myself.
	list<entity_addr_t> ls;
	monmap.list_addrs(ls);
	entity_addr_t local;

	if (have_local_addr(g_ceph_context, ls, &local)) {
	  string name;
	  monmap.get_addr_name(local, name);

	  if (name.find("noname-") == 0) {
	    cout << argv[0] << ": mon." << name << " " << local
		 << " is local, renaming to mon." << g_conf->name.get_id() << std::endl;
	    monmap.rename(name, g_conf->name.get_id());
	  } else {
	    cout << argv[0] << ": mon." << name << " " << local
		 << " is local, but not 'noname-' + something; not assuming it's me" << std::endl;
	  }
	}
      }
    }

    if (!g_conf->fsid.is_zero()) {
      monmap.fsid = g_conf->fsid;
      cout << argv[0] << ": set fsid to " << g_conf->fsid << std::endl;
    }
    
    if (monmap.fsid.is_zero()) {
      cerr << argv[0] << ": generated monmap has no fsid; use '--fsid <uuid>'" << std::endl;
      exit(10);
    }

    //monmap.print(cout);

    // osdmap
    if (osdmapfn.length()) {
      err = osdmapbl.read_file(osdmapfn.c_str(), &error);
      if (err < 0) {
	cerr << argv[0] << ": error reading " << osdmapfn << ": "
	     << error << std::endl;
	exit(1);
      }
    }

    // go
    MonitorStore store(g_conf->mon_data);
    Monitor mon(g_ceph_context, g_conf->name.get_id(), &store, 0, &monmap);
    int r = mon.mkfs(osdmapbl);
    if (r < 0) {
      cerr << argv[0] << ": error creating monfs: " << cpp_strerror(r) << std::endl;
      exit(1);
    }
    cout << argv[0] << ": created monfs at " << g_conf->mon_data 
	 << " for " << g_conf->name << std::endl;
    return 0;
  }

  CompatSet mon_features = get_ceph_mon_feature_compat_set();
  CompatSet ondisk_features;

  MonitorStore store(g_conf->mon_data);
  err = store.mount();
  if (err < 0) {
    cerr << "problem opening monitor store in " << g_conf->mon_data << ": " << cpp_strerror(err) << std::endl;
    exit(1);
  }

  bufferlist magicbl;
  err = store.get_bl_ss(magicbl, "magic", 0);
  if (err < 0) {
    cerr << "unable to read magic from mon data.. did you run mkcephfs?" << std::endl;
    exit(1);
  }
  string magic(magicbl.c_str(), magicbl.length()-1);  // ignore trailing \n
  if (strcmp(magic.c_str(), CEPH_MON_ONDISK_MAGIC)) {
    cerr << "mon fs magic '" << magic << "' != current '" << CEPH_MON_ONDISK_MAGIC << "'" << std::endl;
    exit(1);
  }

  bufferlist features;
  store.get_bl_ss(features, COMPAT_SET_LOC, 0);
  if (features.length() == 0) {
    cerr << "WARNING: mon fs missing feature list.\n"
	 << "Assuming it is old-style and introducing one." << std::endl;
    //we only want the baseline ~v.18 features assumed to be on disk.
    //If new features are introduced this code needs to disappear or
    //be made smarter.
    ondisk_features = get_ceph_mon_feature_compat_set();
  } else {
    bufferlist::iterator it = features.begin();
    ondisk_features.decode(it);
  }
  
  if (!mon_features.writeable(ondisk_features)) {
    cerr << "monitor executable cannot read disk! Missing features: "
	 << std::endl;
    CompatSet diff = mon_features.unsupported(ondisk_features);
    //NEEDS_COMPATSET_ITER
    exit(1);
  }


  // inject new monmap?
  if (!inject_monmap.empty()) {
    bufferlist bl;
    std::string error;
    int r = bl.read_file(inject_monmap.c_str(), &error);
    if (r) {
      cerr << "unable to read monmap from " << inject_monmap << ": "
	   << error << std::endl;
      exit(1);
    }

    // get next version
    version_t v = store.get_int("monmap", "last_committed");
    cout << "last committed monmap epoch is " << v << ", injected map will be " << (v+1) << std::endl;
    v++;

    // set the version
    MonMap tmp;
    tmp.decode(bl);
    if (tmp.get_epoch() != v) {
      cout << "changing monmap epoch from " << tmp.get_epoch() << " to " << v << std::endl;
      tmp.set_epoch(v);
    }
    bufferlist mapbl;
    tmp.encode(mapbl);
    bufferlist final;
    ::encode(v, final);
    ::encode(mapbl, final);

    // save it
    store.put_bl_sn(mapbl, "monmap", v);
    store.put_bl_ss(final, "monmap", "latest");
    store.put_int(v, "monmap", "last_committed");

    cout << "done." << std::endl;
    exit(0);
  }


  // monmap?
  MonMap monmap;
  {
    bufferlist mapbl;
    bufferlist latest;
    store.get_bl_ss(latest, "monmap", "latest");
    if (latest.length() > 0) {
      bufferlist::iterator p = latest.begin();
      version_t v;
      ::decode(v, p);
      ::decode(mapbl, p);
    } else {
      store.get_bl_ss(mapbl, "mkfs", "monmap");
      if (mapbl.length() == 0) {
	cerr << "mon fs missing 'monmap/latest' and 'mkfs/monmap'" << std::endl;
	exit(1);
      }
    }
    try {
      monmap.decode(mapbl);
    }
    catch (const buffer::error& e) {
      cerr << "can't decode monmap: " << e.what() << std::endl;
    }
  }

  // this is what i will bind to
  entity_addr_t ipaddr;

  if (monmap.contains(g_conf->name.get_id())) {
    ipaddr = monmap.get_addr(g_conf->name.get_id());

    // print helpful warning if the conf file doesn't match
    entity_addr_t conf_addr;
    std::vector <std::string> my_sections;
    g_conf->get_my_sections(my_sections);
    std::string mon_addr_str;
    if (g_conf->get_val_from_conf_file(my_sections, "mon addr",
				       mon_addr_str, true) == 0) {
      if (conf_addr.parse(mon_addr_str.c_str()) && (ipaddr != conf_addr)) {
	cerr << "WARNING: 'mon addr' config option " << conf_addr
	     << " does not match monmap file" << std::endl
	     << "         continuing with monmap configuration" << std::endl;
      }
    }
  } else {
    dout(0) << g_conf->name << " does not exist in monmap, will attempt to join an existing cluster" << dendl;

    pick_addresses(g_ceph_context);
    if (!g_conf->public_addr.is_blank_ip()) {
      ipaddr = g_conf->public_addr;
    } else {
      MonMap tmpmap;
      int err = MonClient::build_initial_monmap(g_ceph_context, tmpmap);
      if (err < 0) {
	cerr << argv[0] << ": error generating initial monmap: " << cpp_strerror(err) << std::endl;
	usage();
	exit(1);
      }
      if (tmpmap.contains(g_conf->name.get_id())) {
	ipaddr = tmpmap.get_addr(g_conf->name.get_id());
      } else {
	derr << "no public_addr or public_network specified, and " << g_conf->name
	     << " not present in monmap or ceph.conf" << dendl;
	exit(1);
      }
    }
  }

  // bind
  int rank = monmap.get_rank(g_conf->name.get_id());
  Messenger *messenger = Messenger::create(g_ceph_context,
					   entity_name_t::MON(rank),
					   0);
  messenger->set_cluster_protocol(CEPH_MON_PROTOCOL);
  messenger->set_default_send_priority(CEPH_MSG_PRIO_HIGH);

  uint64_t supported =
    CEPH_FEATURE_UID |
    CEPH_FEATURE_NOSRCADDR |
    CEPH_FEATURE_MONCLOCKCHECK |
    CEPH_FEATURE_PGID64;
  messenger->set_default_policy(Messenger::Policy::stateless_server(supported, 0));
  messenger->set_policy(entity_name_t::TYPE_MON,
                        Messenger::Policy::lossless_peer(supported,
                                                         CEPH_FEATURE_UID |
                                                         CEPH_FEATURE_PGID64));
  messenger->set_policy(entity_name_t::TYPE_OSD,
                        Messenger::Policy::stateless_server(supported,
                                                            CEPH_FEATURE_PGID64 |
                                                            CEPH_FEATURE_OSDENC));

  global_print_banner();

  cout << "starting " << g_conf->name << " rank " << rank
       << " at " << ipaddr
       << " mon_data " << g_conf->mon_data
       << " fsid " << monmap.get_fsid()
       << std::endl;

  err = messenger->bind(ipaddr);
  if (err < 0)
    return 1;

  // start monitor
  mon = new Monitor(g_ceph_context, g_conf->name.get_id(), &store, messenger, &monmap);

  global_init_daemonize(g_ceph_context, 0);
  common_init_finish(g_ceph_context);
  global_init_chdir(g_ceph_context);
  messenger->start();

  // set up signal handlers, now that we've daemonized/forked.
  init_async_signal_handler();
  register_async_signal_handler(SIGHUP, sighup_handler);
  register_async_signal_handler_oneshot(SIGINT, handle_mon_signal);
  register_async_signal_handler_oneshot(SIGTERM, handle_mon_signal);

  mon->init();
  messenger->wait();

  unregister_async_signal_handler(SIGHUP, sighup_handler);
  unregister_async_signal_handler(SIGINT, handle_mon_signal);
  unregister_async_signal_handler(SIGTERM, handle_mon_signal);

  store.umount();
  delete mon;
  delete messenger;

  // cd on exit, so that gmon.out (if any) goes into a separate directory for each node.
  char s[20];
  snprintf(s, sizeof(s), "gmon/%d", getpid());
  if ((mkdir(s, 0755) == 0) && (chdir(s) == 0)) {
    dout(0) << "ceph-mon: gmon.out should be in " << s << dendl;
  }

  return 0;
}

