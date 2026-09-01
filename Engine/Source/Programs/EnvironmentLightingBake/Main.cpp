#include "CoreMinimal.h"

#include "Asset/AssetOperations.h"
#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "Misc/FileHelper.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Serialization/Archive.h"
#include "Threading/Task.h"

namespace
{
	constexpr std::string_view DefaultAssetPath = "/Engine/Renderer/DefaultStudioEnvironment";

	struct FTaskSchedulerGuard
	{
		~FTaskSchedulerGuard() { Durin::ShutdownTaskScheduler(true); }
	};
}

auto main(int ArgumentCount, char** Arguments) -> int
{
	if (ArgumentCount != 2)
	{
		std::cerr << "Usage: EnvironmentLightingBake <Engine/Content directory>\n";
		return 2;
	}

	const std::filesystem::path ContentDirectory =
		std::filesystem::absolute(Arguments[1]).lexically_normal();
	std::error_code ErrorCode;
	std::filesystem::create_directories(ContentDirectory / "Renderer", ErrorCode);
	if (ErrorCode)
	{
		std::cerr << "Failed to create output directory: " << ErrorCode.message() << '\n';
		return 1;
	}

	Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
	Durin::GIsGameThreadIdInitialized = true;
	Durin::FNameInit();
	Durin::DObjectInit();
	if (!Durin::InitializeTaskScheduler())
	{
		std::cerr << "Failed to initialize the Core task scheduler.\n";
		return 1;
	}
	FTaskSchedulerGuard TaskSchedulerGuard;

	std::string Error;
	const Durin::FMountPoint EngineMount{
		.VirtualRoot = "/Engine/",
		.Owner = Durin::EMountOwner::Engine,
		.Root = ContentDirectory.parent_path(),
		.ContentPath = ContentDirectory.filename(),
		.bAutoScan = false,
		.bContentWritable = true};
	if (!Durin::FMountPaths::PublishMountRegistry({&EngineMount, 1}, &Error))
	{
		std::cerr << "Failed to mount Engine Content: " << Error << '\n';
		return 1;
	}
	Durin::InitializeAssetManager();

	std::cout << "Generating default studio environment...\n";
	Durin::FEnvironmentLightingData Data =
		Durin::BuildDefaultStudioEnvironmentData();
	Durin::FByteArray PayloadBytes;
	Durin::FCanonicalMemoryWriter PayloadAr(
		PayloadBytes, Durin::EArchivePurpose::DerivedDataPayload);
	Data.Serialize(PayloadAr);
	if (PayloadAr.HasError())
	{
		std::cerr << "Failed to serialize environment lighting: "
			<< PayloadAr.GetFailure()->Message << '\n';
		return 1;
	}

	Durin::FPackagePath AssetPath;
	if (!Durin::FPackagePath::TryCreate(DefaultAssetPath, AssetPath, &Error))
	{
		std::cerr << "Invalid built-in asset path: " << Error << '\n';
		return 1;
	}
	Durin::FTopLevelAssetPath TopLevelAssetPath;
	if (!Durin::FTopLevelAssetPath::TryCreate(
		AssetPath, AssetPath.GetPackageName(), TopLevelAssetPath, &Error))
	{
		std::cerr << "Invalid built-in top-level asset path: " << Error << '\n';
		return 1;
	}
	Durin::DEnvironmentLighting* Asset = nullptr;
	const Durin::FAssetResult CreateResult =
		Durin::CreateAsset(TopLevelAssetPath, Asset);
	if (!CreateResult)
	{
		std::cerr << "Failed to create environment-lighting asset: "
			<< CreateResult.Message << '\n';
		return 1;
	}

	const std::filesystem::path PayloadPath =
		Durin::DEnvironmentLighting::GetAuthoredPayloadPath(DefaultAssetPath);
	Durin::FFileHelper::FAtomicFileError FileError;
	if (!Durin::FFileHelper::SaveArrayToFileAtomically(PayloadBytes, PayloadPath, &FileError))
	{
		std::cerr << "Failed to write environment-lighting payload: " << FileError.ToString() << '\n';
		return 1;
	}
	const Durin::FAssetResult SaveResult = Durin::SavePackage(Asset->GetPackage());
	if (!SaveResult)
	{
		std::cerr << "Failed to save environment-lighting asset: "
			<< SaveResult.Message << '\n';
		return 1;
	}

	std::cout << "Wrote " << PayloadBytes.size() << " payload bytes to "
		<< PayloadPath.generic_string() << '\n';
	return 0;
}
