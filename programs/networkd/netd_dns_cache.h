// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_DNS_CACHE_H
#define YOS_NETWORKD_DNS_CACHE_H

#include <stdint.h>

#include "netd_types.h"

void netd_dns_cache_init(netd_dns_cache_t* cache);

void netd_dns_cache_cleanup(netd_dns_cache_t* cache);

int netd_dns_cache_lookup(netd_dns_cache_t* cache, const char* name, uint32_t* out_addr);

int netd_dns_cache_insert(netd_dns_cache_t* cache, const char* name, uint32_t addr, uint32_t ttl_ms);

void netd_dns_cache_expire_old(netd_dns_cache_t* cache);

void netd_dns_cache_clear(netd_dns_cache_t* cache);

uint32_t netd_dns_cache_size(const netd_dns_cache_t* cache);

void netd_dns_cache_print(const netd_dns_cache_t* cache);

uint32_t netd_dns_cache_hash(const char* name);

#endif