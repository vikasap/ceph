#include "mds/mdstypes.h"
#include "include/buffer.h"
#include "include/rados/librados.h"
#include "include/rados/librados.hpp"
#include "test/rados-api/test.h"

#include "gtest/gtest.h"
#include <errno.h>
#include <map>
#include <sstream>
#include <string>

using namespace librados;
using ceph::buffer;
using std::map;
using std::ostringstream;
using std::string;

TEST(LibRadosMisc, Version) {
  int major, minor, extra;
  rados_version(&major, &minor, &extra);
}

TEST(LibRadosMisc, VersionPP) {
  int major, minor, extra;
  Rados::version(&major, &minor, &extra);
}

static std::string read_key_from_tmap(IoCtx& ioctx, const std::string &obj,
				      const std::string &key)
{
  bufferlist bl;
  int r = ioctx.read(obj, bl, 0, 0);
  if (r <= 0) {
    ostringstream oss;
    oss << "ioctx.read(" << obj << ", bl, 0, 0) returned " << r;
    return oss.str();
  }
  bufferlist::iterator p = bl.begin();
  bufferlist header;
  map<string, bufferlist> m;
  ::decode(header, p);
  ::decode(m, p);
  map<string, bufferlist>::iterator i = m.find(key);
  if (i == m.end())
    return "";
  std::string retstring;
  ::decode(retstring, i->second);
  return retstring;
}

static std::string add_key_to_tmap(IoCtx &ioctx, const std::string &obj,
	  const std::string &key, const std::string &val)
{
  __u8 c = CEPH_OSD_TMAP_SET;

  bufferlist tmbl;
  ::encode(c, tmbl);
  ::encode(key, tmbl);
  bufferlist blbl;
  ::encode(val, blbl);
  ::encode(blbl, tmbl);
  int ret = ioctx.tmap_update(obj, tmbl);
  if (ret) {
    ostringstream oss;
    oss << "ioctx.tmap_update(obj=" << obj << ", key="
	<< key << ", val=" << val << ") failed with error " << ret;
    return oss.str();
  }
  return "";
}

static int remove_key_from_tmap(IoCtx &ioctx, const std::string &obj,
					const std::string &key)
{
  __u8 c = CEPH_OSD_TMAP_RM;

  bufferlist tmbl;
  ::encode(c, tmbl);
  ::encode(key, tmbl);
  int ret = ioctx.tmap_update(obj, tmbl);
  if (ret) {
    ostringstream oss;
    oss << "ioctx.tmap_update(obj=" << obj << ", key="
	<< key << ") failed with error " << ret;
  }
  return ret;
}

TEST(LibRadosMisc, TmapUpdatePP) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  cluster.ioctx_create(pool_name.c_str(), ioctx);

  // create tmap
  {
    __u8 c = CEPH_OSD_TMAP_CREATE;
    std::string my_tmap("my_tmap");
    bufferlist emptybl;

    bufferlist tmbl;
    ::encode(c, tmbl);
    ::encode(my_tmap, tmbl);
    ::encode(emptybl, tmbl);
    ASSERT_EQ(0, ioctx.tmap_update("foo", tmbl));
  }

  ASSERT_EQ(string(""), add_key_to_tmap(ioctx, "foo", "key1", "val1"));

  ASSERT_EQ(string(""), add_key_to_tmap(ioctx, "foo", "key2", "val2"));

  // read key1 from the tmap
  ASSERT_EQ(string("val1"), read_key_from_tmap(ioctx, "foo", "key1"));

  // remove key1 from tmap
  ASSERT_EQ(0, remove_key_from_tmap(ioctx, "foo", "key1"));
  ASSERT_EQ(-ENOENT, remove_key_from_tmap(ioctx, "foo", "key1"));

  // key should be removed
  ASSERT_EQ(string(""), read_key_from_tmap(ioctx, "foo", "key1"));

  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

TEST(LibRadosMisc, Exec) {
  char buf[128];
  rados_t cluster;
  rados_ioctx_t ioctx;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool(pool_name, &cluster));
  rados_ioctx_create(cluster, pool_name.c_str(), &ioctx);
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ((int)sizeof(buf), rados_write(ioctx, "foo", buf, sizeof(buf), 0));
  char buf2[512];
  int res = rados_exec(ioctx, "foo", "rbd", "test_exec",
			  NULL, 0, buf2, sizeof(buf2));
  ASSERT_GT(res, 0);
  bufferlist bl;
  bl.append(buf2, res);
  bufferlist::iterator iter = bl.begin();
  std::string outstring;
  ::decode(outstring, iter);
  ASSERT_EQ(outstring, string("testing123"));
  rados_ioctx_destroy(ioctx);
  ASSERT_EQ(0, destroy_one_pool(pool_name, &cluster));
}

TEST(LibRadosMisc, ExecPP) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  cluster.ioctx_create(pool_name.c_str(), ioctx);
  bufferlist bl;
  ASSERT_EQ(0, ioctx.write("foo", bl, 0, 0));
  bufferlist bl2, out;
  int r = ioctx.exec("foo", "rbd", "test_exec", bl2, out);
  ASSERT_EQ((int)out.length(), r);
  bufferlist::iterator iter = out.begin();
  std::string outstring;
  ::decode(outstring, iter);
  ASSERT_EQ(outstring, string("testing123"));
  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

TEST(LibRadosMisc, Operate1PP) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  cluster.ioctx_create(pool_name.c_str(), ioctx);

  ObjectWriteOperation o;
  {
    bufferlist bl;
    o.write(0, bl);
  }
  std::string val1("val1");
  {
    bufferlist bl;
    bl.append(val1.c_str(), val1.size() + 1);
    o.setxattr("key1", bl);
  }
  ASSERT_EQ(0, ioctx.operate("foo", &o));
  {
    bufferlist bl;
    ASSERT_GT(ioctx.getxattr("foo", "key1", bl), 0);
    ASSERT_EQ(0, strcmp(bl.c_str(), val1.c_str()));
  }
  ObjectWriteOperation o2;
  {
    bufferlist bl;
    bl.append(val1);
    o2.cmpxattr("key1", CEPH_OSD_CMPXATTR_OP_EQ, bl);
    o2.rmxattr("key1");
  }
  ASSERT_EQ(0, ioctx.operate("foo", &o2));
  ObjectWriteOperation o3;
  {
    bufferlist bl;
    bl.append(val1);
    o3.cmpxattr("key1", CEPH_OSD_CMPXATTR_OP_EQ, bl);
  }
  ASSERT_LT(ioctx.operate("foo", &o3), 0);
  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

TEST(LibRadosMisc, Operate2PP) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  cluster.ioctx_create(pool_name.c_str(), ioctx);

  ObjectWriteOperation o;
  {
    bufferlist bl;
    bl.append("abcdefg");
    o.write(0, bl);
  }
  std::string val1("val1");
  {
    bufferlist bl;
    bl.append(val1.c_str(), val1.size() + 1);
    o.setxattr("key1", bl);
    o.truncate(0);
  }
  ASSERT_EQ(0, ioctx.operate("foo", &o));
  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat("foo", &size, &mtime));
  ASSERT_EQ(0U, size);
  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

void set_completion_complete(rados_completion_t cb, void *arg)
{
  bool *my_aio_complete = (bool*)arg;
  *my_aio_complete = true;
}

TEST(LibRadosMisc, AioOperatePP) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  cluster.ioctx_create(pool_name.c_str(), ioctx);

  bool my_aio_complete = false;
  AioCompletion *my_completion = cluster.aio_create_completion(
	  (void*)&my_aio_complete, set_completion_complete, NULL);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);

  ObjectWriteOperation o;
  {
    bufferlist bl;
    o.write(0, bl);
  }
  std::string val1("val1");
  {
    bufferlist bl;
    bl.append(val1.c_str(), val1.size() + 1);
    o.setxattr("key1", bl);
    bufferlist bl2;
    char buf2[1024];
    memset(buf2, 0xdd, sizeof(buf2));
    bl2.append(buf2, sizeof(buf2));
    o.append(bl2);
  }
  ASSERT_EQ(0, ioctx.aio_operate("foo", my_completion, &o));
  ASSERT_EQ(0, my_completion->wait_for_complete());
  ASSERT_EQ(my_aio_complete, true);

  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat("foo", &size, &mtime));
  ASSERT_EQ(1024U, size);
  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

TEST(LibRadosMisc, CloneRangePP) {
  Rados cluster;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool_pp(pool_name, cluster));
  IoCtx ioctx;
  ASSERT_EQ(0, cluster.ioctx_create(pool_name.c_str(), ioctx));
  char buf[64];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl;
  bl.append(buf, sizeof(buf));
  ASSERT_EQ(sizeof(buf), (size_t)ioctx.write("foo", bl, sizeof(buf), 0));
  ioctx.locator_set_key("foo");
  ASSERT_EQ(0, ioctx.clone_range("bar", 0, "foo", 0, sizeof(buf)));
  bufferlist bl2;
  ASSERT_EQ(sizeof(buf), (size_t)ioctx.read("bar", bl2, sizeof(buf), 0));
  ASSERT_EQ(0, memcmp(buf, bl2.c_str(), sizeof(buf)));
  ioctx.close();
  ASSERT_EQ(0, destroy_one_pool_pp(pool_name, cluster));
}

TEST(LibRadosMisc, CloneRange) {
  char buf[128];
  rados_t cluster;
  rados_ioctx_t ioctx;
  std::string pool_name = get_temp_pool_name();
  ASSERT_EQ("", create_one_pool(pool_name, &cluster));
  rados_ioctx_create(cluster, pool_name.c_str(), &ioctx);
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ((int)sizeof(buf), rados_write(ioctx, "src", buf, sizeof(buf), 0));
  rados_ioctx_locator_set_key(ioctx, "src");
  ASSERT_EQ(0, rados_clone_range(ioctx, "dst", 0, "src", 0, sizeof(buf)));
  char buf2[sizeof(buf)];
  memset(buf2, 0, sizeof(buf2));
  ASSERT_EQ((int)sizeof(buf2), rados_read(ioctx, "dst", buf2, sizeof(buf2), 0));
  ASSERT_EQ(0, memcmp(buf, buf2, sizeof(buf)));
  rados_ioctx_destroy(ioctx);
  ASSERT_EQ(0, destroy_one_pool(pool_name, &cluster));
}
