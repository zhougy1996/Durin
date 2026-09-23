#pragma once

#include "Modules/ModuleManager.h"
#include "Texture/Texture2DBuildTypes.h"
#include "Texture/TextureCubeBuild.h"
#include "Texture/VolumeTextureBuild.h"

namespace Durin
{
	// Fixed Developer module contract. Recipes return detached CPU values only.
	class ITextureBuildModule : public IModuleInterface
	{
	public:
		virtual auto GetTexture2DDescriptor() const -> FTexture2DBuildDescriptor = 0;
		virtual auto GetTextureCubeDescriptor() const -> FTextureCubeBuildDescriptor = 0;
		virtual auto GetVolumeTextureDescriptor() const -> FVolumeTextureBuildDescriptor = 0;
		virtual auto BuildTexture2D(const FTexture2DRecipeBuildRequest& Request,
			const FTexture2DRecipeExecutionControl* Control = nullptr)
			-> std::expected<FTexture2DRecipeBuildProduct, FTexture2DBuildError> = 0;
		virtual auto NormalizeTextureCube(const FTextureCubeBuildRequest& Request)
			-> std::expected<FTextureCubeCanonicalBuildInput, FTextureBuildError> = 0;
		virtual auto BuildTextureCube(const FTextureCubeRecipeBuildRequest& Request)
			-> std::expected<FTextureCubeRecipeBuildProduct, FTextureBuildError> = 0;
		virtual auto BuildVolumeTexture(const FVolumeTextureRecipeBuildRequest& Request)
			-> std::expected<FVolumeTextureRecipeBuildProduct, FTextureBuildError> = 0;
	};

	class FTextureBuildSession
	{
	public:
		// Acquire on the module-control thread before dispatching worker work.
		// Unload is rejected until consumers drain work and release their sessions.
		ENGINE_API static auto Acquire() -> FTextureBuildSession;
		explicit operator bool() const { return Module != nullptr && CodeLease != nullptr; }
		auto GetModule() const -> ITextureBuildModule& { return *Module; }
		auto GetGeneration() const -> uint64 { return Generation; }

	private:
		std::shared_ptr<void> CodeLease;
		ITextureBuildModule* Module = nullptr;
		uint64 Generation = 0;
	};
}
