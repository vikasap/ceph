.. _adjusting-crush:

=========================
 Adjusting the CRUSH map
=========================

There are a few ways to adjust the crush map:

* online, by issuing commands to the monitor
* offline, by extracting the current map to a file, modifying it, and then reinjecting a new map

For offline changes, some can be made directly with ``crushtool``, and
others require you to decompile the file to text form, manually edit
it, and then recompile.


Adding a new device (OSD) to the map
====================================

Adding new devices can be done via the monitor.  The general form is::

  $ ceph osd crush add <id> <name> <weight> [<loc> [<lo2> ...]]

where

  * ``id`` is the numeric device id (the OSD id)
  * ``name`` is an alphanumeric name.  By convention Ceph uses
    ``osd.$id``.
  * ``weight`` is a floating point weight value controlling how much
    data the device will be allocated.  A decent convention is to make
    this the number of TB the device will store.
  * ``loc`` is a list of ``what=where`` pairs indicating where in the
    CRUSH hierarchy the device will be stored.  By default, the
    hierarchy (the ``what``s) includes ``pool`` (the ``default`` pool
    is normally the root of the hierarchy), ``rack``, and ``host``.
    At least one of these location specifiers has to refer to an
    existing point in the hierarchy, and only the lowest (most
    specific) match counts.  Beneath that point, any intervening
    branches will be created as needed.  Specifying the complete
    location is always sufficient, and also safe in that existing
    branches (and devices) won't be moved around.

For example, if the new OSD id is ``123``, we want a weight of ``1.0``
and the new device is on host ``hostfoo`` and rack ``rackbar``::

   $ ceph osd crush add 123 osd.123 1.0 pool=default rack=rackbar host=hostfoo

will add it to the hierarchy.  The rack ``rackbar`` and host
``hostfoo`` will be added as needed, as long as the pool ``default``
exists (as it does in the default Ceph CRUSH map generated during
cluster creation).

Note that if I later add another device in the same host but specify a
different pool or rack::

   $ ceph osd crush add 124 osd.124 1.0 pool=nondefault rack=weirdrack host=hostfoo

the device will still be placed in host ``hostfoo`` at its current
location (rack ``rackbar`` and pool ``default``).


Adjusting the CRUSH weight
==========================

You can adjust the CRUSH weight for a device with::

   $ ceph osd crush reweight osd.123 2.0

Removing a device
=================

You can remove a device from the crush map with::

   $ ceph osd crush remove osd.123

