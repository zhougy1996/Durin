#include "Misc/MountPathTestSupport.h"

#include "Misc/MountPathsInternal.h"
#include "Misc/StringHelper.h"
#include "Threading/RunnableThread.h"

namespace Durin::Testing
{
	auto RegisterMountPointForTests(
		std::string_view VirtualRoot,
		std::string_view PhysicalPath,
		bool bAutoScan,
		bool bContentWritable
	) -> void
	{
		checkf(IsInGameThread(), "RegisterMountPointForTests must be called from the game thread.");
		checkf(!MountPathInternal::RegistryPublished(), "Mount registry is immutable after publication.");
		std::error_code DirectoryError;
		std::filesystem::path Root = std::filesystem::absolute(PhysicalPath, DirectoryError);
		if (!DirectoryError)
		{
			Root = Root.lexically_normal();
			if (Root.filename().empty()) Root = Root.parent_path();
		}
		checkf(!DirectoryError, "Failed to normalize test mount root.");
		std::filesystem::create_directories(Root, DirectoryError);
		checkf(!DirectoryError, "Failed to create test mount root.");
		auto& MountPoints = MountPathInternal::MutableMountPoints();
		const auto Existing = std::ranges::find_if(MountPoints, [&](const FMountPoint& Mount) {
			return StringUtils::FoldAscii(Mount.VirtualRoot)
				== StringUtils::FoldAscii(VirtualRoot);
		});
		FMountPoint Definition{
			.VirtualRoot = std::string(VirtualRoot),
			.Owner = EMountOwner::Test,
			.Root = Root,
			.bAutoScan = bAutoScan,
			.bContentWritable = bContentWritable};
		if (Existing == MountPoints.end()) MountPoints.push_back(std::move(Definition));
		else *Existing = std::move(Definition);
		std::ranges::sort(MountPoints, [](const FMountPoint& A, const FMountPoint& B) {
			return A.VirtualRoot.length() > B.VirtualRoot.length();
		});
	}

	FScopedMountRegistryFixture::FScopedMountRegistryFixture()
		: SavedMounts(MountPathInternal::MutableMountPoints())
		, bSavedPublished(MountPathInternal::RegistryPublished())
	{
		MountPathInternal::MutableMountPoints().clear();
		MountPathInternal::RegistryPublished() = false;
	}

	FScopedMountRegistryFixture::FScopedMountRegistryFixture(std::span<const FMountPoint> Definitions)
		: FScopedMountRegistryFixture()
	{
		FMountPaths::PublishMountRegistry(Definitions, &Error);
	}

	FScopedMountRegistryFixture::~FScopedMountRegistryFixture()
	{
		MountPathInternal::MutableMountPoints() = std::move(SavedMounts);
		MountPathInternal::RegistryPublished() = bSavedPublished;
	}
}
