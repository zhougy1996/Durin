#pragma once

#include "DObject/AssetPath.h"
#include "DurinEdAPI.h"

namespace Durin
{
	class FRHITexture;

	// Identifies the public lifecycle state of one provider-neutral thumbnail request.
	enum class EAssetThumbnailState : uint8
	{
		NotRequested,
		Queued,
		Loading,
		WaitingForResources,
		Rendering,
		Readback,
		Encoding,
		Uploading,
		Ready,
		Invalid,
		Failed
	};

	// Selects scheduler ordering without changing thumbnail cache identity.
	enum class EAssetThumbnailPriority : uint8
	{
		Prefetch,
		Visible
	};

	// Selects the fixed TextureCube direction convention used by rendered previews.
	enum class EAssetThumbnailCubeDirectionConvention : uint8
	{
		WorldSpaceReflectionVector = 1
	};

	// Captures the unloaded package fields that invalidate an asset thumbnail.
	struct FAssetThumbnailPackageFingerprint
	{
		FAssetPath VirtualPath;
		std::string AssetClassName;
		uint32 PackageFormatVersion = 0;
		uint64 FileSize = 0;
		int64 LastWriteTimeTicks = 0;

		auto operator==(const FAssetThumbnailPackageFingerprint&) const -> bool = default;
	};

	// Describes one package and its direct Asset Registry dependency edges.
	struct FAssetThumbnailDependencyNode
	{
		FAssetThumbnailPackageFingerprint Package;
		std::vector<FAssetPath> Dependencies;
	};

	// Defines output settings that participate directly in persistent cache identity.
	struct FAssetThumbnailOutputSettings
	{
		uint32 Width = 256;
		uint32 Height = 256;
		uint32 ColorSpaceVersion = 1;
		uint32 EncodingVersion = 1;

		auto operator==(const FAssetThumbnailOutputSettings&) const -> bool = default;
	};

	// Freezes the initial rendered-thumbnail visual fixture behind one schema version.
	struct FRenderedAssetThumbnailVisualContract
	{
		static constexpr uint32 SchemaVersion = 1;
		static constexpr std::string_view SphereVirtualPath = "/Engine/Editor/MaterialPreview/Sphere";
		static constexpr uint32 SphereFixtureVersion = 1;
		static constexpr std::string_view TextureCubeEnvironmentViewIdentity =
			"/Engine/Editor/TextureCubePreview/WideEnvironment";
		static constexpr uint32 TextureCubeEnvironmentViewVersion = 1;
		static constexpr std::string_view OutputEncoding = "PNG";
		static constexpr std::string_view WorkingColorSpace = "Linear-sRGB";
		static constexpr std::string_view OutputColorSpace = "sRGB";

		FAssetThumbnailOutputSettings Output;
		float BackgroundRed = 0.18f;
		float BackgroundGreen = 0.18f;
		float BackgroundBlue = 0.18f;
		float BackgroundAlpha = 1.0f;
		float CameraDirectionX = 2.6f;
		float CameraDirectionY = -2.6f;
		float CameraDirectionZ = 1.8f;
		float CameraDistance = 4.1f;
		float VerticalFieldOfViewDegrees = 42.0f;
		float NearClipDistance = 0.1f;
		float FarClipDistance = 100.0f;
		float KeyLightDirectionX = -2.6f;
		float KeyLightDirectionY = 2.6f;
		float KeyLightDirectionZ = -2.4f;
		float KeyLightIntensity = 1.0f;
		float FillLightIntensity = 0.15f;
		float Exposure = 1.0f;
		float SphereUniformScale = 1.0f;
		float SphereRotationPitchDegrees = 0.0f;
		float SphereRotationYawDegrees = 0.0f;
		float SphereRotationRollDegrees = 0.0f;
		uint32 PostProcessVersion = 1;
		bool bEditorAssistanceEnabled = false;
		bool bOutputOpaque = true;
		EAssetThumbnailCubeDirectionConvention CubeDirectionConvention =
			EAssetThumbnailCubeDirectionConvention::WorldSpaceReflectionVector;
	};

	// Bounds scheduler work and retained CPU, GPU, and persistent cache resources.
	struct FAssetThumbnailBudgets
	{
		uint32 MaximumQueuedJobs = 512;
		uint32 MaximumConcurrentSourceDecodes = 4;
		uint32 MaximumUploadsPerFrame = 2;
		uint32 MaximumRendersPerFrame = 1;
		uint32 MaximumLivePreviewScenes = 1;
		uint64 CpuPixelBudgetBytes = 64ull * 1024ull * 1024ull;
		uint64 GpuTextureBudgetBytes = 64ull * 1024ull * 1024ull;
		uint64 MaximumEncodedObjectBytes = 16ull * 1024ull * 1024ull;
		uint64 DiskBudgetBytes = 256ull * 1024ull * 1024ull;
	};

	// Contains every provider-neutral field used to derive one persistent cache key.
	struct FAssetThumbnailKeyInput
	{
		FAssetThumbnailPackageFingerprint Asset;
		std::string ProviderName;
		uint32 GeneratorSchemaVersion = 0;
		FAssetThumbnailOutputSettings Output;
		std::string PreviewFixtureIdentity;
		uint32 PreviewFixtureVersion = 0;
		uint32 ShaderContractVersion = 0;
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
	};

	// Identifies one request independently from provider-owned generation data.
	struct FAssetThumbnailRequest
	{
		FAssetThumbnailPackageFingerprint Asset;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
		uint64 RequestSerial = 0;
	};

	// Provides cooperative cancellation shared by scheduler and asynchronous completions.
	class FAssetThumbnailCancellation
	{
	public:
		DURINED_API FAssetThumbnailCancellation();

		DURINED_API auto Cancel() const -> void;
		DURINED_API auto IsCancelled() const -> bool;

	private:
		std::shared_ptr<std::atomic<bool>> State;
	};

	// Owns provider-specific immutable data captured on the game thread.
	class IAssetThumbnailGenerationInput
	{
	public:
		virtual ~IAssetThumbnailGenerationInput() = default;
	};

	// Transfers an immutable provider request across worker and render-thread boundaries.
	struct FAssetThumbnailGenerationRequest
	{
		FAssetThumbnailKeyInput KeyInput;
		std::shared_ptr<const IAssetThumbnailGenerationInput> Input;
		FAssetThumbnailCancellation Cancellation;
		uint64 ProviderGeneration = 0;
		uint64 RequestSerial = 0;
		// Provider snapshots revalidated after asset loading and before every resource-dependent publication.
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;
	};

	// Describes one exact-class provider registration and its generator schema.
	struct FAssetThumbnailProviderRegistration
	{
		std::string AssetClassName;
		std::string ProviderName;
		uint32 GeneratorSchemaVersion = 0;
	};

	// Captures provider-owned immutable generation data on the game thread. Registration and
	// removal are game-thread operations; removal first cancels and drains captured requests.
	class IAssetThumbnailProvider
	{
	public:
		virtual ~IAssetThumbnailProvider() = default;

		virtual auto GetRegistration() const -> FAssetThumbnailProviderRegistration = 0;
		virtual auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool = 0;
	};

	// Retains one exact-class provider together with the generation captured at registration.
	struct FAssetThumbnailProviderHandle
	{
		std::shared_ptr<IAssetThumbnailProvider> Provider;
		uint64 Generation = 0;

		explicit operator bool() const { return Provider != nullptr; }
	};

	// Owns exact-class provider registration and monotonically increasing provider generations.
	class FAssetThumbnailProviderRegistry
	{
	public:
		DURINED_API FAssetThumbnailProviderRegistry();
		DURINED_API ~FAssetThumbnailProviderRegistry();

		FAssetThumbnailProviderRegistry(const FAssetThumbnailProviderRegistry&) = delete;
		FAssetThumbnailProviderRegistry& operator=(const FAssetThumbnailProviderRegistry&) = delete;

		DURINED_API auto Register(
			std::shared_ptr<IAssetThumbnailProvider> Provider,
			std::string& OutError) -> bool;
		DURINED_API auto Unregister(std::string_view AssetClassName, std::string& OutError) -> bool;
		DURINED_API auto Find(std::string_view AssetClassName) const -> FAssetThumbnailProviderHandle;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto IsShuttingDown() const -> bool;
		DURINED_API auto Num() const -> size_t;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Transfers one captured, coalesced request from the provider-neutral queue to a generation backend.
	struct FAssetThumbnailScheduledJob
	{
		std::string CacheKey;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
		FAssetThumbnailGenerationRequest GenerationRequest;
	};

	struct FAssetThumbnailView;

	// Captures exact-class provider requests, coalesces cache keys, and enforces queue ordering and bounds.
	class FAssetThumbnailScheduler
	{
	public:
		DURINED_API explicit FAssetThumbnailScheduler(
			FAssetThumbnailProviderRegistry& Registry,
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FAssetThumbnailScheduler();

		FAssetThumbnailScheduler(const FAssetThumbnailScheduler&) = delete;
		FAssetThumbnailScheduler& operator=(const FAssetThumbnailScheduler&) = delete;

		DURINED_API auto Request(const FAssetThumbnailRequest& Request, std::string& OutError) -> bool;
		DURINED_API auto Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto TakeNext() -> std::optional<FAssetThumbnailScheduledJob>;
		// Advances a captured job only while its key, provider generation, serial, identity, and revisions remain current.
		DURINED_API auto Transition(
			const FAssetThumbnailScheduledJob& Job,
			EAssetThumbnailState ExpectedState,
			EAssetThumbnailState NextState,
			uint64 AssetRevision = 0,
			uint64 ResourceRevision = 0,
			std::string_view Diagnostic = {}) -> bool;
		DURINED_API auto Cancel(const FAssetPath& AssetPath) -> void;
		DURINED_API auto CancelAll() -> void;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto NumQueued() const -> size_t;
		DURINED_API auto IsShuttingDown() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Exposes one service-owned result without transferring UI texture ownership.
	struct FAssetThumbnailView
	{
		EAssetThumbnailState State = EAssetThumbnailState::NotRequested;
		FRHITexture* Texture = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bHasTransparency = false;
		bool bShowTransparencyGrid = true;
		std::string Diagnostic;
		uint64 RequestSerial = 0;
	};

	// Builds a sorted transitive dependency snapshot; missing or conflicting registry data is invalid.
	DURINED_API auto BuildAssetThumbnailDependencyClosure(
		const FAssetPath& Root,
		std::span<const FAssetThumbnailDependencyNode> RegistrySnapshot,
		std::vector<FAssetThumbnailPackageFingerprint>& OutDependencies,
		std::string& OutError) -> bool;

	// Hashes explicit little-endian fields rather than formatted text or native struct memory.
	DURINED_API auto BuildAssetThumbnailCacheKey(const FAssetThumbnailKeyInput& Input) -> std::string;
} // namespace Durin
