#pragma once
#include <circle/types.h>
class CUSBEndpoint { public: void ResetPID() { resets++; } unsigned resets = 0; };
