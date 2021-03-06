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


#ifndef CEPH_OSDMAP_H
#define CEPH_OSDMAP_H

/*
 * describe properties of the OSD cluster.
 *   disks, disk groups, total # osds,
 *
 */
#include "common/config.h"
#include "include/types.h"
#include "osd_types.h"
#include "msg/Message.h"
#include "common/Mutex.h"
#include "common/Clock.h"

#include "crush/CrushWrapper.h"

#include "include/interval_set.h"

#include <vector>
#include <list>
#include <set>
#include <map>
#include <tr1/memory>
using namespace std;

#include <ext/hash_set>
using __gnu_cxx::hash_set;



/*
 * some system constants
 */

// pg roles
#define PG_ROLE_STRAY   -1
#define PG_ROLE_HEAD     0
#define PG_ROLE_ACKER    1
#define PG_ROLE_MIDDLE   2  // der.. misnomer
//#define PG_ROLE_TAIL     2



/*
 * we track up to two intervals during which the osd was alive and
 * healthy.  the most recent is [up_from,up_thru), where up_thru is
 * the last epoch the osd is known to have _started_.  i.e., a lower
 * bound on the actual osd death.  down_at (if it is > up_from) is an
 * upper bound on the actual osd death.
 *
 * the second is the last_clean interval [first,last].  in that case,
 * the last interval is the last epoch known to have been either
 * _finished_, or during which the osd cleanly shut down.  when
 * possible, we push this forward to the epoch the osd was eventually
 * marked down.
 *
 * the lost_at is used to allow build_prior to proceed without waiting
 * for an osd to recover.  In certain cases, progress may be blocked 
 * because an osd is down that may contain updates (i.e., a pg may have
 * gone rw during an interval).  If the osd can't be brought online, we
 * can force things to proceed knowing that we _might_ be losing some
 * acked writes.  If the osd comes back to life later, that's fine to,
 * but those writes will still be lost (the divergent objects will be
 * thrown out).
 */
struct osd_info_t {
  epoch_t last_clean_begin;  // last interval that ended with a clean osd shutdown
  epoch_t last_clean_end;
  epoch_t up_from;   // epoch osd marked up
  epoch_t up_thru;   // lower bound on actual osd death (if > up_from)
  epoch_t down_at;   // upper bound on actual osd death (if > up_from)
  epoch_t lost_at;   // last epoch we decided data was "lost"
  
  osd_info_t() : last_clean_begin(0), last_clean_end(0),
		 up_from(0), up_thru(0), down_at(0), lost_at(0) {}

  void dump(Formatter *f) const;
  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);
  static void generate_test_instances(list<osd_info_t*>& o);
};
WRITE_CLASS_ENCODER(osd_info_t)

ostream& operator<<(ostream& out, const osd_info_t& info);


/** OSDMap
 */
class OSDMap {

public:
  class Incremental {
  public:
    uuid_d fsid;
    epoch_t epoch;   // new epoch; we are a diff from epoch-1 to epoch
    utime_t modified;
    int64_t new_pool_max; //incremented by the OSDMonitor on each pool create
    int32_t new_flags;

    // full (rare)
    bufferlist fullmap;  // in leiu of below.
    bufferlist crush;

    // incremental
    int32_t new_max_osd;
    map<int64_t,pg_pool_t> new_pools;
    map<int64_t,string> new_pool_names;
    set<int64_t> old_pools;
    map<int32_t,entity_addr_t> new_up_client;
    map<int32_t,entity_addr_t> new_up_internal;
    map<int32_t,uint8_t> new_state;             // XORed onto previous state.
    map<int32_t,uint32_t> new_weight;
    map<pg_t,vector<int32_t> > new_pg_temp;     // [] to remove
    map<int32_t,epoch_t> new_up_thru;
    map<int32_t,pair<epoch_t,epoch_t> > new_last_clean_interval;
    map<int32_t,epoch_t> new_lost;

    map<entity_addr_t,utime_t> new_blacklist;
    vector<entity_addr_t> old_blacklist;
    map<int32_t, entity_addr_t> new_hb_up;

    string cluster_snapshot;

    void encode_client_old(bufferlist& bl) const;
    void encode(bufferlist& bl, uint64_t features=-1) const;
    void decode(bufferlist::iterator &p);
    void dump(Formatter *f) const;
    static void generate_test_instances(list<Incremental*>& o);

    Incremental(epoch_t e=0) :
      epoch(e), new_pool_max(-1), new_flags(-1), new_max_osd(-1) {
      memset(&fsid, 0, sizeof(fsid));
    }
    Incremental(bufferlist &bl) {
      bufferlist::iterator p = bl.begin();
      decode(p);
    }
    Incremental(bufferlist::iterator &p) {
      decode(p);
    }
  };
  
private:
  uuid_d fsid;
  epoch_t epoch;        // what epoch of the osd cluster descriptor is this
  utime_t created, modified; // epoch start time
  int32_t pool_max;     // the largest pool num, ever

  uint32_t flags;

  int num_osd;         // not saved
  int32_t max_osd;
  vector<uint8_t> osd_state;
  vector<entity_addr_t> osd_addr;
  vector<entity_addr_t> osd_cluster_addr;
  vector<entity_addr_t> osd_hb_addr;
  vector<__u32>   osd_weight;   // 16.16 fixed point, 0x10000 = "in", 0 = "out"
  vector<osd_info_t> osd_info;
  map<pg_t,vector<int> > pg_temp;  // temp pg mapping (e.g. while we rebuild)

  map<int64_t,pg_pool_t> pools;
  map<int64_t,string> pool_name;
  map<string,int64_t> name_pool;

  hash_map<entity_addr_t,utime_t> blacklist;

  epoch_t cluster_snapshot_epoch;
  string cluster_snapshot;

 public:
  CrushWrapper     crush;       // hierarchical map

  friend class OSDMonitor;
  friend class PGMonitor;
  friend class MDS;

 public:
  OSDMap() : epoch(0), 
	     pool_max(-1),
	     flags(0),
	     num_osd(0), max_osd(0),
	     cluster_snapshot_epoch(0) { 
    memset(&fsid, 0, sizeof(fsid));
  }

  // map info
  const uuid_d& get_fsid() const { return fsid; }
  void set_fsid(uuid_d& f) { fsid = f; }

  epoch_t get_epoch() const { return epoch; }
  void inc_epoch() { epoch++; }

  void set_epoch(epoch_t e) {
    epoch = e;
    for (map<int64_t,pg_pool_t>::iterator p = pools.begin();
	 p != pools.end();
	 p++)
      p->second.last_change = e;
  }

  /* stamps etc */
  const utime_t& get_created() const { return created; }
  const utime_t& get_modified() const { return modified; }

  bool is_blacklisted(const entity_addr_t& a) const;

  string get_cluster_snapshot() const {
    if (cluster_snapshot_epoch == epoch)
      return cluster_snapshot;
    return string();
  }

  /***** cluster state *****/
  /* osds */
  int get_max_osd() const { return max_osd; }
  void set_max_osd(int m);

  int get_num_osds() const {
    return num_osd;
  }
  int calc_num_osds();

  void get_all_osds(set<int32_t>& ls) const {
    for (int i=0; i<max_osd; i++)
      if (exists(i))
	ls.insert(i);
  }
  int get_num_up_osds() const {
    int n = 0;
    for (int i=0; i<max_osd; i++)
      if (osd_state[i] & CEPH_OSD_EXISTS &&
	  osd_state[i] & CEPH_OSD_UP) n++;
    return n;
  }
  int get_num_in_osds() const {
    int n = 0;
    for (int i=0; i<max_osd; i++)
      if (osd_state[i] & CEPH_OSD_EXISTS &&
	  get_weight(i) != CEPH_OSD_OUT) n++;
    return n;
  }

  int get_flags() const { return flags; }
  int test_flag(int f) const { return flags & f; }
  void set_flag(int f) { flags |= f; }
  void clear_flag(int f) { flags &= ~f; }

  static void calc_state_set(int state, set<string>& st) {
    unsigned t = state;
    for (unsigned s = 1; t; s <<= 1) {
      if (t & s) {
	t &= ~s;
	st.insert(ceph_osd_state_name(s));
      }
    }
  }
  int get_state(int o) const {
    assert(o < max_osd);
    return osd_state[o];
  }
  int get_state(int o, set<string>& st) const {
    assert(o < max_osd);
    unsigned t = osd_state[o];
    calc_state_set(t, st);
    return osd_state[o];
  }
  void set_state(int o, unsigned s) {
    assert(o < max_osd);
    osd_state[o] = s;
  }
  void set_weightf(int o, float w) {
    set_weight(o, (int)((float)CEPH_OSD_IN * w));
  }
  void set_weight(int o, unsigned w) {
    assert(o < max_osd);
    osd_weight[o] = w;
    if (w)
      osd_state[o] |= CEPH_OSD_EXISTS;
  }
  unsigned get_weight(int o) const {
    assert(o < max_osd);
    return osd_weight[o];
  }
  float get_weightf(int o) const {
    return (float)get_weight(o) / (float)CEPH_OSD_IN;
  }
  void adjust_osd_weights(const map<int,double>& weights, Incremental& inc) const;

  bool exists(int osd) const {
    //assert(osd >= 0);
    return osd >= 0 && osd < max_osd && (osd_state[osd] & CEPH_OSD_EXISTS);
  }

  bool is_up(int osd) const {
    return exists(osd) && (osd_state[osd] & CEPH_OSD_UP);
  }

  bool is_down(int osd) const {
    return !exists(osd) || !is_up(osd);
  }

  bool is_out(int osd) const {
    return !exists(osd) || get_weight(osd) == CEPH_OSD_OUT;
  }

  bool is_in(int osd) const {
    return exists(osd) && !is_out(osd);
  }
  
  int identify_osd(const entity_addr_t& addr) const {
    for (unsigned i=0; i<osd_addr.size(); i++)
      if ((osd_addr[i] == addr) || (osd_cluster_addr[i] == addr))
	return i;
    return -1;
  }
  bool have_addr(const entity_addr_t& addr) const {
    return identify_osd(addr) >= 0;
  }
  bool find_osd_on_ip(const entity_addr_t& ip) const {
    for (unsigned i=0; i<osd_addr.size(); i++)
      if (osd_addr[i].is_same_host(ip) || osd_cluster_addr[i].is_same_host(ip))
	return i;
    return -1;
  }
  bool have_inst(int osd) const {
    return exists(osd) && is_up(osd); 
  }
  const entity_addr_t &get_addr(int osd) const {
    assert(exists(osd));
    return osd_addr[osd];
  }
  const entity_addr_t &get_cluster_addr(int osd) const {
    assert(exists(osd));
    if (osd_cluster_addr[osd] == entity_addr_t())
      return get_addr(osd);
    return osd_cluster_addr[osd];
  }
  const entity_addr_t &get_hb_addr(int osd) const {
    assert(exists(osd));
    return osd_hb_addr[osd];
  }
  entity_inst_t get_inst(int osd) const {
    assert(exists(osd));
    assert(is_up(osd));
    return entity_inst_t(entity_name_t::OSD(osd), osd_addr[osd]);
  }
  entity_inst_t get_cluster_inst(int osd) const {
    assert(exists(osd));
    assert(is_up(osd));
    if (osd_cluster_addr[osd] == entity_addr_t())
      return get_inst(osd);
    return entity_inst_t(entity_name_t::OSD(osd), osd_cluster_addr[osd]);
  }
  entity_inst_t get_hb_inst(int osd) const {
    assert(exists(osd));
    assert(is_up(osd));
    return entity_inst_t(entity_name_t::OSD(osd), osd_hb_addr[osd]);
  }

  const epoch_t& get_up_from(int osd) const {
    assert(exists(osd));
    return osd_info[osd].up_from;
  }
  const epoch_t& get_up_thru(int osd) const {
    assert(exists(osd));
    return osd_info[osd].up_thru;
  }
  const epoch_t& get_down_at(int osd) const {
    assert(exists(osd));
    return osd_info[osd].down_at;
  }
  const osd_info_t& get_info(int osd) const {
    assert(osd < max_osd);
    return osd_info[osd];
  }
  
  int get_any_up_osd() const {
    for (int i=0; i<max_osd; i++)
      if (is_up(i))
	return i;
    return -1;
  }

  int apply_incremental(Incremental &inc);

  // serialize, unserialize
private:
  void encode_client_old(bufferlist& bl) const;
public:
  void encode(bufferlist& bl, uint64_t features=-1) const;
  void decode(bufferlist& bl);
  void decode(bufferlist::iterator& p);


  /****   mapping facilities   ****/
  int object_locator_to_pg(const object_t& oid, const object_locator_t& loc, pg_t &pg) const {
    // calculate ps (placement seed)
    const pg_pool_t *pool = get_pg_pool(loc.get_pool());
    if (!pool)
      return -ENOENT;
    ps_t ps;
    if (loc.key.length())
      ps = ceph_str_hash(pool->object_hash, loc.key.c_str(), loc.key.length());
    else
      ps = ceph_str_hash(pool->object_hash, oid.name.c_str(), oid.name.length());
    // mix in preferred osd, so we don't get the same peers for
    // all of the placement pgs (e.g. 0.0p*)
    if (loc.get_preferred() >= 0)
      ps += loc.get_preferred();
    pg = pg_t(ps, loc.get_pool(), loc.get_preferred());
    return 0;
  }

  pg_t object_locator_to_pg(const object_t& oid, const object_locator_t& loc) const {
    pg_t pg;
    int ret = object_locator_to_pg(oid, loc, pg);
    assert(ret == 0);
    return pg;
  }

  static object_locator_t file_to_object_locator(const ceph_file_layout& layout) {
    return object_locator_t(layout.fl_pg_pool, layout.fl_pg_preferred);
  }

  // oid -> pg
  ceph_object_layout file_to_object_layout(object_t oid, ceph_file_layout& layout) const {
    return make_object_layout(oid, layout.fl_pg_pool,
			      layout.fl_pg_preferred);
  }

  ceph_object_layout make_object_layout(object_t oid, int pg_pool, int preferred=-1) const {
    object_locator_t loc(pg_pool);
    loc.preferred = preferred;
    
    ceph_object_layout ol;
    pg_t pgid = object_locator_to_pg(oid, loc);
    ol.ol_pgid = pgid.get_old_pg().v;
    ol.ol_stripe_unit = 0;
    return ol;
  }

  int get_pg_num(int pg_pool) const
  {
    const pg_pool_t *pool = get_pg_pool(pg_pool);
    return pool->get_pg_num();
  }

  // pg -> (osd list)
private:
  int _pg_to_osds(const pg_pool_t& pool, pg_t pg, vector<int>& osds) const {
    // map to osds[]
    ps_t pps = pool.raw_pg_to_pps(pg);  // placement ps
    unsigned size = pool.get_size();
    {
      int preferred = pg.preferred();
      if (preferred >= max_osd || preferred >= crush.get_max_devices())
	preferred = -1;

      assert(get_max_osd() >= crush.get_max_devices());

      // what crush rule?
      int ruleno = crush.find_rule(pool.get_crush_ruleset(), pool.get_type(), size);
      if (ruleno >= 0)
	crush.do_rule(ruleno, pps, osds, size, preferred, osd_weight);
    }
  
    return osds.size();
  }

  // pg -> (up osd list)
  void _raw_to_up_osds(pg_t pg, vector<int>& raw, vector<int>& up) const {
    up.clear();
    for (unsigned i=0; i<raw.size(); i++) {
      if (!exists(raw[i]) || is_down(raw[i])) 
	continue;
      up.push_back(raw[i]);
    }
  }
  
  bool _raw_to_temp_osds(const pg_pool_t& pool, pg_t pg, vector<int>& raw, vector<int>& temp) const {
    pg = pool.raw_pg_to_pg(pg);
    map<pg_t,vector<int> >::const_iterator p = pg_temp.find(pg);
    if (p != pg_temp.end()) {
      temp.clear();
      for (unsigned i=0; i<p->second.size(); i++) {
	if (!exists(p->second[i]) || is_down(p->second[i]))
	  continue;
	temp.push_back(p->second[i]);
      }
      return true;
    }
    return false;
  }

public:
  int pg_to_osds(pg_t pg, vector<int>& raw) const {
    const pg_pool_t *pool = get_pg_pool(pg.pool());
    if (!pool)
      return 0;
    return _pg_to_osds(*pool, pg, raw);
  }

  int pg_to_acting_osds(pg_t pg, vector<int>& acting) const {         // list of osd addr's
    const pg_pool_t *pool = get_pg_pool(pg.pool());
    if (!pool)
      return 0;
    vector<int> raw;
    _pg_to_osds(*pool, pg, raw);
    if (!_raw_to_temp_osds(*pool, pg, raw, acting))
      _raw_to_up_osds(pg, raw, acting);
    return acting.size();
  }

  void pg_to_raw_up(pg_t pg, vector<int>& up) {
    const pg_pool_t *pool = get_pg_pool(pg.pool());
    if (!pool)
      return;
    vector<int> raw;
    _pg_to_osds(*pool, pg, raw);
    _raw_to_up_osds(pg, raw, up);
  }
  
  void pg_to_up_acting_osds(pg_t pg, vector<int>& up, vector<int>& acting) const {
    const pg_pool_t *pool = get_pg_pool(pg.pool());
    if (!pool)
      return;
    vector<int> raw;
    _pg_to_osds(*pool, pg, raw);
    _raw_to_up_osds(pg, raw, up);
    if (!_raw_to_temp_osds(*pool, pg, raw, acting))
      acting = up;
  }

  int64_t lookup_pg_pool_name(const char *name) {
    if (name_pool.count(name))
      return name_pool[name];
    return -ENOENT;
  }

  int64_t get_pool_max() const {
    return pool_max;
  }
  const map<int64_t,pg_pool_t>& get_pools() const {
    return pools;
  }
  const char *get_pool_name(int64_t p) const {
    map<int64_t, string>::const_iterator i = pool_name.find(p);
    if (i != pool_name.end())
      return i->second.c_str();
    return 0;
  }
  bool have_pg_pool(int64_t p) const {
    return pools.count(p);
  }
  const pg_pool_t* get_pg_pool(int64_t p) const {
    map<int64_t, pg_pool_t>::const_iterator i = pools.find(p);
    if (i != pools.end())
      return &i->second;
    return NULL;
  }
  unsigned get_pg_size(pg_t pg) const {
    map<int64_t,pg_pool_t>::const_iterator p = pools.find(pg.pool());
    assert(p != pools.end());
    return p->second.get_size();
  }
  int get_pg_type(pg_t pg) const {
    assert(pools.count(pg.pool()));
    return pools.find(pg.pool())->second.get_type();
  }


  pg_t raw_pg_to_pg(pg_t pg) const {
    assert(pools.count(pg.pool()));
    return pools.find(pg.pool())->second.raw_pg_to_pg(pg);
  }

  // pg -> primary osd
  int get_pg_primary(pg_t pg) {
    vector<int> group;
    int nrep = pg_to_osds(pg, group);
    if (nrep)
      return group[0];
    return -1;  // we fail!
  }

  // pg -> acting primary osd
  int get_pg_acting_primary(pg_t pg) {
    vector<int> group;
    int nrep = pg_to_acting_osds(pg, group);
    if (nrep > 0)
      return group[0];
    return -1;  // we fail!
  }
  int get_pg_acting_tail(pg_t pg) {
    vector<int> group;
    int nrep = pg_to_acting_osds(pg, group);
    if (nrep > 0)
      return group[group.size()-1];
    return -1;  // we fail!
  }


  /* what replica # is a given osd? 0 primary, -1 for none. */
  static int calc_pg_rank(int osd, vector<int>& acting, int nrep=0) {
    if (!nrep)
      nrep = acting.size();
    for (int i=0; i<nrep; i++) 
      if (acting[i] == osd)
	return i;
    return -1;
  }
  static int calc_pg_role(int osd, vector<int>& acting, int nrep=0) {
    if (!nrep)
      nrep = acting.size();
    int rank = calc_pg_rank(osd, acting, nrep);
    
    if (rank < 0)
      return PG_ROLE_STRAY;
    else if (rank == 0) 
      return PG_ROLE_HEAD;
    else if (rank == 1) 
      return PG_ROLE_ACKER;
    else
      return PG_ROLE_MIDDLE;
  }
  
  int get_pg_role(pg_t pg, int osd) const {
    vector<int> group;
    int nrep = pg_to_osds(pg, group);
    return calc_pg_role(osd, group, nrep);
  }
  
  /* rank is -1 (stray), 0 (primary), 1,2,3,... (replica) */
  int get_pg_acting_rank(pg_t pg, int osd) {
    vector<int> group;
    int nrep = pg_to_acting_osds(pg, group);
    return calc_pg_rank(osd, group, nrep);
  }
  /* role is -1 (stray), 0 (primary), 1 (replica) */
  int get_pg_acting_role(pg_t pg, int osd) {
    vector<int> group;
    int nrep = pg_to_acting_osds(pg, group);
    return calc_pg_role(osd, group, nrep);
  }


  /*
   * handy helpers to build simple maps...
   */
  void build_simple(CephContext *cct, epoch_t e, uuid_d &fsid,
		    int num_osd, int pg_bits, int pgp_bits, int lpg_bits);
  void build_simple_from_conf(CephContext *cct, epoch_t e, uuid_d &fsid,
			      int pg_bits, int pgp_bits, int lpg_bits);
  static void build_simple_crush_map(CephContext *cct, CrushWrapper& crush,
				     map<int, const char*>& poolsets, int num_osd);
  static void build_simple_crush_map_from_conf(CephContext *cct, CrushWrapper& crush,
					       map<int, const char*>& rulesets);


private:
  void print_osd_line(int cur, ostream& out) const;
public:
  void print(ostream& out) const;
  void print_summary(ostream& out) const;
  void print_tree(ostream& out) const;

  string get_flag_string() const;
  void dump_json(ostream& out) const;
  void dump(Formatter *f) const;
  static void generate_test_instances(list<OSDMap*>& o);
};
WRITE_CLASS_ENCODER_FEATURES(OSDMap)
WRITE_CLASS_ENCODER_FEATURES(OSDMap::Incremental)

typedef std::tr1::shared_ptr<const OSDMap> OSDMapRef;

inline ostream& operator<<(ostream& out, const OSDMap& m) {
  m.print_summary(out);
  return out;
}


#endif
