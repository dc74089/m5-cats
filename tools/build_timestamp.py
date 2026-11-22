"""Inject the true UTC build time as BUILD_EPOCH_UTC.

Parsing __DATE__/__TIME__ yields the build machine's *local* time, which the
firmware would then have to interpret as UTC -- seeding a fresh RTC hours off
and making the first NTP sync a large, visible jump.
"""
import time

Import("env")  # noqa: F821  (provided by PlatformIO)

env.Append(CPPDEFINES=[("BUILD_EPOCH_UTC", int(time.time()))])
