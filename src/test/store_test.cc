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

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <time.h>
#include "os/FileStore.h"
#include "include/Context.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "common/Mutex.h"
#include "common/Cond.h"
#include <boost/scoped_ptr.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/binomial_distribution.hpp>
#include <gtest/gtest.h>

#include <ext/hash_map>
using __gnu_cxx::hash_map;
typedef boost::mt11213b gen_type;

class StoreTest : public ::testing::Test {
public:
  boost::scoped_ptr<ObjectStore> store;

  StoreTest() : store(0) {}
  virtual void SetUp() {
    ::mkdir("store_test_temp_dir", 0777);
    ObjectStore *store_ = new FileStore(string("store_test_temp_dir"), string("store_test_temp_journal"));
    store.reset(store_);
    store->mkfs();
    store->mount();
  }

  virtual void TearDown() {
    store->umount();
  }
};

TEST_F(StoreTest, SimpleColTest) {
  coll_t cid = coll_t("initial");
  int r = 0;
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    cerr << "create collection" << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  {
    ObjectStore::Transaction t;
    t.remove_collection(cid);
    cerr << "remove collection" << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    cerr << "add collection" << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  {
    ObjectStore::Transaction t;
    t.remove_collection(cid);
    cerr << "remove collection" << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
}

TEST_F(StoreTest, SimpleObjectTest) {
  int r;
  coll_t cid = coll_t("coll");
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    cerr << "Creating collection " << cid << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  hobject_t hoid(sobject_t("Object 1", CEPH_NOSNAP));
  {
    ObjectStore::Transaction t;
    t.touch(cid, hoid);
    cerr << "Creating object " << hoid << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  {
    ObjectStore::Transaction t;
    t.remove(cid, hoid);
    t.remove_collection(cid);
    cerr << "Cleaning" << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
}

TEST_F(StoreTest, SimpleObjectLongnameTest) {
  int r;
  coll_t cid = coll_t("coll");
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    cerr << "Creating collection " << cid << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  hobject_t hoid(sobject_t("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaObjectaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1", CEPH_NOSNAP));
  {
    ObjectStore::Transaction t;
    t.touch(cid, hoid);
    cerr << "Creating object " << hoid << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  {
    ObjectStore::Transaction t;
    t.remove(cid, hoid);
    t.remove_collection(cid);
    cerr << "Cleaning" << std::endl;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
}

TEST_F(StoreTest, ManyObjectTest) {
  int NUM_OBJS = 2000;
  int r = 0;
  coll_t cid("blah");
  string base = "";
  for (int i = 0; i < 100; ++i) base.append("aaaaa");
  set<hobject_t> created;
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  for (int i = 0; i < NUM_OBJS; ++i) {
    if (!(i % 5)) {
      cerr << "Object " << i << std::endl;
    }
    ObjectStore::Transaction t;
    char buf[100];
    snprintf(buf, sizeof(buf), "%d", i);
    hobject_t hoid(sobject_t(string(buf) + base, CEPH_NOSNAP));
    t.touch(cid, hoid);
    created.insert(hoid);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }

  for (set<hobject_t>::iterator i = created.begin();
       i != created.end();
       ++i) {
    struct stat buf;
    ASSERT_TRUE(!store->stat(cid, *i, &buf));
  }

  set<hobject_t> listed;
  vector<hobject_t> objects;
  r = store->collection_list(cid, objects);
  ASSERT_EQ(r, 0);

  cerr << "objects.size() is " << objects.size() << std::endl;
  for (vector<hobject_t> ::iterator i = objects.begin();
       i != objects.end();
       ++i) {
    listed.insert(*i);
    ASSERT_TRUE(created.count(*i));
  }
  ASSERT_TRUE(listed.size() == created.size());

  objects.clear();
  listed.clear();
  hobject_t start, next;
  while (1) {
    r = store->collection_list_partial(cid, start,
				       50,
				       60,
				       CEPH_NOSNAP,
				       &objects,
				       &next);
    ASSERT_EQ(r, 0);
    listed.insert(objects.begin(), objects.end());
    if (objects.size() < 50) {
      ASSERT_TRUE(next.max);
      break;
    }
    objects.clear();
    start = next;
  }
  cerr << "listed.size() is " << listed.size() << std::endl;
  ASSERT_TRUE(listed.size() == created.size());
  for (set<hobject_t>::iterator i = listed.begin();
       i != listed.end();
       ++i) {
    ASSERT_TRUE(created.count(*i));
  }

  for (set<hobject_t>::iterator i = created.begin();
       i != created.end();
       ++i) {
    ObjectStore::Transaction t;
    t.remove(cid, *i);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  cerr << "cleaning up" << std::endl;
  {
    ObjectStore::Transaction t;
    t.remove_collection(cid);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
}

class ObjectGenerator {
public:
  virtual hobject_t create_object(gen_type *gen) = 0;
  virtual ~ObjectGenerator() {}
};

class MixedGenerator : public ObjectGenerator {
public:
  unsigned seq;
  MixedGenerator() : seq(0) {}
  hobject_t create_object(gen_type *gen) {
    char buf[100];
    snprintf(buf, sizeof(buf), "%u", seq);

    boost::uniform_int<> true_false(0, 1);
    string name(buf);
    if (true_false(*gen)) {
      // long
      for (int i = 0; i < 100; ++i) name.append("aaaaa");
    } else if (true_false(*gen)) {
      name = "DIR_" + name;
    }

    // hash
    //boost::binomial_distribution<uint32_t> bin(0xFFFFFF, 0.5);
    ++seq;
    return hobject_t(name, string(), CEPH_NOSNAP, rand());
  }
};

class SyntheticWorkloadState {
public:
  static const unsigned max_in_flight = 16;
  static const unsigned max_objects = 3000;
  coll_t cid;
  unsigned in_flight;
  set<hobject_t> available_objects;
  set<hobject_t> in_use_objects;
  ObjectGenerator *object_gen;
  gen_type *rng;
  ObjectStore *store;
  ObjectStore::Sequencer *osr;

  Mutex lock;
  Cond cond;

  class C_SyntheticOnReadable : public Context {
  public:
    SyntheticWorkloadState *state;
    ObjectStore::Transaction *t;
    hobject_t hoid;
    C_SyntheticOnReadable(SyntheticWorkloadState *state,
			  ObjectStore::Transaction *t, hobject_t hoid)
      : state(state), t(t), hoid(hoid) {}

    void finish(int r) {
      ASSERT_TRUE(r >= 0);
      Mutex::Locker locker(state->lock);
      if (state->in_use_objects.count(hoid)) {
	state->available_objects.insert(hoid);
	state->in_use_objects.erase(hoid);
      }
      --(state->in_flight);
      state->cond.Signal();
    }
  };
    
  
  SyntheticWorkloadState(ObjectStore *store, 
			 ObjectGenerator *gen, 
			 gen_type *rng, 
			 ObjectStore::Sequencer *osr,
			 coll_t cid)
    : cid(cid), in_flight(0), object_gen(gen), rng(rng), store(store), osr(osr),
      lock("State lock") {}

  int init() {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    return store->apply_transaction(t);
  }

  hobject_t get_uniform_random_object() {
    while (in_flight >= max_in_flight || available_objects.empty())
      cond.Wait(lock);
    boost::uniform_int<> choose(0, available_objects.size() - 1);
    int index = choose(*rng);
    set<hobject_t>::iterator i = available_objects.begin();
    for ( ; index > 0; --index, ++i) ;
    hobject_t ret = *i;
    available_objects.erase(i);
    return ret;
  }

  void wait_for_ready() {
    while (in_flight >= max_in_flight)
      cond.Wait(lock);
  }

  void wait_for_done() {
    Mutex::Locker locker(lock);
    while (in_flight)
      cond.Wait(lock);
  }
  
  bool can_create() {
    return (available_objects.size() + in_use_objects.size()) < max_objects;
  }

  bool can_unlink() {
    return (available_objects.size() + in_use_objects.size()) > 0;
  }

  int touch() {
    Mutex::Locker locker(lock);
    if (!can_create())
      return -ENOSPC;
    wait_for_ready();
    hobject_t new_obj = object_gen->create_object(rng);
    in_use_objects.insert(new_obj);
    available_objects.erase(new_obj);
    ObjectStore::Transaction *t = new ObjectStore::Transaction;
    t->touch(cid, new_obj);
    ++in_flight;
    return store->queue_transaction(osr, t, new C_SyntheticOnReadable(this, t, new_obj));
  }

  void scan() {
    Mutex::Locker locker(lock);
    while (in_flight)
      cond.Wait(lock);
    vector<hobject_t> objects;
    set<hobject_t> objects_set, objects_set2;
    hobject_t next, current;
    while (1) {
      cerr << "scanning..." << std::endl;
      int r = store->collection_list_partial(cid, current, 50, 100, 
					     CEPH_NOSNAP, &objects, &next);
      ASSERT_EQ(r, 0);
      objects_set.insert(objects.begin(), objects.end());
      objects.clear();
      if (next.max) break;
      current = next;
    }
    ASSERT_EQ(objects_set.size(), available_objects.size());
    for (set<hobject_t>::iterator i = objects_set.begin();
	 i != objects_set.end();
	 ++i) {
      ASSERT_GT(available_objects.count(*i), (unsigned)0);
    }

    int r = store->collection_list(cid, objects);
    ASSERT_EQ(r, 0);
    objects_set2.insert(objects.begin(), objects.end());
    ASSERT_EQ(objects_set2.size(), available_objects.size());
    for (set<hobject_t>::iterator i = objects_set2.begin();
	 i != objects_set2.end();
	 ++i) {
      ASSERT_GT(available_objects.count(*i), (unsigned)0);
    }
  }

  int stat() {
    hobject_t hoid;
    {
      Mutex::Locker locker(lock);
      if (!can_unlink())
	return -ENOENT;
      hoid = get_uniform_random_object();
      in_use_objects.insert(hoid);
      ++in_flight;
    }
    struct stat buf;
    int r = store->stat(cid, hoid, &buf);
    {
      Mutex::Locker locker(lock);
      --in_flight;
      cond.Signal();
      in_use_objects.erase(hoid);
      available_objects.insert(hoid);
    }
    return r;
  }

  int unlink() {
    Mutex::Locker locker(lock);
    if (!can_unlink())
      return -ENOENT;
    hobject_t to_remove = get_uniform_random_object();
    ObjectStore::Transaction *t = new ObjectStore::Transaction;
    t->remove(cid, to_remove);
    ++in_flight;
    return store->queue_transaction(osr, t, new C_SyntheticOnReadable(this, t, to_remove));
  }

  void print_internal_state() {
    Mutex::Locker locker(lock);
    cerr << "available_objects: " << available_objects.size()
	 << " in_use_objects: " << in_use_objects.size()
	 << " total objects: " << in_use_objects.size() + available_objects.size()
	 << " in_flight " << in_flight << std::endl;
  }
};

TEST_F(StoreTest, Synthetic) {
  ObjectStore::Sequencer osr("test");
  MixedGenerator gen;
  gen_type rng(time(NULL));
  coll_t cid("synthetic_1");
  
  SyntheticWorkloadState test_obj(store.get(), &gen, &rng, &osr, cid);
  test_obj.init();
  for (int i = 0; i < 1000; ++i) {
    if (!(i % 10)) cerr << "seeding object " << i << std::endl;
    test_obj.touch();
  }
  for (int i = 0; i < 1000; ++i) {
    if (!(i % 10)) {
      cerr << "Op " << i << std::endl;
      test_obj.print_internal_state();
    }
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val > 97) {
      test_obj.scan();
    } else if (val > 50) {
      test_obj.stat();
    } else if (val > 30) {
      test_obj.unlink();
    } else {
      test_obj.touch();
    }
  }
  test_obj.wait_for_done();
}

TEST_F(StoreTest, HashCollisionTest) {
  coll_t cid("blah");
  int r;
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  string base = "";
  for (int i = 0; i < 100; ++i) base.append("aaaaa");
  set<hobject_t> created;
  for (int i = 0; i < 1000; ++i) {
    char buf[100];
    sprintf(buf, "%d", i);
    if (!(i % 5)) {
      cerr << "Object " << i << std::endl;
    }
    hobject_t hoid(string(buf) + base, string(), CEPH_NOSNAP, 0);
    {
      ObjectStore::Transaction t;
      t.touch(cid, hoid);
      r = store->apply_transaction(t);
      ASSERT_EQ(r, 0);
    }
    created.insert(hoid);
  }
  vector<hobject_t> objects;
  r = store->collection_list(cid, objects);
  ASSERT_EQ(r, 0);
  set<hobject_t> listed(objects.begin(), objects.end());
  cerr << "listed.size() is " << listed.size() << " and created.size() is " << created.size() << std::endl;
  ASSERT_TRUE(listed.size() == created.size());
  objects.clear();
  listed.clear();
  hobject_t current, next;
  while (1) {
    r = store->collection_list_partial(cid, current, 50, 60,
				       CEPH_NOSNAP, &objects, &next);
    ASSERT_EQ(r, 0);
    for (vector<hobject_t>::iterator i = objects.begin();
	 i != objects.end();
	 ++i) {
      if (listed.count(*i))
	cerr << *i << " repeated" << std::endl;
      listed.insert(*i);
    }
    if (objects.size() < 50) {
      ASSERT_TRUE(next.max);
      break;
    }
    objects.clear();
    current = next;
  }
  cerr << "listed.size() is " << listed.size() << std::endl;
  ASSERT_TRUE(listed.size() == created.size());
  for (set<hobject_t>::iterator i = listed.begin();
       i != listed.end();
       ++i) {
    ASSERT_TRUE(created.count(*i));
  }

  for (set<hobject_t>::iterator i = created.begin();
       i != created.end();
       ++i) {
    ObjectStore::Transaction t;
    t.collection_remove(cid, *i);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }
  ObjectStore::Transaction t;
  t.remove_collection(cid);
  store->apply_transaction(t);
}

TEST_F(StoreTest, OMapTest) {
  coll_t cid("blah");
  hobject_t hoid("tesomap", "", CEPH_NOSNAP, 0);
  int r;
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }

  map<string, bufferlist> attrs;
  {
    ObjectStore::Transaction t;
    t.touch(cid, hoid);
    t.omap_clear(cid, hoid);
    map<string, bufferlist> start_set;
    t.omap_setkeys(cid, hoid, start_set);
    store->apply_transaction(t);
  }

  for (int i = 0; i < 100; i++) {
    if (!(i%5)) {
      std::cout << "On iteration " << i << std::endl;
    }
    ObjectStore::Transaction t;
    bufferlist bl;
    map<string, bufferlist> cur_attrs;
    r = store->omap_get(cid, hoid, &bl, &cur_attrs);
    ASSERT_EQ(r, 0);
    for (map<string, bufferlist>::iterator j = attrs.begin();
	 j != attrs.end();
	 ++j) {
      bool correct = cur_attrs.count(j->first) && string(cur_attrs[j->first].c_str()) == string(j->second.c_str());
      if (!correct) {
	std::cout << j->first << " is present in cur_attrs " << cur_attrs.count(j->first) << " times " << std::endl;
	if (cur_attrs.count(j->first) > 0) {
	  std::cout << j->second.c_str() << " : " << cur_attrs[j->first].c_str() << std::endl;
	}
      }
      ASSERT_EQ(correct, true);
    }
    ASSERT_EQ(attrs.size(), cur_attrs.size());

    char buf[100];
    snprintf(buf, sizeof(buf), "%d", i);
    bl.clear();
    bufferptr bp(buf, strlen(buf) + 1);
    bl.append(bp);
    map<string, bufferlist> to_add;
    to_add.insert(pair<string, bufferlist>("key-" + string(buf), bl));
    attrs.insert(pair<string, bufferlist>("key-" + string(buf), bl));
    t.omap_setkeys(cid, hoid, to_add);
    store->apply_transaction(t);
  }

  int i = 0;
  while (attrs.size()) {
    if (!(i%5)) {
      std::cout << "removal: On iteration " << i << std::endl;
    }
    ObjectStore::Transaction t;
    bufferlist bl;
    map<string, bufferlist> cur_attrs;
    r = store->omap_get(cid, hoid, &bl, &cur_attrs);
    ASSERT_EQ(r, 0);
    for (map<string, bufferlist>::iterator j = attrs.begin();
	 j != attrs.end();
	 ++j) {
      bool correct = cur_attrs.count(j->first) && string(cur_attrs[j->first].c_str()) == string(j->second.c_str());
      if (!correct) {
	std::cout << j->first << " is present in cur_attrs " << cur_attrs.count(j->first) << " times " << std::endl;
	if (cur_attrs.count(j->first) > 0) {
	  std::cout << j->second.c_str() << " : " << cur_attrs[j->first].c_str() << std::endl;
	}
      }
      ASSERT_EQ(correct, true);
    }

    string to_remove = attrs.begin()->first;
    set<string> keys_to_remove;
    keys_to_remove.insert(to_remove);
    t.omap_rmkeys(cid, hoid, keys_to_remove);
    store->apply_transaction(t);

    attrs.erase(to_remove);

    ++i;
  }

  ObjectStore::Transaction t;
  t.remove(cid, hoid);
  t.remove_collection(cid);
  store->apply_transaction(t);
}

TEST_F(StoreTest, XattrTest) {
  coll_t cid("blah");
  hobject_t hoid("tesomap", "", CEPH_NOSNAP, 0);
  bufferlist big;
  for (unsigned i = 0; i < 10000; ++i) {
    big.append('\0');
  }
  bufferlist small;
  for (unsigned i = 0; i < 10; ++i) {
    small.append('\0');
  }
  int r;
  {
    ObjectStore::Transaction t;
    t.create_collection(cid);
    t.touch(cid, hoid);
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }

  map<string, bufferlist> attrs;
  {
    ObjectStore::Transaction t;
    t.setattr(cid, hoid, "attr1", small);
    attrs["attr1"] = small;
    t.setattr(cid, hoid, "attr2", big);
    attrs["attr2"] = big;
    t.setattr(cid, hoid, "attr3", small);
    attrs["attr3"] = small;
    t.setattr(cid, hoid, "attr1", small);
    attrs["attr1"] = small;
    t.setattr(cid, hoid, "attr4", big);
    attrs["attr4"] = big;
    t.setattr(cid, hoid, "attr3", big);
    attrs["attr3"] = big;
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }

  map<string, bufferptr> aset;
  store->getattrs(cid, hoid, aset);
  ASSERT_EQ(aset.size(), attrs.size());
  for (map<string, bufferptr>::iterator i = aset.begin();
       i != aset.end();
       ++i) {
    bufferlist bl;
    bl.push_back(i->second);
    ASSERT_TRUE(attrs[i->first] == bl);
  }

  {
    ObjectStore::Transaction t;
    t.rmattr(cid, hoid, "attr2");
    attrs.erase("attr2");
    r = store->apply_transaction(t);
    ASSERT_EQ(r, 0);
  }

  aset.clear();
  store->getattrs(cid, hoid, aset);
  ASSERT_EQ(aset.size(), attrs.size());
  for (map<string, bufferptr>::iterator i = aset.begin();
       i != aset.end();
       ++i) {
    bufferlist bl;
    bl.push_back(i->second);
    ASSERT_TRUE(attrs[i->first] == bl);
  }

  bufferptr bp;
  r = store->getattr(cid, hoid, "attr2", bp);
  ASSERT_EQ(r, -ENODATA);

  r = store->getattr(cid, hoid, "attr3", bp);
  ASSERT_EQ(r, 0);
  bufferlist bl2;
  bl2.push_back(bp);
  ASSERT_TRUE(bl2 == attrs["attr3"]);
}

int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);
  g_ceph_context->_conf->set_val("osd_journal_size", "400");
  g_ceph_context->_conf->apply_changes(NULL);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
