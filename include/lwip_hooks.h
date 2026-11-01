#pragma once

/**
 * @file lwip_hooks.h
 * @brief lwIP hooks (LWIP_HOOK_FILENAME in platformio.ini). Included by lwIP's own C files,
 *        so it must stay plain C and pull in nothing beyond lwIP types.
 *
 * lwIP includes it through a macro, which the build's dependency scan does not follow:
 * after changing this file run `pio run -e pico -t clean` so lwIP is compiled again.
 */

#include "lwip/ip6_addr.h"

struct netif;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Next hop for an off-link IPv6 destination: the static IPv6 gateway, or NULL to let
 *        lwIP pick a router from router advertisements (src/ipv6_static.cpp)
 *
 * Weak: builds without src/ (the on-device tests, test_build_src = no) link lwIP without it,
 * and the hook then always lets lwIP choose.
 */
__attribute__((weak)) const ip6_addr_t* rfpowerIPv6Gateway(struct netif* netif, const ip6_addr_t* dest);

#ifdef __cplusplus
}
#endif

#define LWIP_HOOK_ND6_GET_GW(netif, dest) (rfpowerIPv6Gateway ? rfpowerIPv6Gateway(netif, dest) : NULL)
