========================
Hardware Recommendations
========================
Ceph runs on commodity hardware and a Linux operating system over a TCP/IP network. The hardware 
recommendations for different processes/daemons differ considerably. Ceph OSDs run on commodity hardware 
and a Linux operating system over a TCP/IP network. OSD hosts should have ample data storage in the form of 
a hard drive or a RAID. Ceph OSDs run the RADOS service, calculate data placement with CRUSH, and maintain their
own copy of the cluster map. So OSDs should have a reasonable amount of processing power. Ceph monitors require 
enough disk space for the cluster map, but usually do not encounter heavy loads. Monitors do not need to be very powerful.
Ceph metadata servers distribute their load. However, they must be capable of serving their data quickly. 
Metadata servers should have strong processing capability and plenty of RAM.

.. note:: If you are not using the Ceph File System, you do not need a meta data server.

+--------------+----------------+------------------------------------+
|  Process     | Criteria       | Minimum Recommended                |
+==============+================+====================================+
| ``ceph-osd`` | Processor      |  64-bit AMD-64/i386 dual-core      |
|              +----------------+------------------------------------+
|              | RAM            |  500 MB per daemon                 |
|              +----------------+------------------------------------+
|              | Volume Storage |  1-disk or RAID per daemon         |
|              +----------------+------------------------------------+
|              | Network        |  2-1GB Ethernet NICs               |
+--------------+----------------+------------------------------------+
| ``ceph-mon`` | Processor      |  64-bit AMD-64/i386                |
|              +----------------+------------------------------------+
|              | RAM            |  1 GB per daemon                   |
|              +----------------+------------------------------------+
|              | Disk Space     |  10 GB per daemon                  |
|              +----------------+------------------------------------+
|              | Network        |  2-1GB Ethernet NICs               |
+--------------+----------------+------------------------------------+
| ``ceph-mds`` | Processor      |  64-bit AMD-64/i386 quad-core      |
|              +----------------+------------------------------------+
|              | RAM            |  1 GB minimum per daemon           |
|              +----------------+------------------------------------+
|              | Disk Space     |  1 MB per daemon                   |
|              +----------------+------------------------------------+
|              | Network        |  2-1GB Ethernet NICs               |
+--------------+----------------+------------------------------------+ 

