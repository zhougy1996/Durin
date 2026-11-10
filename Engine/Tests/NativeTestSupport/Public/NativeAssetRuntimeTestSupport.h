#pragma once

#include "Asset/Load.h"
#include "DObject/ObjectLifecycle.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <utility>

namespace Durin::Testing
{
	// Engine-backed hosts explicitly establish the asset domain before CDO creation.
	// Keep the CoreDObject-only helper independent of Engine.
	inline auto InitializeAssetSystemForTests() -> void
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		static const bool bInitialized = [] {
			if (!IsFNameInitialized()) FNameInit();
			const auto Result = InitializeAssetRuntime();
			requiref(Result, "Failed to initialize test asset runtime: {}", Result.Message);
			return true;
		}();
		(void)bInitialized;
		InitializeDObjectSystemForTests();
	}

	// GameThread-only test scope. Restores the entry configuration, not loaded
	// objects, including when a fatal assertion exits the owning test early.
	class FScopedAssetRuntimeForTests
	{
	public:
		FScopedAssetRuntimeForTests()
			: OriginalConfiguration(GetAssetRuntimeConfiguration()) {}
		FScopedAssetRuntimeForTests(const FScopedAssetRuntimeForTests&) = delete;
		auto operator=(const FScopedAssetRuntimeForTests&)
			-> FScopedAssetRuntimeForTests& = delete;

		~FScopedAssetRuntimeForTests()
		{
			EXPECT_TRUE(Restore()) << "Failed to restore the test asset runtime.";
		}

		[[nodiscard]] auto RestartCooked(const std::filesystem::path& CookRoot)
			-> ::testing::AssertionResult
		{
			auto Configuration = FAssetRuntimeConfiguration::Authored();
			const auto Result = FAssetRuntimeConfiguration::Cooked(CookRoot, Configuration);
			if (!Result) return ::testing::AssertionFailure() << Result.Message;
			bNeedsRestore = true;
			return Restart(std::move(Configuration));
		}

		[[nodiscard]] auto Restore() -> ::testing::AssertionResult
		{
			if (!bNeedsRestore) return ::testing::AssertionSuccess();
			const auto Result = Restart(OriginalConfiguration);
			if (Result) bNeedsRestore = false;
			return Result;
		}

	private:
		static auto Restart(FAssetRuntimeConfiguration Configuration) -> ::testing::AssertionResult
		{
			ShutdownAssetRuntime();
			CollectGarbage(Durin::EObjectFlags::Standalone);
			const FAssetWriteResult Result = InitializeAssetRuntime(std::move(Configuration));
			if (!Result) return ::testing::AssertionFailure() << Result.Message;
			return ::testing::AssertionSuccess();
		}

		const FAssetRuntimeConfiguration OriginalConfiguration;
		bool bNeedsRestore = false;
	};
}
