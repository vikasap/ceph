#include "common/pick_address.h"

#include "include/ipaddr.h"
#include "include/str_list.h"
#include "common/debug.h"
#include "common/errno.h"

#include <errno.h>

#define dout_subsys ceph_subsys_

static const struct sockaddr *find_ip_in_subnet_list(CephContext *cct,
						     const struct ifaddrs *ifa,
						     const std::string networks)
{
  std::list<string> nets;
  get_str_list(networks, nets);

  for(std::list<string>::iterator s = nets.begin(); s != nets.end(); s++) {
      struct sockaddr net;
      unsigned int prefix_len;

      if (!parse_network(s->c_str(), &net, &prefix_len)) {
	lderr(cct) << "unable to parse network: " << *s << dendl;
	exit(1);
      }

      const struct sockaddr *found = find_ip_in_subnet(ifa, &net, prefix_len);
      if (found)
	return found;
    }

  return NULL;
}

static void fill_in_one_address(CephContext *cct,
				const struct ifaddrs *ifa,
				const string networks,
				const char *conf_var)
{
  const struct sockaddr *found = find_ip_in_subnet_list(cct, ifa, networks);
  if (!found) {
    lderr(cct) << "unable to find any IP address in networks: " << networks << dendl;
    exit(1);
  }

  char buf[INET6_ADDRSTRLEN];
  int err;

  err = getnameinfo(found,
		    (found->sa_family == AF_INET)
		    ? sizeof(struct sockaddr_in)
		    : sizeof(struct sockaddr_in6),

		    buf, sizeof(buf),
		    NULL, 0,
		    NI_NUMERICHOST);
  if (err != 0) {
    lderr(cct) << "unable to convert chosen address to string: " << gai_strerror(err) << dendl;
    exit(1);
  }

  cct->_conf->set_val_or_die(conf_var, buf);
  cct->_conf->apply_changes(NULL);
}

void pick_addresses(CephContext *cct)
{
  struct ifaddrs *ifa;
  int r = getifaddrs(&ifa);
  if (r<0) {
    string err = cpp_strerror(errno);
    lderr(cct) << "unable to fetch interfaces and addresses: " << err << dendl;
    exit(1);
  }

  if (cct->_conf->public_addr.is_blank_ip() && !cct->_conf->public_network.empty()) {
    fill_in_one_address(cct, ifa, cct->_conf->public_network, "public_addr");
  }

  if (cct->_conf->cluster_addr.is_blank_ip() && !cct->_conf->cluster_network.empty()) {
    fill_in_one_address(cct, ifa, cct->_conf->cluster_network, "cluster_addr");
  }

  freeifaddrs(ifa);
}

bool have_local_addr(CephContext *cct, const list<entity_addr_t>& ls, entity_addr_t *match)
{
  struct ifaddrs *ifa;
  int r = getifaddrs(&ifa);
  if (r < 0) {
    lderr(cct) << "unable to fetch interfaces and addresses: " << cpp_strerror(errno) << dendl;
    exit(1);
  }

  bool found = false;
  for (struct ifaddrs *addrs = ifa; addrs != NULL; addrs = addrs->ifa_next) {
    entity_addr_t a;
    a.set_sockaddr(addrs->ifa_addr);
    for (list<entity_addr_t>::const_iterator p = ls.begin(); p != ls.end(); ++p) {
      if (a.is_same_host(*p)) {
	*match = *p;
	found = true;
	goto out;
      }
    }
  }

 out:
  freeifaddrs(ifa);
  return found;
}
