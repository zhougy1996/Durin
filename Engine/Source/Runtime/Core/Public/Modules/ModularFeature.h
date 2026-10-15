#pragma once

#include "Misc/CoreTypes.h"

#include "Misc/CoreStd.h"

#include "CoreAPI.h"
#include "Misc/Name.h"

namespace Durin
{
	class FModuleManager;
	class FModularFeatureRegistry;

	// Marks a typed interface as eligible for main-thread modular-feature invocation.
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
		// Process-local identity of the selected registration; never a persistent cache key.
		uint64 RegistrationIdentity = 0;

		[[nodiscard]] auto WasInvoked() const -> bool { return Status == EFeatureInvokeStatus::Invoked; }
	};

	// Carries one bounded void visitor outcome.
	template<>
	struct TFeatureInvokeResult<void>
	{
		EFeatureInvokeStatus Status = EFeatureInvokeStatus::Unavailable;
		uint32 MatchingRegistrationCount = 0;
		// Process-local identity of the selected registration; never a persistent cache key.
		uint64 RegistrationIdentity = 0;

		[[nodiscard]] auto WasInvoked() const -> bool { return Status == EFeatureInvokeStatus::Invoked; }
	};

	// Carries the exact registration-set snapshot selected by InvokeAll.
	template<typename TResult>
	struct TFeatureInvokeAllResult
	{
		std::vector<TFeatureInvokeResult<TResult>> Invocations;
		uint32 MatchingRegistrationCount = 0;

		[[nodiscard]] auto WasAvailable() const -> bool { return MatchingRegistrationCount != 0; }
	};

	namespace Detail
	{

		// A stack-only main-thread dispatch scope; mutation/unload is forbidden until return.
		class FModularFeatureDispatchScope final
		{
		public:
			CORE_API FModularFeatureDispatchScope();
			CORE_API ~FModularFeatureDispatchScope();
			FModularFeatureDispatchScope(const FModularFeatureDispatchScope&) = delete;
			auto operator=(const FModularFeatureDispatchScope&) -> FModularFeatureDispatchScope& = delete;
		};
	}

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
		// Idempotent immediate unregistration; main-thread safe point only. Never waits.
		CORE_API auto Reset() -> void;

	private:
		explicit FModularFeatureRegistration(uint64 Identity) : RegistrationIdentity(Identity) {}
		uint64 RegistrationIdentity = 0;

		friend class FModularFeatureRegistry;
	};

	// Main-thread-only discovery and synchronous dispatch. Providers own their resources.
	class FModularFeatureRegistry final
	{
	public:
		CORE_API static auto Get() -> FModularFeatureRegistry&;
		template<CModularFeature T>
		auto RegisterFeature(T& Implementation) -> FModularFeatureRegistration
		{
			return Register({FName(std::string_view(T::FeatureName)), static_cast<uint32>(T::FeatureVersion)}, Implementation);
		}

		[[nodiscard]] CORE_API auto GetPublicationRevision() const -> uint64;
		// Module shutdown/unload must be deferred until the entire dispatch stack returns.
		[[nodiscard]] CORE_API auto IsDispatching() const -> bool;

		template<CModularFeature T, typename F>
		auto InvokeRegistered(uint64 Identity, F&& Visitor) -> TFeatureInvokeResult<std::invoke_result_t<F, T&>>
		{
			Detail::FModularFeatureDispatchScope Scope;
			if (Identity == 0) return {};
			const auto Entries = FindEntries(GetIdentity<T>(), Identity);
			return Entries.empty() ? TFeatureInvokeResult<std::invoke_result_t<F, T&>>{}
				: InvokeEntry<T>(Entries.front(), std::forward<F>(Visitor));
		}

		template<CModularFeature T, typename F>
		auto InvokeSingle(F&& Visitor, uint64 ExpectedRegistrationIdentity = 0) -> TFeatureInvokeResult<std::invoke_result_t<F, T&>>
		{
			Detail::FModularFeatureDispatchScope Scope;
			const auto Entries = FindEntries(GetIdentity<T>());
			TFeatureInvokeResult<std::invoke_result_t<F, T&>> Result;
			Result.MatchingRegistrationCount = static_cast<uint32>(Entries.size());
			if (Entries.empty()) return Result;
			if (Entries.size() != 1) { Result.Status = EFeatureInvokeStatus::Ambiguous; return Result; }
			Result.RegistrationIdentity = Entries.front().Identity;
			if (ExpectedRegistrationIdentity != 0 && ExpectedRegistrationIdentity != Result.RegistrationIdentity) return Result;
			return InvokeEntry<T>(Entries.front(), std::forward<F>(Visitor));
		}

		template<CModularFeature T, typename F>
		auto InvokeAll(F&& Visitor) -> TFeatureInvokeAllResult<std::invoke_result_t<F, T&>>
		{
			Detail::FModularFeatureDispatchScope Scope;
			const auto Entries = FindEntries(GetIdentity<T>());
			TFeatureInvokeAllResult<std::invoke_result_t<F, T&>> Result;
			Result.MatchingRegistrationCount = static_cast<uint32>(Entries.size());
			Result.Invocations.reserve(Entries.size());
			for (const auto& Entry : Entries) Result.Invocations.push_back(InvokeEntry<T>(Entry, Visitor));
			return Result;
		}

	private:
		struct FEntryView { uint64 Identity; IModularFeature* Implementation; };
		template<CModularFeature T>
		static auto GetIdentity() -> FModularFeatureIdentity
		{
			return {FName(std::string_view(T::FeatureName)), static_cast<uint32>(T::FeatureVersion)};
		}
		template<CModularFeature T, typename F>
		static auto InvokeEntry(const FEntryView& Entry, F&& Visitor) -> TFeatureInvokeResult<std::invoke_result_t<F, T&>>
		{
			using TResult = std::invoke_result_t<F, T&>;
			TFeatureInvokeResult<TResult> Result;
			Result.RegistrationIdentity = Entry.Identity;
			Result.MatchingRegistrationCount = 1;
			try
			{
				if constexpr (std::is_void_v<TResult>) std::invoke(std::forward<F>(Visitor), *static_cast<T*>(Entry.Implementation));
				else Result.Value.emplace(std::invoke(std::forward<F>(Visitor), *static_cast<T*>(Entry.Implementation)));
				Result.Status = EFeatureInvokeStatus::Invoked;
			}
			catch (...) { Result.Status = EFeatureInvokeStatus::VisitorFailed; }
			return Result;
		}
		CORE_API auto Register(FModularFeatureIdentity Identity, IModularFeature& Implementation) -> FModularFeatureRegistration;
		CORE_API auto FindEntries(const FModularFeatureIdentity& Identity, uint64 RegistrationIdentity = 0) const -> std::vector<FEntryView>;
		CORE_API auto Unregister(uint64 RegistrationIdentity) -> void;
		CORE_API auto BeginDispatch() -> void;
		CORE_API auto EndDispatch() -> void;

		friend class FModuleManager;
		friend class FModularFeatureRegistration;
		friend class Detail::FModularFeatureDispatchScope;
	};
}
