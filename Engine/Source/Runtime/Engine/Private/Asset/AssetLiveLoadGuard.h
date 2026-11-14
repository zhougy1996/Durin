#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"


namespace Durin::AssetPrivate
{
	// Synchronous owner-thread boundary. A rejected live read poisons every active
	// enclosing guard even if user code ignores the immediate failure.
	class FAssetLiveLoadGuard
	{
	public:
		explicit FAssetLiveLoadGuard(bool bEnabled);
		~FAssetLiveLoadGuard();
		FAssetLiveLoadGuard(const FAssetLiveLoadGuard&) = delete;
		auto operator=(const FAssetLiveLoadGuard&) -> FAssetLiveLoadGuard& = delete;
		static auto Check(std::string_view Operation, std::string_view Path) -> std::expected<void, Durin::UnifiedError::FError>;
		auto GetFailure() const -> std::expected<void, Durin::UnifiedError::FError>;

	private:
		static thread_local FAssetLiveLoadGuard* Active;
		static std::atomic_uint64_t ActiveCount;
		static std::atomic_uint64_t Rejections;
		uint64 InitialRejections = 0;
		FAssetLiveLoadGuard* Previous = nullptr;
		bool bEnabled = false;
		std::expected<void, Durin::UnifiedError::FError> Failure;
	};
}
