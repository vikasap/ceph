// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_RGW_COMMON_H
#define CEPH_RGW_COMMON_H

#include "common/ceph_crypto.h"
#include "common/debug.h"
#include "common/perf_counters.h"

#include "acconfig.h"

#include <errno.h>
#include <string.h>
#include <string>
#include <map>
#include "include/types.h"
#include "include/utime.h"

using namespace std;

namespace ceph {
  class Formatter;
}

using ceph::crypto::MD5;


#define RGW_ROOT_BUCKET ".rgw"

#define RGW_CONTROL_BUCKET ".rgw.control"

#define RGW_ATTR_PREFIX  "user.rgw."

#define RGW_ATTR_ACL		RGW_ATTR_PREFIX "acl"
#define RGW_ATTR_ETAG    	RGW_ATTR_PREFIX "etag"
#define RGW_ATTR_BUCKETS	RGW_ATTR_PREFIX "buckets"
#define RGW_ATTR_META_PREFIX	RGW_ATTR_PREFIX "x-amz-meta-"
#define RGW_ATTR_CONTENT_TYPE	RGW_ATTR_PREFIX "content_type"
#define RGW_ATTR_ID_TAG    	RGW_ATTR_PREFIX "idtag"
#define RGW_ATTR_SHADOW_OBJ    	RGW_ATTR_PREFIX "shadow_name"
#define RGW_ATTR_MANIFEST    	RGW_ATTR_PREFIX "manifest"

#define RGW_BUCKETS_OBJ_PREFIX ".buckets"

#define RGW_MAX_CHUNK_SIZE	(512*1024)
#define RGW_MAX_PENDING_CHUNKS  16
#define RGW_MAX_PUT_SIZE        (5ULL*1024*1024*1024)

#define RGW_FORMAT_XML          1
#define RGW_FORMAT_JSON         2

#define RGW_REST_SWIFT          0x1
#define RGW_REST_SWIFT_AUTH     0x2

#define RGW_SUSPENDED_USER_AUID (uint64_t)-2

#define CGI_PRINTF(state, format, ...) do { \
   int __ret = FCGX_FPrintF(state->fcgx->out, format, __VA_ARGS__); \
   if (state->header_ended) \
     state->bytes_sent += __ret; \
   int l = 32, n; \
   while (1) { \
     char __buf[l]; \
     n = snprintf(__buf, sizeof(__buf), format, __VA_ARGS__); \
     if (n != l) \
       dout(10) << "--> " << __buf << dendl; \
       break; \
     l *= 2; \
   } \
} while (0)

#define CGI_PutStr(state, buf, len) do { \
  FCGX_PutStr(buf, len, state->fcgx->out); \
  if (state->header_ended) \
    state->bytes_sent += len; \
} while (0)

#define CGI_GetStr(state, buf, buf_len, olen) do { \
  olen = FCGX_GetStr(buf, buf_len, state->fcgx->in); \
  state->bytes_received += olen; \
} while (0)

#define STATUS_CREATED           1900
#define STATUS_ACCEPTED          1901
#define STATUS_NO_CONTENT        1902
#define STATUS_PARTIAL_CONTENT   1903

#define ERR_INVALID_BUCKET_NAME  2000
#define ERR_INVALID_OBJECT_NAME  2001
#define ERR_NO_SUCH_BUCKET       2002
#define ERR_METHOD_NOT_ALLOWED   2003
#define ERR_INVALID_DIGEST       2004
#define ERR_BAD_DIGEST           2005
#define ERR_UNRESOLVABLE_EMAIL   2006
#define ERR_INVALID_PART         2007
#define ERR_INVALID_PART_ORDER   2008
#define ERR_NO_SUCH_UPLOAD       2009
#define ERR_REQUEST_TIMEOUT      2010
#define ERR_LENGTH_REQUIRED      2011
#define ERR_REQUEST_TIME_SKEWED  2012
#define ERR_BUCKET_EXISTS        2013
#define ERR_BAD_URL              2014
#define ERR_PRECONDITION_FAILED  2015
#define ERR_NOT_MODIFIED         2016
#define ERR_INVALID_UTF8         2017
#define ERR_UNPROCESSABLE_ENTITY 2018
#define ERR_TOO_LARGE            2019
#define ERR_USER_SUSPENDED       2100
#define ERR_INTERNAL_ERROR       2200

typedef void *RGWAccessHandle;


/* perf counter */

extern PerfCounters *perfcounter;

extern int rgw_perf_start(CephContext *cct);
extern void rgw_perf_stop(CephContext *cct);

enum {
  l_rgw_first = 15000,
  l_rgw_req,
  l_rgw_failed_req,

  l_rgw_get,
  l_rgw_get_b,
  l_rgw_get_lat,

  l_rgw_put,
  l_rgw_put_b,
  l_rgw_put_lat,

  l_rgw_qlen,
  l_rgw_qactive,

  l_rgw_cache_hit,
  l_rgw_cache_miss,

  l_rgw_last,
};


 /* size should be the required string size + 1 */
extern int gen_rand_base64(CephContext *cct, char *dest, int size);
extern int gen_rand_alphanumeric(CephContext *cct, char *dest, int size);
extern int gen_rand_alphanumeric_upper(CephContext *cct, char *dest, int size);

enum RGWIntentEvent {
  DEL_OBJ = 0,
  DEL_DIR = 1,
};

enum RGWObjCategory {
  RGW_OBJ_CATEGORY_NONE      = 0,
  RGW_OBJ_CATEGORY_MAIN      = 1,
  RGW_OBJ_CATEGORY_SHADOW    = 2,
  RGW_OBJ_CATEGORY_MULTIMETA = 3,
};

/** Store error returns for output at a different point in the program */
struct rgw_err {
  rgw_err();
  rgw_err(int http, const std::string &s3);
  void clear();
  bool is_clear() const;
  bool is_err() const;
  friend std::ostream& operator<<(std::ostream& oss, const rgw_err &err);

  int http_ret;
  int ret;
  std::string s3_code;
  std::string message;
};

/* Helper class used for XMLArgs parsing */
class NameVal
{
   string str;
   string name;
   string val;
 public:
    NameVal(string nv) : str(nv) {}

    int parse();

    string& get_name() { return name; }
    string& get_val() { return val; }
};

/** Stores the XML arguments associated with the HTTP request in req_state*/
class XMLArgs
{
  string str, empty_str;
  map<string, string> val_map;
  map<string, string> sub_resources;
 public:
  XMLArgs() {}
  XMLArgs(string s) : str(s) {}
  /** Set the arguments; as received */
  void set(string s) { val_map.clear(); sub_resources.clear(); str = s; }
  /** parse the received arguments */
  int parse();
  /** Get the value for a specific argument parameter */
  string& get(string& name);
  string& get(const char *name);
  /** see if a parameter is contained in this XMLArgs */
  bool exists(const char *name) {
    map<string, string>::iterator iter = val_map.find(name);
    return (iter != val_map.end());
  }
  bool sub_resource_exists(const char *name) {
    map<string, string>::iterator iter = sub_resources.find(name);
    return (iter != sub_resources.end());
  }
  map<string, string>& get_sub_resources() { return sub_resources; }
};

class RGWConf;

class RGWEnv {
  std::map<string, string> env_map;
public:
  RGWConf *conf; 

  RGWEnv();
  ~RGWEnv();
  void init(CephContext *cct, char **envp);
  const char *get(const char *name, const char *def_val = NULL);
  int get_int(const char *name, int def_val = 0);
  bool get_bool(const char *name, bool def_val = 0);
  size_t get_size(const char *name, size_t def_val = 0);
};

class RGWConf {
  friend class RGWEnv;
protected:
  void init(CephContext *cct, RGWEnv * env);
public:
  RGWConf() :
    should_log(1) {}

  int should_log;
};

enum http_op {
  OP_GET,
  OP_PUT,
  OP_DELETE,
  OP_HEAD,
  OP_POST,
  OP_COPY,
  OP_UNKNOWN,
};

class RGWAccessControlPolicy;

struct RGWAccessKey {
  string id;
  string key;
  string subuser;

  RGWAccessKey() {}
  void encode(bufferlist& bl) const {
    ENCODE_START(2, 2, bl);
    ::encode(id, bl);
    ::encode(key, bl);
    ::encode(subuser, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START_LEGACY_COMPAT_LEN_32(2, 2, 2, bl);
     ::decode(id, bl);
     ::decode(key, bl);
     ::decode(subuser, bl);
     DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWAccessKey*>& o);
};
WRITE_CLASS_ENCODER(RGWAccessKey);

struct RGWSubUser {
  string name;
  uint32_t perm_mask;

  RGWSubUser() : perm_mask(0) {}
  void encode(bufferlist& bl) const {
    ENCODE_START(2, 2, bl);
    ::encode(name, bl);
    ::encode(perm_mask, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START_LEGACY_COMPAT_LEN_32(2, 2, 2, bl);
     ::decode(name, bl);
     ::decode(perm_mask, bl);
     DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWSubUser*>& o);
};
WRITE_CLASS_ENCODER(RGWSubUser);


struct RGWUserInfo
{
  uint64_t auid;
  string user_id;
  string display_name;
  string user_email;
  map<string, RGWAccessKey> access_keys;
  map<string, RGWAccessKey> swift_keys;
  map<string, RGWSubUser> subusers;
  __u8 suspended;

  RGWUserInfo() : auid(0), suspended(0) {}

  void encode(bufferlist& bl) const {
     ENCODE_START(9, 9, bl);
     ::encode(auid, bl);
     string access_key;
     string secret_key;
     if (!access_keys.empty()) {
       map<string, RGWAccessKey>::const_iterator iter = access_keys.begin();
       const RGWAccessKey& k = iter->second;
       access_key = k.id;
       secret_key = k.key;
     }
     ::encode(access_key, bl);
     ::encode(secret_key, bl);
     ::encode(display_name, bl);
     ::encode(user_email, bl);
     string swift_name;
     string swift_key;
     if (!swift_keys.empty()) {
       map<string, RGWAccessKey>::const_iterator iter = swift_keys.begin();
       const RGWAccessKey& k = iter->second;
       swift_name = k.id;
       swift_key = k.key;
     }
     ::encode(swift_name, bl);
     ::encode(swift_key, bl);
     ::encode(user_id, bl);
     ::encode(access_keys, bl);
     ::encode(subusers, bl);
     ::encode(suspended, bl);
     ::encode(swift_keys, bl);
     ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
     DECODE_START_LEGACY_COMPAT_LEN_32(9, 9, 9, bl);
     if (struct_v >= 2) ::decode(auid, bl);
     else auid = CEPH_AUTH_UID_DEFAULT;
     string access_key;
     string secret_key;
    ::decode(access_key, bl);
    ::decode(secret_key, bl);
    if (struct_v < 6) {
      RGWAccessKey k;
      k.id = access_key;
      k.key = secret_key;
      access_keys[access_key] = k;
    }
    ::decode(display_name, bl);
    ::decode(user_email, bl);
    string swift_name;
    string swift_key;
    if (struct_v >= 3) ::decode(swift_name, bl);
    if (struct_v >= 4) ::decode(swift_key, bl);
    if (struct_v >= 5)
      ::decode(user_id, bl);
    else
      user_id = access_key;
    if (struct_v >= 6) {
      ::decode(access_keys, bl);
      ::decode(subusers, bl);
    }
    suspended = 0;
    if (struct_v >= 7) {
      ::decode(suspended, bl);
    }
    if (struct_v >= 8) {
      ::decode(swift_keys, bl);
    }
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWUserInfo*>& o);

  void clear() {
    user_id.clear();
    display_name.clear();
    user_email.clear();
    auid = CEPH_AUTH_UID_DEFAULT;
    access_keys.clear();
    suspended = 0;
  }
};
WRITE_CLASS_ENCODER(RGWUserInfo)

struct rgw_bucket {
  std::string name;
  std::string pool;
  std::string marker;
  std::string bucket_id;

  rgw_bucket() { }
  rgw_bucket(const char *n) : name(n) {
    assert(*n == '.'); // only rgw private buckets should be initialized without pool
    pool = n;
    marker = "";
  }
  rgw_bucket(const char *n, const char *p, const char *m, const char *id) :
    name(n), pool(p), marker(m), bucket_id(id) {}

  void clear() {
    name = "";
    pool = "";
    marker = "";
    bucket_id = "";
  }

  void encode(bufferlist& bl) const {
     ENCODE_START(4, 3, bl);
    ::encode(name, bl);
    ::encode(pool, bl);
    ::encode(marker, bl);
    ::encode(bucket_id, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(4, 3, 3, bl);
    ::decode(name, bl);
    ::decode(pool, bl);
    if (struct_v >= 2) {
      ::decode(marker, bl);
      if (struct_v <= 3) {
        uint64_t id;
        ::decode(id, bl);
        char buf[16];
        snprintf(buf, sizeof(buf), "%llu", (long long)id);
        bucket_id = buf;
      } else {
        ::decode(bucket_id, bl);
      }
    }
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<rgw_bucket*>& o);
};
WRITE_CLASS_ENCODER(rgw_bucket)

inline ostream& operator<<(ostream& out, const rgw_bucket b) {
  out << b.name;
  if (b.name.compare(b.pool))
    out << "(@" << b.pool << "[" << b.marker << "])";
  return out;
}

extern rgw_bucket rgw_root_bucket;

enum RGWBucketFlags {
  BUCKET_SUSPENDED = 0x1,
};

struct RGWBucketInfo
{
  rgw_bucket bucket;
  string owner;
  uint32_t flags;

  void encode(bufferlist& bl) const {
     ENCODE_START(4, 4, bl);
     ::encode(bucket, bl);
     ::encode(owner, bl);
     ::encode(flags, bl);
     ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN_32(4, 4, 4, bl);
     ::decode(bucket, bl);
     if (struct_v >= 2)
       ::decode(owner, bl);
     if (struct_v >= 3)
       ::decode(flags, bl);
     DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWBucketInfo*>& o);

  RGWBucketInfo() : flags(0) {}
};
WRITE_CLASS_ENCODER(RGWBucketInfo)

struct RGWBucketStats
{
  RGWObjCategory category;
  uint64_t num_kb;
  uint64_t num_kb_rounded;
  uint64_t num_objects;
};

struct req_state;

struct RGWEnv;
struct FCGX_Request;

/** Store all the state necessary to complete and respond to an HTTP request*/
struct req_state {
   CephContext *cct;
   FCGX_Request *fcgx;
   http_op op;
   bool content_started;
   int format;
   ceph::Formatter *formatter;
   string decoded_uri;
   string request_uri;
   string request_params;
   const char *host;
   const char *method;
   const char *length;
   uint64_t content_length;
   const char *content_type;
   struct rgw_err err;
   bool expect_cont;
   bool header_ended;
   uint64_t bytes_sent; // bytes sent as a response, excluding header
   uint64_t bytes_received; // data received
   uint64_t obj_size;
   bool should_log;
   uint32_t perm_mask;
   utime_t header_time;

   XMLArgs args;

   const char *bucket_name;
   const char *object;

   const char *host_bucket;

   rgw_bucket bucket;
   string bucket_name_str;
   string object_str;
   string bucket_owner;

   map<string, string> x_meta_map;
   bool has_bad_meta;

   RGWUserInfo user; 
   RGWAccessControlPolicy *bucket_acl;
   RGWAccessControlPolicy *object_acl;

   string canned_acl;
   const char *copy_source;
   const char *http_auth;

   int prot_flags;

   const char *os_auth_token;
   char *os_user;
   char *os_groups;

   utime_t time;

   struct RGWEnv *env;

   void *obj_ctx;

   string dialect;

   req_state(CephContext *_cct, struct RGWEnv *e);
   ~req_state();
};

/** Store basic data on an object */
struct RGWObjEnt {
  std::string name;
  std::string owner;
  std::string owner_display_name;
  uint64_t size;
  time_t mtime;
  string etag;
  string content_type;

  void clear() { // not clearing etag
    name="";
    size = 0;
    mtime = 0;
    content_type="";
  }
};

/** Store basic data on bucket */
struct RGWBucketEnt {
  rgw_bucket bucket;
  size_t size;
  size_t size_rounded;
  time_t mtime;
  uint64_t count;

  RGWBucketEnt() : size(0), size_rounded(0), mtime(0), count(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(5, 5, bl);
    uint64_t s = size;
    __u32 mt = mtime;
    string empty_str;  // originally had the bucket name here, but we encode bucket later
    ::encode(empty_str, bl);
    ::encode(s, bl);
    ::encode(mt, bl);
    ::encode(count, bl);
    ::encode(bucket, bl);
    s = size_rounded;
    ::encode(s, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(5, 5, 5, bl);
    __u32 mt;
    uint64_t s;
    string empty_str;  // backward compatibility
    ::decode(empty_str, bl);
    ::decode(s, bl);
    ::decode(mt, bl);
    size = s;
    mtime = mt;
    if (struct_v >= 2)
      ::decode(count, bl);
    if (struct_v >= 3)
      ::decode(bucket, bl);
    if (struct_v >= 4)
      ::decode(s, bl);
    size_rounded = s;
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWBucketEnt*>& o);
  void clear() {
    bucket.clear();
    size = 0;
    size_rounded = 0;
    mtime = 0;
    count = 0;
  }
};
WRITE_CLASS_ENCODER(RGWBucketEnt)

struct RGWUploadPartInfo {
  uint32_t num;
  uint64_t size;
  string etag;
  utime_t modified;

  RGWUploadPartInfo() : num(0), size(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 2, bl);
    ::encode(num, bl);
    ::encode(size, bl);
    ::encode(etag, bl);
    ::encode(modified, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
    ::decode(num, bl);
    ::decode(size, bl);
    ::decode(etag, bl);
    ::decode(modified, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<RGWUploadPartInfo*>& o);
};
WRITE_CLASS_ENCODER(RGWUploadPartInfo)

class rgw_obj {
  std::string orig_obj;
  std::string orig_key;
public:
  rgw_bucket bucket;
  std::string key;
  std::string ns;
  std::string object;

  rgw_obj() {}
  rgw_obj(const char *b, const char *o) {
    rgw_bucket _b(b);
    std::string _o(o);
    init(_b, _o);
  }
  rgw_obj(rgw_bucket& b, const char *o) {
    std::string _o(o);
    init(b, _o);
  }
  rgw_obj(rgw_bucket& b, std::string& o) {
    init(b, o);
  }
  rgw_obj(rgw_bucket& b, std::string& o, std::string& k) {
    init(b, o, k);
  }
  rgw_obj(rgw_bucket& b, std::string& o, std::string& k, std::string& n) {
    init(b, o, k, n);
  }
  void init(rgw_bucket& b, std::string& o, std::string& k, std::string& n) {
    bucket = b;
    set_ns(n);
    set_obj(o);
    set_key(k);
  }
  void init(rgw_bucket& b, std::string& o, std::string& k) {
    bucket = b;
    set_obj(o);
    set_key(k);
  }
  void init(rgw_bucket& b, std::string& o) {
    bucket = b;
    set_obj(o);
    orig_key = key = o;
  }
  void init_ns(rgw_bucket& b, std::string& o, std::string& n) {
    bucket = b;
    set_ns(n);
    set_obj(o);
    reset_key();
  }
  int set_ns(const char *n) {
    if (!n)
      return -EINVAL;
    string ns_str(n);
    return set_ns(ns_str);
  }
  int set_ns(string& n) {
    if (n[0] == '_')
      return -EINVAL;
    ns = n;
    set_obj(orig_obj);
    return 0;
  }

  void set_key(string& k) {
    orig_key = k;
    key = k;
  }

  void reset_key() {
    orig_key.clear();
    key.clear();
  }

  void set_obj(string& o) {
    orig_obj = o;
    if (ns.empty()) {
      if (o.empty())
        return;
      if (o.size() < 1 || o[0] != '_') {
        object = o;
        return;
      }
      object = "_";
      object.append(o);
    } else {
      object = "_";
      object.append(ns);
      object.append("_");
      object.append(o);
    }
    if (orig_key.size())
      set_key(orig_key);
    else
      set_key(orig_obj);
  }

  string loc() {
    if (orig_key.empty())
      return orig_obj;
    else
      return orig_key;
  }

  /**
   * Translate a namespace-mangled object name to the user-facing name
   * existing in the given namespace.
   *
   * If the object is part of the given namespace, it returns true
   * and cuts down the name to the unmangled version. If it is not
   * part of the given namespace, it returns false.
   */
  static bool translate_raw_obj_to_obj_in_ns(string& obj, string& ns) {
    if (ns.empty()) {
      if (obj[0] != '_')
        return true;

      if (obj.size() >= 2 && obj[1] == '_') {
        obj = obj.substr(1);
        return true;
      }

      return false;
    }

    if (obj[0] != '_' || obj.size() < 3) // for namespace, min size would be 3: _x_
      return false;

    int pos = obj.find('_', 1);
    if (pos <= 1) // if it starts with __, it's not in our namespace
      return false;

    string obj_ns = obj.substr(1, pos - 1);
    if (obj_ns.compare(ns) != 0)
        return false;

    obj = obj.substr(pos + 1);
    return true;
  }

  /**
   * Given a mangled object name and an empty namespace string, this
   * function extracts the namespace into the string and sets the object
   * name to be the unmangled version.
   *
   * It returns true after successfully doing so, or
   * false if it fails.
   */
  static bool strip_namespace_from_object(string& obj, string& ns) {
    ns.clear();
    if (obj[0] != '_') {
      return true;
    }

    size_t pos = obj.find('_', 1);
    if (pos == string::npos) {
      return false;
    }

    size_t period_pos = obj.find('.');
    if (period_pos < pos) {
      return false;
    }

    ns = obj.substr(1, pos-1);
    obj = obj.substr(pos+1, string::npos);
    return true;
  }

  void encode(bufferlist& bl) const {
    ENCODE_START(3, 3, bl);
    ::encode(bucket.name, bl);
    ::encode(key, bl);
    ::encode(ns, bl);
    ::encode(object, bl);
    ::encode(bucket, bl);
    ENCODE_FINISH(bl);
  }
  void decode(bufferlist::iterator& bl) {
    DECODE_START_LEGACY_COMPAT_LEN(3, 3, 3, bl);
    ::decode(bucket.name, bl);
    ::decode(key, bl);
    ::decode(ns, bl);
    ::decode(object, bl);
    if (struct_v >= 2)
      ::decode(bucket, bl);
    DECODE_FINISH(bl);
  }
  void dump(Formatter *f) const;
  static void generate_test_instances(list<rgw_obj*>& o);

  bool operator==(const rgw_obj& o) const {
    return (object.compare(o.object) == 0) &&
           (bucket.name.compare(o.bucket.name) == 0) &&
           (ns.compare(o.ns) == 0);
  }
  bool operator<(const rgw_obj& o) const {
    int r = bucket.name.compare(o.bucket.name);
    if (r == 0) {
     r = object.compare(o.object);
     if (r == 0) {
       r = ns.compare(o.ns);
     }
    }

    return (r < 0);
  }
};
WRITE_CLASS_ENCODER(rgw_obj)

inline ostream& operator<<(ostream& out, const rgw_obj o) {
  return out << o.bucket.name << ":" << o.object;
}

static inline void buf_to_hex(const unsigned char *buf, int len, char *str)
{
  int i;
  str[0] = '\0';
  for (i = 0; i < len; i++) {
    sprintf(&str[i*2], "%02x", (int)buf[i]);
  }
}

static inline int hexdigit(char c)
{
  if (c >= '0' && c <= '9')
    return (c - '0');
  c = toupper(c);
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 0xa;
  return -EINVAL;
}

static inline int hex_to_buf(const char *hex, char *buf, int len)
{
  int i = 0;
  const char *p = hex;
  while (*p) {
    if (i >= len)
      return -EINVAL;
    buf[i] = 0;
    int d = hexdigit(*p);
    if (d < 0)
      return d;
    buf[i] = d << 4;
    p++;
    if (!*p)
      return -EINVAL;
    d = hexdigit(*p);
    if (d < 0)
      return -d;
    buf[i] += d;
    i++;
    p++;
  }
  return i;
}

static inline int rgw_str_to_bool(const char *s, int def_val)
{
  if (!s)
    return def_val;

  return (strcasecmp(s, "on") == 0 ||
          strcasecmp(s, "yes") == 0 ||
          strcasecmp(s, "1") == 0);
}

static inline void append_rand_alpha(CephContext *cct, string& src, string& dest, int len)
{
  dest = src;
  char buf[len + 1];
  gen_rand_alphanumeric(cct, buf, len);
  dest.append("_");
  dest.append(buf);
}

static inline const char *rgw_obj_category_name(RGWObjCategory category)
{
  switch (category) {
  case RGW_OBJ_CATEGORY_NONE:
    return "rgw.none";
  case RGW_OBJ_CATEGORY_MAIN:
    return "rgw.main";
  case RGW_OBJ_CATEGORY_SHADOW:
    return "rgw.shadow";
  case RGW_OBJ_CATEGORY_MULTIMETA:
    return "rgw.multimeta";
  }

  return "unknown";
}

/** time parsing */
extern int parse_time(const char *time_str, time_t *time);
extern bool parse_rfc2616(const char *s, struct tm *t);

/** Check if the req_state's user has the necessary permissions
 * to do the requested action */
extern bool verify_bucket_permission(struct req_state *s, int perm);
extern bool verify_object_permission(struct req_state *s, int perm);
/** Convert an input URL into a sane object name
 * by converting %-escaped strings into characters, etc*/
extern bool url_decode(string& src_str, string& dest_str);

extern void calc_hmac_sha1(const char *key, int key_len,
                          const char *msg, int msg_len, char *dest);
/* destination should be CEPH_CRYPTO_HMACSHA1_DIGESTSIZE bytes long */

#endif
