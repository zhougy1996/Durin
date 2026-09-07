#pragma once

#include "Asset/AssetDefinitions.h"

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
		static auto Check(std::string_view Operation, std::string_view Path) -> FAssetResult;
		auto GetFailure() const -> FAssetResult;

	private:
		static thread_local FAssetLiveLoadGuard* Active;
		static std::atomic_uint64_t ActiveCount;
		static std::atomic_uint64_t Rejections;
		uint64 InitialRejections = 0;
		FAssetLiveLoadGuard* Previous = nullptr;
		bool bEnabled = false;
		FAssetResult Failure;
	};
}
