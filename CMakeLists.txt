# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rt1180-ats)

target_sources(app PRIVATE src/main.c)

if (CONFIG_ATS_SOFTWARE_DRIVER OR CONFIG_NETC_SPEEDTEST_ENABLED)
    target_sources(app PRIVATE src/ats_time.c)
    target_sources(app PRIVATE src/ptp_clock.c)
endif()

if(CONFIG_ATS_SOFTWARE_DRIVER)
    target_sources(app PRIVATE src/sender.c)
    target_sources(app PRIVATE src/ats.c)
    target_sources(app PRIVATE src/ats_iface.c)
    if(CONFIG_ATS_VERIFY_ENABLED)
        target_sources(app PRIVATE src/ats_verify.c)
    endif()

    zephyr_linker_sources(DATA_SECTIONS src/ats.ld)
    zephyr_iterable_section(NAME ats GROUP DATA_REGION ${XIP_ALIGN_WITH_INPUT})
endif()

if(CONFIG_NETC_SPEEDTEST_ENABLED)
    target_sources(app PRIVATE src/speedtest.c)
endif()