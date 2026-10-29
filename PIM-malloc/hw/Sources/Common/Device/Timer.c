#include "Common/Device/Timer.h"

void startTimer(struct Timer *timer) { timer->start = perfcounter_get(); }

void stopTimer(struct Timer *timer) { timer->end = perfcounter_get(); }

uint64_t getTime(struct Timer *timer) { return timer->end - timer->start; }
