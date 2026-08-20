// one path for the installed daemon and every client command. letting these
// drift would turn a fresh workstation setup into a very confusing no-daemon
// error even though rlbsd is sitting there doing its job.
#pragma once

namespace rlbs {

inline constexpr char default_control_socket[] = "/run/rlbs/rlbs.sock";

} // namespace rlbs
