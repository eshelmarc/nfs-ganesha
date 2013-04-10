#!/usr/bin/python

# You must initialize the gobject/dbus support for threading
# before doing anything.
import gobject
gobject.threads_init()

from dbus import glib
glib.init_threads()

# Create a session bus.
import dbus
bus = dbus.SystemBus()

# Create an object that will proxy for a particular remote object.
admin = bus.get_object("org.ganesha.nfsd",
                       "/org/ganesha/nfsd/admin")

# call method
ganesha_grace = admin.get_dbus_method('grace',
                               'org.ganesha.nfsd.admin')

print "Start grace period."

ganesha_grace()
