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

#ifndef CEPH_PG_H
#define CEPH_PG_H

#include <boost/statechart/custom_reaction.hpp>
#include <boost/statechart/event.hpp>
#include <boost/statechart/simple_state.hpp>
#include <boost/statechart/state.hpp>
#include <boost/statechart/state_machine.hpp>
#include <boost/statechart/transition.hpp>

// re-include our assert to clobber boost's
#include "include/assert.h" 

#include "include/types.h"
#include "include/stringify.h"
#include "osd_types.h"
#include "include/buffer.h"
#include "include/xlist.h"
#include "include/atomic.h"

#include "OpRequest.h"
#include "OSDMap.h"
#include "os/ObjectStore.h"
#include "msg/Messenger.h"
#include "messages/MOSDRepScrub.h"

#include "common/DecayCounter.h"

#include <list>
#include <memory>
#include <string>
using namespace std;

#include <ext/hash_map>
#include <ext/hash_set>
using namespace __gnu_cxx;


//#define DEBUG_RECOVERY_OIDS   // track set of recovering oids explicitly, to find counting bugs


class OSD;
class MOSDOp;
class MOSDSubOp;
class MOSDSubOpReply;
class MOSDPGScan;
class MOSDPGBackfill;
class MOSDPGInfo;
class MOSDPGLog;


struct PGRecoveryStats {
  struct per_state_info {
    uint64_t enter, exit;     // enter/exit counts
    uint64_t events;
    utime_t event_time;       // time spent processing events
    utime_t total_time;       // total time in state
    utime_t min_time, max_time;

    per_state_info() : enter(0), exit(0), events(0) {}
  };
  map<const char *,per_state_info> info;
  Mutex lock;

  PGRecoveryStats() : lock("PGRecoverStats::lock") {}

  void reset() {
    Mutex::Locker l(lock);
    info.clear();
  }
  void dump(ostream& out) {
    Mutex::Locker l(lock);
    for (map<const char *,per_state_info>::iterator p = info.begin(); p != info.end(); p++) {
      per_state_info& i = p->second;
      out << i.enter << "\t" << i.exit << "\t"
	  << i.events << "\t" << i.event_time << "\t"
	  << i.total_time << "\t"
	  << i.min_time << "\t" << i.max_time << "\t"
	  << p->first << "\n";
	       
    }
  }

  void log_enter(const char *s) {
    Mutex::Locker l(lock);
    info[s].enter++;
  }
  void log_exit(const char *s, utime_t dur, uint64_t events, utime_t event_dur) {
    Mutex::Locker l(lock);
    per_state_info &i = info[s];
    i.exit++;
    i.total_time += dur;
    if (dur > i.max_time)
      i.max_time = dur;
    if (dur < i.min_time || i.min_time == utime_t())
      i.min_time = dur;
    i.events += events;
    i.event_time += event_dur;
  }
};

struct PGPool {
  int id;
  atomic_t nref;
  int num_pg;
  string name;
  uint64_t auid;

  pg_pool_t info;      
  SnapContext snapc;   // the default pool snapc, ready to go.

  interval_set<snapid_t> cached_removed_snaps;      // current removed_snaps set
  interval_set<snapid_t> newly_removed_snaps;  // newly removed in the last epoch

  PGPool(int i, const char *_name, uint64_t au) :
    id(i), num_pg(0), auid(au) {
    if (_name)
      name = _name;
  }

  void get() { nref.inc(); }
  void put() {
    if (nref.dec() == 0)
      delete this;
  }
};


/** PG - Replica Placement Group
 *
 */

class PG {
public:
  /* Exceptions */
  class read_log_error : public buffer::error {
  public:
    explicit read_log_error(const char *what) {
      snprintf(buf, sizeof(buf), "read_log_error: %s", what);
    }
    const char *what() const throw () {
      return buf;
    }
  private:
    char buf[512];
  };

  std::string gen_prefix() const;


  /**
   * IndexLog - adds in-memory index of the log, by oid.
   * plus some methods to manipulate it all.
   */
  struct IndexedLog : public pg_log_t {
    hash_map<hobject_t,pg_log_entry_t*> objects;  // ptrs into log.  be careful!
    hash_map<osd_reqid_t,pg_log_entry_t*> caller_ops;

    // recovery pointers
    list<pg_log_entry_t>::iterator complete_to;  // not inclusive of referenced item
    version_t last_requested;           // last object requested by primary

    /****/
    IndexedLog() {}

    void claim_log(const pg_log_t& o) {
      log = o.log;
      head = o.head;
      tail = o.tail;
      index();
    }

    void zero() {
      unindex();
      pg_log_t::clear();
      reset_recovery_pointers();
    }
    void reset_recovery_pointers() {
      complete_to = log.end();
      last_requested = 0;
    }

    bool logged_object(const hobject_t& oid) const {
      return objects.count(oid);
    }
    bool logged_req(const osd_reqid_t &r) const {
      return caller_ops.count(r);
    }
    eversion_t get_request_version(const osd_reqid_t &r) const {
      hash_map<osd_reqid_t,pg_log_entry_t*>::const_iterator p = caller_ops.find(r);
      if (p == caller_ops.end())
	return eversion_t();
      return p->second->version;    
    }

    void index() {
      objects.clear();
      caller_ops.clear();
      for (list<pg_log_entry_t>::iterator i = log.begin();
           i != log.end();
           i++) {
        objects[i->soid] = &(*i);
	if (i->reqid_is_indexed()) {
	  //assert(caller_ops.count(i->reqid) == 0);  // divergent merge_log indexes new before unindexing old
	  caller_ops[i->reqid] = &(*i);
	}
      }
    }

    void index(pg_log_entry_t& e) {
      if (objects.count(e.soid) == 0 || 
          objects[e.soid]->version < e.version)
        objects[e.soid] = &e;
      if (e.reqid_is_indexed()) {
	//assert(caller_ops.count(i->reqid) == 0);  // divergent merge_log indexes new before unindexing old
	caller_ops[e.reqid] = &e;
      }
    }
    void unindex() {
      objects.clear();
      caller_ops.clear();
    }
    void unindex(pg_log_entry_t& e) {
      // NOTE: this only works if we remove from the _tail_ of the log!
      if (objects.count(e.soid) && objects[e.soid]->version == e.version)
        objects.erase(e.soid);
      if (e.reqid_is_indexed() &&
	  caller_ops.count(e.reqid) &&  // divergent merge_log indexes new before unindexing old
	  caller_ops[e.reqid] == &e)
	caller_ops.erase(e.reqid);
    }


    // accessors
    pg_log_entry_t *is_updated(const hobject_t& oid) {
      if (objects.count(oid) && objects[oid]->is_update()) return objects[oid];
      return 0;
    }
    pg_log_entry_t *is_deleted(const hobject_t& oid) {
      if (objects.count(oid) && objects[oid]->is_delete()) return objects[oid];
      return 0;
    }
    
    // actors
    void add(pg_log_entry_t& e) {
      // add to log
      log.push_back(e);
      assert(e.version > head);
      assert(head.version == 0 || e.version.version > head.version);
      head = e.version;

      // to our index
      objects[e.soid] = &(log.back());
      caller_ops[e.reqid] = &(log.back());
    }

    void trim(ObjectStore::Transaction &t, eversion_t s);

    ostream& print(ostream& out) const;
  };
  

  /**
   * OndiskLog - some info about how we store the log on disk.
   */
  class OndiskLog {
  public:
    // ok
    uint64_t tail;                     // first byte of log. 
    uint64_t head;                     // byte following end of log.
    uint64_t zero_to;                // first non-zeroed byte of log.
    bool has_checksums;

    OndiskLog() : tail(0), head(0), zero_to(0) {}

    uint64_t length() { return head - tail; }
    bool trim_to(eversion_t v, ObjectStore::Transaction& t);

    void zero() {
      tail = 0;
      head = 0;
      zero_to = 0;
    }

    void encode(bufferlist& bl) const {
      ENCODE_START(4, 3, bl);
      ::encode(tail, bl);
      ::encode(head, bl);
      ::encode(zero_to, bl);
      ENCODE_FINISH(bl);
    }
    void decode(bufferlist::iterator& bl) {
      DECODE_START_LEGACY_COMPAT_LEN(3, 3, 3, bl);
      has_checksums = (struct_v >= 2);
      ::decode(tail, bl);
      ::decode(head, bl);
      if (struct_v >= 4)
	::decode(zero_to, bl);
      else
	zero_to = 0;
      DECODE_FINISH(bl);
    }
    void dump(Formatter *f) const {
      f->dump_unsigned("head", head);
      f->dump_unsigned("tail", tail);
      f->dump_unsigned("zero_to", zero_to);
    }
    static void generate_test_instances(list<OndiskLog*>& o) {
      o.push_back(new OndiskLog);
      o.push_back(new OndiskLog);
      o.back()->tail = 2;
      o.back()->head = 3;
      o.back()->zero_to = 1;
    }
  };
  WRITE_CLASS_ENCODER(OndiskLog)



  /*** PG ****/
protected:
  OSD *osd;
  PGPool *pool;

  OSDMapRef osdmap_ref;
  OSDMapRef get_osdmap() const {
    assert(is_locked());
    assert(osdmap_ref);
    return osdmap_ref;
  }

  /** locking and reference counting.
   * I destroy myself when the reference count hits zero.
   * lock() should be called before doing anything.
   * get() should be called on pointer copy (to another thread, etc.).
   * put() should be called on destruction of some previously copied pointer.
   * put_unlock() when done with the current pointer (_most common_).
   */  
  Mutex _lock;
  Cond _cond;
  atomic_t ref;

public:
  bool deleting;  // true while RemoveWQ should be chewing on us

  void lock(bool no_lockdep = false);
  void unlock() {
    //generic_dout(0) << this << " " << info.pgid << " unlock" << dendl;
    osdmap_ref.reset();
    _lock.Unlock();
  }

  /* During handle_osd_map, the osd holds a write lock to the osdmap.
   * *_with_map_lock_held assume that the map_lock is already held */
  void lock_with_map_lock_held(bool no_lockdep = false);

  void assert_locked() {
    assert(_lock.is_locked());
  }
  bool is_locked() const {
    return _lock.is_locked();
  }
  void wait() {
    assert(_lock.is_locked());
    _cond.Wait(_lock);
  }
  void kick() {
    assert(_lock.is_locked());
    _cond.Signal();
  }

  void get() {
    //generic_dout(0) << this << " " << info.pgid << " get " << ref.test() << dendl;
    //assert(_lock.is_locked());
    ref.inc();
  }
  void put() { 
    //generic_dout(0) << this << " " << info.pgid << " put " << ref.test() << dendl;
    if (ref.dec() == 0)
      delete this;
  }


  list<OpRequestRef> op_queue;  // op queue

  bool dirty_info, dirty_log;

public:
  struct Interval {
    vector<int> up, acting;
    epoch_t first, last;
    bool maybe_went_rw;

    Interval() : first(0), last(0), maybe_went_rw(false) {}

    void encode(bufferlist& bl) const {
      ENCODE_START(2, 2, bl);
      ::encode(first, bl);
      ::encode(last, bl);
      ::encode(up, bl);
      ::encode(acting, bl);
      ::encode(maybe_went_rw, bl);
      ENCODE_FINISH(bl);
    }
    void decode(bufferlist::iterator& bl) {
      DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
      ::decode(first, bl);
      ::decode(last, bl);
      ::decode(up, bl);
      ::decode(acting, bl);
      ::decode(maybe_went_rw, bl);
      DECODE_FINISH(bl);
    }
    void dump(Formatter *f) const {
      f->dump_unsigned("first", first);
      f->dump_unsigned("last", last);
      f->dump_int("maybe_went_rw", maybe_went_rw ? 1 : 0);
      f->open_array_section("up");
      for (vector<int>::const_iterator p = up.begin(); p != up.end(); ++p)
	f->dump_int("osd", *p);
      f->close_section();
      f->open_array_section("acting");
      for (vector<int>::const_iterator p = acting.begin(); p != acting.end(); ++p)
	f->dump_int("osd", *p);
      f->close_section();
    }
    static void generate_test_instances(list<Interval*>& o) {
      o.push_back(new Interval);
      o.push_back(new Interval);
      o.back()->up.push_back(1);
      o.back()->acting.push_back(2);
      o.back()->acting.push_back(3);
      o.back()->first = 4;
      o.back()->last = 5;
      o.back()->maybe_went_rw = true;
    }
  };
  WRITE_CLASS_ENCODER(Interval)

  // pg state
  pg_info_t        info;
  const coll_t coll;
  IndexedLog  log;
  hobject_t    log_oid;
  hobject_t    biginfo_oid;
  OndiskLog   ondisklog;
  pg_missing_t     missing;
  map<hobject_t, set<int> > missing_loc;
  set<int> missing_loc_sources;           // superset of missing_loc locations
  
  interval_set<snapid_t> snap_collections;
  map<epoch_t,Interval> past_intervals;

  interval_set<snapid_t> snap_trimq;

  /* You should not use these items without taking their respective queue locks
   * (if they have one) */
  xlist<PG*>::item recovery_item, scrub_item, scrub_finalize_item, snap_trim_item, remove_item, stat_queue_item;
  int recovery_ops_active;
  bool waiting_on_backfill;
#ifdef DEBUG_RECOVERY_OIDS
  set<hobject_t> recovering_oids;
#endif

  utime_t replay_until;

protected:
  int         role;    // 0 = primary, 1 = replica, -1=none.
  unsigned    state;   // PG_STATE_*

public:
  eversion_t  last_update_ondisk;    // last_update that has committed; ONLY DEFINED WHEN is_active()
  eversion_t  last_complete_ondisk;  // last_complete that has committed.
  eversion_t  last_update_applied;

  // primary state
 public:
  vector<int> up, acting, want_acting;
  map<int,eversion_t> peer_last_complete_ondisk;
  eversion_t  min_last_complete_ondisk;  // up: min over last_complete_ondisk, peer_last_complete_ondisk
  eversion_t  pg_trim_to;

  // [primary only] content recovery state
 protected:
  bool prior_set_built;

  struct PriorSet {
    set<int> probe; /// current+prior OSDs we need to probe.
    set<int> down;  /// down osds that would normally be in @a probe and might be interesting.
    map<int,epoch_t> blocked_by;  /// current lost_at values for any OSDs in cur set for which (re)marking them lost would affect cur set

    bool pg_down;   /// some down osds are included in @a cur; the DOWN pg state bit should be set.
    PriorSet(const OSDMap &osdmap,
	     const map<epoch_t, Interval> &past_intervals,
	     const vector<int> &up,
	     const vector<int> &acting,
	     const pg_info_t &info,
	     const PG *debug_pg=NULL);

    bool affected_by_map(const OSDMapRef osdmap, const PG *debug_pg=0) const;
  };

  friend std::ostream& operator<<(std::ostream& oss,
				  const struct PriorSet &prior);

  bool may_need_replay(const OSDMapRef osdmap) const;


public:    
  struct RecoveryCtx {
    utime_t start_time;
    map< int, map<pg_t, pg_query_t> > *query_map;
    map< int, MOSDPGInfo* > *info_map;
    map< int, vector<pg_info_t> > *notify_list;
    list< Context* > *context_list;
    ObjectStore::Transaction *transaction;
    RecoveryCtx() : query_map(0), info_map(0), notify_list(0),
		    context_list(0), transaction(0) {}
    RecoveryCtx(map< int, map<pg_t, pg_query_t> > *query_map,
		map< int, MOSDPGInfo* > *info_map,
		map< int, vector<pg_info_t> > *notify_list,
		list< Context* > *context_list,
		ObjectStore::Transaction *transaction)
      : query_map(query_map), info_map(info_map), 
	notify_list(notify_list),
	context_list(context_list), transaction(transaction) {}
  };

  struct NamedState {
    const char *state_name;
    utime_t enter_time;
    const char *get_state_name() { return state_name; }
    NamedState() : enter_time(ceph_clock_now(g_ceph_context)) {}
    virtual ~NamedState() {}
  };


protected:

  /*
   * peer_info    -- projected (updates _before_ replicas ack)
   * peer_missing -- committed (updates _after_ replicas ack)
   */
  
  bool        need_up_thru;
  set<int>    stray_set;   // non-acting osds that have PG data.
  eversion_t  oldest_update; // acting: lowest (valid) last_update in active set
  map<int,pg_info_t>    peer_info;   // info from peers (stray or prior)
  map<int,pg_missing_t> peer_missing;
  set<int>             peer_log_requested;  // logs i've requested (and start stamps)
  set<int>             peer_missing_requested;
  set<int>             stray_purged;  // i deleted these strays; ignore racing PGInfo from them
  set<int>             peer_activated;

  // primary-only, recovery-only state
  set<int>             might_have_unfound;  // These osds might have objects on them
					    // which are unfound on the primary
  bool need_flush;     // need to flush before any new activity

  epoch_t last_peering_reset;


  /* heartbeat peers */
public:
  Mutex heartbeat_peer_lock;
  set<int> heartbeat_peers;

protected:
  /**
   * BackfillInterval
   *
   * Represents the objects in a range [begin, end)
   *
   * Possible states:
   * 1) begin == end == hobject_t() indicates the the interval is unpopulated
   * 2) Else, objects contains all objects in [begin, end)
   */
  struct BackfillInterval {
    // info about a backfill interval on a peer
    map<hobject_t,eversion_t> objects;
    hobject_t begin;
    hobject_t end;
    
    /// clear content
    void clear() {
      objects.clear();
      begin = end = hobject_t();
    }

    void reset(hobject_t start) {
      clear();
      begin = end = start;
    }

    /// true if there are no objects in this interval
    bool empty() {
      return objects.empty();
    }

    /// true if interval extends to the end of the range
    bool extends_to_end() {
      return end == hobject_t::get_max();
    }

    /// Adjusts begin to the first object
    void trim() {
      if (objects.size())
	begin = objects.begin()->first;
      else
	begin = end;
    }

    /// drop first entry, and adjust @begin accordingly
    void pop_front() {
      assert(!objects.empty());
      objects.erase(objects.begin());
      if (objects.empty())
	begin = end;
      else
	begin = objects.begin()->first;
    }
  };
  
  BackfillInterval backfill_info;
  BackfillInterval peer_backfill_info;
  int backfill_target;

  friend class OSD;

public:
  int get_backfill_target() const {
    return backfill_target;
  }

protected:


  // pg waiters
  list<OpRequestRef>            waiting_for_active;
  list<OpRequestRef>            waiting_for_all_missing;
  map<hobject_t, list<OpRequestRef> > waiting_for_missing_object,
                                        waiting_for_degraded_object;
  map<eversion_t,list<OpRequestRef> > waiting_for_ondisk;
  map<eversion_t,OpRequestRef>   replay_queue;

  void requeue_object_waiters(map<hobject_t, list<OpRequestRef> >& m);

  // stats
  Mutex pg_stats_lock;
  bool pg_stats_valid;
  pg_stat_t pg_stats_stable;

  // for ordering writes
  ObjectStore::Sequencer osr;

  void update_stats();
  void clear_stats();

public:
  void clear_primary_state();

 public:
  bool is_acting(int osd) const { 
    for (unsigned i=0; i<acting.size(); i++)
      if (acting[i] == osd) return true;
    return false;
  }
  bool is_up(int osd) const { 
    for (unsigned i=0; i<up.size(); i++)
      if (up[i] == osd) return true;
    return false;
  }
  
  bool needs_recovery() const;

  void generate_past_intervals();
  void trim_past_intervals();
  void build_prior(std::auto_ptr<PriorSet> &prior_set);

  void remove_down_peer_info(const OSDMapRef osdmap);

  bool adjust_need_up_thru(const OSDMapRef osdmap);

  bool all_unfound_are_queried_or_lost(const OSDMapRef osdmap) const;
  virtual void mark_all_unfound_lost(int how) = 0;

  bool calc_min_last_complete_ondisk() {
    eversion_t min = last_complete_ondisk;
    for (unsigned i=1; i<acting.size(); i++) {
      if (peer_last_complete_ondisk.count(acting[i]) == 0)
	return false;   // we don't have complete info
      eversion_t a = peer_last_complete_ondisk[acting[i]];
      if (a < min)
	min = a;
    }
    if (min == min_last_complete_ondisk)
      return false;
    min_last_complete_ondisk = min;
    return true;
  }

  virtual void calc_trim_to() = 0;

  void proc_replica_log(ObjectStore::Transaction& t, pg_info_t &oinfo, pg_log_t &olog,
			pg_missing_t& omissing, int from);
  void proc_master_log(ObjectStore::Transaction& t, pg_info_t &oinfo, pg_log_t &olog,
		       pg_missing_t& omissing, int from);
  bool proc_replica_info(int from, pg_info_t &info);
  bool merge_old_entry(ObjectStore::Transaction& t, pg_log_entry_t& oe);
  void merge_log(ObjectStore::Transaction& t, pg_info_t &oinfo, pg_log_t &olog, int from);
  void rewind_divergent_log(ObjectStore::Transaction& t, eversion_t newhead);
  bool search_for_missing(const pg_info_t &oinfo, const pg_missing_t *omissing,
			  int fromosd);

  void check_for_lost_objects();
  void forget_lost_objects();

  void discover_all_missing(std::map< int, map<pg_t,pg_query_t> > &query_map);
  
  void trim_write_ahead();

  map<int, pg_info_t>::const_iterator find_best_info(const map<int, pg_info_t> &infos) const;
  bool calc_acting(int& newest_update_osd, vector<int>& want) const;
  bool choose_acting(int& newest_update_osd);
  void build_might_have_unfound();
  void replay_queued_ops();
  void activate(ObjectStore::Transaction& t, list<Context*>& tfin,
		map< int, map<pg_t,pg_query_t> >& query_map,
		map<int, MOSDPGInfo*> *activator_map=0);
  void _activate_committed(epoch_t e, entity_inst_t& primary);
  void all_activated_and_committed();

  void proc_primary_info(ObjectStore::Transaction &t, const pg_info_t &info);

  bool have_unfound() const { 
    return missing.num_missing() > missing_loc.size();
  }
  int get_num_unfound() const {
    return missing.num_missing() - missing_loc.size();
  }

  virtual void clean_up_local(ObjectStore::Transaction& t) = 0;

  virtual int start_recovery_ops(int max, RecoveryCtx *prctx) = 0;

  void purge_strays();

  void update_heartbeat_peers();

  Context *finish_sync_event;

  void finish_recovery(ObjectStore::Transaction& t, list<Context*>& tfin);
  void _finish_recovery(Context *c);
  void cancel_recovery();
  void clear_recovery_state();
  virtual void _clear_recovery_state() = 0;
  void defer_recovery();
  virtual bool check_recovery_sources(const OSDMapRef newmap) = 0;
  void start_recovery_op(const hobject_t& soid);
  void finish_recovery_op(const hobject_t& soid, bool dequeue=false);

  loff_t get_log_write_pos() {
    return 0;
  }

  friend class C_OSD_RepModify_Commit;


  // -- scrub --
  set<int> scrub_reserved_peers;
  map<int,ScrubMap> scrub_received_maps;
  bool finalizing_scrub; 
  bool scrub_reserved, scrub_reserve_failed;
  int scrub_waiting_on;
  epoch_t scrub_epoch_start;
  ScrubMap primary_scrubmap;
  MOSDRepScrub *active_rep_scrub;

  void repair_object(const hobject_t& soid, ScrubMap::object *po, int bad_peer, int ok_peer);
  bool _compare_scrub_objects(ScrubMap::object &auth,
			      ScrubMap::object &candidate,
			      ostream &errorstream);
  void _compare_scrubmaps(const map<int,ScrubMap*> &maps,  
			  map<hobject_t, set<int> > &missing,
			  map<hobject_t, set<int> > &inconsistent,
			  map<hobject_t, int> &authoritative,
			  ostream &errorstream);
  void scrub();
  void scrub_finalize();
  void scrub_clear_state();
  bool scrub_gather_replica_maps();
  void _scan_list(ScrubMap &map, vector<hobject_t> &ls);
  void _request_scrub_map(int replica, eversion_t version);
  void build_scrub_map(ScrubMap &map);
  void build_inc_scrub_map(ScrubMap &map, eversion_t v);
  virtual int _scrub(ScrubMap &map, int& errors, int& fixed) { return 0; }
  void clear_scrub_reserved();
  void scrub_reserve_replicas();
  void scrub_unreserve_replicas();
  bool scrub_all_replicas_reserved() const;
  bool sched_scrub();

  void replica_scrub(class MOSDRepScrub *op);
  void sub_op_scrub_map(OpRequestRef op);
  void sub_op_scrub_reserve(OpRequestRef op);
  void sub_op_scrub_reserve_reply(OpRequestRef op);
  void sub_op_scrub_unreserve(OpRequestRef op);
  void sub_op_scrub_stop(OpRequestRef op);


  // -- recovery state --

  /* Encapsulates PG recovery process */
  class RecoveryState {
    void start_handle(RecoveryCtx *new_ctx) {
      assert(!rctx);
      rctx = new_ctx;
      if (rctx)
	rctx->start_time = ceph_clock_now(g_ceph_context);
    }

    void end_handle() {
      if (rctx) {
	utime_t dur = ceph_clock_now(g_ceph_context) - rctx->start_time;
	machine.event_time += dur;
      }
      machine.event_count++;
      rctx = 0;
    }

    struct QueryState : boost::statechart::event< QueryState > {
      Formatter *f;
      QueryState(Formatter *f) : f(f) {}
    };

    struct MInfoRec : boost::statechart::event< MInfoRec > {
      int from;
      pg_info_t &info;
      MInfoRec(int from, pg_info_t &info) :
	from(from), info(info) {}
    };

    struct MLogRec : boost::statechart::event< MLogRec > {
      int from;
      MOSDPGLog *msg;
      MLogRec(int from, MOSDPGLog *msg) :
	from(from), msg(msg) {}
    };

    struct MNotifyRec : boost::statechart::event< MNotifyRec > {
      int from;
      pg_info_t &info;
      MNotifyRec(int from, pg_info_t &info) :
	from(from), info(info) {}
    };

    struct MQuery : boost::statechart::event< MQuery > {
      int from;
      const pg_query_t &query;
      epoch_t query_epoch;
      MQuery(int from, const pg_query_t &query, epoch_t query_epoch):
	from(from), query(query), query_epoch(query_epoch) {}
    };

    struct AdvMap : boost::statechart::event< AdvMap > {
      OSDMapRef osdmap;
      OSDMapRef lastmap;
      vector<int> newup, newacting;
      AdvMap(OSDMapRef osdmap, OSDMapRef lastmap, vector<int>& newup, vector<int>& newacting):
	osdmap(osdmap), lastmap(lastmap), newup(newup), newacting(newacting) {}
    };

    struct RecoveryComplete : boost::statechart::event< RecoveryComplete > {
      RecoveryComplete() : boost::statechart::event< RecoveryComplete >() {}
    };
    struct ActMap : boost::statechart::event< ActMap > {
      ActMap() : boost::statechart::event< ActMap >() {}
    };
    struct Activate : boost::statechart::event< Activate > {
      Activate() : boost::statechart::event< Activate >() {}
    };
    struct Initialize : boost::statechart::event< Initialize > {
      Initialize() : boost::statechart::event< Initialize >() {}
    };
    struct Load : boost::statechart::event< Load > {
      Load() : boost::statechart::event< Load >() {}
    };
    struct GotInfo : boost::statechart::event< GotInfo > {
      GotInfo() : boost::statechart::event< GotInfo >() {}
    };
    struct NeedUpThru : boost::statechart::event< NeedUpThru > {
      NeedUpThru() : boost::statechart::event< NeedUpThru >() {};
    };


    /* States */
    struct Initial;
    class RecoveryMachine : public boost::statechart::state_machine< RecoveryMachine, Initial > {
      RecoveryState *state;
    public:
      PG *pg;

      utime_t event_time;
      uint64_t event_count;
      
      void clear_event_counters() {
	event_time = utime_t();
	event_count = 0;
      }

      void log_enter(const char *state_name);
      void log_exit(const char *state_name, utime_t duration);

      RecoveryMachine(RecoveryState *state, PG *pg) : state(state), pg(pg), event_count(0) {}

      /* Accessor functions for state methods */
      ObjectStore::Transaction* get_cur_transaction() {
	assert(state->rctx->transaction);
	return state->rctx->transaction;
      }

      void send_query(int to, const pg_query_t &query) {
	assert(state->rctx->query_map);
	(*state->rctx->query_map)[to][pg->info.pgid] = query;
      }

      map<int, map<pg_t, pg_query_t> > *get_query_map() {
	assert(state->rctx->query_map);
	return state->rctx->query_map;
      }

      map<int, MOSDPGInfo*> *get_info_map() {
	assert(state->rctx->info_map);
	return state->rctx->info_map;
      }

      list< Context* > *get_context_list() {
	assert(state->rctx->context_list);
	return state->rctx->context_list;
      }

      void send_notify(int to, const pg_info_t &info) {
	assert(state->rctx->notify_list);
	(*state->rctx->notify_list)[to].push_back(info);
      }
    };
    friend class RecoveryMachine;

    /* States */
    struct NamedState {
      const char *state_name;
      utime_t enter_time;
      const char *get_state_name() { return state_name; }
      NamedState() : enter_time(ceph_clock_now(g_ceph_context)) {}
      virtual ~NamedState() {}
    };

    struct Crashed : boost::statechart::state< Crashed, RecoveryMachine >, NamedState {
      Crashed(my_context ctx);
    };

    struct Started;
    struct Reset;

    struct Initial : boost::statechart::state< Initial, RecoveryMachine >, NamedState {
      Initial(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::transition< Initialize, Started >,
	boost::statechart::transition< Load, Reset >,
	boost::statechart::custom_reaction< MNotifyRec >,
	boost::statechart::custom_reaction< MInfoRec >,
	boost::statechart::custom_reaction< MLogRec >,
	boost::statechart::transition< boost::statechart::event_base, Crashed >
	> reactions;

      boost::statechart::result react(const MNotifyRec&);
      boost::statechart::result react(const MInfoRec&);
      boost::statechart::result react(const MLogRec&);
    };

    struct Reset : boost::statechart::state< Reset, RecoveryMachine >, NamedState {
      Reset(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< AdvMap >,
	boost::statechart::custom_reaction< ActMap >,
	boost::statechart::transition< boost::statechart::event_base, Crashed >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const AdvMap&);
      boost::statechart::result react(const ActMap&);
    };

    struct Start;

    struct Started : boost::statechart::state< Started, RecoveryMachine, Start >, NamedState {
      Started(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< AdvMap >,
	boost::statechart::transition< boost::statechart::event_base, Crashed >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const AdvMap&);
    };

    struct MakePrimary : boost::statechart::event< MakePrimary > {
      MakePrimary() : boost::statechart::event< MakePrimary >() {}
    };
    struct MakeStray : boost::statechart::event< MakeStray > {
      MakeStray() : boost::statechart::event< MakeStray >() {}
    };
    struct Primary;
    struct Stray;

    struct Start : boost::statechart::state< Start, Started >, NamedState {
      Start(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::transition< MakePrimary, Primary >,
	boost::statechart::transition< MakeStray, Stray >
	> reactions;
    };

    struct Peering;
    struct WaitActingChange;
    struct NeedActingChange : boost::statechart::event< NeedActingChange > {
      NeedActingChange() : boost::statechart::event< NeedActingChange >() {}
    };
    struct Incomplete;
    struct IsIncomplete : boost::statechart::event< IsIncomplete > {
      IsIncomplete() : boost::statechart::event< IsIncomplete >() {}
    };

    struct Primary : boost::statechart::state< Primary, Started, Peering >, NamedState {
      Primary(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< ActMap >,
	boost::statechart::custom_reaction< MNotifyRec >,
	boost::statechart::transition< NeedActingChange, WaitActingChange >,
	boost::statechart::transition< IsIncomplete, Incomplete >
	> reactions;
      boost::statechart::result react(const ActMap&);
      boost::statechart::result react(const MNotifyRec&);
    };

    struct WaitActingChange : boost::statechart::state< WaitActingChange, Primary>,
			      NamedState {
      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< AdvMap >,
	boost::statechart::custom_reaction< MLogRec >,
	boost::statechart::custom_reaction< MInfoRec >,
	boost::statechart::custom_reaction< MNotifyRec >
	> reactions;
      WaitActingChange(my_context ctx);
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const AdvMap&);
      boost::statechart::result react(const MLogRec&);
      boost::statechart::result react(const MInfoRec&);
      boost::statechart::result react(const MNotifyRec&);
      void exit();
    };

    struct Incomplete : boost::statechart::state< Incomplete, Primary>,
			NamedState {
      Incomplete(my_context ctx);
      void exit();
    };
    
    struct GetInfo;
    struct Active;

    struct Peering : boost::statechart::state< Peering, Primary, GetInfo >, NamedState {
      std::auto_ptr< PriorSet > prior_set;

      Peering(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::transition< Activate, Active >,
	boost::statechart::custom_reaction< AdvMap >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const AdvMap &advmap);
    };

    struct Active : boost::statechart::state< Active, Primary >, NamedState {
      Active(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< ActMap >,
	boost::statechart::custom_reaction< AdvMap >,
	boost::statechart::custom_reaction< MInfoRec >,
	boost::statechart::custom_reaction< MNotifyRec >,
	boost::statechart::custom_reaction< MLogRec >,
	boost::statechart::custom_reaction< RecoveryComplete >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const ActMap&);
      boost::statechart::result react(const AdvMap&);
      boost::statechart::result react(const MInfoRec& infoevt);
      boost::statechart::result react(const MNotifyRec& notevt);
      boost::statechart::result react(const MLogRec& logevt);
      boost::statechart::result react(const RecoveryComplete&);
    };

    struct ReplicaActive : boost::statechart::state< ReplicaActive, Started >, NamedState {
      ReplicaActive(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< ActMap >,
	boost::statechart::custom_reaction< MQuery >,
	boost::statechart::custom_reaction< MInfoRec >,
	boost::statechart::custom_reaction< MLogRec >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const MInfoRec& infoevt);
      boost::statechart::result react(const MLogRec& logevt);
      boost::statechart::result react(const ActMap&);
      boost::statechart::result react(const MQuery&);
    };

    struct Stray : boost::statechart::state< Stray, Started >, NamedState {
      map<int, pair<pg_query_t, epoch_t> > pending_queries;

      Stray(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< MQuery >,
	boost::statechart::custom_reaction< MLogRec >,
	boost::statechart::custom_reaction< MInfoRec >,
	boost::statechart::custom_reaction< ActMap >,
	boost::statechart::transition< Activate, ReplicaActive >
	> reactions;
      boost::statechart::result react(const MQuery& query);
      boost::statechart::result react(const MLogRec& logevt);
      boost::statechart::result react(const MInfoRec& infoevt);
      boost::statechart::result react(const ActMap&);
    };

    struct GetLog;

    struct GetInfo : boost::statechart::state< GetInfo, Peering >, NamedState {
      set<int> peer_info_requested;

      GetInfo(my_context ctx);
      void exit();
      void get_infos();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::transition< GotInfo, GetLog >,
	boost::statechart::custom_reaction< MNotifyRec >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const MNotifyRec& infoevt);
    };

    struct GetMissing;
    struct GotLog : boost::statechart::event< GotLog > {
      GotLog() : boost::statechart::event< GotLog >() {}
    };

    struct GetLog : boost::statechart::state< GetLog, Peering >, NamedState {
      int newest_update_osd;
      MOSDPGLog *msg;

      GetLog(my_context ctx);
      ~GetLog();
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< MLogRec >,
	boost::statechart::custom_reaction< GotLog >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const MLogRec& logevt);
      boost::statechart::result react(const GotLog&);
    };

    struct WaitUpThru;

    struct GetMissing : boost::statechart::state< GetMissing, Peering >, NamedState {
      set<int> peer_missing_requested;

      GetMissing(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< MLogRec >,
	boost::statechart::transition< NeedUpThru, WaitUpThru >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const MLogRec& logevt);
    };

    struct WaitUpThru : boost::statechart::state< WaitUpThru, Peering >, NamedState {
      WaitUpThru(my_context ctx);
      void exit();

      typedef boost::mpl::list <
	boost::statechart::custom_reaction< QueryState >,
	boost::statechart::custom_reaction< ActMap >,
	boost::statechart::custom_reaction< MLogRec >
	> reactions;
      boost::statechart::result react(const QueryState& q);
      boost::statechart::result react(const ActMap& am);
      boost::statechart::result react(const MLogRec& logrec);
    };


    RecoveryMachine machine;
    PG *pg;
    RecoveryCtx *rctx;

  public:
    RecoveryState(PG *pg) : machine(this, pg), pg(pg), rctx(0) {
      machine.initiate();
    }

    void handle_notify(int from, pg_info_t& i, RecoveryCtx *ctx);
    void handle_info(int from, pg_info_t& i, RecoveryCtx *ctx);
    void handle_log(int from,
		    MOSDPGLog *msg,
		    RecoveryCtx *ctx);
    void handle_query(int from, const pg_query_t& q,
		      epoch_t query_epoch,
		      RecoveryCtx *ctx);
    void handle_advance_map(OSDMapRef osdmap, OSDMapRef lastmap,
			    vector<int>& newup, vector<int>& newacting, 
			    RecoveryCtx *ctx);
    void handle_activate_map(RecoveryCtx *ctx);
    void handle_recovery_complete(RecoveryCtx *ctx);
    void handle_create(RecoveryCtx *ctx);
    void handle_loaded(RecoveryCtx *ctx);
    void handle_query_state(Formatter *f);
  } recovery_state;


 public:  
  PG(OSD *o, PGPool *_pool, pg_t p, const hobject_t& loid, const hobject_t& ioid) : 
    osd(o), pool(_pool),
    _lock("PG::_lock"),
    ref(0), deleting(false), dirty_info(false), dirty_log(false),
    info(p), coll(p), log_oid(loid), biginfo_oid(ioid),
    recovery_item(this), scrub_item(this), scrub_finalize_item(this), snap_trim_item(this), remove_item(this), stat_queue_item(this),
    recovery_ops_active(0),
    waiting_on_backfill(0),
    role(0),
    state(0),
    need_up_thru(false),
    need_flush(false),
    last_peering_reset(0),
    heartbeat_peer_lock("PG::heartbeat_peer_lock"),
    backfill_target(-1),
    pg_stats_lock("PG::pg_stats_lock"),
    pg_stats_valid(false),
    osr(stringify(p)),
    finish_sync_event(NULL),
    finalizing_scrub(false),
    scrub_reserved(false), scrub_reserve_failed(false),
    scrub_waiting_on(0),
    active_rep_scrub(0),
    recovery_state(this)
  {
    pool->get();
  }
  virtual ~PG() {
    pool->put();
  }
  
 private:
  // Prevent copying
  PG(const PG& rhs);
  PG& operator=(const PG& rhs);

 public:
  pg_t       get_pgid() const { return info.pgid; }
  int        get_nrep() const { return acting.size(); }

  int        get_primary() { return acting.empty() ? -1:acting[0]; }
  
  int        get_role() const { return role; }
  void       set_role(int r) { role = r; }

  bool       is_primary() const { return role == PG_ROLE_HEAD; }
  bool       is_replica() const { return role > 0; }

  epoch_t get_last_peering_reset() const { return last_peering_reset; }
  
  //int  get_state() const { return state; }
  bool state_test(int m) const { return (state & m) != 0; }
  void state_set(int m) { state |= m; }
  void state_clear(int m) { state &= ~m; }

  bool is_complete() const { return info.last_complete == info.last_update; }

  int get_state() const { return state; }
  bool       is_active() const { return state_test(PG_STATE_ACTIVE); }
  bool       is_peering() const { return state_test(PG_STATE_PEERING); }
  bool       is_down() const { return state_test(PG_STATE_DOWN); }
  bool       is_replay() const { return state_test(PG_STATE_REPLAY); }
  bool       is_clean() const { return state_test(PG_STATE_CLEAN); }
  bool       is_degraded() const { return state_test(PG_STATE_DEGRADED); }
  bool       is_stray() const { return state_test(PG_STATE_STRAY); }

  bool       is_scrubbing() const { return state_test(PG_STATE_SCRUBBING); }

  bool  is_empty() const { return info.last_update == eversion_t(0,0); }

  void init(int role, vector<int>& up, vector<int>& acting, pg_history_t& history,
	    ObjectStore::Transaction *t);

  // pg on-disk state
  void do_pending_flush();

  void write_info(ObjectStore::Transaction& t);
  void write_log(ObjectStore::Transaction& t);

  void add_log_entry(pg_log_entry_t& e, bufferlist& log_bl);
  void append_log(vector<pg_log_entry_t>& logv, eversion_t trim_to, ObjectStore::Transaction &t);

  void read_log(ObjectStore *store);
  bool check_log_for_corruption(ObjectStore *store);
  void trim(ObjectStore::Transaction& t, eversion_t v);
  void trim_ondisklog(ObjectStore::Transaction& t);
  void trim_peers();

  std::string get_corrupt_pg_log_name() const;
  void read_state(ObjectStore *store);
  coll_t make_snap_collection(ObjectStore::Transaction& t, snapid_t sn);
  void update_snap_collections(vector<pg_log_entry_t> &log_entries,
			       ObjectStore::Transaction& t);
  void filter_snapc(SnapContext& snapc);
  void adjust_local_snaps();

  void log_weirdness();

  void queue_snap_trim();
  bool queue_scrub();

  /// share pg info after a pg is active
  void share_pg_info();
  /// share new pg log entries after a pg is active
  void share_pg_log();

  void start_peering_interval(const OSDMapRef lastmap,
			      const vector<int>& newup,
			      const vector<int>& newacting);
  void set_last_peering_reset();

  void fulfill_info(int from, const pg_query_t &query, 
		    pair<int, pg_info_t> &notify_info);
  void fulfill_log(int from, const pg_query_t &query, epoch_t query_epoch);
  bool acting_up_affected(const vector<int>& newup, const vector<int>& newacting);
  bool old_peering_msg(epoch_t reply_epoch, epoch_t query_epoch);

  // recovery bits
  void handle_notify(int from, pg_info_t& i, RecoveryCtx *rctx) {
    recovery_state.handle_notify(from, i, rctx);
  }
  void handle_info(int from, pg_info_t& i, RecoveryCtx *rctx) {
    recovery_state.handle_info(from, i, rctx);
  }
  void handle_log(int from,
		  MOSDPGLog *msg,
		  RecoveryCtx *rctx) {
    recovery_state.handle_log(from, msg, rctx);
  }
  void handle_query(int from, const pg_query_t& q,
		    epoch_t query_epoch,
		    RecoveryCtx *rctx) {
    recovery_state.handle_query(from, q, query_epoch, rctx);
  }
  void handle_advance_map(OSDMapRef osdmap, OSDMapRef lastmap,
			  vector<int>& newup, vector<int>& newacting,
			  RecoveryCtx *rctx) {
    recovery_state.handle_advance_map(osdmap, lastmap, newup, newacting, rctx);
  }
  void handle_activate_map(RecoveryCtx *rctx) {
    recovery_state.handle_activate_map(rctx);
  }
  void handle_recovery_complete(RecoveryCtx *rctx) {
    recovery_state.handle_recovery_complete(rctx);
  }
  void handle_create(RecoveryCtx *rctx) {
    recovery_state.handle_create(rctx);
  }
  void handle_loaded(RecoveryCtx *rctx) {
    recovery_state.handle_loaded(rctx);
  }

  void on_removal();


  // abstract bits
  void do_request(OpRequestRef op);

  virtual void do_op(OpRequestRef op) = 0;
  virtual void do_sub_op(OpRequestRef op) = 0;
  virtual void do_sub_op_reply(OpRequestRef op) = 0;
  virtual void do_scan(OpRequestRef op) = 0;
  virtual void do_backfill(OpRequestRef op) = 0;
  virtual bool snap_trimmer() = 0;

  virtual int do_command(vector<string>& cmd, ostream& ss,
			 bufferlist& idata, bufferlist& odata) = 0;

  virtual bool same_for_read_since(epoch_t e) = 0;
  virtual bool same_for_modify_since(epoch_t e) = 0;
  virtual bool same_for_rep_modify_since(epoch_t e) = 0;

  virtual void on_role_change() = 0;
  virtual void on_change() = 0;
  virtual void on_activate() = 0;
  virtual void on_shutdown() = 0;
  virtual void remove_watchers_and_notifies() = 0;

  virtual void register_unconnected_watcher(void *obc,
					    entity_name_t entity,
					    utime_t expire) = 0;
  virtual void unregister_unconnected_watcher(void *obc,
					      entity_name_t entity) = 0;
  virtual void handle_watch_timeout(void *obc,
				    entity_name_t entity,
				    utime_t expire) = 0;
};

WRITE_CLASS_ENCODER(PG::Interval)
WRITE_CLASS_ENCODER(PG::OndiskLog)

inline ostream& operator<<(ostream& out, const PG::Interval& i)
{
  out << "interval(" << i.first << "-" << i.last << " " << i.up << "/" << i.acting;
  if (i.maybe_went_rw)
    out << " maybe_went_rw";
  out << ")";
  return out;
}

ostream& operator<<(ostream& out, const PG& pg);

#endif
