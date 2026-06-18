#include "icmp_ping.h"

#include <SPI.h>
#include <utility/w5100.h>

namespace {

const uint8_t ICMP_ECHO_REPLY = 0;
const uint8_t ICMP_ECHO_REQUEST = 8;
const uint8_t ICMP_HEADER_SIZE = 8;
const uint8_t ICMP_DATA_SIZE = 16;
const uint8_t ICMP_PACKET_SIZE = ICMP_HEADER_SIZE + ICMP_DATA_SIZE;
const uint8_t IPRAW_HEADER_SIZE = 6;
const uint16_t ICMP_IDENTIFIER = 0x5532;

uint16_t sequenceNumber = 0;

uint16_t internetChecksum(const uint8_t *data, uint16_t length) {
  uint32_t sum = 0;

  while (length > 1) {
    sum += (static_cast<uint16_t>(data[0]) << 8) | data[1];
    data += 2;
    length -= 2;
  }

  if (length > 0) {
    sum += static_cast<uint16_t>(data[0]) << 8;
  }

  while (sum >> 16) {
    sum = (sum & 0xFFFFU) + (sum >> 16);
  }

  return static_cast<uint16_t>(~sum);
}

void writeSocketBuffer(uint16_t base, uint16_t pointer, const uint8_t *data,
                       uint16_t length) {
  uint16_t offset = pointer & W5100.SMASK;
  uint16_t firstLength = min(length, W5100.SSIZE - offset);

  W5100.write(base + offset, data, firstLength);
  if (length > firstLength) {
    W5100.write(base, data + firstLength, length - firstLength);
  }
}

void readSocketBuffer(uint16_t base, uint16_t pointer, uint8_t *data,
                      uint16_t length) {
  uint16_t offset = pointer & W5100.SMASK;
  uint16_t firstLength = min(length, W5100.SSIZE - offset);

  W5100.read(base + offset, data, firstLength);
  if (length > firstLength) {
    W5100.read(base, data + firstLength, length - firstLength);
  }
}

uint8_t findFreeSocket() {
  uint8_t socketCount = W5100.getChip() == 51 ? 4 : MAX_SOCK_NUM;

  for (uint8_t socketNumber = 0; socketNumber < socketCount; ++socketNumber) {
    if (W5100.readSnSR(socketNumber) == SnSR::CLOSED) {
      return socketNumber;
    }
  }

  return MAX_SOCK_NUM;
}

void closeSocket(uint8_t socketNumber) {
  W5100.execCmdSn(socketNumber, Sock_CLOSE);
  W5100.writeSnIR(socketNumber, 0xFF);
}

bool matchesReply(const uint8_t *sourceIp, IPAddress target,
                  const uint8_t *packet, uint16_t packetLength,
                  uint16_t expectedSequence) {
  if (packetLength < ICMP_HEADER_SIZE || packet[0] != ICMP_ECHO_REPLY ||
      packet[1] != 0) {
    return false;
  }

  for (uint8_t index = 0; index < 4; ++index) {
    if (sourceIp[index] != target[index]) {
      return false;
    }
  }

  uint16_t identifier =
      (static_cast<uint16_t>(packet[4]) << 8) | packet[5];
  uint16_t receivedSequence =
      (static_cast<uint16_t>(packet[6]) << 8) | packet[7];
  return identifier == ICMP_IDENTIFIER &&
         receivedSequence == expectedSequence;
}

} // namespace

bool icmpPing(IPAddress target, unsigned long timeoutMs) {
  if (W5100.getChip() == 0) {
    return false;
  }

  uint8_t packet[ICMP_PACKET_SIZE] = {
      ICMP_ECHO_REQUEST, 0, 0, 0,
      static_cast<uint8_t>(ICMP_IDENTIFIER >> 8),
      static_cast<uint8_t>(ICMP_IDENTIFIER & 0xFF), 0, 0};
  uint16_t currentSequence = ++sequenceNumber;
  packet[6] = static_cast<uint8_t>(currentSequence >> 8);
  packet[7] = static_cast<uint8_t>(currentSequence & 0xFF);
  for (uint8_t index = ICMP_HEADER_SIZE; index < ICMP_PACKET_SIZE; ++index) {
    packet[index] = index;
  }

  uint16_t checksum = internetChecksum(packet, sizeof(packet));
  packet[2] = static_cast<uint8_t>(checksum >> 8);
  packet[3] = static_cast<uint8_t>(checksum & 0xFF);

  unsigned long startedAt = millis();
  bool replyReceived = false;

  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  uint8_t socketNumber = findFreeSocket();
  if (socketNumber == MAX_SOCK_NUM) {
    SPI.endTransaction();
    return false;
  }

  closeSocket(socketNumber);
  W5100.writeSnMR(socketNumber, SnMR::IPRAW);
  W5100.writeSnPROTO(socketNumber, IPPROTO::ICMP);
  W5100.writeSnTTL(socketNumber, 64);
  W5100.writeSnIR(socketNumber, 0xFF);
  W5100.execCmdSn(socketNumber, Sock_OPEN);

  if (W5100.readSnSR(socketNumber) != SnSR::IPRAW) {
    closeSocket(socketNumber);
    SPI.endTransaction();
    return false;
  }

  uint8_t targetBytes[4] = {target[0], target[1], target[2], target[3]};
  W5100.writeSnDIPR(socketNumber, targetBytes);

  uint16_t transmitPointer = W5100.readSnTX_WR(socketNumber);
  writeSocketBuffer(W5100.SBASE(socketNumber), transmitPointer, packet,
                    sizeof(packet));
  W5100.writeSnTX_WR(socketNumber, transmitPointer + sizeof(packet));
  W5100.execCmdSn(socketNumber, Sock_SEND);

  while (millis() - startedAt < timeoutMs) {
    uint8_t interruptFlags = W5100.readSnIR(socketNumber);
    if (interruptFlags & SnIR::SEND_OK) {
      W5100.writeSnIR(socketNumber, SnIR::SEND_OK);
      break;
    }
    if (interruptFlags & SnIR::TIMEOUT) {
      W5100.writeSnIR(socketNumber, SnIR::TIMEOUT);
      closeSocket(socketNumber);
      SPI.endTransaction();
      return false;
    }
  }

  while (millis() - startedAt < timeoutMs && !replyReceived) {
    uint16_t available = W5100.readSnRX_RSR(socketNumber);
    if (available < IPRAW_HEADER_SIZE) {
      continue;
    }

    uint16_t receivePointer = W5100.readSnRX_RD(socketNumber);
    uint8_t rawHeader[IPRAW_HEADER_SIZE];
    readSocketBuffer(W5100.RBASE(socketNumber), receivePointer, rawHeader,
                     sizeof(rawHeader));

    uint16_t packetLength =
        (static_cast<uint16_t>(rawHeader[4]) << 8) | rawHeader[5];
    uint16_t frameLength = IPRAW_HEADER_SIZE + packetLength;
    if (available < frameLength) {
      continue;
    }

    uint8_t receivedPacket[ICMP_PACKET_SIZE];
    uint16_t bytesToRead = min(packetLength,
                               static_cast<uint16_t>(sizeof(receivedPacket)));
    readSocketBuffer(W5100.RBASE(socketNumber),
                     receivePointer + IPRAW_HEADER_SIZE, receivedPacket,
                     bytesToRead);

    W5100.writeSnRX_RD(socketNumber, receivePointer + frameLength);
    W5100.execCmdSn(socketNumber, Sock_RECV);

    replyReceived = matchesReply(rawHeader, target, receivedPacket, bytesToRead,
                                 currentSequence);
  }

  closeSocket(socketNumber);
  SPI.endTransaction();
  return replyReceived;
}
