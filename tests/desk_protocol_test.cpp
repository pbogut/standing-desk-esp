#include "desk_protocol.h"

#include <assert.h>
#include <stdio.h>

static void frame(DeskMonitor &monitor, const uint8_t (&data)[6], uint32_t now) {
  for (uint8_t byte : data) monitor.feed(byte, now);
}

int main() {
  const uint8_t captures[][6] = {
      {0x5A, 0x07, 0xBF, 0x3F, 0x10, 0x15},
      {0x5A, 0x07, 0x86, 0x3F, 0x10, 0xDC},
      {0x5A, 0x07, 0xDB, 0x3F, 0x10, 0x31},
      {0x5A, 0x07, 0xCF, 0x3F, 0x10, 0x25},
      {0x5A, 0x07, 0xCF, 0x6D, 0x10, 0x53},
      {0x5A, 0x07, 0xFF, 0x7D, 0x10, 0x93},
      {0x5A, 0x06, 0x3F, 0x6D, 0x10, 0xC2}};
  const uint16_t heights[] = {7000, 7100, 7200, 7300, 7350, 7860, 10500};
  DeskMonitor monitor;
  assert(!monitor.fresh(0));
  for (unsigned i = 0; i < 7; ++i) {
    frame(monitor, captures[i], i * 10);
    assert(monitor.heightHundredths == heights[i]);
    assert(monitor.fresh(i * 10));
  }
  assert(monitor.validFrames == 7);
  assert(monitor.fresh(1059));
  assert(!monitor.fresh(1060));

  // A bad checksum never updates height or its freshness timestamp.
  const uint8_t bad[] = {0x5A, 0x07, 0xBF, 0x3F, 0x10, 0x16};
  frame(monitor, bad, 100);
  assert(monitor.checksumFailures == 1);
  assert(monitor.lastHeightMs == 60);
  assert(monitor.heightHundredths == 10500);

  // A valid error/blank display invalidates the reading immediately.
  const uint8_t blank[] = {0x5A, 0, 0, 0, 0x10, 0x10};
  frame(monitor, blank, 110);
  assert(monitor.invalidDisplays == 1 && !monitor.fresh(110));
  frame(monitor, captures[0], 120);
  assert(monitor.fresh(120));

  const uint8_t multipleDots[] = {0x5A, 0x87, 0xBF, 0x3F, 0x10, 0x95};
  const uint8_t trailingDot[] = {0x5A, 0x07, 0x3F, 0xBF, 0x10, 0x15};
  frame(monitor, multipleDots, 130);
  frame(monitor, trailingDot, 140);
  assert(monitor.invalidDisplays == 3 && !monitor.fresh(140));
  const uint8_t firstDot[] = {0x5A, 0x87, 0x7F, 0x7D, 0x10, 0x93};
  frame(monitor, firstDot, 150);
  assert(monitor.heightHundredths == 786);
  const uint8_t maximum[] = {0x5A, 0x6F, 0x6F, 0x6F, 0x10, 0x5D};
  frame(monitor, maximum, 160);
  assert(monitor.heightHundredths == 99900);

  // Recover when the next real header lands inside a corrupt candidate.
  DeskMonitor recovery;
  recovery.feed(0xFF, 0);
  recovery.feed(0x5A, 0);
  recovery.feed(0x99, 0);
  frame(recovery, captures[5], 1);
  assert(recovery.checksumFailures == 1);
  assert(recovery.fresh(1) && recovery.heightHundredths == 7860);
  // A deleted byte, followed by a complete frame, also resynchronizes.
  for (unsigned i = 0; i < 5; ++i) recovery.feed(captures[0][i], 2);
  frame(recovery, captures[6], 3);
  assert(recovery.heightHundredths == 10500);

  // A header-valued status/checksum byte must not split a valid frame.
  const uint8_t headerStatus[] = {0x5A, 0x06, 0x3F, 0x6D, 0x5A, 0x0C};
  const uint8_t headerChecksum[] = {0x5A, 0x06, 0x3F, 0x6D, 0xA8, 0x5A};
  frame(recovery, headerStatus, 4);
  frame(recovery, headerChecksum, 5);
  assert(recovery.validFrames == 4);

  recovery.feed(0x5A, 10);
  recovery.feed(0x07, 11);
  frame(recovery, captures[0], 41);
  assert(recovery.partialTimeouts == 1 && recovery.heightHundredths == 7000);
  frame(recovery, captures[5], UINT32_MAX - 500);
  assert(recovery.fresh(498));
  assert(!recovery.fresh(499));
  puts("All desk protocol tests passed.");
}
