#pragma once

#include "CoreAPI.h"
#include "Misc/Name.h"

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Durin
{
	class FModuleManager;
	class FModuleStartup;
	class FModuleTestOwner;
	class FModuleTestHarness;
	class FModularFeatureRegistry;

	// Marks a typed interface as eligible for bounded modular-feature invocation.
	class IModularFeature
	{
	public:
		virtual ~IModularFeature() = default;
	};

	// Requires a feature interface to publish stable cross-DLL name and version identity.
	template<typename T>
	concept CModularFeature = std::derived_from<T, IModularFeature>
		&& requires {
			{ T::FeatureName } -> std::convertible_to<std::string_view>;
			{ T::FeatureVersion } -> std::convertible_to<uint32>;
		};

	// Identifies one feature interface contract without RTTI or DLL-local addresses.
	struct FModularFeatureIdentity
	{
		FName Name;
		uint32 Version = 0;

		auto operator==(const FModularFeatureIdentity&) const -> bool = default;
	};

	// Categorizes singleton or per-registration visitor admission and execution.
	enum class EFeatureInvokeStatus : uint8
	{
		Unavailable,
		Invoked,
		Ambiguous,
		VisitorFailed,
	};

	// Carries one bounded visitor outcome and its value when non-void.
	template<typename TResult>
	struct TFeatureInvokeResult
	{
		EFeatureInvokeStatus Status = EFeatureInvokeStatus::Unavailable;
		std::optional<std::remove_cvref_t<TResult>> Value;
		uint32 MatchingRegistrationCount = 0;

		[[nodiscard]] auto WasInvoked() const -> bool { return Status == EFeatureInvokeStatus::Invoked; }
	};

	// Carries one bounded void visitor outcome.
	template<>
	struct TFeatureInvokeResult<void>
	{
		EFeatureInvokeStatus Status = EFeatureInvokeStatus::Unavailable;
		uint32 MatchingRegistrationCount = 0;

		[[nodiscard]] auto WasInvoked() const -> bool { return Status == EFeatureInvokeStatus::Invoked; }
	};

	// Carries the exact registration-set snapshot admitted by InvokeAll.
	template<typename TResult>
	struct TFeatureInvokeAllResult
	{
		std::vector<TFeatureInvokeResult<TResult>> Invocations;
		uint32 MatchingRegistrationCount = 0;

		[[nodiscard]] auto WasAvailable() const -> bool { return MatchingRegistrationCount != 0; }
	};

	// Reports owner or registration publication and in-flight counts at one instant.
	struct FModularFeatureRetirementSnapshot
	{
		FName OwnerName;
		uint64 OwnerGeneration = 0;
		uint32 RegistrationCount = 0;
		uint32 PublishedCount = 0;
		uint32 RetiringCount = 0;
		uint32 InFlightInvocationCount = 0;
		uint32 RetainedResourceCount = 0;
	};

	// Categorizes bounded synchronous retirement and unsupported self-waits.
	enum class EModularFeatureRetirementStatus : uint8
	{
		Succeeded,
		TimedOut,
		SelfWait,
		InvalidRegistration,
	};

	// Carries the categorized retirement outcome and diagnostic evidence.
	struct FModularFeatureRetirementResult
	{
		EModularFeatureRetirementStatus Status = EModularFeatureRetirementStatus::Succeeded;
		FModularFeatureRetirementSnapshot Snapshot;
		std::string Message;

		[[nodiscard]] auto Succeeded() const -> bool { return Status == EModularFeatureRetirementStatus::Succeeded; }
	};

	namespace Detail
	{
		struct FModuleOwnerState;
		struct FModularFeatureEntryState;

		class FModularFeatureInvocation final
		{
		public:
			FModularFeatureInvocation() = default;
			CORE_API FModularFeatureInvocation(FModularFeatureInvocation&& Other) noexcept;
			CORE_API auto operator=(FModularFeatureInvocation&& Other) noexcept -> FModularFeatureInvocation&;
			FModularFeatureInvocation(const FModularFeatureInvocation&) = delete;
			auto operator=(const FModularFeatureInvocation&) -> FModularFeatureInvocation& = delete;
			CORE_API ~FModularFeatureInvocation();

			[[nodiscard]] auto IsValid() const -> bool { return Entry != nullptr; }
			CORE_API auto GetImplementation() const -> IModularFeature*;
			CORE_API auto Enter() -> void;
			CORE_API auto Leave() -> void;

		private:
			explicit FModularFeatureInvocation(std::shared_ptr<FModularFeatureEntryState> InEntry);
			std::shared_ptr<FModularFeatureEntryState> Entry;
			bool bEntered = false;

			friend class ::Durin::FModularFeatureRegistry;
		};
	}

	class FModuleOwnedCallbackGate;

	// Marks one bounded call through a specialized cross-DLL registry.
	class FModuleOwnedCallbackInvocation final
	{
	public:
		FModuleOwnedCallbackInvocation() = default;
		FModuleOwnedCallbackInvocation(FModuleOwnedCallbackInvocation&&) noexcept = default;
		auto operator=(FModuleOwnedCallbackInvocation&&) noexcept
			-> FModuleOwnedCallbackInvocation& = default;
		FModuleOwnedCallbackInvocation(const FModuleOwnedCallbackInvocation&) = delete;
		auto operator=(const FModuleOwnedCallbackInvocation&)
			-> FModuleOwnedCallbackInvocation& = delete;
		[[nodiscard]] explicit operator bool() const { return bAdmitted; }

	private:
		explicit FModuleOwnedCallbackInvocation(Detail::FModularFeatureInvocation InInvocation)
			: Invocation(std::move(InInvocation)), bAdmitted(true) { Invocation.Enter(); }
		Detail::FModularFeatureInvocation Invocation;
		bool bAdmitted = false;

		friend class FModuleOwnedCallbackGate;
	};

	// Counts provider objects, plans, sessions, or other Plugin-owned resources
	// that may execute Plugin destruction after their originating call returns.
	class FModuleOwnedResourceLease final
	{
	public:
		FModuleOwnedResourceLease() = default;
		CORE_API ~FModuleOwnedResourceLease();
		FModuleOwnedResourceLease(const FModuleOwnedResourceLease&) = delete;
		auto operator=(const FModuleOwnedResourceLease&) -> FModuleOwnedResourceLease& = delete;
		CORE_API FModuleOwnedResourceLease(FModuleOwnedResourceLease&& Other) noexcept;
		CORE_API auto operator=(FModuleOwnedResourceLease&& Other) noexcept
			-> FModuleOwnedResourceLease&;
		[[nodiscard]] explicit operator bool() const { return Entry != nullptr; }

	private:
		explicit FModuleOwnedResourceLease(std::shared_ptr<Detail::FModularFeatureEntryState> InEntry)
			: Entry(std::move(InEntry)) {}
		CORE_API auto Release() -> void;
		std::shared_ptr<Detail::FModularFeatureEntryState> Entry;

		friend class FModuleOwnedCallbackGate;
		friend class FModularFeatureRegistry;
	};

	// Copyable admission capability stored beside specialized registry entries.
	class FModuleOwnedCallbackGate final
	{
	public:
		FModuleOwnedCallbackGate() = default;
		[[nodiscard]] CORE_API auto IsValid() const -> bool;
		[[nodiscard]] CORE_API auto TryEnter() const -> FModuleOwnedCallbackInvocation;
		[[nodiscard]] CORE_API auto RetainResource() const -> FModuleOwnedResourceLease;

	private:
		explicit FModuleOwnedCallbackGate(std::shared_ptr<Detail::FModularFeatureEntryState> InEntry)
			: Entry(std::move(InEntry)) {}
		std::shared_ptr<Detail::FModularFeatureEntryState> Entry;

		friend class FModuleOwnedCallbackRegistration;
	};

	// Module-owned token for one specialized-registry admission domain.
	class FModuleOwnedCallbackRegistration final
	{
	public:
		FModuleOwnedCallbackRegistration() = default;
		CORE_API ~FModuleOwnedCallbackRegistration();
		FModuleOwnedCallbackRegistration(const FModuleOwnedCallbackRegistration&) = delete;
		auto operator=(const FModuleOwnedCallbackRegistration&)
			-> FModuleOwnedCallbackRegistration& = delete;
		CORE_API FModuleOwnedCallbackRegistration(FModuleOwnedCallbackRegistration&& Other) noexcept;
		CORE_API auto operator=(FModuleOwnedCallbackRegistration&& Other) noexcept
			-> FModuleOwnedCallbackRegistration&;
		[[nodiscard]] CORE_API auto IsValid() const -> bool;
		[[nodiscard]] auto GetGate() const -> FModuleOwnedCallbackGate
		{
			return FModuleOwnedCallbackGate(Entry);
		}
		CORE_API auto Retire() -> FModularFeatureRetirementSnapshot;
		CORE_API auto Reset(std::chrono::milliseconds Timeout = std::chrono::seconds(5))
			-> FModularFeatureRetirementResult;

	private:
		explicit FModuleOwnedCallbackRegistration(
			std::shared_ptr<Detail::FModularFeatureEntryState> InEntry)
			: Entry(std::move(InEntry)) {}
		std::shared_ptr<Detail::FModularFeatureEntryState> Entry;

		friend class FModularFeatureRegistry;
	};

	// Owns the move-only identity token for one exact feature registration.
	class FModularFeatureRegistration final
	{
	public:
		FModularFeatureRegistration() = default;
		CORE_API FModularFeatureRegistration(FModularFeatureRegistration&& Other) noexcept;
		CORE_API auto operator=(FModularFeatureRegistration&& Other) noexcept -> FModularFeatureRegistration&;
		FModularFeatureRegistration(const FModularFeatureRegistration&) = delete;
		auto operator=(const FModularFeatureRegistration&) -> FModularFeatureRegistration& = delete;
		CORE_API ~FModularFeatureRegistration();

		[[nodiscard]] CORE_API auto IsValid() const -> bool;
		CORE_API auto Retire() -> FModularFeatureRetirementSnapshot;
		CORE_API auto Reset(std::chrono::milliseconds Timeout = std::chrono::seconds(5)) -> FModularFeatureRetirementResult;

	private:
		explicit FModularFeatureRegistration(std::shared_ptr<Detail::FModularFeatureEntryState> InEntry);
		std::shared_ptr<Detail::FModularFeatureEntryState> Entry;

		friend class FModuleStartup;
		friend class FModuleTestOwner;
		friend class FModularFeatureRegistry;
	};

	// Admits typed visitors and coordinates Core-owned synchronous retirement.
	class FModularFeatureRegistry final
	{
	public:
		CORE_API static auto Get() -> FModularFeatureRegistry&;

		template<CModularFeature T, typename F>
		auto InvokeSingle(F&& Visitor) -> TFeatureInvokeResult<std::invoke_result_t<F, T&>>
		{
			using TResult = std::invoke_result_t<F, T&>;
			TFeatureInvokeResult<TResult> Result;
			auto Invocations = BeginInvoke(GetIdentity<T>());
			Result.MatchingRegistrationCount = static_cast<uint32>(Invocations.size());
			if (Invocations.empty()) return Result;
			if (Invocations.size() != 1)
			{
				Result.Status = EFeatureInvokeStatus::Ambiguous;
				return Result;
			}

			auto& Invocation = Invocations.front();
			Invocation.Enter();
			try
			{
				if constexpr (std::is_void_v<TResult>)
				{
					std::invoke(std::forward<F>(Visitor), *static_cast<T*>(Invocation.GetImplementation()));
				}
				else
				{
					Result.Value.emplace(std::invoke(std::forward<F>(Visitor), *static_cast<T*>(Invocation.GetImplementation())));
				}
				Result.Status = EFeatureInvokeStatus::Invoked;
			}
			catch (...)
			{
				Result.Status = EFeatureInvokeStatus::VisitorFailed;
			}
			Invocation.Leave();
			return Result;
		}

		template<CModularFeature T, typename F>
		auto InvokeAll(F&& Visitor) -> TFeatureInvokeAllResult<std::invoke_result_t<F, T&>>
		{
			using TResult = std::invoke_result_t<F, T&>;
			TFeatureInvokeAllResult<TResult> Result;
			auto Invocations = BeginInvoke(GetIdentity<T>());
			Result.MatchingRegistrationCount = static_cast<uint32>(Invocations.size());
			Result.Invocations.reserve(Invocations.size());
			for (auto& Invocation : Invocations)
			{
				TFeatureInvokeResult<TResult> Item;
				Item.MatchingRegistrationCount = 1;
				Invocation.Enter();
				try
				{
					if constexpr (std::is_void_v<TResult>)
					{
						std::invoke(Visitor, *static_cast<T*>(Invocation.GetImplementation()));
					}
					else
					{
						Item.Value.emplace(std::invoke(Visitor, *static_cast<T*>(Invocation.GetImplementation())));
					}
					Item.Status = EFeatureInvokeStatus::Invoked;
				}
				catch (...)
				{
					Item.Status = EFeatureInvokeStatus::VisitorFailed;
				}
				Invocation.Leave();
				Result.Invocations.push_back(std::move(Item));
			}
			return Result;
		}

	private:
		template<CModularFeature T>
		static auto GetIdentity() -> FModularFeatureIdentity
		{
			return {FName(std::string_view(T::FeatureName)), static_cast<uint32>(T::FeatureVersion)};
		}

		CORE_API auto Register(
			const std::shared_ptr<Detail::FModuleOwnerState>& Owner,
			FModularFeatureIdentity Identity,
			IModularFeature& Implementation
		) -> FModularFeatureRegistration;
		CORE_API auto RegisterOwnedCallback(
			const std::shared_ptr<Detail::FModuleOwnerState>& Owner,
			FName DomainName) -> FModuleOwnedCallbackRegistration;
		CORE_API auto BeginInvoke(const FModularFeatureIdentity& Identity) -> std::vector<Detail::FModularFeatureInvocation>;
		CORE_API auto BeginInvokeEntry(
			const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry)
			-> Detail::FModularFeatureInvocation;
		CORE_API auto RetainEntryResource(
			const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry)
			-> FModuleOwnedResourceLease;
		CORE_API auto CreateOwner(FName OwnerName, uint64 Generation) -> std::shared_ptr<Detail::FModuleOwnerState>;
		CORE_API auto RetireOwner(
			const std::shared_ptr<Detail::FModuleOwnerState>& Owner,
			std::chrono::milliseconds Timeout
		) -> FModularFeatureRetirementResult;
		CORE_API auto SnapshotOwner(const std::shared_ptr<Detail::FModuleOwnerState>& Owner) -> FModularFeatureRetirementSnapshot;
		CORE_API auto RetireEntry(const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry) -> FModularFeatureRetirementSnapshot;
		CORE_API auto WaitEntry(
			const std::shared_ptr<Detail::FModularFeatureEntryState>& Entry,
			std::chrono::milliseconds Timeout
		) -> FModularFeatureRetirementResult;

		friend class FModuleStartup;
		friend class FModuleManager;
		friend class FModuleTestOwner;
		friend class FModuleTestHarness;
		friend class FModularFeatureRegistration;
		friend class FModuleOwnedCallbackGate;
		friend class FModuleOwnedCallbackRegistration;
		friend class FModuleOwnedResourceLease;
	};
}
