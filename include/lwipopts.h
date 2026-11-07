#pragma once

// lwIP options on top of the Arduino-Pico core's lwipopts.h.
//
// `-I include` comes first in the include path (platformio.ini), so lwIP's `#include "lwipopts.h"`
// finds this file; #include_next then loads the core's file, and the lines below change only what
// they name. A -D in platformio.ini cannot do this: the core's file sets LWIP_STATS 0 without #ifndef.

#include_next <lwipopts.h>

// lwIP's MIB-II counters (lwip_stats.mib2) for the SNMP ip, icmp, tcp and udp groups.
// MIB2_STATS counts through the STATS_INC machinery, so LWIP_STATS has to be on.
#undef LWIP_STATS
#define LWIP_STATS 1
#define MIB2_STATS 1

// ...but none of lwIP's debug statistics: they would cost RAM and nothing reads them.
// (LINK_STATS, MEM_STATS, MEMP_STATS, SYS_STATS and LWIP_STATS_DISPLAY are 0 in the core's file.)
#define ETHARP_STATS 0
#define IP_STATS 0
#define IPFRAG_STATS 0
#define ICMP_STATS 0
#define IGMP_STATS 0
#define UDP_STATS 0
#define TCP_STATS 0
#define IP6_STATS 0
#define ICMP6_STATS 0
#define IP6_FRAG_STATS 0
#define MLD6_STATS 0
#define ND6_STATS 0

// NTP: lwIP's SNTP client sets the core's clock through timeServiceSetTime() (src/time_service.cpp),
// which also notes when it was last synced. The core's file points this at __setSystemTime directly.
#undef SNTP_SET_SYSTEM_TIME_US
#ifdef __cplusplus
extern "C"
#endif
void timeServiceSetTime(unsigned long sec, unsigned long us);
#define SNTP_SET_SYSTEM_TIME_US(sec, us) timeServiceSetTime((sec), (us))
