#ifndef ICMP_PING_H
#define ICMP_PING_H

#include <Arduino.h>
#include <Ethernet.h>

enum IcmpPingResult {
  ICMP_PING_REPLY,
  ICMP_PING_NO_REPLY,
  ICMP_PING_NO_SOCKET,
  ICMP_PING_SOCKET_ERROR
};

IcmpPingResult icmpPing(IPAddress target, unsigned long timeoutMs);

#endif
