// Forced include for the public emulator's sources in core_compare only, so the test can read its
// registers while the sources stay unmodified. Standard headers come first, as they forbid the macro.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <vector>
#define _ALLOW_KEYWORD_MACROS
#define private public
