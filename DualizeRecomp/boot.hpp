#pragma once

#include <string>

namespace dualize
{
class Machine;

// Boots the disc's game partition the way the system menu launches it; on
// return the CPU is at the DOL's entry point.
bool BootWiiDisc(Machine& machine, std::string* error);
}  // namespace dualize
