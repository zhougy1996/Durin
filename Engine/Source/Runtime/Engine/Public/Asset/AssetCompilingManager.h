#pragma once

#include "Delegates/Delegate.h"
#include "DObject/WeakObjectPtr.h"
#include "EngineAPI.h"
#include "Misc/Name.h"
#include "Modules/ModularFeature.h"

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Durin
{
	class DObject;

	struct FAssetCompileProcessParams
	{
		uint32 MaximumCompletions = 64;
		std::optional<std::chrono::steady_clock::time_point> Deadline;
	};

	struct FAssetCompileProcessResult
	{
		uint32 ProcessedCompletionCount = 0;
		std::vector<FWeakObjectPtr> SuccessfullyCompiledAssets;
	};

	struct FAssetCompileDomainDiagnostics
	{
		FName DomainName;
		uint64 RemainingAssetCount = 0;
	};

	struct FAssetCompilingManagerDiagnostics
	{
		uint32 ManagerCount = 0;
		uint64 RemainingAssetCount = 0;
		uint64 ProcessedCompletionCount = 0;
		bool bAcceptingRequests = false;
		bool bShutdown = false;
		std::vector<FAssetCompileDomainDiagnostics> Domains;
		std::vector<std::string> Messages;
	};

	struct FAssetPostCompileData
	{
		FName DomainName;
		std::vector<FWeakObjectPtr> Assets;
	};

	DECLARE_MULTICAST_DELEGATE_OneParam(FAssetPostCompileEvent, const FAssetPostCompileData&)

	class IAssetCompilingManager
	{
	public:
		virtual ~IAssetCompilingManager() = default;
		virtual auto GetAssetTypeName() const -> FName = 0;
		virtual auto GetDependentTypeNames() const -> std::vector<FName> { return {}; }
		virtual auto Start(std::string* OutError) -> bool = 0;
		virtual auto StopAdmission() -> void = 0;
		virtual auto GetNumRemainingAssets() const -> uint64 = 0;
		virtual auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
			-> FAssetCompileProcessResult = 0;
		virtual auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> FAssetCompileProcessResult = 0;
		virtual auto MarkCompilationAsCanceled(std::span<DObject* const> Objects)
			-> void = 0;
		virtual auto FinishAllCompilation() -> FAssetCompileProcessResult = 0;
		virtual auto Shutdown() -> void = 0;
	};

	class FAssetCompilingManagerRegistration
	{
	public:
		FAssetCompilingManagerRegistration() = default;
		ENGINE_API ~FAssetCompilingManagerRegistration();
		FAssetCompilingManagerRegistration(const FAssetCompilingManagerRegistration&) = delete;
		auto operator=(const FAssetCompilingManagerRegistration&)
			-> FAssetCompilingManagerRegistration& = delete;
		ENGINE_API FAssetCompilingManagerRegistration(
			FAssetCompilingManagerRegistration&& Other) noexcept;
		ENGINE_API auto operator=(FAssetCompilingManagerRegistration&& Other) noexcept
			-> FAssetCompilingManagerRegistration&;
		ENGINE_API auto Reset() -> void;
		[[nodiscard]] auto IsValid() const -> bool { return Generation != 0; }

	private:
		FName DomainName;
		uint64 Generation = 0;

		friend class FAssetCompilingManager;
		friend ENGINE_API auto InitializeAssetCompilingManager() -> bool;
	};

	class FAssetCompilingManager final
	{
	public:
		ENGINE_API static auto Get() -> FAssetCompilingManager&;
		ENGINE_API auto Start(std::string* OutError = nullptr) -> bool;
		ENGINE_API auto RegisterManager(
			std::shared_ptr<IAssetCompilingManager> Manager,
			FModuleOwnedCallbackGate OwnerGate,
			std::string* OutError = nullptr) -> FAssetCompilingManagerRegistration;
		ENGINE_API auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params = {})
			-> FAssetCompileProcessResult;
		ENGINE_API auto GetNumRemainingAssets() const -> uint64;
		ENGINE_API auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> FAssetCompileProcessResult;
		ENGINE_API auto MarkCompilationAsCanceled(std::span<DObject* const> Objects)
			-> bool;
		ENGINE_API auto FinishAllCompilation() -> FAssetCompileProcessResult;
		ENGINE_API auto GetDiagnostics() const -> FAssetCompilingManagerDiagnostics;
		ENGINE_API auto OnAssetPostCompile() -> FAssetPostCompileEvent&;
		ENGINE_API auto Shutdown() -> void;
		ENGINE_API auto IsAcceptingRequests() const -> bool;

	private:
		FAssetCompilingManager() = default;
		auto RegisterBuiltInManager(
			std::shared_ptr<IAssetCompilingManager> Manager,
			std::string* OutError) -> FAssetCompilingManagerRegistration;
		auto Unregister(FName DomainName, uint64 Generation) -> void;

		friend class FAssetCompilingManagerRegistration;
		friend ENGINE_API auto InitializeAssetCompilingManager() -> bool;
	};

	ENGINE_API auto InitializeAssetCompilingManager() -> bool;
	ENGINE_API auto ShutdownAssetCompilingManager() -> void;
}
