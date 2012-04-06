// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include "msg/Messenger.h"
#include "ObjectCacher.h"
#include "Objecter.h"



/*** ObjectCacher::BufferHead ***/


/*** ObjectCacher::Object ***/

#define dout_subsys ceph_subsys_objectcacher
#undef dout_prefix
#define dout_prefix *_dout << oc->objecter->messenger->get_myname() << ".objectcacher.object(" << oid << ") "

ObjectCacher::
ObjectCacher(CephContext *cct_, Objecter *o, Mutex& l, flush_set_callback_t flush_callback,
	     void *flush_callback_arg) : 
    cct(cct_), objecter(o), filer(o), lock(l),
    flush_set_callback(flush_callback), flush_set_callback_arg(flush_callback_arg),
    flusher_stop(false), flusher_thread(this),
    stat_waiter(0),
    stat_clean(0), stat_dirty(0), stat_rx(0), stat_tx(0), stat_missing(0) {
  }

ObjectCacher::BufferHead *ObjectCacher::Object::split(BufferHead *left, loff_t off)
{
  ldout(oc->cct, 20) << "split " << *left << " at " << off << dendl;
  
  // split off right
  ObjectCacher::BufferHead *right = new BufferHead(this);
  right->last_write_tid = left->last_write_tid;
  right->set_state(left->get_state());
  right->snapc = left->snapc;
  
  loff_t newleftlen = off - left->start();
  right->set_start(off);
  right->set_length(left->length() - newleftlen);
  
  // shorten left
  oc->bh_stat_sub(left);
  left->set_length(newleftlen);
  oc->bh_stat_add(left);
  
  // add right
  oc->bh_add(this, right);
  
  // split buffers too
  bufferlist bl;
  bl.claim(left->bl);
  if (bl.length()) {
    assert(bl.length() == (left->length() + right->length()));
    right->bl.substr_of(bl, left->length(), right->length());
    left->bl.substr_of(bl, 0, left->length());
  }
  
  // move read waiters
  if (!left->waitfor_read.empty()) {
    map<loff_t, list<Context*> >::iterator o, p = left->waitfor_read.end();
    p--;
    while (p != left->waitfor_read.begin()) {
      if (p->first < right->start()) break;      
      ldout(oc->cct, 0) << "split  moving waiters at byte " << p->first << " to right bh" << dendl;
      right->waitfor_read[p->first].swap( p->second );
      o = p;
      p--;
      left->waitfor_read.erase(o);
    }
  }
  
  ldout(oc->cct, 20) << "split    left is " << *left << dendl;
  ldout(oc->cct, 20) << "split   right is " << *right << dendl;
  return right;
}


void ObjectCacher::Object::merge_left(BufferHead *left, BufferHead *right)
{
  assert(left->end() == right->start());
  assert(left->get_state() == right->get_state());

  ldout(oc->cct, 10) << "merge_left " << *left << " + " << *right << dendl;
  oc->bh_remove(this, right);
  oc->bh_stat_sub(left);
  left->set_length(left->length() + right->length());
  oc->bh_stat_add(left);

  // data
  left->bl.claim_append(right->bl);
  
  // version 
  // note: this is sorta busted, but should only be used for dirty buffers
  left->last_write_tid =  MAX( left->last_write_tid, right->last_write_tid );
  left->last_write = MAX( left->last_write, right->last_write );

  // waiters
  for (map<loff_t, list<Context*> >::iterator p = right->waitfor_read.begin();
       p != right->waitfor_read.end();
       p++) 
    left->waitfor_read[p->first].splice( left->waitfor_read[p->first].begin(),
                                         p->second );
  
  // hose right
  delete right;

  ldout(oc->cct, 10) << "merge_left result " << *left << dendl;
}

void ObjectCacher::Object::try_merge_bh(BufferHead *bh)
{
  ldout(oc->cct, 10) << "try_merge_bh " << *bh << dendl;

  // to the left?
  map<loff_t,BufferHead*>::iterator p = data.find(bh->start());
  assert(p->second == bh);
  if (p != data.begin()) {
    p--;
    if (p->second->end() == bh->start() &&
	p->second->get_state() == bh->get_state()) {
      merge_left(p->second, bh);
      bh = p->second;
    } else 
      p++;
  }
  // to the right?
  assert(p->second == bh);
  p++;
  if (p != data.end() &&
      p->second->start() == bh->end() &&
      p->second->get_state() == bh->get_state()) 
    merge_left(bh, p->second);
}

/*
 * count bytes we have cached in given range
 */
bool ObjectCacher::Object::is_cached(loff_t cur, loff_t left)
{
  map<loff_t, BufferHead*>::iterator p = data.lower_bound(cur);
  
  if (p != data.begin() && 
      (p == data.end() || p->first > cur)) {
    p--;     // might overlap!
    if (p->first + p->second->length() <= cur) 
      p++;   // doesn't overlap.
  }
  
  while (left > 0) {
    if (p == data.end())
      return false;

    if (p->first <= cur) {
      // have part of it
      loff_t lenfromcur = MIN(p->second->end() - cur, left);
      cur += lenfromcur;
      left -= lenfromcur;
      p++;
      continue;
    } else if (p->first > cur) {
      // gap
      return false;
    } else
      assert(0);
  }

  return true;
}

/*
 * map a range of bytes into buffer_heads.
 * - create missing buffer_heads as necessary.
 */
int ObjectCacher::Object::map_read(OSDRead *rd,
                                   map<loff_t, BufferHead*>& hits,
                                   map<loff_t, BufferHead*>& missing,
                                   map<loff_t, BufferHead*>& rx)
{
  for (vector<ObjectExtent>::iterator ex_it = rd->extents.begin();
       ex_it != rd->extents.end();
       ex_it++) {
    
    if (ex_it->oid != oid.oid) continue;
    
    ldout(oc->cct, 10) << "map_read " << ex_it->oid 
             << " " << ex_it->offset << "~" << ex_it->length << dendl;
    
    map<loff_t, BufferHead*>::iterator p = data.lower_bound(ex_it->offset);
    // p->first >= start
    
    loff_t cur = ex_it->offset;
    loff_t left = ex_it->length;
    
    if (p != data.begin() && 
        (p == data.end() || p->first > cur)) {
      p--;     // might overlap!
      if (p->first + p->second->length() <= cur) 
        p++;   // doesn't overlap.
    }
    
    while (left > 0) {
      // at end?
      if (p == data.end()) {
        // rest is a miss.
        BufferHead *n = new BufferHead(this);
        n->set_start(cur);
        n->set_length(left);
        oc->bh_add(this, n);
        missing[cur] = n;
        ldout(oc->cct, 20) << "map_read miss " << left << " left, " << *n << dendl;
        cur += left;
        left -= left;
        assert(left == 0);
        assert(cur == (loff_t)ex_it->offset + (loff_t)ex_it->length);
        break;  // no more.
      }
      
      if (p->first <= cur) {
        // have it (or part of it)
        BufferHead *e = p->second;
        
        if (e->is_clean() ||
            e->is_dirty() ||
            e->is_tx()) {
          hits[cur] = e;     // readable!
          ldout(oc->cct, 20) << "map_read hit " << *e << dendl;
        } 
        else if (e->is_rx()) {
          rx[cur] = e;       // missing, not readable.
          ldout(oc->cct, 20) << "map_read rx " << *e << dendl;
        }
        else assert(0);
        
        loff_t lenfromcur = MIN(e->end() - cur, left);
        cur += lenfromcur;
        left -= lenfromcur;
        p++;
        continue;  // more?
        
      } else if (p->first > cur) {
        // gap.. miss
        loff_t next = p->first;
        BufferHead *n = new BufferHead(this);
        n->set_start( cur );
        n->set_length( MIN(next - cur, left) );
        oc->bh_add(this,n);
        missing[cur] = n;
        cur += MIN(left, n->length());
        left -= MIN(left, n->length());
        ldout(oc->cct, 20) << "map_read gap " << *n << dendl;
        continue;    // more?
      }
      else 
        assert(0);
    }
  }
  return(0);
}

/*
 * map a range of extents on an object's buffer cache.
 * - combine any bh's we're writing into one
 * - break up bufferheads that don't fall completely within the range
 * //no! - return a bh that includes the write.  may also include other dirty data to left and/or right.
 */
ObjectCacher::BufferHead *ObjectCacher::Object::map_write(OSDWrite *wr)
{
  BufferHead *final = 0;

  for (vector<ObjectExtent>::iterator ex_it = wr->extents.begin();
       ex_it != wr->extents.end();
       ex_it++) {
    
    if (ex_it->oid != oid.oid) continue;
    
    ldout(oc->cct, 10) << "map_write oex " << ex_it->oid
             << " " << ex_it->offset << "~" << ex_it->length << dendl;
    
    map<loff_t, BufferHead*>::iterator p = data.lower_bound(ex_it->offset);
    // p->first >= start
    
    loff_t cur = ex_it->offset;
    loff_t left = ex_it->length;
    
    if (p != data.begin() && 
        (p == data.end() || p->first > cur)) {
      p--;     // might overlap or butt up!

      /*// dirty and butts up?
      if (p->first + p->second->length() == cur &&
          p->second->is_dirty()) {
        ldout(oc->cct, 10) << "map_write will append to tail of " << *p->second << dendl;
        final = p->second;
      }
      */
      if (p->first + p->second->length() <= cur) 
        p++;   // doesn't overlap.
    }    
    
    while (left > 0) {
      loff_t max = left;

      // at end ?
      if (p == data.end()) {
        if (final == NULL) {
          final = new BufferHead(this);
          final->set_start( cur );
          final->set_length( max );
          oc->bh_add(this, final);
          ldout(oc->cct, 10) << "map_write adding trailing bh " << *final << dendl;
        } else {
	  oc->bh_stat_sub(final);
          final->set_length(final->length() + max);
	  oc->bh_stat_add(final);
        }
        left -= max;
        cur += max;
        continue;
      }
      
      ldout(oc->cct, 10) << "cur is " << cur << ", p is " << *p->second << dendl;
      //oc->verify_stats();

      if (p->first <= cur) {
        BufferHead *bh = p->second;
        ldout(oc->cct, 10) << "map_write bh " << *bh << " intersected" << dendl;
        
        if (p->first < cur) {
          assert(final == 0);
          if (cur + max >= p->first + p->second->length()) {
            // we want right bit (one splice)
            final = split(bh, cur);   // just split it, take right half.
            p++;
            assert(p->second == final);
          } else {
            // we want middle bit (two splices)
            final = split(bh, cur);
            p++;
            assert(p->second == final);
            split(final, cur+max);
          }
        } else if (p->first == cur) {
          if (p->second->length() <= max) {
            // whole bufferhead, piece of cake.
          } else {
            // we want left bit (one splice)
            split(bh, cur + max);        // just split
          }
          if (final) {
	    oc->mark_dirty(bh);
	    oc->mark_dirty(final);
	    p--;  // move iterator back to final
	    assert(p->second == final);
            merge_left(final, bh);
	  } else
            final = bh;
        }
        
        // keep going.
        loff_t lenfromcur = final->end() - cur;
        cur += lenfromcur;
        left -= lenfromcur;
        p++;
        continue; 
      } else {
        // gap!
        loff_t next = p->first;
        loff_t glen = MIN(next - cur, max);
        ldout(oc->cct, 10) << "map_write gap " << cur << "~" << glen << dendl;
        if (final) {
	  oc->bh_stat_sub(final);
          final->set_length(final->length() + glen);
	  oc->bh_stat_add(final);
        } else {
          final = new BufferHead(this);
          final->set_start( cur );
          final->set_length( glen );
          oc->bh_add(this, final);
        }
        
        cur += glen;
        left -= glen;
        continue;    // more?
      }
    }
  }
  
  // set versoin
  assert(final);
  ldout(oc->cct, 10) << "map_write final is " << *final << dendl;

  return final;
}

void ObjectCacher::Object::truncate(loff_t s)
{
  ldout(oc->cct, 10) << "truncate " << *this << " to " << s << dendl;

  while (!data.empty()) {
    BufferHead *bh = data.rbegin()->second;
    if (bh->end() <= s) 
      break;

    // split bh at truncation point?
    if (bh->start() < s) {
      split(bh, s);
      continue;
    }

    // remove bh entirely
    assert(bh->start() >= s);
    oc->bh_remove(this, bh);
    delete bh;
  }
}



/*** ObjectCacher ***/

#undef dout_prefix
#define dout_prefix *_dout << objecter->messenger->get_myname() << ".objectcacher "

/* private */

void ObjectCacher::close_object(Object *ob) 
{
  ldout(cct, 10) << "close_object " << *ob << dendl;
  assert(ob->can_close());
  
  // ok!
  objects[ob->oloc.pool].erase(ob->get_soid());
  delete ob;
}




void ObjectCacher::bh_read(BufferHead *bh)
{
  ldout(cct, 7) << "bh_read on " << *bh << dendl;

  mark_rx(bh);

  // finisher
  C_ReadFinish *onfinish = new C_ReadFinish(this, bh->ob->oloc.pool,
                                            bh->ob->get_soid(), bh->start(), bh->length());

  ObjectSet *oset = bh->ob->oset;

  // go
  objecter->read_trunc(bh->ob->get_oid(), bh->ob->get_oloc(), 
		 bh->start(), bh->length(), bh->ob->get_snap(),
		 &onfinish->bl, 0,
		 oset->truncate_size, oset->truncate_seq,
		 onfinish);
}

void ObjectCacher::bh_read_finish(int64_t poolid, sobject_t oid, loff_t start, uint64_t length, bufferlist &bl)
{
  //lock.Lock();
  ldout(cct, 7) << "bh_read_finish " 
          << oid
          << " " << start << "~" << length
	  << " (bl is " << bl.length() << ")"
          << dendl;

  if (bl.length() < length) {
    bufferptr bp(length - bl.length());
    bp.zero();
    ldout(cct, 7) << "bh_read_finish " << oid << " padding " << start << "~" << length 
	    << " with " << bp.length() << " bytes of zeroes" << dendl;
    bl.push_back(bp);
  }
  
  if (objects[poolid].count(oid) == 0) {
    ldout(cct, 7) << "bh_read_finish no object cache" << dendl;
  } else {
    Object *ob = objects[poolid][oid];
    
    // apply to bh's!
    loff_t opos = start;
    map<loff_t, BufferHead*>::iterator p = ob->data.lower_bound(opos);
    
    while (p != ob->data.end() &&
           opos < start+(loff_t)length) {
      BufferHead *bh = p->second;
      
      if (bh->start() > opos) {
        ldout(cct, 1) << "weirdness: gap when applying read results, " 
                << opos << "~" << bh->start() - opos 
                << dendl;
        opos = bh->start();
        continue;
      }
      
      if (!bh->is_rx()) {
        ldout(cct, 10) << "bh_read_finish skipping non-rx " << *bh << dendl;
        opos = bh->end();
        p++;
        continue;
      }
      
      assert(opos >= bh->start());
      assert(bh->start() == opos);   // we don't merge rx bh's... yet!
      assert(bh->length() <= start+(loff_t)length-opos);
      
      bh->bl.substr_of(bl,
                       opos-bh->start(),
                       bh->length());
      mark_clean(bh);
      ldout(cct, 10) << "bh_read_finish read " << *bh << dendl;
      
      opos = bh->end();
      p++;

      // finishers?
      // called with lock held.
      list<Context*> ls;
      for (map<loff_t, list<Context*> >::iterator p = bh->waitfor_read.begin();
           p != bh->waitfor_read.end();
           p++)
        ls.splice(ls.end(), p->second);
      bh->waitfor_read.clear();
      finish_contexts(cct, ls);

      // clean up?
      ob->try_merge_bh(bh);
    }
  }
  //lock.Unlock();
}


void ObjectCacher::bh_write(BufferHead *bh)
{
  ldout(cct, 7) << "bh_write " << *bh << dendl;
  
  // finishers
  C_WriteCommit *oncommit = new C_WriteCommit(this, bh->ob->oloc.pool,
                                              bh->ob->get_soid(), bh->start(), bh->length());

  ObjectSet *oset = bh->ob->oset;

  // go
  tid_t tid = objecter->write_trunc(bh->ob->get_oid(), bh->ob->get_oloc(),
			      bh->start(), bh->length(),
			      bh->snapc, bh->bl, bh->last_write, 0,
			      oset->truncate_size, oset->truncate_seq,
				    NULL, oncommit);

  // set bh last_write_tid
  oncommit->tid = tid;
  bh->ob->last_write_tid = tid;
  bh->last_write_tid = tid;

  mark_tx(bh);
}

void ObjectCacher::lock_ack(int64_t poolid, list<sobject_t>& oids, tid_t tid)
{
  for (list<sobject_t>::iterator i = oids.begin();
       i != oids.end();
       i++) {
    sobject_t oid = *i;

    if (objects[poolid].count(oid) == 0) {
      ldout(cct, 7) << "lock_ack no object cache" << dendl;
      assert(0);
    } 
    
    Object *ob = objects[poolid][oid];

    list<Context*> ls;

    // waiters?
    if (ob->waitfor_commit.count(tid)) {
      ls.splice(ls.end(), ob->waitfor_commit[tid]);
      ob->waitfor_commit.erase(tid);
    }
    
    assert(tid <= ob->last_write_tid);
    if (ob->last_write_tid == tid) {
      ldout(cct, 10) << "lock_ack " << *ob
               << " tid " << tid << dendl;

      switch (ob->lock_state) {
      case Object::LOCK_RDUNLOCKING: 
      case Object::LOCK_WRUNLOCKING: 
        ob->lock_state = Object::LOCK_NONE; 
        break;
      case Object::LOCK_RDLOCKING: 
      case Object::LOCK_DOWNGRADING: 
        ob->lock_state = Object::LOCK_RDLOCK; 
        ls.splice(ls.begin(), ob->waitfor_rd);
        break;
      case Object::LOCK_UPGRADING: 
      case Object::LOCK_WRLOCKING: 
        ob->lock_state = Object::LOCK_WRLOCK; 
        ls.splice(ls.begin(), ob->waitfor_wr);
        ls.splice(ls.begin(), ob->waitfor_rd);
        break;

      default:
        assert(0);
      }
      
      ob->last_commit_tid = tid;
      
      if (ob->can_close())
        close_object(ob);
    } else {
      ldout(cct, 10) << "lock_ack " << *ob 
               << " tid " << tid << " obsolete" << dendl;
    }

    finish_contexts(cct, ls);

  }
}

void ObjectCacher::bh_write_commit(int64_t poolid, sobject_t oid, loff_t start, uint64_t length, tid_t tid)
{
  //lock.Lock();
  
  ldout(cct, 7) << "bh_write_commit " 
          << oid 
          << " tid " << tid
          << " " << start << "~" << length
          << dendl;
  if (objects[poolid].count(oid) == 0) {
    ldout(cct, 7) << "bh_write_commit no object cache" << dendl;
  } else {
    Object *ob = objects[poolid][oid];
    
    // apply to bh's!
    for (map<loff_t, BufferHead*>::iterator p = ob->data.lower_bound(start);
         p != ob->data.end();
         p++) {
      BufferHead *bh = p->second;
      
      if (bh->start() > start+(loff_t)length)
	break;

      if (bh->start() < start &&
          bh->end() > start+(loff_t)length) {
        ldout(cct, 20) << "bh_write_commit skipping " << *bh << dendl;
        continue;
      }
      
      // make sure bh is tx
      if (!bh->is_tx()) {
        ldout(cct, 10) << "bh_write_commit skipping non-tx " << *bh << dendl;
        continue;
      }
      
      // make sure bh tid matches
      if (bh->last_write_tid != tid) {
        assert(bh->last_write_tid > tid);
        ldout(cct, 10) << "bh_write_commit newer tid on " << *bh << dendl;
        continue;
      }
      
      // ok!  mark bh clean.
      mark_clean(bh);
      ldout(cct, 10) << "bh_write_commit clean " << *bh << dendl;
    }
    
    // update last_commit.
    assert(ob->last_commit_tid < tid);
    ob->last_commit_tid = tid;

    // waiters?
    if (ob->waitfor_commit.count(tid)) {
      list<Context*> ls;
      ls.splice(ls.begin(), ob->waitfor_commit[tid]);
      ob->waitfor_commit.erase(tid);
      finish_contexts(cct, ls);
    }

    // is the entire object set now clean and fully committed?
    ObjectSet *oset = ob->oset;
    if (ob->can_close())
      close_object(ob);

    // is the entire object set now clean?
    if (flush_set_callback &&
	oset->dirty_or_tx == 0) {        // nothing dirty/tx
      flush_set_callback(flush_set_callback_arg, oset);      
    }
  }
  //lock.Unlock();
}

void ObjectCacher::flush(loff_t amount)
{
  utime_t cutoff = ceph_clock_now(cct);
  //cutoff.sec_ref() -= cct->_conf->client_oc_max_dirty_age;

  ldout(cct, 10) << "flush " << amount << dendl;
  
  /*
   * NOTE: we aren't actually pulling things off the LRU here, just looking at the
   * tail item.  Then we call bh_write, which moves it to the other LRU, so that we
   * can call lru_dirty.lru_get_next_expire() again.
   */
  loff_t did = 0;
  while (amount == 0 || did < amount) {
    BufferHead *bh = (BufferHead*) lru_dirty.lru_get_next_expire();
    if (!bh) break;
    if (bh->last_write > cutoff) break;

    did += bh->length();
    bh_write(bh);
  }    
}


void ObjectCacher::trim(loff_t max)
{
  if (max < 0) 
    max = cct->_conf->client_oc_size;
  
  ldout(cct, 10) << "trim  start: max " << max 
           << "  clean " << get_stat_clean()
           << dendl;

  while (get_stat_clean() > max) {
    BufferHead *bh = (BufferHead*) lru_rest.lru_expire();
    if (!bh) break;
    
    ldout(cct, 10) << "trim trimming " << *bh << dendl;
    assert(bh->is_clean());
    
    Object *ob = bh->ob;
    bh_remove(ob, bh);
    delete bh;
    
    if (ob->can_close()) {
      ldout(cct, 10) << "trim trimming " << *ob << dendl;
      close_object(ob);
    }
  }
  
  ldout(cct, 10) << "trim finish: max " << max 
           << "  clean " << get_stat_clean()
           << dendl;
}



/* public */

bool ObjectCacher::is_cached(ObjectSet *oset, vector<ObjectExtent>& extents, snapid_t snapid)
{
  for (vector<ObjectExtent>::iterator ex_it = extents.begin();
       ex_it != extents.end();
       ex_it++) {
    ldout(cct, 10) << "is_cached " << *ex_it << dendl;

    // get Object cache
    sobject_t soid(ex_it->oid, snapid);
    Object *o = get_object_maybe(soid, ex_it->oloc);
    if (!o)
      return false;
    if (!o->is_cached(ex_it->offset, ex_it->length))
      return false;
  }
  return true;
}


/*
 * returns # bytes read (if in cache).  onfinish is untouched (caller must delete it)
 * returns 0 if doing async read
 */
int ObjectCacher::readx(OSDRead *rd, ObjectSet *oset, Context *onfinish)
{
  bool success = true;
  list<BufferHead*> hit_ls;
  map<uint64_t, bufferlist> stripe_map;  // final buffer offset -> substring

  for (vector<ObjectExtent>::iterator ex_it = rd->extents.begin();
       ex_it != rd->extents.end();
       ex_it++) {
    ldout(cct, 10) << "readx " << *ex_it << dendl;

    // get Object cache
    sobject_t soid(ex_it->oid, rd->snap);
    Object *o = get_object(soid, oset, ex_it->oloc);
    
    // map extent into bufferheads
    map<loff_t, BufferHead*> hits, missing, rx;
    o->map_read(rd, hits, missing, rx);
    
    if (!missing.empty() || !rx.empty()) {
      // read missing
      for (map<loff_t, BufferHead*>::iterator bh_it = missing.begin();
           bh_it != missing.end();
           bh_it++) {
        bh_read(bh_it->second);
        if (success && onfinish) {
          ldout(cct, 10) << "readx missed, waiting on " << *bh_it->second 
                   << " off " << bh_it->first << dendl;
	  bh_it->second->waitfor_read[bh_it->first].push_back( new C_RetryRead(this, rd, oset, onfinish) );
        }
	success = false;
      }

      // bump rx
      for (map<loff_t, BufferHead*>::iterator bh_it = rx.begin();
           bh_it != rx.end();
           bh_it++) {
        touch_bh(bh_it->second);        // bump in lru, so we don't lose it.
        if (success && onfinish) {
          ldout(cct, 10) << "readx missed, waiting on " << *bh_it->second 
                   << " off " << bh_it->first << dendl;
	  bh_it->second->waitfor_read[bh_it->first].push_back( new C_RetryRead(this, rd, oset, onfinish) );
        }
	success = false;
      }      
    } else {
      assert(!hits.empty());

      // make a plain list
      for (map<loff_t, BufferHead*>::iterator bh_it = hits.begin();
           bh_it != hits.end();
           bh_it++) {
	ldout(cct, 10) << "readx hit bh " << *bh_it->second << dendl;
        hit_ls.push_back(bh_it->second);
      }

      // create reverse map of buffer offset -> object for the eventual result.
      // this is over a single ObjectExtent, so we know that
      //  - the bh's are contiguous
      //  - the buffer frags need not be (and almost certainly aren't)
      loff_t opos = ex_it->offset;
      map<loff_t, BufferHead*>::iterator bh_it = hits.begin();
      assert(bh_it->second->start() <= opos);
      uint64_t bhoff = opos - bh_it->second->start();
      map<uint64_t, uint64_t>::iterator f_it = ex_it->buffer_extents.begin();
      uint64_t foff = 0;
      while (1) {
        BufferHead *bh = bh_it->second;
        assert(opos == (loff_t)(bh->start() + bhoff));

        ldout(cct, 10) << "readx rmap opos " << opos
                 << ": " << *bh << " +" << bhoff
                 << " frag " << f_it->first << "~" << f_it->second << " +" << foff
                 << dendl;

        uint64_t len = MIN(f_it->second - foff,
                         bh->length() - bhoff);
	bufferlist bit;  // put substr here first, since substr_of clobbers, and
	                 // we may get multiple bh's at this stripe_map position
	bit.substr_of(bh->bl,
		      opos - bh->start(),
		      len);
        stripe_map[f_it->first].claim_append(bit);

        opos += len;
        bhoff += len;
        foff += len;
        if (opos == bh->end()) {
          bh_it++;
          bhoff = 0;
        }
        if (foff == f_it->second) {
          f_it++;
          foff = 0;
        }
        if (bh_it == hits.end()) break;
        if (f_it == ex_it->buffer_extents.end()) break;
      }
      assert(f_it == ex_it->buffer_extents.end());
      assert(opos == (loff_t)ex_it->offset + (loff_t)ex_it->length);
    }
  }
  
  // bump hits in lru
  for (list<BufferHead*>::iterator bhit = hit_ls.begin();
       bhit != hit_ls.end();
       bhit++) 
    touch_bh(*bhit);
  
  if (!success) return 0;  // wait!

  // no misses... success!  do the read.
  assert(!hit_ls.empty());
  ldout(cct, 10) << "readx has all buffers" << dendl;
  
  // ok, assemble into result buffer.
  uint64_t pos = 0;
  if (rd->bl) {
    rd->bl->clear();
    for (map<uint64_t,bufferlist>::iterator i = stripe_map.begin();
	 i != stripe_map.end();
	 i++) {
      assert(pos == i->first);
      ldout(cct, 10) << "readx  adding buffer len " << i->second.length() << " at " << pos << dendl;
      pos += i->second.length();
      rd->bl->claim_append(i->second);
      assert(rd->bl->length() == pos);
    }
    ldout(cct, 10) << "readx  result is " << rd->bl->length() << dendl;
  } else {
    ldout(cct, 10) << "readx  no bufferlist ptr (readahead?), done." << dendl;
  }

  // done with read.
  delete rd;

  trim();
  
  return pos;
}


int ObjectCacher::writex(OSDWrite *wr, ObjectSet *oset)
{
  utime_t now = ceph_clock_now(cct);
  
  for (vector<ObjectExtent>::iterator ex_it = wr->extents.begin();
       ex_it != wr->extents.end();
       ex_it++) {
    // get object cache
    sobject_t soid(ex_it->oid, CEPH_NOSNAP);
    Object *o = get_object(soid, oset, ex_it->oloc);

    // map it all into a single bufferhead.
    BufferHead *bh = o->map_write(wr);
    bh->snapc = wr->snapc;
    
    // adjust buffer pointers (ie "copy" data into my cache)
    // this is over a single ObjectExtent, so we know that
    //  - there is one contiguous bh
    //  - the buffer frags need not be (and almost certainly aren't)
    // note: i assume striping is monotonic... no jumps backwards, ever!
    loff_t opos = ex_it->offset;
    for (map<uint64_t, uint64_t>::iterator f_it = ex_it->buffer_extents.begin();
         f_it != ex_it->buffer_extents.end();
         f_it++) {
      ldout(cct, 10) << "writex writing " << f_it->first << "~" << f_it->second << " into " << *bh << " at " << opos << dendl;
      uint64_t bhoff = bh->start() - opos;
      assert(f_it->second <= bh->length() - bhoff);

      // get the frag we're mapping in
      bufferlist frag; 
      frag.substr_of(wr->bl, 
                     f_it->first, f_it->second);

      // keep anything left of bhoff
      bufferlist newbl;
      if (bhoff)
	newbl.substr_of(bh->bl, 0, bhoff);
      newbl.claim_append(frag);
      bh->bl.swap(newbl);

      opos += f_it->second;
    }

    // ok, now bh is dirty.
    mark_dirty(bh);
    touch_bh(bh);
    bh->last_write = now;

    o->try_merge_bh(bh);
  }

  delete wr;

  //verify_stats();
  trim();
  return 0;
}
 

// blocking wait for write.
bool ObjectCacher::wait_for_write(uint64_t len, Mutex& lock)
{
  int blocked = 0;
  const md_config_t *conf = cct->_conf;

  // wait for writeback?
  while (get_stat_dirty() + get_stat_tx() >= conf->client_oc_max_dirty) {
    ldout(cct, 10) << "wait_for_write waiting on " << len << ", dirty|tx " 
	     << (get_stat_dirty() + get_stat_tx()) 
	     << " >= " << conf->client_oc_max_dirty 
	     << dendl;
    flusher_cond.Signal();
    stat_waiter++;
    stat_cond.Wait(lock);
    stat_waiter--;
    blocked++;
    ldout(cct, 10) << "wait_for_write woke up" << dendl;
  }

  // start writeback anyway?
  if (get_stat_dirty() > conf->client_oc_target_dirty) {
    ldout(cct, 10) << "wait_for_write " << get_stat_dirty() << " > target "
	     << conf->client_oc_target_dirty << ", nudging flusher" << dendl;
    flusher_cond.Signal();
  }
  return blocked;
}

void ObjectCacher::flusher_entry()
{
  const md_config_t *conf = cct->_conf;
  ldout(cct, 10) << "flusher start" << dendl;
  lock.Lock();
  while (!flusher_stop) {
    while (!flusher_stop) {
      loff_t all = get_stat_tx() + get_stat_rx() + get_stat_clean() + get_stat_dirty();
      ldout(cct, 11) << "flusher "
               << all << " / " << conf->client_oc_size << ":  "
               << get_stat_tx() << " tx, "
               << get_stat_rx() << " rx, "
               << get_stat_clean() << " clean, "
               << get_stat_dirty() << " dirty ("
	       << conf->client_oc_target_dirty << " target, "
	       << conf->client_oc_max_dirty << " max)"
               << dendl;
      if (get_stat_dirty() > conf->client_oc_target_dirty) {
        // flush some dirty pages
        ldout(cct, 10) << "flusher " 
                 << get_stat_dirty() << " dirty > target "
		 << conf->client_oc_target_dirty
                 << ", flushing some dirty bhs" << dendl;
        flush(get_stat_dirty() - conf->client_oc_target_dirty);
      }
      else {
        // check tail of lru for old dirty items
        utime_t cutoff = ceph_clock_now(cct);
        cutoff.sec_ref()--;
        BufferHead *bh = 0;
        while ((bh = (BufferHead*)lru_dirty.lru_get_next_expire()) != 0 &&
               bh->last_write < cutoff) {
          ldout(cct, 10) << "flusher flushing aged dirty bh " << *bh << dendl;
          bh_write(bh);
        }
        break;
      }
    }
    if (flusher_stop) break;
    flusher_cond.WaitInterval(cct, lock, utime_t(1,0));
  }
  lock.Unlock();
  ldout(cct, 10) << "flusher finish" << dendl;
}

// locking -----------------------------

void ObjectCacher::rdlock(Object *o)
{
  // lock?
  if (o->lock_state == Object::LOCK_NONE ||
      o->lock_state == Object::LOCK_RDUNLOCKING ||
      o->lock_state == Object::LOCK_WRUNLOCKING) {
    ldout(cct, 10) << "rdlock rdlock " << *o << dendl;
    
    o->lock_state = Object::LOCK_RDLOCKING;
    
    C_LockAck *ack = new C_LockAck(this, o->oloc.pool, o->get_soid());
    C_WriteCommit *commit = new C_WriteCommit(this, o->oloc.pool,
                                              o->get_soid(), 0, 0);
    
    commit->tid = 
      ack->tid = 
      o->last_write_tid = objecter->lock(o->get_oid(), o->get_oloc(), CEPH_OSD_OP_RDLOCK, 0, ack, commit);
  }
  
  // stake our claim.
  o->rdlock_ref++;  
  
  // wait?
  if (o->lock_state == Object::LOCK_RDLOCKING ||
      o->lock_state == Object::LOCK_WRLOCKING) {
    ldout(cct, 10) << "rdlock waiting for rdlock|wrlock on " << *o << dendl;
    Mutex flock("ObjectCacher::rdlock flock");
    Cond cond;
    bool done = false;
    o->waitfor_rd.push_back(new C_SafeCond(&flock, &cond, &done));
    while (!done) cond.Wait(flock);
  }
  assert(o->lock_state == Object::LOCK_RDLOCK ||
         o->lock_state == Object::LOCK_WRLOCK ||
         o->lock_state == Object::LOCK_UPGRADING ||
         o->lock_state == Object::LOCK_DOWNGRADING);
}

void ObjectCacher::wrlock(Object *o)
{
  // lock?
  if (o->lock_state != Object::LOCK_WRLOCK &&
      o->lock_state != Object::LOCK_WRLOCKING &&
      o->lock_state != Object::LOCK_UPGRADING) {
    ldout(cct, 10) << "wrlock wrlock " << *o << dendl;
    
    int op = 0;
    if (o->lock_state == Object::LOCK_RDLOCK) {
      o->lock_state = Object::LOCK_UPGRADING;
      op = CEPH_OSD_OP_UPLOCK;
    } else {
      o->lock_state = Object::LOCK_WRLOCKING;
      op = CEPH_OSD_OP_WRLOCK;
    }
    
    C_LockAck *ack = new C_LockAck(this, o->oloc.pool, o->get_soid());
    C_WriteCommit *commit = new C_WriteCommit(this, o->oloc.pool,
                                              o->get_soid(), 0, 0);
    
    commit->tid = 
      ack->tid = 
      o->last_write_tid = objecter->lock(o->get_oid(), o->get_oloc(), op, 0, ack, commit);
  }
  
  // stake our claim.
  o->wrlock_ref++;  
  
  // wait?
  if (o->lock_state == Object::LOCK_WRLOCKING ||
      o->lock_state == Object::LOCK_UPGRADING) {
    ldout(cct, 10) << "wrlock waiting for wrlock on " << *o << dendl;
    Mutex flock("ObjectCacher::wrlock flock");
    Cond cond;
    bool done = false;
    o->waitfor_wr.push_back(new C_SafeCond(&flock, &cond, &done));
    while (!done) cond.Wait(flock);
  }
  assert(o->lock_state == Object::LOCK_WRLOCK);
}


void ObjectCacher::rdunlock(Object *o)
{
  ldout(cct, 10) << "rdunlock " << *o << dendl;
  assert(o->lock_state == Object::LOCK_RDLOCK ||
         o->lock_state == Object::LOCK_WRLOCK ||
         o->lock_state == Object::LOCK_UPGRADING ||
         o->lock_state == Object::LOCK_DOWNGRADING);

  assert(o->rdlock_ref > 0);
  o->rdlock_ref--;
  if (o->rdlock_ref > 0 ||
      o->wrlock_ref > 0) {
    ldout(cct, 10) << "rdunlock " << *o << " still has rdlock|wrlock refs" << dendl;
    return;
  }

  release(o);  // release first

  o->lock_state = Object::LOCK_RDUNLOCKING;

  C_LockAck *lockack = new C_LockAck(this, o->oloc.pool, o->get_soid());
  C_WriteCommit *commit = new C_WriteCommit(this, o->oloc.pool,
                                            o->get_soid(), 0, 0);
  commit->tid = 
    lockack->tid = 
    o->last_write_tid = objecter->lock(o->get_oid(), o->get_oloc(), CEPH_OSD_OP_RDUNLOCK, 0, lockack, commit);
}

void ObjectCacher::wrunlock(Object *o)
{
  ldout(cct, 10) << "wrunlock " << *o << dendl;
  assert(o->lock_state == Object::LOCK_WRLOCK);

  assert(o->wrlock_ref > 0);
  o->wrlock_ref--;
  if (o->wrlock_ref > 0) {
    ldout(cct, 10) << "wrunlock " << *o << " still has wrlock refs" << dendl;
    return;
  }

  flush(o);  // flush first

  int op = 0;
  if (o->rdlock_ref > 0) {
    ldout(cct, 10) << "wrunlock rdlock " << *o << dendl;
    op = CEPH_OSD_OP_DNLOCK;
    o->lock_state = Object::LOCK_DOWNGRADING;
  } else {
    ldout(cct, 10) << "wrunlock wrunlock " << *o << dendl;
    op = CEPH_OSD_OP_WRUNLOCK;
    o->lock_state = Object::LOCK_WRUNLOCKING;
  }

  C_LockAck *lockack = new C_LockAck(this, o->oloc.pool, o->get_soid());
  C_WriteCommit *commit = new C_WriteCommit(this, o->oloc.pool,
                                            o->get_soid(), 0, 0);
  commit->tid = 
    lockack->tid = 
    o->last_write_tid = objecter->lock(o->get_oid(), o->get_oloc(), op, 0, lockack, commit);
}


// -------------------------------------------------


bool ObjectCacher::set_is_cached(ObjectSet *oset)
{
  if (oset->objects.empty())
    return false;
  
  for (xlist<Object*>::iterator p = oset->objects.begin();
       !p.end(); ++p) {
    Object *ob = *p;
    for (map<loff_t,BufferHead*>::iterator q = ob->data.begin();
         q != ob->data.end();
         q++) {
      BufferHead *bh = q->second;
      if (!bh->is_dirty() && !bh->is_tx()) 
        return true;
    }
  }

  return false;
}

bool ObjectCacher::set_is_dirty_or_committing(ObjectSet *oset)
{
  if (oset->objects.empty())
    return false;
  
  for (xlist<Object*>::iterator i = oset->objects.begin();
       !i.end(); ++i) {
    Object *ob = *i;
    
    for (map<loff_t,BufferHead*>::iterator p = ob->data.begin();
         p != ob->data.end();
         p++) {
      BufferHead *bh = p->second;
      if (bh->is_dirty() || bh->is_tx()) 
        return true;
    }
  }  
  
  return false;
}


// purge.  non-blocking.  violently removes dirty buffers from cache.
void ObjectCacher::purge(Object *ob)
{
  ldout(cct, 10) << "purge " << *ob << dendl;

  ob->truncate(0);

  if (ob->can_close()) {
    ldout(cct, 10) << "purge closing " << *ob << dendl;
    close_object(ob);
  }
}


// flush.  non-blocking.  no callback.
// true if clean, already flushed.  
// false if we wrote something.
bool ObjectCacher::flush(Object *ob)
{
  bool clean = true;
  for (map<loff_t,BufferHead*>::iterator p = ob->data.begin();
       p != ob->data.end();
       p++) {
    BufferHead *bh = p->second;
    if (bh->is_tx()) {
      clean = false;
      continue;
    }
    if (!bh->is_dirty()) continue;
    
    bh_write(bh);
    clean = false;
  }
  return clean;
}

// flush.  non-blocking, takes callback.
// returns true if already flushed
bool ObjectCacher::flush_set(ObjectSet *oset, Context *onfinish)
{
  if (oset->objects.empty()) {
    ldout(cct, 10) << "flush_set on " << oset << " dne" << dendl;
    return true;
  }

  ldout(cct, 10) << "flush_set " << oset << dendl;

  // we'll need to wait for all objects to flush!
  C_GatherBuilder gather(cct, onfinish);

  bool safe = true;
  for (xlist<Object*>::iterator i = oset->objects.begin();
       !i.end(); ++i) {
    Object *ob = *i;

    if (!flush(ob)) {
      // we'll need to gather...
      safe = false;

      ldout(cct, 10) << "flush_set " << oset << " will wait for ack tid " 
               << ob->last_write_tid 
               << " on " << *ob
               << dendl;
      if (onfinish != NULL)
        ob->waitfor_commit[ob->last_write_tid].push_back(gather.new_sub());
    }
  }
  if (onfinish != NULL)
    gather.activate();
  
  if (safe) {
    ldout(cct, 10) << "flush_set " << oset << " has no dirty|tx bhs" << dendl;
    return true;
  }
  return false;
}


// commit.  non-blocking, takes callback.
// return true if already flushed.
bool ObjectCacher::commit_set(ObjectSet *oset, Context *onfinish)
{
  assert(onfinish);  // doesn't make any sense otherwise.

  if (oset->objects.empty()) {
    ldout(cct, 10) << "commit_set on " << oset << " dne" << dendl;
    return true;
  }

  ldout(cct, 10) << "commit_set " << oset << dendl;

  // make sure it's flushing.
  flush_set(oset);

  // we'll need to wait for all objects to commit
  C_GatherBuilder gather(cct, onfinish);

  bool safe = true;
  for (xlist<Object*>::iterator i = oset->objects.begin();
       !i.end(); ++i) {
    Object *ob = *i;
    
    if (ob->last_write_tid > ob->last_commit_tid) {
      ldout(cct, 10) << "commit_set " << oset << " " << *ob 
               << " will finish on commit tid " << ob->last_write_tid
               << dendl;
      safe = false;
      if (onfinish != NULL)
        ob->waitfor_commit[ob->last_write_tid].push_back(gather.new_sub());
    }
  }
  gather.activate();

  if (safe) {
    ldout(cct, 10) << "commit_set " << oset << " all committed" << dendl;
    return true;
  }
  return false;
}

void ObjectCacher::purge_set(ObjectSet *oset)
{
  if (oset->objects.empty()) {
    ldout(cct, 10) << "purge_set on " << oset << " dne" << dendl;
    return;
  }

  ldout(cct, 10) << "purge_set " << oset << dendl;

  for (xlist<Object*>::iterator i = oset->objects.begin();
       !i.end(); ++i) {
    Object *ob = *i;
	purge(ob);
  }
}


loff_t ObjectCacher::release(Object *ob)
{
  list<BufferHead*> clean;
  loff_t o_unclean = 0;

  for (map<loff_t,BufferHead*>::iterator p = ob->data.begin();
       p != ob->data.end();
       p++) {
    BufferHead *bh = p->second;
    if (bh->is_clean()) 
      clean.push_back(bh);
    else 
      o_unclean += bh->length();
  }

  for (list<BufferHead*>::iterator p = clean.begin();
       p != clean.end();
       p++) {
    bh_remove(ob, *p);
    delete *p;
  }

  if (ob->can_close()) {
    ldout(cct, 10) << "trim trimming " << *ob << dendl;
    close_object(ob);
    assert(o_unclean == 0);
    return 0;
  }

  return o_unclean;
}

loff_t ObjectCacher::release_set(ObjectSet *oset)
{
  // return # bytes not clean (and thus not released).
  loff_t unclean = 0;

  if (oset->objects.empty()) {
    ldout(cct, 10) << "release_set on " << oset << " dne" << dendl;
    return 0;
  }

  ldout(cct, 10) << "release_set " << oset << dendl;

  xlist<Object*>::iterator q;
  for (xlist<Object*>::iterator p = oset->objects.begin();
       !p.end(); ) {
    q = p;
    ++q;
    Object *ob = *p;

    loff_t o_unclean = release(ob);
    unclean += o_unclean;

    if (o_unclean) 
      ldout(cct, 10) << "release_set " << oset << " " << *ob 
               << " has " << o_unclean << " bytes left"
               << dendl;
    p = q;
  }

  if (unclean) {
    ldout(cct, 10) << "release_set " << oset
             << ", " << unclean << " bytes left" << dendl;
  }

  return unclean;
}


uint64_t ObjectCacher::release_all()
{
  ldout(cct, 10) << "release_all" << dendl;
  uint64_t unclean = 0;
  
  vector<hash_map<sobject_t, Object*> >::iterator i = objects.begin();
  while (i != objects.end()) {
    hash_map<sobject_t, Object*>::iterator p = i->begin();
    while (p != i->end()) {
      hash_map<sobject_t, Object*>::iterator n = p;
      n++;

      Object *ob = p->second;

      loff_t o_unclean = release(ob);
      unclean += o_unclean;

      if (o_unclean)
        ldout(cct, 10) << "release_all " << *ob
        << " has " << o_unclean << " bytes left"
        << dendl;
    p = n;
    }
    ++i;
  }

  if (unclean) {
    ldout(cct, 10) << "release_all unclean " << unclean << " bytes left" << dendl;
  }

  return unclean;
}



/**
 * Truncate an ObjectSet by removing the objects in exls from the in-memory oset.
 */
void ObjectCacher::truncate_set(ObjectSet *oset, vector<ObjectExtent>& exls)
{
  if (oset->objects.empty()) {
    ldout(cct, 10) << "truncate_set on " << oset << " dne" << dendl;
    return;
  }
  
  ldout(cct, 10) << "truncate_set " << oset << dendl;

  bool were_dirty = oset->dirty_or_tx > 0;

  for (vector<ObjectExtent>::iterator p = exls.begin();
       p != exls.end();
       ++p) {
    ObjectExtent &ex = *p;
    sobject_t soid(ex.oid, CEPH_NOSNAP);
    if (objects[oset->poolid].count(soid) == 0)
      continue;
    Object *ob = objects[oset->poolid][soid];
    
    // purge or truncate?
    if (ex.offset == 0) {
      ldout(cct, 10) << "truncate_set purging " << *ob << dendl;
      purge(ob);
    } else {
      // hrm, truncate object
      ldout(cct, 10) << "truncate_set truncating " << *ob << " at " << ex.offset << dendl;
      ob->truncate(ex.offset);
      
      if (ob->can_close()) {
	ldout(cct, 10) << "truncate_set trimming " << *ob << dendl;
	close_object(ob);
      }
    }
  }

  // did we truncate off dirty data?
  if (flush_set_callback &&
      were_dirty && oset->dirty_or_tx == 0)
    flush_set_callback(flush_set_callback_arg, oset);
}


void ObjectCacher::kick_sync_writers(ObjectSet *oset)
{
  if (oset->objects.empty()) {
    ldout(cct, 10) << "kick_sync_writers on " << oset << " dne" << dendl;
    return;
  }

  ldout(cct, 10) << "kick_sync_writers on " << oset << dendl;

  list<Context*> ls;

  for (xlist<Object*>::iterator i = oset->objects.begin();
       !i.end(); ++i) {
    Object *ob = *i;
    
    ls.splice(ls.begin(), ob->waitfor_wr);
  }

  finish_contexts(cct, ls);
}

void ObjectCacher::kick_sync_readers(ObjectSet *oset)
{
  if (oset->objects.empty()) {
    ldout(cct, 10) << "kick_sync_readers on " << oset << " dne" << dendl;
    return;
  }

  ldout(cct, 10) << "kick_sync_readers on " << oset << dendl;

  list<Context*> ls;

  for (xlist<Object*>::iterator i = oset->objects.begin();
       !i.end(); ++i) {
    Object *ob = *i;
    
    ls.splice(ls.begin(), ob->waitfor_rd);
  }

  finish_contexts(cct, ls);
}



void ObjectCacher::verify_stats() const
{
  ldout(cct, 10) << "verify_stats" << dendl;

  loff_t clean = 0, dirty = 0, rx = 0, tx = 0, missing = 0;
  for (vector<hash_map<sobject_t, Object*> >::const_iterator i = objects.begin();
      i != objects.end();
      ++i) {
    for (hash_map<sobject_t, Object*>::const_iterator p = i->begin();
        p != i->end();
        ++p) {
      Object *ob = p->second;
      for (map<loff_t, BufferHead*>::const_iterator q = ob->data.begin();
          q != ob->data.end();
          q++) {
        BufferHead *bh = q->second;
        switch (bh->get_state()) {
        case BufferHead::STATE_MISSING:
          missing += bh->length();
          break;
        case BufferHead::STATE_CLEAN:
          clean += bh->length();
          break;
        case BufferHead::STATE_DIRTY:
          dirty += bh->length();
          break;
        case BufferHead::STATE_TX:
          tx += bh->length();
          break;
        case BufferHead::STATE_RX:
          rx += bh->length();
          break;
        default:
          assert(0);
        }
      }
    }
  }

  ldout(cct, 10) << " clean " << clean
	   << " rx " << rx 
	   << " tx " << tx
	   << " dirty " << dirty
	   << " missing " << missing
	   << dendl;
  assert(clean == stat_clean);
  assert(rx == stat_rx);
  assert(tx == stat_tx);
  assert(dirty == stat_dirty);
  assert(missing == stat_missing);
}

