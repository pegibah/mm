# SPDX-License-Identifier: Apache-2.0

zephyr_library()


zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/net/ip)
zephyr_library_sources(modem_mgsm.c)

