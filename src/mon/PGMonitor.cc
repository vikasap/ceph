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


#include "PGMonitor.h"
#include "Monitor.h"
#include "MDSMonitor.h"
#include "OSDMonitor.h"
#include "MonitorStore.h"

#include "messages/MPGStats.h"
#include "messages/MPGStatsAck.h"
#include "messages/MGetPoolStats.h"
#include "messages/MGetPoolStatsReply.h"

#include "messages/MStatfs.h"
#include "messages/MStatfsReply.h"
#include "messages/MOSDPGCreate.h"
#include "messages/MMonCommand.h"
#include "messages/MOSDScrub.h"

#include "common/Timer.h"
#include "common/Formatter.h"
#include "common/ceph_argparse.h"
#include "common/perf_counters.h"

#include "osd/osd_types.h"
#include "osd/PG.h"  // yuck

#include "common/config.h"
#include <sstream>

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, pg_map)
static ostream& _prefix(std::ostream *_dout, const Monitor *mon, const PGMap& pg_map) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
		<< "(" << mon->get_state_name()
		<< ").pg v" << pg_map.version << " ";
}

class RatioMonitor : public md_config_obs_t {
  PGMonitor *mon;
public:
  RatioMonitor(PGMonitor *pgmon) : mon(pgmon) {}
  virtual ~RatioMonitor() {}
  virtual const char **get_tracked_conf_keys() const {
    static const char *KEYS[] = { "mon_osd_full_ratio",
				  "mon_osd_nearfull_ratio",
				  NULL };
    return KEYS;
  }
  virtual void handle_conf_change(const md_config_t *conf,
				  const std::set<std::string>& changed) {
    mon->update_full_ratios(conf->mon_osd_full_ratio,
			    conf->mon_osd_nearfull_ratio);
  }
};

PGMonitor::PGMonitor(Monitor *mn, Paxos *p)
  : PaxosService(mn, p),
    ratio_lock("PGMonitor::ratio_lock"),
    need_full_ratio_update(false),
    need_nearfull_ratio_update(false),
    need_check_down_pgs(false)
{
  ratio_monitor = new RatioMonitor(this);
  g_conf->add_observer(ratio_monitor);
}

PGMonitor::~PGMonitor()
{
  g_conf->remove_observer(ratio_monitor);
  delete ratio_monitor;
}

/*
 Tick function to update the map based on performance every N seconds
*/

void PGMonitor::on_restart()
{
  // clear leader state
  last_sent_pg_create.clear();
  last_osd_report.clear();
}

void PGMonitor::on_active()
{
  if (mon->is_leader()) {
    check_osd_map(mon->osdmon()->osdmap.epoch);
    need_check_down_pgs = true;
  }

  update_logger();
}

void PGMonitor::update_logger()
{
  dout(10) << "update_logger" << dendl;

  mon->cluster_logger->set(l_cluster_osd_kb, pg_map.osd_sum.kb);
  mon->cluster_logger->set(l_cluster_osd_kb_used, pg_map.osd_sum.kb_used);
  mon->cluster_logger->set(l_cluster_osd_kb_avail, pg_map.osd_sum.kb_avail);

  mon->cluster_logger->set(l_cluster_num_pool, pg_map.pg_pool_sum.size());
  mon->cluster_logger->set(l_cluster_num_pg, pg_map.pg_stat.size());

  unsigned active = 0, active_clean = 0, peering = 0;
  for (hash_map<int,int>::iterator p = pg_map.num_pg_by_state.begin();
       p != pg_map.num_pg_by_state.end();
       ++p) {
    if (p->second & PG_STATE_ACTIVE) {
      active++;
      if (p->second & PG_STATE_CLEAN)
	active_clean++;
    }
    if (p->second & PG_STATE_PEERING)
      peering++;
  }
  mon->cluster_logger->set(l_cluster_num_pg_active_clean, active_clean);
  mon->cluster_logger->set(l_cluster_num_pg_active, active);
  mon->cluster_logger->set(l_cluster_num_pg_peering, peering);

  mon->cluster_logger->set(l_cluster_num_object, pg_map.pg_sum.stats.sum.num_objects);
  mon->cluster_logger->set(l_cluster_num_object_degraded, pg_map.pg_sum.stats.sum.num_objects_degraded);
  mon->cluster_logger->set(l_cluster_num_object_unfound, pg_map.pg_sum.stats.sum.num_objects_unfound);
  mon->cluster_logger->set(l_cluster_num_bytes, pg_map.pg_sum.stats.sum.num_bytes);
}

void PGMonitor::update_full_ratios(float full_ratio, float nearfull_ratio)
{
  Mutex::Locker l(ratio_lock);

  if (full_ratio > 1.0)
    full_ratio /= 100.0;
  if (nearfull_ratio > 1.0)
    nearfull_ratio /= 100.0;

  dout(10) << "update_full_ratios full " << full_ratio << " nearfull " << nearfull_ratio << dendl;
  if (full_ratio != 0) {
    new_full_ratio = full_ratio;
    need_full_ratio_update = true;
  }
  if (nearfull_ratio != 0) {
    new_nearfull_ratio = nearfull_ratio;
    need_nearfull_ratio_update = true;
  }
}

void PGMonitor::tick() 
{
  if (!paxos->is_active()) return;

  update_from_paxos();
  handle_osd_timeouts();

  if (mon->is_leader()) {
    ratio_lock.Lock();
    bool propose = false;
    if (need_full_ratio_update) {
      dout(10) << "tick need full ratio update " << new_full_ratio << dendl;
      need_full_ratio_update = false;
      if (pg_map.full_ratio != new_full_ratio) {
	pending_inc.full_ratio = new_full_ratio;
	propose = true;
      }
    }
    if (need_nearfull_ratio_update) {
      dout(10) << "tick need nearfull ratio update " << new_nearfull_ratio << dendl;
      need_nearfull_ratio_update = false;
      if (pg_map.nearfull_ratio != new_nearfull_ratio) {
	pending_inc.nearfull_ratio = new_nearfull_ratio;
	propose = true;
      }
    }
    ratio_lock.Unlock();
    
    if (need_check_down_pgs && check_down_pgs())
      propose = true;
    
    if (propose) {
      propose_pending();
    }
  }

  dout(10) << pg_map << dendl;
}

void PGMonitor::create_initial()
{
  dout(10) << "create_initial -- creating initial map" << dendl;
  pg_map.full_ratio = g_conf->mon_osd_full_ratio;
  if (pg_map.full_ratio > 1.0)
    pg_map.full_ratio /= 100.0;
  pg_map.nearfull_ratio = g_conf->mon_osd_nearfull_ratio;
  if (pg_map.nearfull_ratio > 1.0)
    pg_map.nearfull_ratio /= 100.0;
}

void PGMonitor::update_from_paxos()
{
  version_t paxosv = paxos->get_version();
  if (paxosv == pg_map.version)
    return;
  assert(paxosv >= pg_map.version);

  if (pg_map.version != paxos->get_stashed_version()) {
    bufferlist latest;
    version_t v = paxos->get_stashed(latest);
    dout(7) << "update_from_paxos loading latest full pgmap v" << v << dendl;
    try {
      PGMap tmp_pg_map;
      bufferlist::iterator p = latest.begin();
      tmp_pg_map.decode(p);
      pg_map = tmp_pg_map;
    }
    catch (const std::exception &e) {
      dout(0) << "update_from_paxos: error parsing update: "
	      << e.what() << dendl;
      assert(0 == "update_from_paxos: error parsing update");
      return;
    }
  } 

  // walk through incrementals
  utime_t now(ceph_clock_now(g_ceph_context));
  while (paxosv > pg_map.version) {
    bufferlist bl;
    bool success = paxos->read(pg_map.version+1, bl);
    assert(success);

    dout(7) << "update_from_paxos  applying incremental " << pg_map.version+1 << dendl;
    PGMap::Incremental inc;
    try {
      bufferlist::iterator p = bl.begin();
      inc.decode(p);
    }
    catch (const std::exception &e) {
      dout(0) << "update_from_paxos: error parsing "
	      << "incremental update: " << e.what() << dendl;
      assert(0 == "update_from_paxos: error parsing incremental update");
      return;
    }

    pg_map.apply_incremental(inc);
    
    dout(10) << pg_map << dendl;

    if (inc.pg_scan)
      last_sent_pg_create.clear();  // reset pg_create throttle timer
  }

  assert(paxosv == pg_map.version);

  // save latest
  bufferlist bl;
  pg_map.encode(bl);
  paxos->stash_latest(paxosv, bl);

  // dump pgmap summaries?  (useful for debugging)
  if (0) {
    stringstream ds;
    pg_map.dump(ds);
    bufferlist d;
    d.append(ds);
    mon->store->put_bl_sn(d, "pgmap_dump", paxosv);
  }

  unsigned max = g_conf->mon_max_pgmap_epochs;
  if (mon->is_leader() &&
      paxosv > max)
    paxos->trim_to(paxosv - max);

  send_pg_creates();

  update_logger();
}

void PGMonitor::handle_osd_timeouts()
{
  if (!mon->is_leader())
    return;
  utime_t now(ceph_clock_now(g_ceph_context));
  utime_t timeo(g_conf->mon_osd_report_timeout, 0);
  if (now - mon->get_leader_since() < timeo) {
    // We haven't been the leader for long enough to consider OSD timeouts
    return;
  }

  if (mon->osdmon()->paxos->is_writeable())
    mon->osdmon()->handle_osd_timeouts(now, last_osd_report);
}

void PGMonitor::create_pending()
{
  pending_inc = PGMap::Incremental();
  pending_inc.version = pg_map.version + 1;
  dout(10) << "create_pending v " << pending_inc.version << dendl;
}

void PGMonitor::encode_pending(bufferlist &bl)
{
  dout(10) << "encode_pending v " << pending_inc.version << dendl;
  assert(paxos->get_version() + 1 == pending_inc.version);
  pending_inc.encode(bl);
}

bool PGMonitor::preprocess_query(PaxosServiceMessage *m)
{
  dout(10) << "preprocess_query " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case CEPH_MSG_STATFS:
    handle_statfs((MStatfs*)m);
    return true;
  case MSG_GETPOOLSTATS:
    return preprocess_getpoolstats((MGetPoolStats*)m);
    
  case MSG_PGSTATS:
    return preprocess_pg_stats((MPGStats*)m);

  case MSG_MON_COMMAND:
    return preprocess_command((MMonCommand*)m);


  default:
    assert(0);
    m->put();
    return true;
  }
}

bool PGMonitor::prepare_update(PaxosServiceMessage *m)
{
  dout(10) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_PGSTATS:
    return prepare_pg_stats((MPGStats*)m);

  case MSG_MON_COMMAND:
    return prepare_command((MMonCommand*)m);

  default:
    assert(0);
    m->put();
    return false;
  }
}

void PGMonitor::handle_statfs(MStatfs *statfs)
{
  // check caps
  MonSession *session = statfs->get_session();
  if (!session)
    goto out;
  if (!session->caps.check_privileges(PAXOS_PGMAP, MON_CAP_R)) {
    dout(0) << "MStatfs received from entity with insufficient privileges "
	    << session->caps << dendl;
    goto out;
  }
  MStatfsReply *reply;

  dout(10) << "handle_statfs " << *statfs << " from " << statfs->get_orig_source() << dendl;

  if (statfs->fsid != mon->monmap->fsid) {
    dout(0) << "handle_statfs on fsid " << statfs->fsid << " != " << mon->monmap->fsid << dendl;
    goto out;
  }

  // fill out stfs
  reply = new MStatfsReply(mon->monmap->fsid, statfs->get_tid(), paxos->get_version());

  // these are in KB.
  reply->h.st.kb = pg_map.osd_sum.kb;
  reply->h.st.kb_used = pg_map.osd_sum.kb_used;
  reply->h.st.kb_avail = pg_map.osd_sum.kb_avail;
  reply->h.st.num_objects = pg_map.pg_sum.stats.sum.num_objects;

  // reply
  mon->send_reply(statfs, reply);
 out:
  statfs->put();
}

bool PGMonitor::preprocess_getpoolstats(MGetPoolStats *m)
{
  MGetPoolStatsReply *reply;

  MonSession *session = m->get_session();
  if (!session)
    goto out;
  if (!session->caps.check_privileges(PAXOS_PGMAP, MON_CAP_R)) {
    dout(0) << "MGetPoolStats received from entity with insufficient caps "
	    << session->caps << dendl;
    goto out;
  }

  if (m->fsid != mon->monmap->fsid) {
    dout(0) << "preprocess_getpoolstats on fsid " << m->fsid << " != " << mon->monmap->fsid << dendl;
    goto out;
  }
  
  reply = new MGetPoolStatsReply(m->fsid, m->get_tid(), paxos->get_version());

  for (list<string>::iterator p = m->pools.begin();
       p != m->pools.end();
       p++) {
    int64_t poolid = mon->osdmon()->osdmap.lookup_pg_pool_name(p->c_str());
    if (poolid < 0)
      continue;
    if (pg_map.pg_pool_sum.count(poolid) == 0)
      continue;
    reply->pool_stats[*p] = pg_map.pg_pool_sum[poolid];
  }

  mon->send_reply(m, reply);

 out:
  m->put();
  return true;
}


bool PGMonitor::preprocess_pg_stats(MPGStats *stats)
{
  // check caps
  MonSession *session = stats->get_session();
  if (!session) {
    derr << "PGMonitor::preprocess_pg_stats: no monitor session!" << dendl;
    stats->put();
    return true;
  }
  if (!session->caps.check_privileges(PAXOS_PGMAP, MON_CAP_R)) {
    derr << "PGMonitor::preprocess_pg_stats: MPGStats received from entity "
         << "with insufficient privileges " << session->caps << dendl;
    stats->put();
    return true;
  }

  // First, just see if they need a new osdmap. But
  // only if they've had the map for a while.
  if (stats->had_map_for > 30.0 && 
      mon->osdmon()->paxos->is_readable() &&
      stats->epoch < mon->osdmon()->osdmap.get_epoch())
    mon->osdmon()->send_latest_now_nodelete(stats, stats->epoch+1);

  // Always forward the PGStats to the leader, even if they are the same as
  // the old PGStats. The leader will mark as down osds that haven't sent
  // PGStats for a few minutes.
  return false;
}

bool PGMonitor::pg_stats_have_changed(int from, const MPGStats *stats) const
{
  // any new osd info?
  hash_map<int,osd_stat_t>::const_iterator s = pg_map.osd_stat.find(from);
  if (s == pg_map.osd_stat.end())
    return true;
  if (s->second != stats->osd_stat)
    return true;

  // any new pg info?
  for (map<pg_t,pg_stat_t>::const_iterator p = stats->pg_stat.begin();
       p != stats->pg_stat.end(); ++p) {
    hash_map<pg_t,pg_stat_t>::const_iterator t = pg_map.pg_stat.find(p->first);
    if (t == pg_map.pg_stat.end())
      return true;
    if (t->second.reported != p->second.reported)
      return true;
  }

  return false;
}

bool PGMonitor::prepare_pg_stats(MPGStats *stats) 
{
  dout(10) << "prepare_pg_stats " << *stats << " from " << stats->get_orig_source() << dendl;
  int from = stats->get_orig_source().num();

  if (stats->fsid != mon->monmap->fsid) {
    dout(0) << "prepare_pg_stats on fsid " << stats->fsid << " != " << mon->monmap->fsid << dendl;
    stats->put();
    return false;
  }

  last_osd_report[from] = ceph_clock_now(g_ceph_context);

  if (!stats->get_orig_source().is_osd() ||
      !mon->osdmon()->osdmap.is_up(from) ||
      stats->get_orig_source_inst() != mon->osdmon()->osdmap.get_inst(from)) {
    dout(1) << " ignoring stats from non-active osd." << dendl;
    stats->put();
    return false;
  }
      
  if (!pg_stats_have_changed(from, stats)) {
    dout(10) << " message contains no new osd|pg stats" << dendl;
    MPGStatsAck *ack = new MPGStatsAck;
    for (map<pg_t,pg_stat_t>::const_iterator p = stats->pg_stat.begin();
	 p != stats->pg_stat.end();
	 ++p) {
      ack->pg_stat[p->first] = p->second.reported;
    }
    mon->send_reply(stats, ack);
    stats->put();
    return false;
  }

  // osd stat
  pending_inc.osd_stat_updates[from] = stats->osd_stat;
  
  if (pg_map.osd_stat.count(from))
    dout(10) << " got osd." << from << " " << stats->osd_stat << " (was " << pg_map.osd_stat[from] << ")" << dendl;
  else
    dout(10) << " got osd." << from << " " << stats->osd_stat << " (first report)" << dendl;

  // apply to live map too (screw consistency)
  /*
    actually, no, don't.  that screws up our "latest" stash.  and can
    lead to weird output where things appear to jump backwards in
    time... that's just confusing.

  if (pg_map.osd_stat.count(from))
    pg_map.stat_osd_sub(pg_map.osd_stat[from]);
  pg_map.osd_stat[from] = stats->osd_stat;
  pg_map.stat_osd_add(stats->osd_stat);
  */

  // pg stats
  MPGStatsAck *ack = new MPGStatsAck;
  ack->set_tid(stats->get_tid());
  for (map<pg_t,pg_stat_t>::iterator p = stats->pg_stat.begin();
       p != stats->pg_stat.end();
       p++) {
    pg_t pgid = p->first;
    ack->pg_stat[pgid] = p->second.reported;

    if ((pg_map.pg_stat.count(pgid) && 
	 pg_map.pg_stat[pgid].reported > p->second.reported)) {
      dout(15) << " had " << pgid << " from " << pg_map.pg_stat[pgid].reported << dendl;
      continue;
    }
    if (pending_inc.pg_stat_updates.count(pgid) && 
	pending_inc.pg_stat_updates[pgid].reported > p->second.reported) {
      dout(15) << " had " << pgid << " from " << pending_inc.pg_stat_updates[pgid].reported
	       << " (pending)" << dendl;
      continue;
    }

    if (pg_map.pg_stat.count(pgid) == 0) {
      dout(15) << " got " << pgid << " reported at " << p->second.reported
	       << " state " << pg_state_string(p->second.state)
	       << " but DNE in pg_map; pool was probably deleted."
	       << dendl;
      continue;
    }
      
    dout(15) << " got " << pgid
	     << " reported at " << p->second.reported
	     << " state " << pg_state_string(pg_map.pg_stat[pgid].state)
	     << " -> " << pg_state_string(p->second.state)
	     << dendl;
    pending_inc.pg_stat_updates[pgid] = p->second;

    /*
    // we don't care much about consistency, here; apply to live map.
    pg_map.stat_pg_sub(pgid, pg_map.pg_stat[pgid]);
    pg_map.pg_stat[pgid] = p->second;
    pg_map.stat_pg_add(pgid, pg_map.pg_stat[pgid]);
    */
  }
  
  paxos->wait_for_commit(new C_Stats(this, stats, ack));
  return true;
}

void PGMonitor::_updated_stats(MPGStats *req, MPGStatsAck *ack)
{
  dout(7) << "_updated_stats for " << req->get_orig_source_inst() << dendl;
  mon->send_reply(req, ack);
  req->put();
}



// ------------------------

struct RetryCheckOSDMap : public Context {
  PGMonitor *pgmon;
  epoch_t epoch;
  RetryCheckOSDMap(PGMonitor *p, epoch_t e) : pgmon(p), epoch(e) {}
  void finish(int r) {
    pgmon->check_osd_map(epoch);
  }
};

void PGMonitor::check_osd_map(epoch_t epoch)
{
  if (mon->is_peon()) 
    return; // whatever.

  if (pg_map.last_osdmap_epoch >= epoch) {
    dout(10) << "check_osd_map already seen " << pg_map.last_osdmap_epoch << " >= " << epoch << dendl;
    return;
  }

  if (!mon->osdmon()->paxos->is_readable()) {
    dout(10) << "check_osd_map -- osdmap not readable, waiting" << dendl;
    mon->osdmon()->paxos->wait_for_readable(new RetryCheckOSDMap(this, epoch));
    return;
  }

  if (!paxos->is_writeable()) {
    dout(10) << "check_osd_map -- pgmap not writeable, waiting" << dendl;
    paxos->wait_for_writeable(new RetryCheckOSDMap(this, epoch));
    return;
  }

  // apply latest map(s)
  for (epoch_t e = pg_map.last_osdmap_epoch+1;
       e <= epoch;
       e++) {
    dout(10) << "check_osd_map applying osdmap e" << e << " to pg_map" << dendl;
    bufferlist bl;
    mon->store->get_bl_sn(bl, "osdmap", e);
    assert(bl.length());
    OSDMap::Incremental inc(bl);
    for (map<int32_t,uint32_t>::iterator p = inc.new_weight.begin();
	 p != inc.new_weight.end();
	 p++)
      if (p->second == CEPH_OSD_OUT) {
	dout(10) << "check_osd_map  osd." << p->first << " went OUT" << dendl;
	pending_inc.osd_stat_rm.insert(p->first);
      } else {
	dout(10) << "check_osd_map  osd." << p->first << " is IN" << dendl;
	pending_inc.osd_stat_rm.erase(p->first);
	pending_inc.osd_stat_updates[p->first]; 
      }

    // this is conservative: we want to know if any osds (maybe) got marked down.
    for (map<int32_t,uint8_t>::iterator p = inc.new_state.begin();
	 p != inc.new_state.end();
	 ++p)
      if (p->second & CEPH_OSD_UP)   // true if marked up OR down, but we're too lazy to check which
	need_check_down_pgs = true;
  }

  bool propose = false;
  if (pg_map.last_osdmap_epoch < epoch) {
    pending_inc.osdmap_epoch = epoch;
    propose = true;
  }

  // scan pg space?
  if (register_new_pgs())
    propose = true;

  if (need_check_down_pgs && check_down_pgs())
    propose = true;
  
  if (propose)
    propose_pending();

  send_pg_creates();
}

void PGMonitor::register_pg(pg_pool_t& pool, pg_t pgid, epoch_t epoch, bool new_pool)
{
  pg_t parent;
  int split_bits = 0;
  if (!new_pool) {
    parent = pgid;
    while (1) {
      // remove most significant bit
      int msb = pool.calc_bits_of(parent.ps());
      if (!msb) break;
      parent.set_ps(parent.ps() & ~(1<<(msb-1)));
      split_bits++;
      dout(10) << " is " << pgid << " parent " << parent << " ?" << dendl;
      //if (parent.u.pg.ps < mon->osdmon->osdmap.get_pgp_num()) {
      if (pg_map.pg_stat.count(parent) &&
	  pg_map.pg_stat[parent].state != PG_STATE_CREATING) {
	dout(10) << "  parent is " << parent << dendl;
	break;
      }
    }
  }
  
  pending_inc.pg_stat_updates[pgid].state = PG_STATE_CREATING;
  pending_inc.pg_stat_updates[pgid].created = epoch;
  pending_inc.pg_stat_updates[pgid].parent = parent;
  pending_inc.pg_stat_updates[pgid].parent_split_bits = split_bits;

  if (split_bits == 0) {
    dout(10) << "register_new_pgs  will create " << pgid << dendl;
  } else {
    dout(10) << "register_new_pgs  will create " << pgid
	     << " parent " << parent
	     << " by " << split_bits << " bits"
	     << dendl;
  }
}

bool PGMonitor::register_new_pgs()
{
  // iterate over crush mapspace
  epoch_t epoch = mon->osdmon()->osdmap.get_epoch();
  dout(10) << "register_new_pgs checking pg pools for osdmap epoch " << epoch
	   << ", last_pg_scan " << pg_map.last_pg_scan << dendl;

  OSDMap *osdmap = &mon->osdmon()->osdmap;

  int created = 0;
  for (map<int64_t,pg_pool_t>::iterator p = osdmap->pools.begin();
       p != osdmap->pools.end();
       p++) {
    int64_t poolid = p->first;
    pg_pool_t &pool = p->second;
    int ruleno = pool.get_crush_ruleset();
    if (!osdmap->crush.rule_exists(ruleno)) 
      continue;

    if (pool.get_last_change() <= pg_map.last_pg_scan ||
	pool.get_last_change() <= pending_inc.pg_scan) {
      dout(10) << " no change in pool " << p->first << " " << pool << dendl;
      continue;
    }

    dout(10) << "register_new_pgs scanning pool " << p->first << " " << pool << dendl;

    bool new_pool = pg_map.pg_pool_sum.count(poolid) == 0;  // first pgs in this pool

    for (ps_t ps = 0; ps < pool.get_pg_num(); ps++) {
      pg_t pgid(ps, poolid, -1);
      if (pg_map.pg_stat.count(pgid)) {
	dout(20) << "register_new_pgs  have " << pgid << dendl;
	continue;
      }
      created++;
      register_pg(pool, pgid, pool.get_last_change(), new_pool);
    }

    for (ps_t ps = 0; ps < pool.get_lpg_num(); ps++) {
      for (int osd = 0; osd < osdmap->get_max_osd(); osd++) {
	pg_t pgid(ps, poolid, osd);
	if (pg_map.pg_stat.count(pgid)) {
	  dout(20) << "register_new_pgs  have " << pgid << dendl;
	  continue;
	}
	created++;
	register_pg(pool, pgid, pool.get_last_change(), new_pool);
      }
    }
  }

  int max = MIN(osdmap->get_max_osd(), osdmap->crush.get_max_devices());
  int removed = 0;
  for (set<pg_t>::iterator p = pg_map.creating_pgs.begin();
       p != pg_map.creating_pgs.end();
       p++) {
    if (p->preferred() >= max) {
      dout(20) << " removing creating_pg " << *p << " because preferred >= max osd or crush device" << dendl;
      pending_inc.pg_remove.insert(*p);
      removed++;
    }
    if (!osdmap->have_pg_pool(p->pool())) {
      dout(20) << " removing creating_pg " << *p << " because containing pool deleted" << dendl;
      pending_inc.pg_remove.insert(*p);
      ++removed;
    }
  }

  // deleted pools?
  for (hash_map<pg_t,pg_stat_t>::const_iterator p = pg_map.pg_stat.begin();
       p != pg_map.pg_stat.end(); ++p) {
    if (!osdmap->have_pg_pool(p->first.pool())) {
      dout(20) << " removing creating_pg " << p->first << " because "
	       << "containing pool deleted" << dendl;
      pending_inc.pg_remove.insert(p->first);
      ++removed;
    }
  }

  dout(10) << "register_new_pgs registered " << created << " new pgs, removed "
	   << removed << " uncreated pgs" << dendl;
  if (created || removed) {
    pending_inc.pg_scan = epoch;
    return true;
  }
  return false;
}

void PGMonitor::send_pg_creates()
{
  dout(10) << "send_pg_creates to " << pg_map.creating_pgs.size() << " pgs" << dendl;

  map<int, MOSDPGCreate*> msg;
  utime_t now = ceph_clock_now(g_ceph_context);
  
  OSDMap *osdmap = &mon->osdmon()->osdmap;
  int max = MIN(osdmap->get_max_osd(), osdmap->crush.get_max_devices());

  for (set<pg_t>::iterator p = pg_map.creating_pgs.begin();
       p != pg_map.creating_pgs.end();
       p++) {
    pg_t pgid = *p;
    pg_t on = pgid;
    if (pg_map.pg_stat[pgid].parent_split_bits)
      on = pg_map.pg_stat[pgid].parent;
    vector<int> acting;
    int nrep = mon->osdmon()->osdmap.pg_to_acting_osds(on, acting);
    if (!nrep) {
      dout(20) << "send_pg_creates  " << pgid << " -> no osds in epoch "
	       << mon->osdmon()->osdmap.get_epoch() << ", skipping" << dendl;
      continue;  // blarney!
    }
    int osd = acting[0];

    // don't send creates for non-existant preferred osds!
    if (pgid.preferred() >= max)
      continue;

    // throttle?
    if (last_sent_pg_create.count(osd) &&
	now - g_conf->mon_pg_create_interval < last_sent_pg_create[osd]) 
      continue;
      
    dout(20) << "send_pg_creates  " << pgid << " -> osd." << osd 
	     << " in epoch " << pg_map.pg_stat[pgid].created << dendl;
    if (msg.count(osd) == 0)
      msg[osd] = new MOSDPGCreate(mon->osdmon()->osdmap.get_epoch());
    msg[osd]->mkpg[pgid] = pg_create_t(pg_map.pg_stat[pgid].created,
				       pg_map.pg_stat[pgid].parent,
				       pg_map.pg_stat[pgid].parent_split_bits);
  }

  for (map<int, MOSDPGCreate*>::iterator p = msg.begin();
       p != msg.end();
       p++) {
    dout(10) << "sending pg_create to osd." << p->first << dendl;
    mon->messenger->send_message(p->second, mon->osdmon()->osdmap.get_inst(p->first));
    last_sent_pg_create[p->first] = ceph_clock_now(g_ceph_context);
  }
}

bool PGMonitor::check_down_pgs()
{
  dout(10) << "check_down_pgs" << dendl;

  OSDMap *osdmap = &mon->osdmon()->osdmap;
  bool ret = false;

  for (hash_map<pg_t,pg_stat_t>::iterator p = pg_map.pg_stat.begin();
       p != pg_map.pg_stat.end();
       ++p) {
    if ((p->second.state & PG_STATE_STALE) == 0 &&
	p->second.acting.size() &&
	osdmap->is_down(p->second.acting[0])) {
      dout(10) << " marking pg " << p->first << " stale with acting " << p->second.acting << dendl;

      map<pg_t,pg_stat_t>::iterator q = pending_inc.pg_stat_updates.find(p->first);
      pg_stat_t *stat;
      if (q == pending_inc.pg_stat_updates.end()) {
	stat = &pending_inc.pg_stat_updates[p->first];
	*stat = p->second;
      } else {
	stat = &q->second;
      }
      stat->state |= PG_STATE_STALE;
      stat->last_unstale = ceph_clock_now(g_ceph_context);
      ret = true;
    }
  }
  need_check_down_pgs = false;
  return ret;
}


bool PGMonitor::preprocess_command(MMonCommand *m)
{
  int r = -1;
  bufferlist rdata;
  stringstream ss;

  vector<const char*> args;
  for (unsigned i = 1; i < m->cmd.size(); i++)
    args.push_back(m->cmd[i].c_str());

  if (m->cmd.size() > 1) {
    if (m->cmd[1] == "stat") {
      ss << pg_map;
      r = 0;
    }
    else if (m->cmd[1] == "getmap") {
      pg_map.encode(rdata);
      ss << "got pgmap version " << pg_map.version;
      r = 0;
    }
    else if (m->cmd[1] == "send_pg_creates") {
      send_pg_creates();
      ss << "sent pg creates ";
      r = 0;
    }
    else if (m->cmd[1] == "dump") {
      string format = "plain";
      string what = "all";
      string val;
      for (std::vector<const char*>::iterator i = args.begin()+1; i != args.end(); ) {
	if (ceph_argparse_double_dash(args, i)) {
	  break;
	} else if (ceph_argparse_witharg(args, i, &val, "-f", "--format", (char*)NULL)) {
	  format = val;
	} else {
	  what = *i++;
	}
      }
      Formatter *f = 0;
      r = 0;
      if (format == "json")
	f = new JSONFormatter(true);
      else if (format == "plain")
	f = 0; //new PlainFormatter();
      else {
	r = -EINVAL;
	ss << "unknown format '" << format << "'";
      }

      if (r == 0) {
	stringstream ds;
	if (f) {
	  if (what == "all") {
	    f->open_object_section("pg_map");
	    pg_map.dump(f);
	    f->close_section();
	  } else if (what == "summary" || what == "sum") {
	    f->open_object_section("pg_map");
	    pg_map.dump_basic(f);
	    f->close_section();
	  } else if (what == "pools") {
	    pg_map.dump_pool_stats(f);
	  } else if (what == "osds") {
	    pg_map.dump_osd_stats(f);
	  } else if (what == "pgs") {
	    pg_map.dump_pg_stats(f);
	  } else {
	    r = -EINVAL;
	    ss << "i don't know how to dump '" << what << "' is";
	  }
	  if (r == 0)
	    f->flush(ds);
	  delete f;
	} else {
	  pg_map.dump(ds);
	}
	if (r == 0) {
	  rdata.append(ds);
	  ss << "dumped " << what << " in format " << format;
	}
	r = 0;
      }
    }
    else if (m->cmd[1] == "dump_json") {
      ss << "ok";
      r = 0;
      JSONFormatter jsf(true);
      jsf.open_object_section("pg_map");
      pg_map.dump(&jsf);
      jsf.close_section();
      stringstream ds;
      jsf.flush(ds);
      rdata.append(ds);
    }
    else if (m->cmd[1] == "dump_stuck") {
      r = dump_stuck_pg_stats(ss, rdata, args);
    }
    else if (m->cmd[1] == "dump_pools_json") {
      ss << "ok";
      r = 0;
      JSONFormatter jsf(true);
      jsf.open_object_section("pg_map");
      pg_map.dump(&jsf);
      jsf.close_section();
      stringstream ds;
      jsf.flush(ds);
      rdata.append(ds);
    }
    else if (m->cmd[1] == "map" && m->cmd.size() == 3) {
      pg_t pgid;
      r = -EINVAL;
      if (pgid.parse(m->cmd[2].c_str())) {
	vector<int> up, acting;
	pg_t mpgid = mon->osdmon()->osdmap.raw_pg_to_pg(pgid);
	mon->osdmon()->osdmap.pg_to_up_acting_osds(pgid, up, acting);
	ss << "osdmap e" << mon->osdmon()->osdmap.get_epoch()
	   << " pg " << pgid << " (" << mpgid << ")"
	   << " -> up " << up << " acting " << acting;
	r = 0;
      } else
	ss << "invalid pgid '" << m->cmd[2] << "'";
    }
    else if ((m->cmd[1] == "scrub" || m->cmd[1] == "repair") && m->cmd.size() == 3) {
      pg_t pgid;
      r = -EINVAL;
      if (pgid.parse(m->cmd[2].c_str())) {
	if (pg_map.pg_stat.count(pgid)) {
	  if (pg_map.pg_stat[pgid].acting.size()) {
	    int osd = pg_map.pg_stat[pgid].acting[0];
	    if (mon->osdmon()->osdmap.is_up(osd)) {
	      vector<pg_t> pgs(1);
	      pgs[0] = pgid;
	      mon->try_send_message(new MOSDScrub(mon->monmap->fsid, pgs,
						  m->cmd[1] == "repair"),
				    mon->osdmon()->osdmap.get_inst(osd));
	      ss << "instructing pg " << pgid << " on osd." << osd << " to " << m->cmd[1];
	      r = 0;
	    } else
	      ss << "pg " << pgid << " primary osd." << osd << " not up";
	  } else
	    ss << "pg " << pgid << " has no primary osd";
	} else
	  ss << "pg " << pgid << " dne";
      } else
	ss << "invalid pgid '" << m->cmd[2] << "'";
    }
    else if ((m->cmd[1] == "debug") && (m->cmd.size() > 2)) {
      if (m->cmd[2] == "unfound_objects_exist") {
	bool unfound_objects_exist = false;
	hash_map<pg_t,pg_stat_t>::const_iterator end = pg_map.pg_stat.end();
	for (hash_map<pg_t,pg_stat_t>::const_iterator s = pg_map.pg_stat.begin();
	     s != end; ++s)
	{
	  if (s->second.stats.sum.num_objects_unfound > 0) {
	    unfound_objects_exist = true;
	    break;
	  }
	}
	if (unfound_objects_exist)
	  ss << "TRUE";
	else
	  ss << "FALSE";

	r = 0;
      }
      else if (m->cmd[2] == "degraded_pgs_exist") {
	bool degraded_pgs_exist = false;
	hash_map<pg_t,pg_stat_t>::const_iterator end = pg_map.pg_stat.end();
	for (hash_map<pg_t,pg_stat_t>::const_iterator s = pg_map.pg_stat.begin();
	     s != end; ++s)
	{
	  if (s->second.stats.sum.num_objects_degraded > 0) {
	    degraded_pgs_exist = true;
	    break;
	  }
	}
	if (degraded_pgs_exist)
	  ss << "TRUE";
	else
	  ss << "FALSE";

	r = 0;
      }
    }
  }

  if (r != -1) {
    string rs;
    getline(ss, rs);
    mon->reply_command(m, r, rs, rdata, paxos->get_version());
    return true;
  } else
    return false;
}


bool PGMonitor::prepare_command(MMonCommand *m)
{
  stringstream ss;
  pg_t pgid;
  epoch_t epoch = mon->osdmon()->osdmap.get_epoch();
  string rs;

  if (m->cmd.size() <= 1 || m->cmd[1] != "force_create_pg") {
    // nothing here yet
    ss << "unrecognized command";
    goto out;
  }
  if (m->cmd.size() <= 2) {
    ss << "usage: pg force_create_pg <pg>";
    goto out;
  }
  if (!pgid.parse(m->cmd[2].c_str())) {
    ss << "pg " << m->cmd[2] << " invalid";
    goto out;
  }
  if (!pg_map.pg_stat.count(pgid)) {
    ss << "pg " << pgid << " dne";
    goto out;
  }
  if (pg_map.creating_pgs.count(pgid)) {
    ss << "pg " << pgid << " already creating";
    goto out;
  }
  {
    pg_stat_t& s = pending_inc.pg_stat_updates[pgid];
    s.state = PG_STATE_CREATING;
    s.created = epoch;
    s.last_change = ceph_clock_now(g_ceph_context);
  }
  ss << "pg " << m->cmd[2] << " now creating, ok";
  getline(ss, rs);
  paxos->wait_for_commit(new Monitor::C_Command(mon, m, 0, rs, paxos->get_version()));
  return true;

 out:
  int err = -EINVAL;
  getline(ss, rs);
  mon->reply_command(m, err, rs, paxos->get_version());
  return false;
}

void PGMonitor::get_health(list<pair<health_status_t,string> >& summary,
			   list<pair<health_status_t,string> > *detail) const
{
  map<string,int> note;
  hash_map<int,int>::const_iterator p = pg_map.num_pg_by_state.begin();
  hash_map<int,int>::const_iterator p_end = pg_map.num_pg_by_state.end();
  for (; p != p_end; ++p) {
    if (p->first & PG_STATE_STALE)
      note["stale"] += p->second;
    if (p->first & PG_STATE_DOWN)
      note["down"] += p->second;
    if (p->first & PG_STATE_DEGRADED)
      note["degraded"] += p->second;
    if (p->first & PG_STATE_INCONSISTENT)
      note["inconsistent"] += p->second;
    if (p->first & PG_STATE_PEERING)
      note["peering"] += p->second;
    if (p->first & PG_STATE_REPAIR)
      note["repair"] += p->second;
    if (p->first & PG_STATE_SPLITTING)
      note["splitting"] += p->second;
    if (p->first & PG_STATE_RECOVERING)
      note["recovering"] += p->second;
    if (p->first & PG_STATE_INCOMPLETE)
      note["incomplete"] += p->second;
    if (p->first & PG_STATE_BACKFILL)
      note["backfill"] += p->second;
  }

  hash_map<pg_t, pg_stat_t> stuck_pgs;
  utime_t now(ceph_clock_now(g_ceph_context));
  utime_t cutoff = now - utime_t(g_conf->mon_pg_stuck_threshold, 0);

  pg_map.get_stuck_stats(PGMap::STUCK_INACTIVE, cutoff, stuck_pgs);
  if (!stuck_pgs.empty()) {
    note["stuck inactive"] = stuck_pgs.size();
  }
  stuck_pgs.clear();

  pg_map.get_stuck_stats(PGMap::STUCK_UNCLEAN, cutoff, stuck_pgs);
  if (!stuck_pgs.empty()) {
    note["stuck unclean"] = stuck_pgs.size();
  }
  stuck_pgs.clear();

  pg_map.get_stuck_stats(PGMap::STUCK_STALE, cutoff, stuck_pgs);
  if (!stuck_pgs.empty()) {
    note["stuck stale"] = stuck_pgs.size();
  }

  if (detail) {
    for (hash_map<pg_t,pg_stat_t>::iterator p = stuck_pgs.begin();
	 p != stuck_pgs.end();
	 ++p) {
      ostringstream ss;
      ss << "pg " << p->first << " is stuck " << pg_state_string(p->second.state)
	 << ", last acting " << p->second.acting;
      detail->push_back(make_pair(HEALTH_WARN, ss.str()));
    }
  }

  if (!note.empty()) {
    for (map<string,int>::iterator p = note.begin(); p != note.end(); p++) {
      ostringstream ss;
      ss << p->second << " pgs " << p->first;
      summary.push_back(make_pair(HEALTH_WARN, ss.str()));
    }
    if (detail) {
      for (hash_map<pg_t,pg_stat_t>::const_iterator p = pg_map.pg_stat.begin();
	   p != pg_map.pg_stat.end();
	   ++p) {
	if (p->second.state & (PG_STATE_STALE |
			       PG_STATE_DOWN |
			       PG_STATE_DEGRADED |
			       PG_STATE_INCONSISTENT |
			       PG_STATE_PEERING |
			       PG_STATE_REPAIR |
			       PG_STATE_SPLITTING |
			       PG_STATE_RECOVERING |
			       PG_STATE_INCOMPLETE |
			       PG_STATE_BACKFILL) &&
	    stuck_pgs.count(p->first) == 0) {
	  ostringstream ss;
	  ss << "pg " << p->first << " is " << pg_state_string(p->second.state);
	  if (p->second.stats.sum.num_objects_unfound)
	    ss << ", " << p->second.stats.sum.num_objects_unfound << " unfound";
	  detail->push_back(make_pair(HEALTH_WARN, ss.str()));
	}
      }
    }
  }

  stringstream rss;
  pg_map.recovery_summary(rss);
  if (!rss.str().empty()) {
    summary.push_back(make_pair(HEALTH_WARN, "recovery " + rss.str()));
    if (detail)
      detail->push_back(make_pair(HEALTH_WARN, "recovery " + rss.str()));
  }
  
  check_full_osd_health(summary, detail, pg_map.full_osds, "full", HEALTH_ERR);
  check_full_osd_health(summary, detail, pg_map.nearfull_osds, "near full", HEALTH_WARN);
}

void PGMonitor::check_full_osd_health(list<pair<health_status_t,string> >& summary,
				      list<pair<health_status_t,string> > *detail,
				      const set<int>& s, const char *desc,
				      health_status_t sev) const
{
  if (s.size() > 0) {
    ostringstream ss;
    ss << s.size() << " " << desc << " osd(s)";
    summary.push_back(make_pair(sev, ss.str()));
    if (detail) {
      for (set<int>::const_iterator p = s.begin(); p != s.end(); ++p) {
	ostringstream ss;
	const osd_stat_t& os = pg_map.osd_stat.find(*p)->second;
	int ratio = (int)(((float)os.kb_used) / (float) os.kb * 100.0);
	ss << "osd." << *p << " is " << desc << " at " << ratio << "%";
	detail->push_back(make_pair(sev, ss.str()));
      }
    }
  }
}

int PGMonitor::dump_stuck_pg_stats(ostream& ss,
				   bufferlist& rdata,
				   vector<const char*>& args) const
{
  string format = "plain";
  string val;
  int threshold = g_conf->mon_pg_stuck_threshold;
  int seconds;
  ostringstream err;

  if (args.size() < 2) {
    ss << "Must specify inactive or unclean or stale.";
    return -EINVAL;
  }

  PGMap::StuckPG stuck_type = PGMap::STUCK_NONE;
  string type = args[1];
  if (type == "inactive")
    stuck_type = PGMap::STUCK_INACTIVE;
  if (type == "unclean")
    stuck_type = PGMap::STUCK_UNCLEAN;
  if (type == "stale")
    stuck_type = PGMap::STUCK_STALE;
  if (stuck_type == PGMap::STUCK_NONE) {
    ss << "Invalid stuck type '" << type
       << "'. Valid types are: inactive, unclean, or stale";
    return -EINVAL;
  }

  for (std::vector<const char*>::iterator i = args.begin() + 2;
       i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_witharg(args, i, &val,
				     "-f", "--format", (char*)NULL)) {
      if (val != "json" && val != "plain") {
	ss << "format must be json or plain";
	return -EINVAL;
      }
      format = val;
    } else if (ceph_argparse_withint(args, i, &seconds, &err,
				     "-t", "--threshold", (char*)NULL)) {
      if (!err.str().empty()) {
	ss << err.str();
	return -EINVAL;
      }
      threshold = seconds;
    } else if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      stringstream ds;
      ds << "Usage: ceph pg dump_stuck inactive|unclean|stale [options]" << std::endl
	 << std::endl
	 << "Get stats for pgs that have not been active, clean, or refreshed in some number of seconds." << std::endl
	 << std::endl
	 << "Options: " << std::endl
	 << "  -h, --help                   display usage info" << std::endl
	 << "  -f, --format [plain|json]    output format (default: plain)" << std::endl
	 << "  -t, --threshold [seconds]    how many seconds 'stuck' is (default: 300)" << std::endl;
      rdata.append(ds);
      return 0;
    } else {
      ss << "invalid argument '" << *i << "'";
      return -EINVAL;
    }
  }

  utime_t now(ceph_clock_now(g_ceph_context));
  utime_t cutoff = now - utime_t(threshold, 0);

  stringstream ds;
  if (format == "json") {
    JSONFormatter jsf(true);
    pg_map.dump_stuck(&jsf, stuck_type, cutoff);
    jsf.flush(ds);
  } else {
    pg_map.dump_stuck_plain(ds, stuck_type, cutoff);
  }
  rdata.append(ds);
  ss << "ok";
  return 0;
}
