#pragma once
class CScheduler { public: static CScheduler* Get() { static CScheduler s; return &s; } void MsSleep(unsigned) {} };
