#pragma once
#include <switch.h>

/* Ounce 23.0.1 ARP/LDN ABI. The fallback detaches only our own process mapping
   around CreateNetwork, then restores and reads it back before returning. */
Result labCreateGameNetwork(Service *sm_proxy, const LdnSecurityConfig *security,
    const LdnUserConfig *user, const LdnNetworkConfig *config);
