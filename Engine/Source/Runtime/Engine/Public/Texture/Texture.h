#pragma once

#include "EngineAPI.h"
#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "RHIResources.h"

#include "Texture.gen.h"

namespace Durin
{
	inline constexpr uint32 TextureSourceSchemaVersion = 1;
	inline constexpr uint64 MaximumTextureSourceBytes = 512ull * 1024ull * 1024ull;

	// Identifies the dimensional interpretation of persistent texture source art.
	DENUM()
	enum class ETextureSourceKind : uint8
	{
		Texture2D,
		TextureCube,
		Volume
	};

	// Identifies the stored texel representation of persistent texture source art.
	DENUM()
	enum class ETextureSourceFormat : uint8
	{
		Invalid,
		RGBA8,
		R8_UNORM,
		RG8_UNORM,
		R16_FLOAT,
		RGBA16_FLOAT
	};

	// Owns editor source art independently from family-specific build inputs.
	DSTRUCT()
	struct FTextureSource
	{
		GENERATED_BODY()

		DPROPERTY()
		FEditorBulkData Payload;

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint32 Depth = 1;

		DPROPERTY()
		uint8 NumSlices = 1;

		DPROPERTY()
		uint8 SourceChannelCount = 0;

		DPROPERTY()
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;

		DPROPERTY()
		ETextureSourceKind Kind = ETextureSourceKind::Texture2D;

		DPROPERTY()
		bool bHasTransparency = false;

		DPROPERTY()
		uint8 TransparencyMask = 0;

		DPROPERTY()
		uint32 SchemaVersion = TextureSourceSchemaVersion;

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto SetPayload(std::span<const std::byte> Bytes) -> bool;
		auto GetPayload() const -> FSharedByteBuffer
		{
			return Payload.GetPayload().Wait().Buffer;
		}
	};

	class FTextureAssetResource;
	class FTextureReference;
	class FTextureResourceCompletion;

	// Tracks the revisioned render-thread lifecycle of a texture resource.
	DENUM()
	enum class ERenderResourceState : uint8
	{
		Idle,
		Pending,
		Building,
		Ready,
		Failed,
		Released,
	};

	// Identifies the current render-resource revision's actionable failure boundary.
	enum class ETextureRenderFailure : uint8
	{
		None,
		UnsupportedFormat,
		CreateOrUpload,
	};

	// Common reflected boundary and render-resource lifecycle owner for texture assets.
	DCLASS(Abstract)
	class DTexture : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API ~DTexture() override;
		ENGINE_API auto BeginDestroy() -> void override;

		ENGINE_API auto GetTextureReferenceRHI() const
			-> FRHITextureReferenceRef;
		ENGINE_API auto GetRenderResourceState() const
			-> ERenderResourceState;
		ENGINE_API auto GetRenderFailure() const -> ETextureRenderFailure;
		ENGINE_API auto GetAppliedRenderRevision() const -> uint64;
		auto GetBuildRevision() const -> uint64 { return BuildRevision; }
		auto GetSource() const -> const FTextureSource& { return Source; }
		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		// Accepts validated import data owned by this texture as an inner object.
		ENGINE_API auto SetAssetImportData(
			DAssetImportData& Value, std::string& OutError) -> bool;
		// Queries installed CPU data without loading bulk data or updating resources.
		virtual auto HasPlatformData() const -> bool = 0;
		auto GetCookedPlatformData() const -> const FBulkData&
		{
			return CookedPlatformData;
		}
		// GameThread only. Loads and installs cooked data synchronously when absent,
		// then calls UpdateResource (GPU completion is asynchronous). Does not build
		// authored data. Already-installed data succeeds without another update.
		// On failure, logs the texture path and reason and returns false.
		ENGINE_API auto EnsurePlatformDataLoadedBlocking() -> bool;
		// Replaces the current concrete resource from installed platform data.
		ENGINE_API auto UpdateResource() -> void;

	protected:
		ENGINE_API explicit DTexture(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto SetSource(FTextureSource Value, std::string& OutError) -> bool;
		// Restricted to family serializers and blocking loaders.
		auto GetMutableCookedPlatformData() -> FBulkData&
		{
			return CookedPlatformData;
		}

		virtual auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> = 0;
		// Installs family-specific cooked data and queues its resource update.
		virtual auto LoadCookedPlatformData(std::string& OutError) -> bool = 0;

	private:
		auto ReleaseRenderResources() -> void;

		std::unique_ptr<FTextureReference> TextureReference;
		std::unique_ptr<FTextureAssetResource> RenderResource;
		std::shared_ptr<FTextureResourceCompletion> RenderCompletion;
		bool bTextureReferenceInitializationQueued = false;
		bool bAcceptingRenderResourceBuilds = true;
		uint64 BuildRevision = 0;

		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FTextureSource Source;

		FBulkData CookedPlatformData;

	};
}
