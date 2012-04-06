// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "common/Thread.h"
#include "common/admin_socket.h"
#include "common/config.h"
#include "common/config_obs.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/perf_counters.h"
#include "common/pipe.h"
#include "common/safe_io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <map>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdint.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "include/compat.h"

#define dout_subsys ceph_subsys_

using std::ostringstream;

/*
 * UNIX domain sockets created by an application persist even after that
 * application closes, unless they're explicitly unlinked. This is because the
 * directory containing the socket keeps a reference to the socket.
 *
 * This code makes things a little nicer by unlinking those dead sockets when
 * the application exits normally.
 */
static pthread_mutex_t cleanup_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector <const char*> cleanup_files;
static bool cleanup_atexit = false;

static void remove_cleanup_file(const char *file)
{
  pthread_mutex_lock(&cleanup_lock);
  TEMP_FAILURE_RETRY(unlink(file));
  for (std::vector <const char*>::iterator i = cleanup_files.begin();
       i != cleanup_files.end(); ++i) {
    if (strcmp(file, *i) == 0) {
      free((void*)*i);
      cleanup_files.erase(i);
      break;
    }
  }
  pthread_mutex_unlock(&cleanup_lock);
}

static void remove_all_cleanup_files()
{
  pthread_mutex_lock(&cleanup_lock);
  for (std::vector <const char*>::iterator i = cleanup_files.begin();
       i != cleanup_files.end(); ++i) {
    TEMP_FAILURE_RETRY(unlink(*i));
    free((void*)*i);
  }
  cleanup_files.clear();
  pthread_mutex_unlock(&cleanup_lock);
}

static void add_cleanup_file(const char *file)
{
  char *fname = strdup(file);
  if (!fname)
    return;
  pthread_mutex_lock(&cleanup_lock);
  cleanup_files.push_back(fname);
  if (!cleanup_atexit) {
    atexit(remove_all_cleanup_files);
    cleanup_atexit = true;
  }
  pthread_mutex_unlock(&cleanup_lock);
}


AdminSocket::AdminSocket(CephContext *cct)
  : m_cct(cct),
    m_sock_fd(-1),
    m_shutdown_rd_fd(-1),
    m_shutdown_wr_fd(-1),
    m_lock("AdminSocket::m_lock")
{
}

AdminSocket::~AdminSocket()
{
  shutdown();
}

/*
 * This thread listens on the UNIX domain socket for incoming connections.
 * It only handles one connection at a time at the moment. All I/O is nonblocking,
 * so that we can implement sensible timeouts. [TODO: make all I/O nonblocking]
 *
 * This thread also listens to m_shutdown_rd_fd. If there is any data sent to this
 * pipe, the thread terminates itself gracefully, allowing the
 * AdminSocketConfigObs class to join() it.
 */

#define PFL_SUCCESS ((void*)(intptr_t)0)
#define PFL_FAIL ((void*)(intptr_t)1)

std::string AdminSocket::create_shutdown_pipe(int *pipe_rd, int *pipe_wr)
{
  int pipefd[2];
  int ret = pipe_cloexec(pipefd);
  if (ret < 0) {
    ostringstream oss;
    oss << "AdminSocket::create_shutdown_pipe error: " << cpp_strerror(ret);
    return oss.str();
  }
  
  *pipe_rd = pipefd[0];
  *pipe_wr = pipefd[1];
  return "";
}

std::string AdminSocket::bind_and_listen(const std::string &sock_path, int *fd)
{
  struct sockaddr_un address;
  if (sock_path.size() > sizeof(address.sun_path) - 1) {
    ostringstream oss;
    oss << "AdminSocket::bind_and_listen: "
	<< "The UNIX domain socket path " << sock_path << " is too long! The "
	<< "maximum length on this system is "
	<< (sizeof(address.sun_path) - 1);
    return oss.str();
  }
  int sock_fd = socket(PF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    int err = errno;
    ostringstream oss;
    oss << "AdminSocket::bind_and_listen: "
	<< "failed to create socket: " << cpp_strerror(err);
    return oss.str();
  }
  fcntl(sock_fd, F_SETFD, FD_CLOEXEC);
  memset(&address, 0, sizeof(struct sockaddr_un));
  address.sun_family = AF_UNIX;
  snprintf(address.sun_path, sizeof(address.sun_path),
	   "%s", sock_path.c_str());
  if (bind(sock_fd, (struct sockaddr*)&address,
	   sizeof(struct sockaddr_un)) != 0) {
    int err = errno;
    if (err == EADDRINUSE) {
      // The old UNIX domain socket must still be there.
      // Let's unlink it and try again.
      TEMP_FAILURE_RETRY(unlink(sock_path.c_str()));
      if (bind(sock_fd, (struct sockaddr*)&address,
	       sizeof(struct sockaddr_un)) == 0) {
	err = 0;
      }
      else {
	err = errno;
      }
    }
    if (err != 0) {
      ostringstream oss;
      oss << "AdminSocket::bind_and_listen: "
	  << "failed to bind the UNIX domain socket to '" << sock_path
	  << "': " << cpp_strerror(err);
      close(sock_fd);
      return oss.str();
    }
  }
  if (listen(sock_fd, 5) != 0) {
    int err = errno;
    ostringstream oss;
    oss << "AdminSocket::bind_and_listen: "
	  << "failed to listen to socket: " << cpp_strerror(err);
    close(sock_fd);
    TEMP_FAILURE_RETRY(unlink(sock_path.c_str()));
    return oss.str();
  }
  *fd = sock_fd;
  return "";
}

void* AdminSocket::entry()
{
  while (true) {
    struct pollfd fds[2];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = m_sock_fd;
    fds[0].events = POLLIN | POLLRDBAND;
    fds[1].fd = m_shutdown_rd_fd;
    fds[1].events = POLLIN | POLLRDBAND;

    int ret = poll(fds, 2, -1);
    if (ret < 0) {
      int err = errno;
      if (err == EINTR) {
	continue;
      }
      lderr(m_cct) << "AdminSocket: poll(2) error: '"
		   << cpp_strerror(err) << dendl;
      return PFL_FAIL;
    }

    if (fds[0].revents & POLLIN) {
      // Send out some data
      do_accept();
    }
    if (fds[1].revents & POLLIN) {
      // Parent wants us to shut down
      return PFL_SUCCESS;
    }
  }
}


bool AdminSocket::do_accept()
{
  int ret;
  struct sockaddr_un address;
  socklen_t address_length = sizeof(address);
  ldout(m_cct, 30) << "AdminSocket: calling accept" << dendl;
  int connection_fd = accept(m_sock_fd, (struct sockaddr*) &address,
			     &address_length);
  ldout(m_cct, 30) << "AdminSocket: finished accept" << dendl;
  if (connection_fd < 0) {
    int err = errno;
    lderr(m_cct) << "AdminSocket: do_accept error: '"
			   << cpp_strerror(err) << dendl;
    return false;
  }

  char cmd[80];
  int pos = 0;
  string c;
  while (1) {
    ret = safe_read(connection_fd, &cmd[pos], 1);
    if (ret <= 0) {
      lderr(m_cct) << "AdminSocket: error reading request code: "
		   << cpp_strerror(ret) << dendl;
      close(connection_fd);
      return false;
    }
    //ldout(m_cct, 0) << "AdminSocket read byte " << (int)cmd[pos] << " pos " << pos << dendl;
    if (cmd[0] == '\0') {
      // old protocol: __be32
      if (pos == 3 && cmd[0] == '\0') {
	switch (cmd[3]) {
	case 0:
	  c = "version";
	  break;
	case 1:
	  c = "perfcounters_dump";
	  break;
	case 2:
	  c = "perfcounters_schema";
	  break;
	default:
	  c = "foo";
	  break;
	}
	break;
      }
    } else {
      // new protocol: null or \n terminated string
      if (cmd[pos] == '\n' || cmd[pos] == '\0') {
	cmd[pos] = '\0';
	c = cmd;
	break;
      }
    }
    pos++;
  }

  bool rval = false;

  m_lock.Lock();
  map<string,AdminSocketHook*>::iterator p = m_hooks.find(c);
  bufferlist out;
  if (p == m_hooks.end()) {
    lderr(m_cct) << "AdminSocket: request '" << c << "' not defined" << dendl;
  } else {
    bool success = p->second->call(c, out);
    if (!success) {
      ldout(m_cct, 0) << "AdminSocket: request '" << c << "' to " << p->second << " failed" << dendl;
      out.append("failed");
    } else {
      ldout(m_cct, 20) << "AdminSocket: request '" << c << "' to " << p->second
		       << " returned " << out.length() << " bytes" << dendl;
    }
    uint32_t len = htonl(out.length());
    int ret = safe_write(connection_fd, &len, sizeof(len));
    if (ret < 0) {
      lderr(m_cct) << "AdminSocket: error writing response length "
		   << cpp_strerror(ret) << dendl;
    } else {
      ret = out.write_fd(connection_fd);
      if (ret >= 0)
	rval = true;
    }
  }
  m_lock.Unlock();

  TEMP_FAILURE_RETRY(close(connection_fd));
  return rval;
}

int AdminSocket::register_command(std::string command, AdminSocketHook *hook, std::string help)
{
  int ret;
  m_lock.Lock();
  if (m_hooks.count(command)) {
    ret = -EEXIST;
  } else {
    m_hooks[command] = hook;
    if (help.length())
      m_help[command] = help;
    ret = 0;
  }  
  m_lock.Unlock();
  return ret;
}

int AdminSocket::unregister_command(std::string command)
{
  int ret;
  m_lock.Lock();
  if (m_hooks.count(command)) {
    m_hooks.erase(command);
    m_help.erase(command);
    ret = 0;
  } else {
    ret = -ENOENT;
  }  
  m_lock.Unlock();
  return ret;
}

const char** AdminSocket::get_tracked_conf_keys() const
{
  static const char *KEYS[] = {
    "admin_socket",
    "internal_safe_to_start_threads",
    NULL
  };
  return KEYS;
}

void AdminSocket::handle_conf_change(const md_config_t *conf,
				     const std::set <std::string> &changed)
{
  if (!conf->internal_safe_to_start_threads) {
    // We can't do anything until it's safe to start threads.
    return;
  }
  shutdown();
  if (conf->admin_socket.empty()) {
    // The admin socket is disabled.
    return;
  }
  if (!init(conf->admin_socket)) {
    lderr(m_cct) << "AdminSocketConfigObs: failed to start AdminSocket" << dendl;
  }
}

class VersionHook : public AdminSocketHook {
public:
  virtual bool call(std::string command, bufferlist& out) {
    out.append(CEPH_ADMIN_SOCK_VERSION);
    return true;
  }
};

class HelpHook : public AdminSocketHook {
  AdminSocket *m_as;
public:
  HelpHook(AdminSocket *as) : m_as(as) {}
  bool call(string command, bufferlist& out) {
    unsigned max = 0;
    for (map<string,string>::iterator p = m_as->m_help.begin();
	 p != m_as->m_help.end();
	 ++p) {
      if (p->first.length() > max)
	max = p->first.length();
    }
    max += 1;
    char spaces[max];
    for (unsigned i=0; i<max; ++i)
      spaces[i] = ' ';
    for (map<string,string>::iterator p = m_as->m_help.begin();
	 p != m_as->m_help.end();
	 ++p) {
      out.append(p->first);
      out.append(spaces, max - p->first.length());
      out.append(p->second);
      out.append("\n");
    }
    return true;
  }
};

bool AdminSocket::init(const std::string &path)
{
  /* Set up things for the new thread */
  std::string err;
  int pipe_rd = -1, pipe_wr = -1;
  err = create_shutdown_pipe(&pipe_rd, &pipe_wr);
  if (!err.empty()) {
    lderr(m_cct) << "AdminSocketConfigObs::init: error: " << err << dendl;
    return false;
  }
  int sock_fd;
  err = bind_and_listen(path, &sock_fd);
  if (!err.empty()) {
    lderr(m_cct) << "AdminSocketConfigObs::init: failed: " << err << dendl;
    close(pipe_rd);
    close(pipe_wr);
    return false;
  }

  /* Create new thread */
  m_sock_fd = sock_fd;
  m_shutdown_rd_fd = pipe_rd;
  m_shutdown_wr_fd = pipe_wr;
  m_path = path;

  m_version_hook = new VersionHook;
  register_command("version", m_version_hook, "get protocol version");
  register_command("0", m_version_hook, "");
  m_help_hook = new HelpHook(this);
  register_command("help", m_help_hook, "list available commands");

  create();
  add_cleanup_file(m_path.c_str());
  return true;
}

void AdminSocket::shutdown()
{
  if (m_shutdown_wr_fd < 0)
    return;

  // Send a byte to the shutdown pipe that the thread is listening to
  char buf[1] = { 0x0 };
  int ret = safe_write(m_shutdown_wr_fd, buf, sizeof(buf));
  TEMP_FAILURE_RETRY(close(m_shutdown_wr_fd));
  m_shutdown_wr_fd = -1;

  if (ret == 0) {
    join();
  } else {
    lderr(m_cct) << "AdminSocket::shutdown: failed to write "
      "to thread shutdown pipe: error " << ret << dendl;
  }

  unregister_command("version");
  unregister_command("0");
  delete m_version_hook;
  unregister_command("help");
  delete m_help_hook;

  remove_cleanup_file(m_path.c_str());
  m_path.clear();
}
