#include "CoreMinimal.h"
#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageSerialization.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Math/Operations.h"
#include "Misc/MountPaths.h"
#include "Modules/ModuleManager.h"
#include "Texture/TextureCubeBuildProvider.h"
#include "Threading/Task.h"

namespace
{
	using namespace Durin;

	constexpr uint32 PanoramaWidth = 512;
	constexpr uint32 PanoramaHeight = 256;
	constexpr uint32 CubeFaceDimension = 128;

	// Preserve the Studio source recipe independently of runtime sky evaluation.
	auto MakeStudioPanorama() -> FTextureCubePanoramaFloatImage
	{
		FTextureCubePanoramaFloatImage Panorama;
		Panorama.Width = PanoramaWidth;
		Panorama.Height = PanoramaHeight;
		Panorama.Pixels.resize(PanoramaWidth * PanoramaHeight * 3);
		const auto KeyDirection = Math::Normalize(FVector3f(-0.4f, 0.5f, 0.75f));
		for (uint32 Y = 0; Y < PanoramaHeight; ++Y)
		{
			for (uint32 X = 0; X < PanoramaWidth; ++X)
			{
				const float Longitude = ((float(X) + 0.5f) / float(PanoramaWidth) - 0.5f) * 2.f * Math::Pi<float>();
				const float Latitude = (0.5f - (float(Y) + 0.5f) / float(PanoramaHeight)) * Math::Pi<float>();
				const FVector3f Direction(std::cos(Latitude) * std::cos(Longitude),
					std::cos(Latitude) * std::sin(Longitude), std::sin(Latitude));
				const float Amount = std::clamp(Direction.z * 0.5f + 0.5f, 0.f, 1.f);
				const auto Base = Math::Lerp(FVector3f(.025f, .020f, .018f), FVector3f(.18f, .28f, .50f), Amount);
				const float Key = std::pow(std::clamp(Math::Dot(Direction, KeyDirection), 0.f, 1.f), 256.f);
				const auto Color = Base + 6.f * FVector3f(1, .78f, .55f) * Key;
				const size_t Index = (Y * PanoramaWidth + X) * 3;
				Panorama.Pixels[Index] = Color.x;
				Panorama.Pixels[Index + 1] = Color.y;
				Panorama.Pixels[Index + 2] = Color.z;
			}
		}
		return Panorama;
	}

	// Drain asset compilation before shutting down its scheduler.
	struct FCompilationCleanup
	{
		~FCompilationCleanup()
		{
			ShutdownAssetCompilingManager();
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
		}
	};
}

auto main(int Count, char** Args) -> int
{
	using namespace Durin;
	if (Count != 2)
	{
		std::cerr << "Usage: StudioCubeGenerate <Engine/Content>\n";
		return 2;
	}
	GGameThreadId = FPlatformLTS::GetCurrentThreadId();
	GIsGameThreadIdInitialized = true;
	FNameInit();
	DObjectInit();
	if (!InitializeTaskScheduler(2) || !InitializeGameThreadDeferredExecutor() || !InitializeAssetCompilingManager())
		return 1;
	FCompilationCleanup Cleanup;

	const auto Content = std::filesystem::absolute(Args[1]).lexically_normal();
	std::vector<FMountPoint> Mounts{{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Engine,
		.Root = Content.parent_path(), .ContentPath = Content.filename(), .bAutoScan = true, .bContentWritable = true}};
	std::string Error;
	if (!FMountPaths::PublishMountRegistry(Mounts, &Error))
	{
		std::cerr << Error;
		return 1;
	}
	InitializeAssetManager();
	FModuleManager::Get().LoadModuleChecked("TextureBuild");
	if (!RefreshAssetRegistry()) return 1;

	FObjectPath CubeObjectPath;
	const bool ValidPath = FObjectPath::TryCreate("/Engine/Renderer/DefaultStudioCube.DefaultStudioCube", CubeObjectPath);
	require(ValidPath);
	auto* Package = CreatePackage(CubeObjectPath.GetPackagePath());
	if (!Package) return 1;
	FStaticConstructObjectParameters Construction{DTextureCube::StaticClass(), Package,
		FName("DefaultStudioCube"), sizeof(DTextureCube), EObjectFlags::Public};
	auto* Object = StaticConstructObject(Construction);
	DObjectForceRegistration(Object);
	auto* Cube = Cast<DTextureCube>(Object);
	if (!Cube) return 1;
	Package->MarkDirty();
	Package->MarkAsNewlyCreated();

	FTextureCubeBuildRequest Request;
	Request.Input = FTextureCubePanoramaBuildInput{MakeStudioPanorama(),
		{.FaceDimension = CubeFaceDimension, .Output = ETextureCubeOutput::HDR}};
	const auto Built = BuildTextureCubeSynchronously(*Cube, Request, {});
	if (!Built)
	{
		std::cerr << Built.Diagnostic;
		return 1;
	}
	const auto Saved = SavePackage(Package);
	if (!Saved)
	{
		std::cerr << Saved.Message;
		return 1;
	}
	std::cout << "Generated /Engine/Renderer/DefaultStudioCube (linear RGBA32F, "
		<< CubeFaceDimension << " pixels per face).\n";
	return 0;
}
