#include "netcount.h"
#include "debug.h"

#include <stdbool.h>
#include <sys/socket.h>

// Video packets are a full shard: the packet size (1024 bytes or more) plus
// headers. Only the video socket gets packets this big, and once one has,
// only that socket's full-size packets are counted.
#define VIDEO_PACKET_MIN_BYTES 1000
// Remembers this many recent sequence numbers, to tell a late packet (which
// fills a gap) from a repeat (which doesn't)
#define SEEN_WINDOW 4096

// Written only by common-c's video receive thread, so no lock is needed
static volatile uint32_t received = 0;
static volatile uint32_t gaps = 0;
static volatile uint32_t late = 0;
static volatile uint32_t repeats = 0;
static bool have_sequence = false;
static uint16_t last_sequence = 0;
static int video_socket = -1;
// seen[n % SEEN_WINDOW] holds n + 1 for the last sequence number n that
// landed there, so 0 means empty
static uint32_t seen[SEEN_WINDOW];

static bool already_seen(uint16_t sequence) {
  return seen[sequence % SEEN_WINDOW] == (uint32_t)sequence + 1;
}

static void mark_seen(uint16_t sequence) {
  seen[sequence % SEEN_WINDOW] = (uint32_t)sequence + 1;
}

// Wrapped at link time (-Wl,--wrap=bindUdpSocket): logs the receive buffer
// the network stack granted, which common-c only logs in debug builds. It
// asks for room for 2048 video packets and steps down until one is accepted.
// On a Vita the grant is 512 KB whether the net pool (main.c) is 1 MB or 4 MB.
int __real_bindUdpSocket(int addressFamily, struct sockaddr_storage *localAddr, socklen_t addrLen,
                         int bufferSize, int socketQosType);
int __wrap_bindUdpSocket(int addressFamily, struct sockaddr_storage *localAddr, socklen_t addrLen,
                         int bufferSize, int socketQosType) {
  int s = __real_bindUdpSocket(addressFamily, localAddr, addrLen, bufferSize, socketQosType);
  if (s >= 0 && bufferSize > 0) {
    int granted = 0;
    socklen_t len = sizeof(granted);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, &granted, &len) == 0) {
      vita_debug_log("[NET] UDP receive buffer: asked %d bytes, granted %d", bufferSize, granted);
    }
  }
  return s;
}

// Wrapped at link time (-Wl,--wrap=recvUdpSocket), so common-c needs no change
int __real_recvUdpSocket(int s, char *buffer, int size, bool useSelect);
int __wrap_recvUdpSocket(int s, char *buffer, int size, bool useSelect) {
  int ret = __real_recvUdpSocket(s, buffer, size, useSelect);
  if (ret < VIDEO_PACKET_MIN_BYTES) {
    return ret;
  }
  if (s != video_socket) {
    // A new stream's video socket: its sequence numbers start afresh
    video_socket = s;
    have_sequence = false;
    for (int i = 0; i < SEEN_WINDOW; i++) {
      seen[i] = 0;
    }
  }
  // The RTP header's sequence number, big-endian at bytes 2 and 3. The host
  // numbers every video packet in turn, so a gap is a packet lost on the way,
  // unless it turns up late.
  uint16_t sequence = (uint16_t)(((uint8_t)buffer[2] << 8) | (uint8_t)buffer[3]);
  if (already_seen(sequence)) {
    repeats++;
    return ret;
  }
  mark_seen(sequence);
  received++;
  if (!have_sequence) {
    have_sequence = true;
    last_sequence = sequence;
    return ret;
  }
  uint16_t step = (uint16_t)(sequence - last_sequence);
  if (step >= 0x8000) {
    // Behind the newest packet and not seen before: it fills a gap
    late++;
    return ret;
  }
  gaps += step - 1;
  last_sequence = sequence;
  return ret;
}

void netcount_reset(void) {
  // The next video packet starts a new sequence, even if the new stream's
  // socket reuses the old one's descriptor
  video_socket = -1;
}

void netcount_read(netcount *out) {
  out->received = received;
  out->gaps = gaps;
  out->late = late;
  out->repeats = repeats;
}

uint32_t netcount_lost(const netcount *start, const netcount *end) {
  uint32_t gap_count = end->gaps - start->gaps;
  uint32_t late_count = end->late - start->late;
  return gap_count > late_count ? gap_count - late_count : 0;
}
