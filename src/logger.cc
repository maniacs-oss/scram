/*
 * Copyright (C) 2014-2016 Olzhas Rakhimov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/// @file logger.cc
/// Initializing static members of Logger class.

#include "logger.h"

#include <cstdio>

#include <string>

#include "error.h"

namespace scram {

const char* const Logger::kLevelToString_[] = {"ERROR", "WARNING", "INFO",
                                               "DEBUG1", "DEBUG2", "DEBUG3",
                                               "DEBUG4", "DEBUG5"};
LogLevel Logger::report_level_ = ERROR;

Logger::~Logger() noexcept {
  os_ << std::endl;
  // fprintf used for thread safety.
  std::fprintf(stderr, "%s", os_.str().c_str());
  std::fflush(stderr);
}

void Logger::SetVerbosity(int level) {
  if (level < 0 || level > kMaxVerbosity) {
    throw InvalidArgument("Log verbosity must be between 0 and " +
                          std::to_string(kMaxVerbosity));
  }
  report_level_ = static_cast<LogLevel>(level);
}

std::ostringstream& Logger::Get(LogLevel level) {
  os_ << Logger::kLevelToString_[level] << ": ";
  if (level >= DEBUG1)
    os_ << std::string(level - DEBUG1 + 1, '\t');
  return os_;
}

}  // namespace scram
