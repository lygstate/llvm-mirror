//===- llvm/Support/ErrorHandling.h - Callbacks for errors ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an API used to indicate error conditions.
// Callbacks can be registered for these errors through this API.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_ERRORHANDLING_H
#define LLVM_SUPPORT_ERRORHANDLING_H

#include "llvm/Support/Compiler.h"
#include <string>

namespace llvm {
  /// An error handler callback.
  typedef void (*llvm_error_handler_t)(const std::string& reason);

  /// Installs a new error handler: this function will be called whenever a
  /// serious error is encountered by LLVM.
  /// If you are using llvm_start_multithreaded, you should register the handler
  /// before doing that.
  ///
  /// If no error handler is installed the default is to print the error message
  /// to stderr, and call exit(1).
  /// If an error handler is installed then it is the handler's responsibility
  /// to log the message, it will no longer be printed to stderr.
  /// If the error handler returns, then exit(1) will be called.
  void llvm_install_error_handler(llvm_error_handler_t handler);

  /// Restores default error handling behaviour.
  /// This must not be called between llvm_start_multithreaded() and
  /// llvm_stop_multithreaded().
  void llvm_remove_error_handler(void);

  /// Reports a serious error, calling any installed error handler.
  /// If no error handler is installed the default is to print the message to
  /// standard error, followed by a newline.
  /// After the error handler is called this function will call exit(1), it 
  /// does not return.
  void llvm_report_error(const std::string &reason) NORETURN;

  /// This function calls abort(), and prints the optional message to stderr.
  /// Call this instead of assert(0), so that compiler knows the path is not
  /// reachable even for NDEBUG builds.
  /// Use the LLVM_UNREACHABLE macro instead that adds location info.
  void llvm_unreachable(const char *msg=0, const char *file=0,
                        unsigned line=0) NORETURN;
}

/// Macro that calls llvm_unreachable with location info and message in 
/// debug mode. In NDEBUG mode it calls llvm_unreachable with no message.
#ifndef NDEBUG
#define LLVM_UNREACHABLE(msg) llvm_unreachable(msg, __FILE__, __LINE__)
#else
#define LLVM_UNREACHABLE(msg) llvm_unreachable()
#endif

#endif

