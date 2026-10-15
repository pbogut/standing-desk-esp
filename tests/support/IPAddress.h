#pragma once
#include <lwip/dns.h>
#include <stdio.h>
#include <string>
class IPAddress {
 public:
  IPAddress() = default;
  IPAddress &operator=(const ip_addr_t &address) { value=address.addr;return *this; }
  bool fromString(const char *text) {
    unsigned a,b,c,d;char extra;
    if(sscanf(text,"%u.%u.%u.%u%c",&a,&b,&c,&d,&extra)!=4||a>255||b>255||c>255||d>255)return false;
    value=a|(b<<8)|(c<<16)|(d<<24);return true;
  }
  std::string toString() const {
    return std::to_string(value&255)+"."+std::to_string((value>>8)&255)+"."
        +std::to_string((value>>16)&255)+"."+std::to_string(value>>24);
  }
 private:
  uint32_t value=0;
};
