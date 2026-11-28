-- A client of the D-Bus session and system buses, with no library under it: the wire
-- format, the connection on a thread of its own, calls, signals, names, properties,
-- introspection and descriptors. Linux's; elsewhere dbus_open says so. Its customers are
-- media_platform (MPRIS, keep-awake, sleep) and hifi_platform (the device reservation).
module "dbus" {
  deps = { "core", "platform" },
}
