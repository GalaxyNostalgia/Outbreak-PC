/* Sony libcdvd for the PC port: the real-time clock comes from the PC's clock. */
#include "common.h"

#include <time.h>

typedef struct {
    u8 stat, second, minute, hour, pad, day, month, year;
} sceCdCLOCK;

static u8 bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }

s32 sceCdReadClock(sceCdCLOCK *c) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);       /* the PS2 clock is set to local (Japan) time */
    c->stat = 0;
    c->second = bcd(t->tm_sec);
    c->minute = bcd(t->tm_min);
    c->hour = bcd(t->tm_hour);
    c->pad = 0;
    c->day = bcd(t->tm_mday);
    c->month = bcd(t->tm_mon + 1);
    c->year = bcd(t->tm_year % 100);
    return 1;
}
