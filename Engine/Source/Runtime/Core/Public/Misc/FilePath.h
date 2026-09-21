#pragma once

#include <filesystem>

namespace Durin
{
	// Physical filesystem path; not an asset identity or a mounted virtual path.
	using FFilePath = std::filesystem::path;
}
