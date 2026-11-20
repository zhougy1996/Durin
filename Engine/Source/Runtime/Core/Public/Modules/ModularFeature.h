#pragma once

#include "Misc/CoreTypes.h"
#include "Misc/CoreStd.h"

#include "CoreAPI.h"
#include "Delegates/Delegate.h"
#include "Misc/Name.h"

namespace Durin
{
	class IModularFeature
	{
	public:
		virtual ~IModularFeature() = default;
	};

	// Each name identifies one interface contract. Providers own their implementations.
	class IModularFeatures
	{
		class FFeatureEvent : public TThreadSafeMulticastDelegate<void(const FName&, IModularFeature*)>
		{
			using FDelegate = TThreadSafeMulticastDelegate<void(const FName&, IModularFeature*)>;
			using FDelegate::Broadcast;
			using FDelegate::Clear;
			friend class IModularFeatures;
		};

	public:
		class FOnModularFeatureRegistered : public FFeatureEvent {};
		class FOnModularFeatureUnregistered : public FFeatureEvent {};

		CORE_API static auto Get() -> IModularFeatures&;
		virtual ~IModularFeatures() = default;
		virtual auto RegisterModularFeature(FName Type, IModularFeature* Feature) -> void = 0;
		virtual auto UnregisterModularFeature(FName Type, IModularFeature* Feature) -> void = 0;
		virtual auto GetModularFeatureImplementationCount(FName Type) -> int32 = 0;
		virtual auto GetModularFeatureImplementation(FName Type, int32 Index) -> IModularFeature* = 0;
		virtual auto LockModularFeatureList() -> void = 0;
		virtual auto UnlockModularFeatureList() -> void = 0;
		virtual auto OnModularFeatureRegistered() -> FOnModularFeatureRegistered& = 0;
		virtual auto OnModularFeatureUnregistered() -> FOnModularFeatureUnregistered& = 0;

		struct FScopedLockModularFeatureList
		{
			FScopedLockModularFeatureList() { Get().LockModularFeatureList(); }
			~FScopedLockModularFeatureList() { Get().UnlockModularFeatureList(); }
			FScopedLockModularFeatureList(const FScopedLockModularFeatureList&) = delete;
			auto operator=(const FScopedLockModularFeatureList&) -> FScopedLockModularFeatureList& = delete;
		};

		auto IsModularFeatureAvailable(FName Type) -> bool
		{
			return GetModularFeatureImplementationCount(Type) > 0;
		}

		// Hold the list lock across compound lookups and use of borrowed implementations.
		template<std::derived_from<IModularFeature> T>
		auto GetModularFeature(FName Type) -> T&
		{
			return static_cast<T&>(*GetModularFeatureImplementation(Type, 0));
		}

		template<std::derived_from<IModularFeature> T>
		auto GetModularFeatureImplementations(FName Type) -> std::vector<T*>
		{
			std::vector<T*> Implementations;
			ForEachImplementation(Type, [&](IModularFeature* Feature) {
				Implementations.push_back(static_cast<T*>(Feature));
			});
			return Implementations;
		}

	protected:
		static auto Broadcast(FFeatureEvent& Event, const FName& Type, IModularFeature* Feature) noexcept -> void;

	private:
		virtual auto ForEachImplementation(FName Type, const std::function<void(IModularFeature*)>& Append) -> void = 0;
	};

	// A synchronous borrow on the module-control thread. Registry mutation and module
	// shutdown are excluded until the scope ends.
	class FScopedModularFeatureAccess final
	{
	public:
		CORE_API FScopedModularFeatureAccess();
		CORE_API ~FScopedModularFeatureAccess();
		FScopedModularFeatureAccess(const FScopedModularFeatureAccess&) = delete;
		auto operator=(const FScopedModularFeatureAccess&) -> FScopedModularFeatureAccess& = delete;
	};

	[[nodiscard]] CORE_API auto IsModularFeatureDispatching() -> bool;

	template<std::derived_from<IModularFeature> T, typename F>
	auto ForEachModularFeature(F&& Visitor) -> void
	{
		FScopedModularFeatureAccess Scope;
		for (auto* Feature : IModularFeatures::Get().GetModularFeatureImplementations<T>(FName(T::FeatureName)))
			std::invoke(Visitor, *Feature);
	}

	// Optional single-owner registration for module-control-thread providers.
	class FModularFeatureRegistration final
	{
	public:
		FModularFeatureRegistration() = default;
		CORE_API FModularFeatureRegistration(FName Type, IModularFeature& Feature);
		CORE_API FModularFeatureRegistration(FModularFeatureRegistration&& Other) noexcept;
		CORE_API auto operator=(FModularFeatureRegistration&& Other) noexcept -> FModularFeatureRegistration&;
		FModularFeatureRegistration(const FModularFeatureRegistration&) = delete;
		auto operator=(const FModularFeatureRegistration&) -> FModularFeatureRegistration& = delete;
		CORE_API ~FModularFeatureRegistration();

		[[nodiscard]] auto IsValid() const -> bool { return Feature != nullptr; }
		CORE_API auto Reset() -> void;

	private:
		FName Type;
		IModularFeature* Feature = nullptr;
	};

	template<std::derived_from<IModularFeature> T>
	auto RegisterModularFeature(T& Feature) -> FModularFeatureRegistration
	{
		return FModularFeatureRegistration(FName(T::FeatureName), Feature);
	}
}
