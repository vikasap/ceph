#ifndef __LIBRADOS_HPP
#define __LIBRADOS_HPP

#include <stdbool.h>
#include <string>
#include <list>
#include <map>
#include <set>
#include <tr1/memory>
#include <vector>
#include <utility>
#include "buffer.h"

#include "librados.h"

namespace librados
{
  using ceph::bufferlist;

  class AioCompletionImpl;
  class IoCtx;
  class IoCtxImpl;
  class ObjectOperationImpl;
  class ObjListCtx;
  class PoolAsyncCompletionImpl;
  class RadosClient;

  typedef void *list_ctx_t;
  typedef uint64_t snap_t;
  typedef uint64_t auid_t;
  typedef void *config_t;

  struct cluster_stat_t {
    uint64_t kb, kb_used, kb_avail;
    uint64_t num_objects;
  };

  struct pool_stat_t {
    uint64_t num_bytes;    // in bytes
    uint64_t num_kb;       // in KB
    uint64_t num_objects;
    uint64_t num_object_clones;
    uint64_t num_object_copies;  // num_objects * num_replicas
    uint64_t num_objects_missing_on_primary;
    uint64_t num_objects_unfound;
    uint64_t num_objects_degraded;
    uint64_t num_rd, num_rd_kb, num_wr, num_wr_kb;
  };

  typedef std::map<std::string, pool_stat_t> stats_map;

  typedef void *completion_t;
  typedef void (*callback_t)(completion_t cb, void *arg);

  struct SnapContext {
    snap_t seq;
    std::vector<snap_t> snaps;
  };

  class ObjectIterator : public std::iterator <std::forward_iterator_tag, std::string> {
  public:
    static const ObjectIterator __EndObjectIterator;
    ObjectIterator() {}
    ObjectIterator(ObjListCtx *ctx_);
    ~ObjectIterator();
    bool operator==(const ObjectIterator& rhs) const;
    bool operator!=(const ObjectIterator& rhs) const;
    const std::pair<std::string, std::string>& operator*() const;
    const std::pair<std::string, std::string>* operator->() const;
    ObjectIterator &operator++(); // Preincrement
    ObjectIterator operator++(int); // Postincrement
    friend class IoCtx;
  private:
    void get_next();
    std::tr1::shared_ptr < ObjListCtx > ctx;
    std::pair<std::string, std::string> cur_obj;
  };

  class WatchCtx {
  public:
    virtual ~WatchCtx();
    virtual void notify(uint8_t opcode, uint64_t ver, bufferlist& bl) = 0;
  };

  struct AioCompletion {
    AioCompletion(AioCompletionImpl *pc_) : pc(pc_) {}
    int set_complete_callback(void *cb_arg, callback_t cb);
    int set_safe_callback(void *cb_arg, callback_t cb);
    int wait_for_complete();
    int wait_for_safe();
    bool is_complete();
    bool is_safe();
    int get_return_value();
    int get_version();
    void release();
    AioCompletionImpl *pc;
  };

  struct PoolAsyncCompletion {
    PoolAsyncCompletion(PoolAsyncCompletionImpl *pc_) : pc(pc_) {}
    int set_callback(void *cb_arg, callback_t cb);
    int wait();
    bool is_complete();
    int get_return_value();
    void release();
    PoolAsyncCompletionImpl *pc;
  };

  enum ObjectOperationFlags {
    OP_EXCL =   1,
    OP_FAILOK = 2,
  };

  /*
   * ObjectOperation : compount object operation
   * Batch multiple object operations into a single request, to be applied
   * atomically.
   */
  class ObjectOperation
  {
  public:
    ObjectOperation();
    virtual ~ObjectOperation();

    size_t size();
    void set_op_flags(ObjectOperationFlags flags);

    void cmpxattr(const char *name, uint8_t op, const bufferlist& val);
    void cmpxattr(const char *name, uint8_t op, uint64_t v);
    void src_cmpxattr(const std::string& src_oid,
		      const char *name, int op, const bufferlist& val);
    void src_cmpxattr(const std::string& src_oid,
		      const char *name, int op, uint64_t v);
    void exec(const char *cls, const char *method, bufferlist& inbl);

  protected:
    ObjectOperationImpl *impl;
    ObjectOperation(const ObjectOperation& rhs);
    ObjectOperation& operator=(const ObjectOperation& rhs);
    friend class IoCtx;
    friend class Rados;
  };

  /*
   * ObjectWriteOperation : compount object write operation
   * Batch multiple object operations into a single request, to be applied
   * atomically.
   */
  class ObjectWriteOperation : public ObjectOperation
  {
  protected:
    time_t *pmtime;
  public:
    ObjectWriteOperation() : pmtime(NULL) {}
    ~ObjectWriteOperation() {}

    void mtime(time_t *pt) {
      pmtime = pt;
    }

    void create(bool exclusive);
    void create(bool exclusive, const std::string& category);
    void write(uint64_t off, const bufferlist& bl);
    void write_full(const bufferlist& bl);
    void append(const bufferlist& bl);
    void remove();
    void truncate(uint64_t off);
    void zero(uint64_t off, uint64_t len);
    void rmxattr(const char *name);
    void setxattr(const char *name, const bufferlist& bl);
    void tmap_update(const bufferlist& cmdbl);
    void tmap_put(const bufferlist& bl);
    void clone_range(uint64_t dst_off,
                     const std::string& src_oid, uint64_t src_off,
                     size_t len);

    /**
     * set keys and values according to map
     *
     * @param map [in] keys and values to set
     */
    void omap_set(const std::map<std::string, bufferlist> &map);

    /**
     * set header
     *
     * @param bl [in] header to set
     */
    void omap_set_header(const bufferlist &bl);

    /**
     * Clears omap contents
     */
    void omap_clear();

    /**
     * Clears keys in to_rm
     *
     * @param to_rm [in] keys to remove
     */
    void omap_rm_keys(const std::set<std::string> &to_rm);

    friend class IoCtx;
  };

  /*
   * ObjectReadOperation : compount object operation that return value
   * Batch multiple object operations into a single request, to be applied
   * atomically.
   */
  class ObjectReadOperation : public ObjectOperation
  {
  public:
    ObjectReadOperation() {}
    ~ObjectReadOperation() {}

    void stat(uint64_t *psize, time_t *pmtime, int *prval);
    void getxattr(const char *name, bufferlist *pbl, int *prval);
    void getxattrs(std::map<std::string, bufferlist> *pattrs, int *prval);
    void read(size_t off, uint64_t len, bufferlist *pbl, int *prval);
    void tmap_get(bufferlist *pbl, int *prval);

    /**
     * omap_get_vals: keys and values from the object omap
     *
     * Get up to max_return keys and values beginning after start_after
     *
     * @param start_after [in] list no keys smaller than start_after
     * @parem max_return [in] list no more than max_return key/value pairs
     * @param out_vals [out] place returned values in out_vals on completion
     * @param prval [out] place error code in prval upon completion
     */
    void omap_get_vals(
      const std::string &start_after,
      uint64_t max_return,
      std::map<std::string, bufferlist> *out_vals,
      int *prval);

    /**
     * omap_get_vals: keys and values from the object omap
     *
     * Get up to max_return keys and values beginning after start_after
     *
     * @param start_after [in] list no keys smaller than start_after
     * @param filter_prefix [in] list only keys beginning with filter_prefix
     * @parem max_return [in] list no more than max_return key/value pairs
     * @param out_vals [out] place returned values in out_vals on completion
     * @param prval [out] place error code in prval upon completion
     */
    void omap_get_vals(
      const std::string &start_after,
      const std::string &filter_prefix,
      uint64_t max_return,
      std::map<std::string, bufferlist> *out_vals,
      int *prval);


    /**
     * omap_get_keys: keys from the object omap
     *
     * Get up to max_return keys beginning after start_after
     *
     * @param start_after [in] list no keys smaller than start_after
     * @parem max_return [in] list no more than max_return keys
     * @param out_keys [out] place returned values in out_keys on completion
     * @param prval [out] place error code in prval upon completion
     */
    void omap_get_keys(const std::string &start_after,
                       uint64_t max_return,
                       std::set<std::string> *out_keys,
                       int *prval);

    /**
     * omap_get_header: get header from object omap
     *
     * @param header [out] place header here upon completion
     * @param prval [out] place error code in prval upon completion
     */
    void omap_get_header(bufferlist *header, int *prval);

    /**
     * get key/value paris for specified keys
     *
     * @param to_get [in] keys to get
     * @param out_vals [out] place key/value pairs found here on completion
     * @param prval [out] place error code in prval upon completion
     */
    void omap_get_vals_by_keys(const std::set<std::string> &keys,
			       std::map<std::string, bufferlist> *map,
			       int *prval);
  };


  /* IoCtx : This is a context in which we can perform I/O.
   * It includes a Pool,
   *
   * Typical use (error checking omitted):
   *
   * IoCtx p;
   * rados.ioctx_create("my_pool", p);
   * p->stat(&stats);
   * ... etc ...
   */
  class IoCtx
  {
  public:
    IoCtx();
    static void from_rados_ioctx_t(rados_ioctx_t p, IoCtx &pool);
    IoCtx(const IoCtx& rhs);
    IoCtx& operator=(const IoCtx& rhs);

    ~IoCtx();

    // Close our pool handle
    void close();

    // deep copy
    void dup(const IoCtx& rhs);

    // set pool auid
    int set_auid(uint64_t auid_);

    // set pool auid
    int set_auid_async(uint64_t auid_, PoolAsyncCompletion *c);

    // get pool auid
    int get_auid(uint64_t *auid_);

    // create an object
    int create(const std::string& oid, bool exclusive);
    int create(const std::string& oid, bool exclusive, const std::string& category);

    int write(const std::string& oid, bufferlist& bl, size_t len, uint64_t off);
    int append(const std::string& oid, bufferlist& bl, size_t len);
    int write_full(const std::string& oid, bufferlist& bl);
    int clone_range(const std::string& dst_oid, uint64_t dst_off,
                   const std::string& src_oid, uint64_t src_off,
                   size_t len);
    int read(const std::string& oid, bufferlist& bl, size_t len, uint64_t off);
    int remove(const std::string& oid);
    int trunc(const std::string& oid, uint64_t size);
    int mapext(const std::string& o, uint64_t off, size_t len, std::map<uint64_t,uint64_t>& m);
    int sparse_read(const std::string& o, std::map<uint64_t,uint64_t>& m, bufferlist& bl, size_t len, uint64_t off);
    int getxattr(const std::string& oid, const char *name, bufferlist& bl);
    int getxattrs(const std::string& oid, std::map<std::string, bufferlist>& attrset);
    int setxattr(const std::string& oid, const char *name, bufferlist& bl);
    int rmxattr(const std::string& oid, const char *name);
    int stat(const std::string& oid, uint64_t *psize, time_t *pmtime);
    int exec(const std::string& oid, const char *cls, const char *method,
	     bufferlist& inbl, bufferlist& outbl);
    int tmap_update(const std::string& oid, bufferlist& cmdbl);
    int tmap_put(const std::string& oid, bufferlist& bl);
    int tmap_get(const std::string& oid, bufferlist& bl);

    int omap_get_vals(const std::string& oid,
                      const std::string& start_after,
                      uint64_t max_return,
                      std::map<std::string, bufferlist> *out_vals);
    int omap_get_vals(const std::string& oid,
                      const std::string& start_after,
                      const std::string& filter_prefix,
                      uint64_t max_return,
                      std::map<std::string, bufferlist> *out_vals);
    int omap_get_keys(const std::string& oid,
                      const std::string& start_after,
                      uint64_t max_return,
                      std::set<std::string> *out_keys);
    int omap_get_header(const std::string& oid,
                        bufferlist *bl);
    int omap_get_vals_by_keys(const std::string& oid,
                              const std::set<std::string>& keys,
                              std::map<std::string, bufferlist> *vals);
    int omap_set(const std::string& oid,
                 const std::map<std::string, bufferlist>& map);
    int omap_set_header(const std::string& oid,
                        const bufferlist& bl);
    int omap_clear(const std::string& oid);
    int omap_rm_keys(const std::string& oid,
                     const std::set<std::string>& keys);

    void snap_set_read(snap_t seq);
    int selfmanaged_snap_set_write_ctx(snap_t seq, std::vector<snap_t>& snaps);

    // Create a snapshot with a given name
    int snap_create(const char *snapname);

    // Look up a snapshot by name.
    // Returns 0 on success; error code otherwise
    int snap_lookup(const char *snapname, snap_t *snap);

    // Gets a timestamp for a snap
    int snap_get_stamp(snap_t snapid, time_t *t);

    // Gets the name of a snap
    int snap_get_name(snap_t snapid, std::string *s);

    // Remove a snapshot from this pool
    int snap_remove(const char *snapname);

    int snap_list(std::vector<snap_t> *snaps);

    int rollback(const std::string& oid, const char *snapname);

    int selfmanaged_snap_create(uint64_t *snapid);

    int selfmanaged_snap_remove(uint64_t snapid);

    int selfmanaged_snap_rollback(const std::string& oid, uint64_t snapid);

    ObjectIterator objects_begin();
    const ObjectIterator& objects_end() const;

    uint64_t get_last_version();

    int aio_read(const std::string& oid, AioCompletion *c,
		 bufferlist *pbl, size_t len, uint64_t off);
    int aio_sparse_read(const std::string& oid, AioCompletion *c,
			std::map<uint64_t,uint64_t> *m, bufferlist *data_bl,
			size_t len, uint64_t off);
    int aio_write(const std::string& oid, AioCompletion *c, const bufferlist& bl,
		  size_t len, uint64_t off);
    int aio_append(const std::string& oid, AioCompletion *c, const bufferlist& bl,
		  size_t len);
    int aio_write_full(const std::string& oid, AioCompletion *c, const bufferlist& bl);
    
    int aio_flush();

    int aio_exec(const std::string& oid, AioCompletion *c, const char *cls, const char *method,
	         bufferlist& inbl, bufferlist *outbl);

    // compound object operations
    int operate(const std::string& oid, ObjectWriteOperation *op);
    int operate(const std::string& oid, ObjectReadOperation *op, bufferlist *pbl);
    int aio_operate(const std::string& oid, AioCompletion *c, ObjectWriteOperation *op);
    int aio_operate(const std::string& oid, AioCompletion *c, ObjectReadOperation *op,
		    bufferlist *pbl);

    // watch/notify
    int watch(const std::string& o, uint64_t ver, uint64_t *handle,
	      librados::WatchCtx *ctx);
    int unwatch(const std::string& o, uint64_t handle);
    int notify(const std::string& o, uint64_t ver, bufferlist& bl);
    void set_notify_timeout(uint32_t timeout);

    // assert version for next sync operations
    void set_assert_version(uint64_t ver);
    void set_assert_src_version(const std::string& o, uint64_t ver);

    const std::string& get_pool_name() const;

    void locator_set_key(const std::string& key);

    int64_t get_id();

    config_t cct();

  private:
    /* You can only get IoCtx instances from Rados */
    IoCtx(IoCtxImpl *io_ctx_impl_);

    friend class Rados; // Only Rados can use our private constructor to create IoCtxes.

    IoCtxImpl *io_ctx_impl;
  };

  class Rados
  {
  public:
    static void version(int *major, int *minor, int *extra);

    Rados();
    ~Rados();

    int init(const char * const id);
    int init_with_context(config_t cct_);
    config_t cct();
    int connect();
    void shutdown();
    int conf_read_file(const char * const path) const;
    int conf_parse_argv(int argc, const char ** argv) const;
    int conf_parse_env(const char *env) const;
    int conf_set(const char *option, const char *value);
    int conf_get(const char *option, std::string &val);

    int pool_create(const char *name);
    int pool_create(const char *name, uint64_t auid);
    int pool_create(const char *name, uint64_t auid, __u8 crush_rule);
    int pool_create_async(const char *name, PoolAsyncCompletion *c);
    int pool_create_async(const char *name, uint64_t auid, PoolAsyncCompletion *c);
    int pool_create_async(const char *name, uint64_t auid, __u8 crush_rule, PoolAsyncCompletion *c);
    int pool_delete(const char *name);
    int pool_delete_async(const char *name, PoolAsyncCompletion *c);
    int64_t pool_lookup(const char *name);

    uint64_t get_instance_id();

    int ioctx_create(const char *name, IoCtx &pioctx);

    /* listing objects */
    int pool_list(std::list<std::string>& v);
    int get_pool_stats(std::list<std::string>& v,
		       std::map<std::string, stats_map>& stats);
    int get_pool_stats(std::list<std::string>& v,
                       std::string& category,
		       std::map<std::string, stats_map>& stats);
    int cluster_stat(cluster_stat_t& result);

    /* pool aio */
    static PoolAsyncCompletion *pool_async_create_completion();

   // -- aio --
    static AioCompletion *aio_create_completion();
    static AioCompletion *aio_create_completion(void *cb_arg, callback_t cb_complete,
						callback_t cb_safe);

    friend std::ostream& operator<<(std::ostream &oss, const Rados& r);
  private:
    // We don't allow assignment or copying
    Rados(const Rados& rhs);
    const Rados& operator=(const Rados& rhs);
    RadosClient *client;
  };
}

#endif

