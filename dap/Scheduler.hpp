#pragma once

#include "vm/Scheduler.hpp"

namespace quartz::dap {

// Hosts written against the scheduler's old home keep compiling until they include vm/Scheduler.hpp.
using vm::TickScheduler;

} // namespace quartz::dap
