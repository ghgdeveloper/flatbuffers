/*
 * Copyright 2026 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FLATBUFFERS_ASSERT_H_
#define FLATBUFFERS_ASSERT_H_

#include <cstdio>
#include <cstdlib>

// FLATBUFFERS_ASSERT
//
// - FLATBUFFERS_ASSERTS_ENABLED: whether the checks are compiled in.
//   Defaults to following NDEBUG, matching the CRT assert() this replaces.
//   Define to 1 to keep assertions in optimized builds, or 0 to strip them.
//
// - SetAssertionHandler(): runtime hook invoked when an enabled assertion
//   fails. If no handler is installed or the handler returns, the failure
//   is printed to stderr and abort() is called.
//
// Defining FLATBUFFERS_ASSERT replaces this entirely
#if !defined(FLATBUFFERS_ASSERTS_ENABLED)
  #if defined(NDEBUG)
    #define FLATBUFFERS_ASSERTS_ENABLED 0
  #else
    #define FLATBUFFERS_ASSERTS_ENABLED 1
  #endif
#endif

namespace flatbuffers {

typedef void (*AssertionHandler)(const char *expression, const char *file,
                                 int line);

inline AssertionHandler &AssertionHandlerStorage() {
  static AssertionHandler handler = nullptr;
  return handler;
}

inline AssertionHandler GetAssertionHandler() {
  return AssertionHandlerStorage();
}

inline AssertionHandler SetAssertionHandler(AssertionHandler handler) {
  AssertionHandler previous = AssertionHandlerStorage();
  AssertionHandlerStorage() = handler;
  return previous;
}

inline void AssertionFailed(const char *expression, const char *file, int line) {
  if (const AssertionHandler handler = GetAssertionHandler()) {
    handler(expression, file, line);
  }
  fprintf(stderr, "flatbuffers: assertion failed: %s (%s:%d)\n", expression,
          file, line);
  fflush(stderr);
  abort();
}

}  // namespace flatbuffers

#if !defined(FLATBUFFERS_ASSERT)
  #if FLATBUFFERS_ASSERTS_ENABLED
    #define FLATBUFFERS_ASSERT(expr) \
      ((expr) ? (void)0             \
              : ::flatbuffers::AssertionFailed(#expr, __FILE__, __LINE__))
  #else
    #define FLATBUFFERS_ASSERT(expr) ((void)0)
  #endif
#endif

#endif  // FLATBUFFERS_ASSERT_H_
