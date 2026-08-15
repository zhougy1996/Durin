#pragma once

#include "Modules/ModuleManager.h"

namespace Durin
{
	// Owns one isolated test generation for registry and async-operation tests.
	class FModuleTestOwner final
	{
	public:
		CORE_API explicit FModuleTestOwner(FName ModuleName);

		FModuleTestOwner(FModuleTestOwner&&) noexcept = default;
		auto operator=(FModuleTestOwner&&) noexcept -> FModuleTestOwner& = default;
		FModuleTestOwner(const FModuleTestOwner&) = delete;
		auto operator=(const FModuleTestOwner&) -> FModuleTestOwner& = delete;

		template<CModularFeature T>
		auto RegisterFeature(T& Implementation) -> FModularFeatureRegistration
		{
			return FModularFeatureRegistry::Get().Register(
				ModuleOwner,
				{FName(std::string_view(T::FeatureName)), static_cast<uint32>(T::FeatureVersion)},
				Implementation);
		}

		auto CreateAsyncOperationGroup(
			FName GroupName,
			FAsyncOperationGroupOptions Options = {}) -> FAsyncOperationGroup
		{
			return Detail::CreateAsyncOperationGroup(
				ModuleOwner, std::move(GroupName), Options);
		}

		auto CreateOwnedCallbackRegistration(FName DomainName)
			-> FModuleOwnedCallbackRegistration
		{
			return FModularFeatureRegistry::Get().RegisterOwnedCallback(
				ModuleOwner, std::move(DomainName));
		}

		CORE_API auto BeginRetirement(
			std::chrono::milliseconds Timeout = std::chrono::seconds(5))
			-> FModularFeatureRetirementResult;
		CORE_API auto DrainAsyncOperations(
			std::chrono::milliseconds Timeout = std::chrono::seconds(5))
			-> FAsyncOperationDrainResult;
		[[nodiscard]] CORE_API auto GetFeatureSnapshot() const
			-> FModularFeatureRetirementSnapshot;
		[[nodiscard]] CORE_API auto GetAsyncOperationSnapshot() const
			-> FAsyncOperationOwnerSnapshot;

	private:
		FName ModuleName;
		std::shared_ptr<Detail::FModuleOwnerState> ModuleOwner;

		friend class FModuleTestHarness;
	};

	// Drives a concrete stack-owned module through isolated startup and shutdown in tests.
	class FModuleTestHarness final
	{
	public:
		CORE_API explicit FModuleTestHarness(FName ModuleName);
		CORE_API ~FModuleTestHarness();

		FModuleTestHarness(const FModuleTestHarness&) = delete;
		auto operator=(const FModuleTestHarness&) -> FModuleTestHarness& = delete;

		// Starts exactly one module and requires Shutdown before harness destruction.
		CORE_API auto Start(IModuleInterface& Module) -> void;
		CORE_API auto Shutdown() -> void;

		CORE_API static auto InstallStartedModule(
			FName ModuleName,
			std::unique_ptr<IModuleInterface> Module
		) -> IModuleInterface*;
		CORE_API static auto SetRetirementTimeout(
			std::chrono::milliseconds Timeout) -> std::chrono::milliseconds;

	private:
		FModuleTestOwner Owner;
		IModuleInterface* StartedModule = nullptr;
	};
}
