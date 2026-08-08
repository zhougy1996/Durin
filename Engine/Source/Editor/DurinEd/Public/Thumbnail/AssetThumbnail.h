#pragma once

#include "CoreGlobals.h"
#include "DObject/AssetPath.h"
#include "DurinEdAPI.h"

namespace Durin
{
	namespace Asset
	{
		struct FAssetData;
	}
	class FRHITexture;
	class IRenderedAssetThumbnailGenerationSession;

	namespace Detail
	{
		struct FAssetThumbnailGenerationLeaseState;
		struct FAssetThumbnailProviderRegistryState;
	}

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
		static constexpr std::string_view SphereVirtualPath = "/Engine/Models/Sphere";
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

	// Keeps provider-owned request data behind a core-owned invalidation boundary.
	// Copies share only DurinEd state; provider removal clears the input and session
	// from every copy before the scoped registration handle returns.
	class FAssetThumbnailGenerationLease
	{
	public:
		FAssetThumbnailGenerationLease() = default;

		DURINED_API auto IsActive() const -> bool;
		DURINED_API auto GetInput() const -> const IAssetThumbnailGenerationInput*;
		DURINED_API auto GetRenderedSession() const
			-> IRenderedAssetThumbnailGenerationSession*;
		DURINED_API auto ReleaseRenderedSession() const -> void;

	private:
		friend class FAssetThumbnailProviderRegistry;
		friend struct FAssetThumbnailGenerationRequest;
		explicit FAssetThumbnailGenerationLease(
			std::shared_ptr<Detail::FAssetThumbnailGenerationLeaseState> InState)
			: State(std::move(InState))
		{
		}

		std::shared_ptr<Detail::FAssetThumbnailGenerationLeaseState> State;
	};

	// Transfers an immutable provider request across worker and render-thread boundaries.
	struct FAssetThumbnailGenerationRequest
	{
		FAssetThumbnailKeyInput KeyInput;
		// Capture-only transfer slot. The registry moves this into ProviderLease
		// before a request enters provider-neutral scheduling.
		std::shared_ptr<const IAssetThumbnailGenerationInput> Input;
		FAssetThumbnailGenerationLease ProviderLease;
		FAssetThumbnailCancellation Cancellation;
		uint64 ProviderGeneration = 0;
		uint64 RequestSerial = 0;
		// Provider-selected UI compositing policy; it does not affect persistent identity.
		bool bHasTransparency = true;
		// Provider snapshots revalidated after asset loading and before every resource-dependent publication.
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;

		DURINED_API auto GetInput() const -> const IAssetThumbnailGenerationInput*;
		DURINED_API auto BeginRenderedSession(std::string& OutError) const
			-> IRenderedAssetThumbnailGenerationSession*;
		DURINED_API auto GetRenderedSession() const
			-> IRenderedAssetThumbnailGenerationSession*;
		DURINED_API auto ReleaseRenderedSession() const -> void;
	};

	// Describes one exact-class provider registration and its generator schema.
	struct FAssetThumbnailProviderRegistration
	{
		std::string AssetClassName;
		std::string ProviderName;
		uint32 GeneratorSchemaVersion = 0;
	};

	// Describes a provider-selected source image for one authored asset. The
	// Content Browser keeps decoding, persistence, upload, and presentation generic.
	struct FAssetThumbnailSourceImage
	{
		std::string PhysicalPath;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
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
		virtual auto UsesSourceImage() const -> bool { return false; }
		virtual auto CaptureSourceImage(
			const Asset::FAssetData&,
			FAssetThumbnailSourceImage& OutSource,
			std::string& OutError) -> bool
		{
			OutSource = {};
			OutError.clear();
			return false;
		}
	};

	// Retains one exact-class provider together with the generation captured at registration.
	struct FAssetThumbnailProviderHandle
	{
		uint64 Generation = 0;

		explicit operator bool() const { return Generation != 0; }
	};

	// Owns one exact-class provider registration. Reset first closes admission,
	// invalidates captured generations, and releases provider-owned inputs/sessions.
	class FAssetThumbnailProviderRegistrationHandle
	{
	public:
		FAssetThumbnailProviderRegistrationHandle() = default;
		DURINED_API ~FAssetThumbnailProviderRegistrationHandle();
		FAssetThumbnailProviderRegistrationHandle(
			const FAssetThumbnailProviderRegistrationHandle&) = delete;
		auto operator=(const FAssetThumbnailProviderRegistrationHandle&)
			-> FAssetThumbnailProviderRegistrationHandle& = delete;
		DURINED_API FAssetThumbnailProviderRegistrationHandle(
			FAssetThumbnailProviderRegistrationHandle&& Other) noexcept;
		DURINED_API auto operator=(
			FAssetThumbnailProviderRegistrationHandle&& Other) noexcept
			-> FAssetThumbnailProviderRegistrationHandle&;

		auto IsValid() const -> bool { return RegistrationId != 0 && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
		DURINED_API auto Reset() -> void;

	private:
		friend class FAssetThumbnailProviderRegistry;
		FAssetThumbnailProviderRegistrationHandle(
			std::weak_ptr<Detail::FAssetThumbnailProviderRegistryState> InState,
			uint64 InRegistrationId)
			: State(std::move(InState))
			, RegistrationId(InRegistrationId)
		{
		}

		std::weak_ptr<Detail::FAssetThumbnailProviderRegistryState> State;
		uint64 RegistrationId = 0;
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
		DURINED_API auto RegisterScoped(
			std::unique_ptr<IAssetThumbnailProvider> Provider,
			std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle;
		DURINED_API auto Unregister(std::string_view AssetClassName, std::string& OutError) -> bool;
		DURINED_API auto Find(std::string_view AssetClassName) const -> FAssetThumbnailProviderHandle;
		DURINED_API auto UsesSourceImage(std::string_view AssetClassName) const -> bool;
		DURINED_API auto CaptureSourceImage(
			const Asset::FAssetData& Asset,
			FAssetThumbnailSourceImage& OutSource,
			std::string& OutError) const -> bool;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto IsShuttingDown() const -> bool;
		DURINED_API auto Num() const -> size_t;

	private:
		friend class FAssetThumbnailScheduler;
		auto Capture(
			const FAssetThumbnailRequest& Request,
			uint64 ProviderGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			FAssetThumbnailProviderRegistration& OutRegistration,
			std::string& OutError) -> bool;

		std::shared_ptr<Detail::FAssetThumbnailProviderRegistryState> State;
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
