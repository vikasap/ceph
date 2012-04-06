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

#include "msg/SimpleMessenger.h"
#include "messages/MMonGetMap.h"
#include "messages/MMonGetVersion.h"
#include "messages/MMonGetVersionReply.h"
#include "messages/MMonMap.h"
#include "messages/MAuth.h"
#include "messages/MAuthReply.h"

#include "messages/MMonSubscribe.h"
#include "messages/MMonSubscribeAck.h"
#include "common/ConfUtils.h"
#include "common/ceph_argparse.h"
#include "common/errno.h"
#include "common/LogClient.h"

#include "MonClient.h"
#include "MonMap.h"

#include "auth/Auth.h"
#include "auth/KeyRing.h"
#include "auth/AuthSupported.h"

#include "include/str_list.h"
#include "include/addr_parsing.h"

#include "common/config.h"


#define dout_subsys ceph_subsys_monc
#undef dout_prefix
#define dout_prefix *_dout << "monclient" << (hunting ? "(hunting)":"") << ": "

MonClient::MonClient(CephContext *cct_) :
  Dispatcher(cct_),
  state(MC_STATE_NONE),
  messenger(NULL),
  cur_con(NULL),
  monc_lock("MonClient::monc_lock"),
  timer(cct_, monc_lock), finisher(cct_),
  initialized(false),
  log_client(NULL),
  auth_supported(NULL),
  hunting(true),
  want_monmap(true),
  want_keys(0), global_id(0),
  authenticate_err(0),
  auth(NULL),
  keyring(NULL),
  rotating_secrets(NULL),
  version_req_id(0)
{
}

MonClient::~MonClient()
{
  delete auth_supported;
  delete auth;
  delete keyring;
  delete rotating_secrets;
}

/*
 * build an initial monmap with any known monitor
 * addresses.
 */
int MonClient::build_initial_monmap(CephContext *cct, MonMap &monmap)
{
  const md_config_t *conf = cct->_conf;
  // file?
  if (!conf->monmap.empty()) {
    int r;
    try {
      r = monmap.read(conf->monmap.c_str());
    }
    catch (const buffer::error &e) {
      r = -EINVAL;
    }
    if (r >= 0)
      return 0;
    cerr << "unable to read/decode monmap from " << conf->monmap
	 << ": " << cpp_strerror(-r) << std::endl;
    return r;
  }

  // fsid from conf?
  if (!cct->_conf->fsid.is_zero()) {
    monmap.fsid = cct->_conf->fsid;
  }

  // -m foo?
  if (!conf->mon_host.empty()) {
    vector<entity_addr_t> addrs;
    if (parse_ip_port_vec(conf->mon_host.c_str(), addrs)) {
      for (unsigned i=0; i<addrs.size(); i++) {
	char n[2];
	n[0] = 'a' + i;
	n[1] = 0;
	if (addrs[i].get_port() == 0)
	  addrs[i].set_port(CEPH_MON_PORT);
	string name = "noname-";
	name += n;
	monmap.add(name, addrs[i]);
      }
      return 0;
    } else { //maybe they passed us a DNS-resolvable name
      char *hosts = NULL;
      hosts = resolve_addrs(conf->mon_host.c_str());
      if (!hosts)
        return -EINVAL;
      bool success = parse_ip_port_vec(hosts, addrs);
      free(hosts);
      if (success) {
        for (unsigned i=0; i<addrs.size(); i++) {
          char n[2];
          n[0] = 'a' + i;
          n[1] = 0;
          if (addrs[i].get_port() == 0)
            addrs[i].set_port(CEPH_MON_PORT);
          monmap.add(n, addrs[i]);
        }
        return 0;
      } else cerr << "couldn't parse_ip_port_vec on " << hosts << std::endl;
    }
    cerr << "unable to parse addrs in '" << conf->mon_host << "'" << std::endl;
  }

  // What monitors are in the config file?
  std::vector <std::string> sections;
  int ret = conf->get_all_sections(sections);
  if (ret) {
    cerr << "Unable to find any monitors in the configuration "
         << "file, because there was an error listing the sections. error "
	 << ret << std::endl;
    return -ENOENT;
  }
  std::vector <std::string> mon_names;
  for (std::vector <std::string>::const_iterator s = sections.begin();
       s != sections.end(); ++s) {
    if ((s->substr(0, 4) == "mon.") && (s->size() > 4)) {
      mon_names.push_back(s->substr(4));
    }
  }

  // Find an address for each monitor in the config file.
  for (std::vector <std::string>::const_iterator m = mon_names.begin();
       m != mon_names.end(); ++m) {
    std::vector <std::string> sections;
    std::string m_name("mon");
    m_name += ".";
    m_name += *m;
    sections.push_back(m_name);
    sections.push_back("mon");
    sections.push_back("global");
    std::string val;
    int res = conf->get_val_from_conf_file(sections, "mon addr", val, true);
    if (res) {
      cerr << "failed to get an address for mon." << *m << ": error "
	   << res << std::endl;
      continue;
    }
    entity_addr_t addr;
    if (!addr.parse(val.c_str())) {
      cerr << "unable to parse address for mon." << *m
	   << ": addr='" << val << "'" << std::endl;
      continue;
    }
    if (addr.get_port() == 0)
      addr.set_port(CEPH_MON_PORT);    
    monmap.add(m->c_str(), addr);
  }

  if (monmap.size() == 0) {
    cerr << "unable to find any monitors in conf. "
	 << "please specify monitors via -m monaddr or -c ceph.conf" << std::endl;
    return -ENOENT;
  }
  return 0;
}

int MonClient::build_initial_monmap()
{
  ldout(cct, 10) << "build_initial_monmap" << dendl;
  return build_initial_monmap(cct, monmap);
}

int MonClient::get_monmap()
{
  ldout(cct, 10) << "get_monmap" << dendl;
  Mutex::Locker l(monc_lock);
  
  _sub_want("monmap", 0, 0);
  if (cur_mon.empty())
    _reopen_session();

  while (want_monmap)
    map_cond.Wait(monc_lock);

  ldout(cct, 10) << "get_monmap done" << dendl;
  return 0;
}

int MonClient::get_monmap_privately()
{
  ldout(cct, 10) << "get_monmap_privately" << dendl;
  Mutex::Locker l(monc_lock);
  
  bool temp_msgr = false;
  SimpleMessenger* smessenger = NULL;
  if (!messenger) {
    messenger = smessenger = new SimpleMessenger(cct,
                                                 entity_name_t::CLIENT(-1),
                                                 getpid());
    messenger->add_dispatcher_head(this);
    smessenger->start();
    temp_msgr = true; 
  }
  
  int attempt = 10;
  
  ldout(cct, 10) << "have " << monmap.epoch << " fsid " << monmap.fsid << dendl;
  
  while (monmap.fsid.is_zero()) {
    cur_mon = monmap.pick_random_mon();
    cur_con = messenger->get_connection(monmap.get_inst(cur_mon));
    ldout(cct, 10) << "querying mon." << cur_mon << " " << cur_con->get_peer_addr() << dendl;
    messenger->send_message(new MMonGetMap, cur_con);
    
    if (--attempt == 0)
      break;
    
    utime_t interval(1, 0);
    map_cond.WaitInterval(cct, monc_lock, interval);

    if (monmap.fsid.is_zero()) {
      messenger->mark_down(cur_con);  // nope, clean that connection up
      cur_con->put();
    }
  }

  if (temp_msgr) {
    monc_lock.Unlock();
    messenger->shutdown();
    if (smessenger)
      smessenger->wait();
    delete messenger;
    messenger = 0;
    monc_lock.Lock();
  }
 
  hunting = true;  // reset this to true!
  cur_mon.clear();

  if (cur_con) {
    cur_con->put();
    cur_con = NULL;
  }

  if (!monmap.fsid.is_zero())
    return 0;
  return -1;
}


bool MonClient::ms_dispatch(Message *m)
{
  if (my_addr == entity_addr_t())
    my_addr = messenger->get_myaddr();

  // we only care about these message types
  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
  case CEPH_MSG_AUTH_REPLY:
  case CEPH_MSG_MON_SUBSCRIBE_ACK:
  case CEPH_MSG_MON_GET_VERSION_REPLY:
    break;
  default:
    return false;
  }

  Mutex::Locker lock(monc_lock);

  // ignore any messages outside our current session
  if (m->get_connection() != cur_con) {
    ldout(cct, 10) << "discarding stray monitor message " << *m << dendl;
    m->put();
    return true;
  }

  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
    handle_monmap((MMonMap*)m);
    break;
  case CEPH_MSG_AUTH_REPLY:
    handle_auth((MAuthReply*)m);
    break;
  case CEPH_MSG_MON_SUBSCRIBE_ACK:
    handle_subscribe_ack((MMonSubscribeAck*)m);
    break;
  case CEPH_MSG_MON_GET_VERSION_REPLY:
    handle_get_version_reply((MMonGetVersionReply*)m);
  }
  return true;
}

void MonClient::handle_monmap(MMonMap *m)
{
  ldout(cct, 10) << "handle_monmap " << *m << dendl;
  bufferlist::iterator p = m->monmapbl.begin();
  ::decode(monmap, p);

  assert(!cur_mon.empty());
  ldout(cct, 10) << " got monmap " << monmap.epoch
		 << ", mon." << cur_mon << " is now rank " << monmap.get_rank(cur_mon)
		 << dendl;
  ldout(cct, 10) << "dump:\n";
  monmap.print(*_dout);
  *_dout << dendl;

  _sub_got("monmap", monmap.get_epoch());

  if (!monmap.get_addr_name(cur_con->get_peer_addr(), cur_mon)) {
    ldout(cct, 10) << "mon." << cur_mon << " went away" << dendl;
    _reopen_session();  // can't find the mon we were talking to (above)
  } else {
    _finish_hunting();
  }

  map_cond.Signal();
  want_monmap = false;

  m->put();
}

// ----------------------

int MonClient::init()
{
  ldout(cct, 10) << "init auth_supported " << cct->_conf->auth_supported << dendl;

  messenger->add_dispatcher_head(this);

  int r = KeyRing::from_ceph_context(cct, &keyring);
  if (r < 0) {
    lderr(cct) << "failed to open keyring: " << cpp_strerror(r) << dendl;
    return r;
  }
  rotating_secrets = new RotatingKeyRing(cct, cct->get_module_type(), keyring);

  entity_name = cct->_conf->name;
  
  Mutex::Locker l(monc_lock);
  timer.init();
  finisher.start();
  schedule_tick();

  // seed rng so we choose a different monitor each time
  srand(getpid());

  auth_supported = new AuthSupported(cct);
  ldout(cct, 10) << "auth_supported " << auth_supported->get_supported_set() << dendl;

  initialized = true;
  return 0;
}

void MonClient::shutdown()
{
  if (initialized) {
    finisher.stop();
  }
  monc_lock.Lock();
  timer.shutdown();
  if (cur_con) {
    cur_con->put();
    cur_con = NULL;
  }

  monc_lock.Unlock();
}

int MonClient::authenticate(double timeout)
{
  Mutex::Locker lock(monc_lock);

  if (state == MC_STATE_HAVE_SESSION) {
    ldout(cct, 5) << "already authenticated" << dendl;;
    return 0;
  }

  _sub_want("monmap", monmap.get_epoch() ? monmap.get_epoch() + 1 : 0, 0);
  if (cur_mon.empty())
    _reopen_session();

  utime_t until = ceph_clock_now(cct);
  until += timeout;
  if (timeout > 0.0)
    ldout(cct, 10) << "authenticate will time out at " << until << dendl;
  while (state != MC_STATE_HAVE_SESSION && !authenticate_err) {
    if (timeout > 0.0) {
      int r = auth_cond.WaitUntil(monc_lock, until);
      if (r == ETIMEDOUT) {
	ldout(cct, 0) << "authenticate timed out after " << timeout << dendl;
	authenticate_err = -r;
      }
    } else {
      auth_cond.Wait(monc_lock);
    }
  }

  if (state == MC_STATE_HAVE_SESSION) {
    ldout(cct, 5) << "authenticate success, global_id " << global_id << dendl;
  }

  return authenticate_err;
}

void MonClient::handle_auth(MAuthReply *m)
{
  bufferlist::iterator p = m->result_bl.begin();
  if (state == MC_STATE_NEGOTIATING) {
    if (!auth || (int)m->protocol != auth->get_protocol()) {
      delete auth;
      auth = get_auth_client_handler(cct, m->protocol, rotating_secrets);
      if (!auth) {
	ldout(cct, 10) << "no handler for protocol " << m->protocol << dendl;
	if (m->result == -ENOTSUP) {
	  ldout(cct, 10) << "none of our auth protocols are supported by the server"
			 << dendl;
	  authenticate_err = m->result;
	  auth_cond.SignalAll();
	}
	m->put();
	return;
      }
      auth->set_want_keys(want_keys);
      auth->init(entity_name);
      auth->set_global_id(global_id);
    } else {
      auth->reset();
    }
    state = MC_STATE_AUTHENTICATING;
  }
  assert(auth);
  if (m->global_id && m->global_id != global_id) {
    global_id = m->global_id;
    auth->set_global_id(global_id);
    ldout(cct, 10) << "my global_id is " << m->global_id << dendl;
  }

  int ret = auth->handle_response(m->result, p);
  m->put();

  if (ret == -EAGAIN) {
    MAuth *ma = new MAuth;
    ma->protocol = auth->get_protocol();
    ret = auth->build_request(ma->auth_payload);
    _send_mon_message(ma, true);
    return;
  }

  _finish_hunting();

  authenticate_err = ret;
  if (ret == 0) {
    if (state != MC_STATE_HAVE_SESSION) {
      state = MC_STATE_HAVE_SESSION;
      while (!waiting_for_session.empty()) {
	_send_mon_message(waiting_for_session.front());
	waiting_for_session.pop_front();
      }

      if (log_client) {
	log_client->reset_session();
	Message *lm = log_client->get_mon_log_message();
	if (lm)
	  _send_mon_message(lm);
      }
    }
  
    _check_auth_tickets();
  }
  auth_cond.SignalAll();
}


// ---------

void MonClient::_send_mon_message(Message *m, bool force)
{
  assert(monc_lock.is_locked());
  assert(!cur_mon.empty());
  if (force || state == MC_STATE_HAVE_SESSION) {
    assert(cur_con);
    ldout(cct, 10) << "_send_mon_message to mon." << cur_mon
		   << " at " << cur_con->get_peer_addr() << dendl;
    messenger->send_message(m, cur_con);
  } else {
    waiting_for_session.push_back(m);
  }
}

void MonClient::_pick_new_mon()
{
  assert(monc_lock.is_locked());

  if (!cur_mon.empty() && monmap.size() > 1) {
    // pick a _different_ mon
    cur_mon = monmap.pick_random_mon_not(cur_mon);
  } else {
    cur_mon = monmap.pick_random_mon();
  }

  if (cur_con) {
    messenger->mark_down(cur_con);
    cur_con->put();
  }
  cur_con = messenger->get_connection(monmap.get_inst(cur_mon));

  ldout(cct, 10) << "_pick_new_mon picked mon." << cur_mon << " con " << cur_con
		 << " addr " << cur_con->get_peer_addr()
		 << dendl;
}


void MonClient::_reopen_session()
{
  assert(monc_lock.is_locked());
  ldout(cct, 10) << "_reopen_session" << dendl;

  _pick_new_mon();

  // throw out old queued messages
  while (!waiting_for_session.empty()) {
    waiting_for_session.front()->put();
    waiting_for_session.pop_front();
  }

  // throw out version check requests
  while (!version_requests.empty()) {
    finisher.queue(version_requests.begin()->second->context, -1);
    version_requests.erase(version_requests.begin());
  }

  // restart authentication handshake
  state = MC_STATE_NEGOTIATING;

  MAuth *m = new MAuth;
  m->protocol = 0;
  m->monmap_epoch = monmap.get_epoch();
  __u8 struct_v = 1;
  ::encode(struct_v, m->auth_payload);
  ::encode(auth_supported->get_supported_set(), m->auth_payload);
  ::encode(entity_name, m->auth_payload);
  ::encode(global_id, m->auth_payload);
  _send_mon_message(m, true);

  if (!sub_have.empty())
    _renew_subs();
}


bool MonClient::ms_handle_reset(Connection *con)
{
  Mutex::Locker lock(monc_lock);

  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    if (cur_mon.empty() || con != cur_con) {
      ldout(cct, 10) << "ms_handle_reset stray mon " << con->get_peer_addr() << dendl;
      return true;
    } else {
      ldout(cct, 10) << "ms_handle_reset current mon " << con->get_peer_addr() << dendl;
      if (hunting)
	return true;
      
      ldout(cct, 0) << "hunting for new mon" << dendl;
      hunting = true;
      _reopen_session();
    }
  }
  return false;
}

void MonClient::_finish_hunting()
{
  assert(monc_lock.is_locked());
  if (hunting) {
    ldout(cct, 1) << "found mon." << cur_mon << dendl; 
    hunting = false;
  }
}

void MonClient::tick()
{
  ldout(cct, 10) << "tick" << dendl;

  _check_auth_tickets();
  
  if (hunting) {
    ldout(cct, 1) << "continuing hunt" << dendl;
    _reopen_session();
  } else if (!cur_mon.empty()) {
    // just renew as needed
    utime_t now = ceph_clock_now(cct);
    if (now > sub_renew_after)
      _renew_subs();

    messenger->send_keepalive(cur_con);
   
    if (state == MC_STATE_HAVE_SESSION &&
	log_client) {
      Message *m = log_client->get_mon_log_message();
      if (m)
	_send_mon_message(m);
    }
  }

  if (auth)
    auth->tick();

  schedule_tick();
}

void MonClient::schedule_tick()
{
  if (hunting)
    timer.add_event_after(cct->_conf->mon_client_hunt_interval, new C_Tick(this));
  else
    timer.add_event_after(cct->_conf->mon_client_ping_interval, new C_Tick(this));
}


// ---------

void MonClient::_renew_subs()
{
  assert(monc_lock.is_locked());
  if (sub_have.empty()) {
    ldout(cct, 10) << "renew_subs - empty" << dendl;
    return;
  }

  ldout(cct, 10) << "renew_subs" << dendl;
  if (cur_mon.empty())
    _reopen_session();
  else {
    if (sub_renew_sent == utime_t())
      sub_renew_sent = ceph_clock_now(cct);

    MMonSubscribe *m = new MMonSubscribe;
    m->what = sub_have;
    _send_mon_message(m);
  }
}

void MonClient::handle_subscribe_ack(MMonSubscribeAck *m)
{
  _finish_hunting();

  if (sub_renew_sent != utime_t()) {
    sub_renew_after = sub_renew_sent;
    sub_renew_after += m->interval / 2.0;
    ldout(cct, 10) << "handle_subscribe_ack sent " << sub_renew_sent << " renew after " << sub_renew_after << dendl;
    sub_renew_sent = utime_t();
  } else {
    ldout(cct, 10) << "handle_subscribe_ack sent " << sub_renew_sent << ", ignoring" << dendl;
  }

  m->put();
}

int MonClient::_check_auth_tickets()
{
  assert(monc_lock.is_locked());
  if (state == MC_STATE_HAVE_SESSION && auth) {
    if (auth->need_tickets()) {
      ldout(cct, 10) << "_check_auth_tickets getting new tickets!" << dendl;
      MAuth *m = new MAuth;
      m->protocol = auth->get_protocol();
      auth->build_request(m->auth_payload);
      _send_mon_message(m);
    }

    _check_auth_rotating();
  }
  return 0;
}

int MonClient::_check_auth_rotating()
{
  assert(monc_lock.is_locked());
  if (!rotating_secrets ||
      !auth_principal_needs_rotating_keys(entity_name)) {
    ldout(cct, 20) << "_check_auth_rotating not needed by " << entity_name << dendl;
    return 0;
  }

  if (!auth || state != MC_STATE_HAVE_SESSION) {
    ldout(cct, 10) << "_check_auth_rotating waiting for auth session" << dendl;
    return 0;
  }

  utime_t cutoff = ceph_clock_now(cct);
  cutoff -= MIN(30.0, cct->_conf->auth_service_ticket_ttl / 4.0);
  if (!rotating_secrets->need_new_secrets(cutoff)) {
    ldout(cct, 10) << "_check_auth_rotating have uptodate secrets (they expire after " << cutoff << ")" << dendl;
    rotating_secrets->dump_rotating();
    return 0;
  }

  ldout(cct, 10) << "_check_auth_rotating renewing rotating keys (they expired before " << cutoff << ")" << dendl;
  MAuth *m = new MAuth;
  m->protocol = auth->get_protocol();
  if (auth->build_rotating_request(m->auth_payload)) {
    _send_mon_message(m);
  } else {
    m->put();
  }
  return 0;
}

int MonClient::wait_auth_rotating(double timeout)
{
  Mutex::Locker l(monc_lock);
  utime_t until = ceph_clock_now(cct);
  until += timeout;

  if (auth->get_protocol() == CEPH_AUTH_NONE)
    return 0;
  
  if (!rotating_secrets)
    return 0;

  while (auth_principal_needs_rotating_keys(entity_name) &&
	 rotating_secrets->need_new_secrets()) {
    utime_t now = ceph_clock_now(cct);
    if (now >= until) {
      ldout(cct, 0) << "wait_auth_rotating timed out after " << timeout << dendl;
      return -ETIMEDOUT;
    }
    ldout(cct, 10) << "wait_auth_rotating waiting (until " << until << ")" << dendl;
    auth_cond.WaitUntil(monc_lock, until);
  }
  ldout(cct, 10) << "wait_auth_rotating done" << dendl;
  return 0;
}

// ---------

struct C_IsLatestMap : public Context {
  Context *onfinish;
  version_t newest;
  version_t have;
  C_IsLatestMap(Context *f, version_t h) : onfinish(f), have(h) {}
  void finish(int r) {
    onfinish->complete(have != newest);
  }
};

void MonClient::is_latest_map(string map, version_t cur_ver, Context *onfinish)
{
  ldout(cct, 10) << "is_latest_map " << map << " current " << cur_ver << dendl;;
  C_IsLatestMap *c = new C_IsLatestMap(onfinish, cur_ver);
  get_version(map, &c->newest, NULL, c);
}

void MonClient::get_version(string map, version_t *newest, version_t *oldest, Context *onfinish)
{
  ldout(cct, 10) << "get_version " << map << dendl;
  Mutex::Locker l(monc_lock);
  MMonGetVersion *m = new MMonGetVersion();
  m->what = map;
  m->handle = ++version_req_id;
  version_requests[m->handle] = new version_req_d(onfinish, newest, oldest);
  _send_mon_message(m);
}

void MonClient::handle_get_version_reply(MMonGetVersionReply* m)
{
  assert(monc_lock.is_locked());
  map<tid_t, version_req_d*>::iterator iter = version_requests.find(m->handle);
  if (iter == version_requests.end()) {
    ldout(cct, 0) << "version request with handle " << m->handle
		  << " not found" << dendl;
  } else {
    version_req_d *req = iter->second;
    version_requests.erase(iter);
    if (req->newest)
      *req->newest = m->version;
    if (req->oldest)
      *req->oldest = m->oldest_version;
    finisher.queue(req->context, 0);
    delete req;
  }
}
