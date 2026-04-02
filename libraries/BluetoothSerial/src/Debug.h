// Copyright 2026 Tobias Hahnen, DIMATE GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DEBUG_H_
#define DEBUG_H_

/**
 *  Define all the debug macros to be used inside the code, should not have any
 *  effect when building the "Release" configuration!
 */
#ifdef DEBUG_ENABLED
#define DEBUG_SERIAL(x) Serial.println(x);
#else
#define DEBUG_SERIAL(x)
#endif

#endif
