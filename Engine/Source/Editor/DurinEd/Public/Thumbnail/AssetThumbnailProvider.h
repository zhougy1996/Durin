#pragma once

#include "Thumbnail/AssetThumbnailKey.h"

namespace Durin
{
	namespace Asset
	{
		struct FAssetData;
	}

	namespace Editor
	{
		class IRenderedAssetThumbnailGenerationSession;

		namespace Detail
		{
			struct FAssetThumbnailGenerationLeaseState;
			struct FAssetThumbnailProviderRegistryState;
		}
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

	// Captures provider-owned immutable generation data on the game thread.
	// Registration and removal are game-thread operations; removal first cancels
	// and drains captured requests.
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

	} // namespace Editor
} // namespace Durin
