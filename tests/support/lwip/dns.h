#pragma once
#include <stdint.h>
struct ip_addr_t { uint32_t addr; };
using err_t = int;
static constexpr err_t ERR_OK=0, ERR_INPROGRESS=-5;
using dns_found_callback = void (*)(const char *, const ip_addr_t *, void *);
err_t dns_gethostbyname(const char *, ip_addr_t *, dns_found_callback, void *);
