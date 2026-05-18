#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    uint32_t epoch;          // host UTC epoch seconds (0 = not provided)
    int tz_off;              // host UTC offset in seconds
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
