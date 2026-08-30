#pragma once

#include "Misc/MountPaths.h"

namespace Durin::Testing
{
	CORE_API auto RegisterMountPointForTests(std::string_view VirtualRoot,
		std::string_view PhysicalPath, bool bAutoScan = true, bool bContentWritable = true) -> void;

	class CORE_API FScopedMountRegistryFixture
	{
	public:
		FScopedMountRegistryFixture();
		explicit FScopedMountRegistryFixture(std::span<const FMountPoint> Definitions);
		~FScopedMountRegistryFixture();
		FScopedMountRegistryFixture(const FScopedMountRegistryFixture&) = delete;
		auto operator=(const FScopedMountRegistryFixture&) -> FScopedMountRegistryFixture& = delete;
		auto IsValid() const -> bool { return Error.empty(); }
		auto GetError() const -> const std::string& { return Error; }
	private:
		std::vector<FMountPoint> SavedMounts;
		bool bSavedPublished = false;
		std::string Error;
	};
}
