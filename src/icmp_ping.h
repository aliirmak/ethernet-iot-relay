#ifndef ICMP_PING_H
#define ICMP_PING_H

#include <Arduino.h>
#include <Ethernet.h>

bool icmpPing(IPAddress target, unsigned long timeoutMs);

#endif
