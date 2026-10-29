#ifndef COMMON_DEVICE_TIMER_H_
#define COMMON_DEVICE_TIMER_H_

#include <perfcounter.h>

struct Timer {
  perfcounter_t start;
  perfcounter_t end;
  perfcounter_t end2;
};

void startTimer(struct Timer *timer);

void stopTimer(struct Timer *timer);

uint64_t getTime(struct Timer *timer);

#endif
