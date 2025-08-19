#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <source_location>
#include <memory>

// Container types
#include <vector>
#include <array>
#include <list>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <stack>
#include <queue>
#include <string>
#include <tuple>

// Multithreading support
#include <mutex>
#include <atomic>

#include "Math/MathFwd.h"
#include "Containers/ContainersFwd.h"

namespace FFileSystem = std::filesystem;
using FPath = FFileSystem::path;
