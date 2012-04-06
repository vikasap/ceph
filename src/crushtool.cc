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

#include "common/debug.h"
#include "common/errno.h"
#include "common/config.h"

#include "common/ceph_argparse.h"
#include "global/global_context.h"
#include "global/global_init.h"
#include "crush/CrushWrapper.h"
#include "crush/CrushCompiler.h"
#include "crush/CrushTester.h"

#include <fstream>

#define dout_subsys ceph_subsys_crush

using namespace std;

void usage();


const char *infn = "stdin";


////////////////////////////////////////////////////////////////////////////

void usage()
{
  cout << "usage: crushtool ...\n";
  cout << "   --decompile|-d map    decompile a crush map to source\n";
  cout << "   --compile|-c map.txt  compile a map from source\n";
  cout << "   [-o outfile [--clobber]]\n";
  cout << "                         specify output for for (de)compilation\n";
  cout << "   --build --num_osds N layer1 ...\n";
  cout << "                         build a new map, where each 'layer' is\n";
  cout << "                           'name (uniform|straw|list|tree) size'\n";
  cout << "   -i mapfn --test       test a range of inputs on the map\n";
  cout << "      [--min-x x] [--max-x x] [--x x]\n";
  cout << "      [--min-rule r] [--max-rule r] [--rule r]\n";
  cout << "      [--num-rep n]\n";
  cout << "      [--weight|-w devno weight]\n";
  cout << "                         where weight is 0 to 1.0\n";
  cout << "   -i mapfn --add-item id weight name [--loc type name ...]\n";
  cout << "                         insert an item into the hierarchy at the\n";
  cout << "                         given location\n";
  cout << "   -i mapfn --remove-item name\n"
       << "                         remove the given item\n";
  cout << "   -i mapfn --reweight-item name weight\n";
  cout << "                         reweight a given item (and adjust ancestor\n"
       << "                         weights as needed)\n";
  cout << "   -i mapfn --reweight   recalculate all bucket weights\n";
  exit(1);
}

struct bucket_types_t {
  const char *name;
  int type;
} bucket_types[] = {
  { "uniform", CRUSH_BUCKET_UNIFORM },
  { "list", CRUSH_BUCKET_LIST },
  { "straw", CRUSH_BUCKET_STRAW },
  { "tree", CRUSH_BUCKET_TREE },
  { 0, 0 },
};

struct layer_t {
  const char *name;
  const char *buckettype;
  int size;
};

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);

  const char *me = argv[0];
  std::string infn, srcfn, outfn, add_name, remove_name, reweight_name;
  bool compile = false;
  bool decompile = false;
  bool test = false;
  bool verbose = false;

  bool reweight = false;
  int add_item = -1;
  float add_weight = 0;
  map<string,string> add_loc;
  float reweight_weight = 0;

  int build = 0;
  int num_osds =0;
  vector<layer_t> layers;

  CrushWrapper crush;

  CrushTester tester(crush, cerr, 1);

  vector<const char *> empty_args;  // we use -c, don't confuse the generic arg parsing
  global_init(NULL, empty_args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY,
	      CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);

  int x;

  std::string val;
  std::ostringstream err;
  int tmp;
  for (std::vector<const char*>::iterator i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_witharg(args, i, &val, "-d", "--decompile", (char*)NULL)) {
      infn = val;
      decompile = true;
    } else if (ceph_argparse_witharg(args, i, &val, "-i", "--infn", (char*)NULL)) {
      infn = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-o", "--outfn", (char*)NULL)) {
      outfn = val;
    } else if (ceph_argparse_flag(args, i, "-v", "--verbose", (char*)NULL)) {
      verbose = true;
      tester.set_verbosity(2);
    } else if (ceph_argparse_witharg(args, i, &val, "-c", "--compile", (char*)NULL)) {
      srcfn = val;
      compile = true;
    } else if (ceph_argparse_flag(args, i, "-t", "--test", (char*)NULL)) {
      test = true;
    } else if (ceph_argparse_flag(args, i, "--reweight", (char*)NULL)) {
      reweight = true;
    } else if (ceph_argparse_withint(args, i, &add_item, &err, "--add_item", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      if (i == args.end())
	usage();
      add_weight = atof(*i);
      i = args.erase(i);
      if (i == args.end())
	usage();
      add_name.assign(*i);
      i = args.erase(i);
    } else if (ceph_argparse_witharg(args, i, &val, "--loc", (char*)NULL)) {
      std::string type(val);
      if (i == args.end())
	usage();
      std::string name(*i);
      i = args.erase(i);
      add_loc[type] = name;
    } else if (ceph_argparse_witharg(args, i, &val, "--remove_item", (char*)NULL)) {
      remove_name = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--reweight_item", (char*)NULL)) {
      reweight_name = val;
      if (i == args.end())
	usage();
      reweight_weight = atof(*i);
      i = args.erase(i);
    } else if (ceph_argparse_flag(args, i, "--build", (char*)NULL)) {
      build = true;
    } else if (ceph_argparse_withint(args, i, &num_osds, &err, "--num_osds", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
    } else if (ceph_argparse_withint(args, i, &x, &err, "--num_rep", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_num_rep(x);
    } else if (ceph_argparse_withint(args, i, &x, &err, "--max_x", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_max_x(x);
    } else if (ceph_argparse_withint(args, i, &x, &err, "--min_x", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_min_x(x);
    } else if (ceph_argparse_withint(args, i, &x, &err, "--x", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_x(x);
    } else if (ceph_argparse_withint(args, i, &x, &err, "--force", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_force(x);
    } else if (ceph_argparse_withint(args, i, &x, &err, "--max_rule", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_max_rule(x);
    } else if (ceph_argparse_withint(args, i, &x, &err, "--min_rule", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_min_rule(x);
    } else if (ceph_argparse_withint(args, i, &x, &err, "--rule", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      tester.set_rule(x);
    } else if (ceph_argparse_withint(args, i, &tmp, &err, "--weight", (char*)NULL)) {
      if (!err.str().empty()) {
	cerr << err.str() << std::endl;
	exit(EXIT_FAILURE);
      }
      int dev = tmp;
      if (i == args.end())
	usage();
      float f = atof(*i);
      i = args.erase(i);
      tester.set_device_weight(dev, f);
    }
    else {
      ++i;
    }
  }

  if (decompile + compile + build > 1) {
    usage();
  }
  if (!compile && !decompile && !build && !test && !reweight && add_item < 0 &&
      remove_name.empty() && reweight_name.empty()) {
    usage();
  }
  if ((!build) && (args.size() > 0)) {
    cerr << "too many arguments!" << std::endl;
    usage();
  }
  else {
    if ((args.size() % 3) != 0U) {
      cerr << "layers must be specified with 3-tuples of (name, buckettype, size)"
    	   << std::endl;
      usage();
    }
    for (size_t j = 0; j < args.size(); j += 3) {
      layer_t l;
      l.name = args[j];
      l.buckettype = args[j+1];
      l.size = atoi(args[j+2]);
      layers.push_back(l);
    }
  }

  /*
  if (outfn) cout << "outfn " << outfn << std::endl;
  if (cinfn) cout << "cinfn " << cinfn << std::endl;
  if (dinfn) cout << "dinfn " << dinfn << std::endl;
  */

  bool modified = false;

  if (!infn.empty()) {
    bufferlist bl;
    std::string error;
    int r = bl.read_file(infn.c_str(), &error);
    if (r < 0) {
      cerr << me << ": error reading '" << infn << "': " 
	   << error << std::endl;
      exit(1);
    }
    bufferlist::iterator p = bl.begin();
    crush.decode(p);
  }

  if (decompile) {
    CrushCompiler cc(crush, cerr);
    if (!outfn.empty()) {
      ofstream o;
      o.open(outfn.c_str(), ios::out | ios::binary | ios::trunc);
      if (!o.is_open()) {
	cerr << me << ": error writing '" << outfn << "'" << std::endl;
	exit(1);
      }
      cc.decompile(o);
      o.close();
    } else {
      cc.decompile(cout);
    }
  }

  if (compile) {
    crush.create();

    // read the file
    ifstream in(srcfn.c_str());
    if (!in.is_open()) {
      cerr << "input file " << srcfn << " not found" << std::endl;
      return -ENOENT;
    }

    CrushCompiler cc(crush, cerr);
    int r = cc.compile(in, srcfn.c_str());
    if (r < 0) 
      exit(1);

    modified = true;
  }

  if (build) {
    if (layers.empty()) {
      cerr << me << ": must specify at least one layer" << std::endl;
      exit(1);
    }

    crush.create();

    vector<int> lower_items;
    vector<int> lower_weights;

    for (int i=0; i<num_osds; i++) {
      lower_items.push_back(i);
      lower_weights.push_back(0x10000);
    }

    int type = 1;
    int rootid = 0;
    for (vector<layer_t>::iterator p = layers.begin(); p != layers.end(); p++, type++) {
      layer_t &l = *p;

      dout(0) << "layer " << type
	      << "  " << l.name
	      << "  bucket type " << l.buckettype
	      << "  " << l.size 
	      << dendl;

      crush.set_type_name(type, l.name);

      int buckettype = -1;
      for (int i = 0; bucket_types[i].name; i++)
	if (l.buckettype && strcmp(l.buckettype, bucket_types[i].name) == 0) {
	  buckettype = bucket_types[i].type;
	  break;
	}
      if (buckettype < 0) {
	cerr << "unknown bucket type '" << l.buckettype << "'" << std::endl << std::endl;
	usage();
      }

      // build items
      vector<int> cur_items;
      vector<int> cur_weights;
      unsigned lower_pos = 0;  // lower pos

      dout(0) << "lower_items " << lower_items << dendl;
      dout(0) << "lower_weights " << lower_weights << dendl;

      int i = 0;
      while (1) {
	if (lower_pos == lower_items.size())
	  break;

	int items[num_osds];
	int weights[num_osds];

	int weight = 0;
	int j;
	for (j=0; j<l.size || l.size==0; j++) {
	  if (lower_pos == lower_items.size())
	    break;
	  items[j] = lower_items[lower_pos];
	  weights[j] = lower_weights[lower_pos];
	  weight += weights[j];
	  lower_pos++;
	  dout(0) << "  item " << items[j] << " weight " << weights[j] << dendl;
	}

	crush_bucket *b = crush_make_bucket(buckettype, CRUSH_HASH_DEFAULT, type, j, items, weights);
	int id = crush_add_bucket(crush.crush, 0, b);
	rootid = id;

	char format[20];
	format[sizeof(format)-1] = '\0';
	if (l.size)
	  snprintf(format, sizeof(format)-1, "%s%%d", l.name);
	else
	  strncpy(format, l.name, sizeof(format)-1);
	char name[20];
	snprintf(name, sizeof(name), format, i);
	crush.set_item_name(id, name);

	dout(0) << " in bucket " << id << " '" << name << "' size " << j << " weight " << weight << dendl;

	cur_items.push_back(id);
	cur_weights.push_back(weight);
	i++;
      }

      lower_items.swap(cur_items);
      lower_weights.swap(cur_weights);
    }
    
    // make a generic rules
    int ruleset=1;
    crush_rule *rule = crush_make_rule(3, ruleset, CEPH_PG_TYPE_REP, 2, 2);
    crush_rule_set_step(rule, 0, CRUSH_RULE_TAKE, rootid, 0);
    crush_rule_set_step(rule, 1, CRUSH_RULE_CHOOSE_LEAF_FIRSTN, CRUSH_CHOOSE_N, 1);
    crush_rule_set_step(rule, 2, CRUSH_RULE_EMIT, 0, 0);
    int rno = crush_add_rule(crush.crush, rule, -1);
    crush.set_rule_name(rno, "data");

    modified = true;
  }

  if (!reweight_name.empty()) {
    cout << me << " reweighting item " << reweight_name << " to " << reweight_weight << std::endl;
    int r;
    if (!crush.name_exists(reweight_name.c_str())) {
      cerr << " name " << reweight_name << " dne" << std::endl;
      r = -ENOENT;
    } else {
      int item = crush.get_item_id(reweight_name.c_str());
      r = crush.adjust_item_weightf(g_ceph_context, item, reweight_weight);
    }
    if (r == 0)
      modified = true;
    else {
      cerr << me << " " << cpp_strerror(r) << std::endl;
      return r;
    }
        
  }
  if (!remove_name.empty()) {
    cout << me << " removing item " << remove_name << std::endl;
    int r;
    if (!crush.name_exists(remove_name.c_str())) {
      cerr << " name " << remove_name << " dne" << std::endl;
      r = -ENOENT;
    } else {
      int remove_item = crush.get_item_id(remove_name.c_str());
      r = crush.remove_item(g_ceph_context, remove_item);
    }
    if (r == 0)
      modified = true;
    else {
      cerr << me << " " << cpp_strerror(r) << std::endl;
      return r;
    }
  }
  if (add_item >= 0) {
    cout << me << " adding item " << add_item << " weight " << add_weight
	 << " at " << add_loc << std::endl;
    int r = crush.insert_item(g_ceph_context, add_item, add_weight, add_name.c_str(), add_loc);
    if (r == 0)
      modified = true;
    else {
      cerr << me << " " << cpp_strerror(r) << std::endl;
      return r;
    }
  }
  if (reweight) {
    crush.reweight(g_ceph_context);
    modified = true;
  }

  if (modified) {
    crush.finalize();

    if (outfn.empty()) {
      cout << me << " successfully built or modified map.  Use '-o <file>' to write it out." << std::endl;
    } else {
      bufferlist bl;
      crush.encode(bl);
      int r = bl.write_file(outfn.c_str());
      if (r < 0) {
	char buf[80];
	cerr << me << ": error writing '" << outfn << "': " << strerror_r(-r, buf, sizeof(buf)) << std::endl;
	exit(1);
      }
      if (verbose)
	cout << "wrote crush map to " << outfn << std::endl;
    }
  }

  if (test) {
    int r = tester.test();
    if (r < 0)
      exit(1);
  }

  return 0;
}
