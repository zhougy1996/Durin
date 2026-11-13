#pragma once

#include "CoreMinimal.h"
#include "SceneView.h"
#include "Thumbnail/AssetThumbnailTypes.h"

namespace Durin
{
	class DObject;
	class DClass;
	class FThumbnailPreviewScene;
	class FThumbnailRenderJob;
	struct FThumbnailDrawResult;

	// Describes a renderer-selected camera without exposing concrete asset types.
	struct FThumbnailPreviewView
	{
		std::array<double, 3> CameraPosition{};
		std::array<double, 3> CameraForward{1.0, 0.0, 0.0};
		std::array<double, 3> CameraRight{0.0, 1.0, 0.0};
		std::array<double, 3> CameraUp{0.0, 0.0, 1.0};
		double VerticalFieldOfViewDegrees = 42.0;
		double NearClipDistance = 0.1;
		double FarClipDistance = 100.0;
		bool bForceLOD0 = false;
		float ClearRed = 0.0f;
		float ClearGreen = 0.0f;
		float ClearBlue = 0.0f;
		float ClearAlpha = 0.0f;
	};

	// Supplies capture output and view setup without owning renderer scenes.
	// Optional image renderers return SRGBA8 under the same capture/readback budget as scenes.
	using FThumbnailImageRenderer = std::function<FTextureRHIRef(
		FRHICommandListImmediate&, uint32, uint32)>;

	// One pool-owned draw attempt. Asset loads, snapshots and scene reservations
	// live here until readback completes; renderers supply no session factory.
	class FThumbnailDrawContext final
	{
	public:
		DURINED_API explicit FThumbnailDrawContext(DObject* Object, FTopLevelAssetPath Path = {});
		DURINED_API ~FThumbnailDrawContext();
		FThumbnailDrawContext(const FThumbnailDrawContext&) = delete;
		FThumbnailDrawContext& operator=(const FThumbnailDrawContext&) = delete;
		template<typename T, typename... Args> auto GetState(Args&&... Values) -> T&
		{
			static const char Tag = 0;
			checkf(!State || StateType == &Tag, "A thumbnail draw context belongs to one renderer state type.");
			if (!State) { State = std::make_shared<T>(std::forward<Args>(Values)...); StateType = &Tag; }
			return *static_cast<T*>(State.get());
		}
		DURINED_API auto GetAssetPath() const -> const FTopLevelAssetPath&;
		DURINED_API auto LoadAsset(const FSoftObjectPath& Path, DClass* Class) -> DObject*;
		// A draw starts waiting; only completed setup marks it ready.
		DURINED_API auto MarkReady() -> void;
		DURINED_API auto Fail(std::string Diagnostic) -> void;
		DURINED_API auto SetValidation(std::function<bool()> Validator) -> void;
		// False defers for a busy scene; an unavailable scene fails this draw.
		DURINED_API auto TrySetScene(FThumbnailPreviewScene& Scene) -> bool;
		DURINED_API auto SetView(const FThumbnailPreviewView& View) -> void;
		DURINED_API auto SetViewEnvironment(const FViewEnvironmentOverride& Environment) -> void;
		DURINED_API auto SetImageRenderer(FThumbnailImageRenderer Renderer) -> void;
	private:
		friend class FThumbnailRenderJob;
		auto ResetResult() -> void;
		auto GetResult() const -> FThumbnailDrawResult;
		auto Validate() const -> std::expected<void, std::string>;
		auto BindCapture(class FThumbnailCapture& Capture) -> std::expected<void, std::string>;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		std::shared_ptr<void> State;
		const void* StateType = nullptr;
	};
} // namespace Durin
