#include "Modules/ModularFeature.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Logging/LogMacros.h"

namespace Durin
{
	namespace
	{
		class FModularFeatures final : public IModularFeatures
		{
		public:
			auto RegisterModularFeature(FName Type, IModularFeature* Feature) -> void override;
			auto UnregisterModularFeature(FName Type, IModularFeature* Feature) -> void override;
			auto GetModularFeatureImplementationCount(FName Type) -> int32 override;
			auto GetModularFeatureImplementation(FName Type, int32 Index) -> IModularFeature* override;
			auto LockModularFeatureList() -> void override;
			auto UnlockModularFeatureList() -> void override;
			auto OnModularFeatureRegistered() -> FOnModularFeatureRegistered& override;
			auto OnModularFeatureUnregistered() -> FOnModularFeatureUnregistered& override;
		private:
			auto ForEachImplementation(FName Type, const std::function<void(IModularFeature*)>& Append) -> void override;
		};

		struct FEntry
		{
			FName Type;
			IModularFeature* Implementation;
		};
		struct FRegistryState
		{
			FModularFeatures Features;
			uint32 StartupThreadId = FPlatformLTS::GetCurrentThreadId();
			uint32 DispatchDepth = 0;
			uint32 NotificationDepth = 0;
			std::recursive_mutex Mutex;
			std::vector<FEntry> Entries;
			IModularFeatures::FOnModularFeatureRegistered Registered;
			IModularFeatures::FOnModularFeatureUnregistered Unregistered;
		};
		auto State() -> FRegistryState&
		{
			// Mapped modules can unregister during process-exit destruction.
			static auto* Instance = new FRegistryState;
			return *Instance;
		}
		auto CheckThread() -> void
		{
			const auto Expected = GIsGameThreadIdInitialized ? GGameThreadId : State().StartupThreadId;
			requiref(FPlatformLTS::GetCurrentThreadId() == Expected, "Modular features require the module-control/GameThread.");
		}
		auto CheckMutation() -> void
		{
			requiref(State().DispatchDepth == 0, "Feature registration changes require a safe point outside dispatch.");
		}

		auto Matches(const FEntry& Entry, const FName& Type, const IModularFeature* Feature) -> bool
		{
			return Entry.Type == Type && Entry.Implementation == Feature;
		}
	}

	FModularFeatureRegistration::FModularFeatureRegistration(FName InType, IModularFeature& InFeature)
		: Type(InType)
	{
		CheckThread();
		IModularFeatures::FScopedLockModularFeatureList Lock;
		CheckMutation();
		if (std::ranges::any_of(State().Entries, [&](const FEntry& Entry) { return Matches(Entry, Type, &InFeature); }))
			return;
		IModularFeatures::Get().RegisterModularFeature(Type, &InFeature);
		Feature = &InFeature;
	}

	FModularFeatureRegistration::FModularFeatureRegistration(FModularFeatureRegistration&& Other) noexcept
		: Type(Other.Type), Feature(std::exchange(Other.Feature, nullptr)) {}

	auto FModularFeatureRegistration::operator=(FModularFeatureRegistration&& Other) noexcept -> FModularFeatureRegistration&
	{
		if (this != &Other)
		{
			Reset();
			Type = Other.Type;
			Feature = std::exchange(Other.Feature, nullptr);
		}
		return *this;
	}

	FModularFeatureRegistration::~FModularFeatureRegistration() { Reset(); }

	auto FModularFeatureRegistration::Reset() -> void
	{
		if (!Feature) return;
		CheckThread();
		IModularFeatures::Get().UnregisterModularFeature(Type, std::exchange(Feature, nullptr));
	}

	auto IModularFeatures::Get() -> IModularFeatures&
	{
		return State().Features;
	}
	auto FModularFeatures::LockModularFeatureList() -> void { State().Mutex.lock(); }
	auto FModularFeatures::UnlockModularFeatureList() -> void { State().Mutex.unlock(); }
	auto FModularFeatures::OnModularFeatureRegistered() -> FOnModularFeatureRegistered& { return State().Registered; }
	auto FModularFeatures::OnModularFeatureUnregistered() -> FOnModularFeatureUnregistered& { return State().Unregistered; }

	auto IModularFeatures::Broadcast(FFeatureEvent& Event, const FName& Type, IModularFeature* Feature) noexcept -> void
	{
		++State().NotificationDepth;
		try
		{
			Event.Broadcast(Type, Feature);
		}
		catch (...)
		{
			DURIN_ERROR("Modular-feature notification failed for {}.", Type.ToString());
		}
		--State().NotificationDepth;
	}

	auto FModularFeatures::RegisterModularFeature(FName Type, IModularFeature* Feature) -> void
	{
		require(Feature && !Type.IsNone());
		auto& Registry = State();
		std::lock_guard Lock(Registry.Mutex);
		CheckMutation();
		if (std::ranges::none_of(Registry.Entries, [&](const FEntry& Entry) { return Matches(Entry, Type, Feature); }))
			Registry.Entries.push_back({Type, Feature});
		Broadcast(Registry.Registered, Type, Feature);
	}

	auto FModularFeatures::UnregisterModularFeature(FName Type, IModularFeature* Feature) -> void
	{
		if (!Feature) return;
		require(!Type.IsNone());
		auto& Registry = State();
		std::lock_guard Lock(Registry.Mutex);
		CheckMutation();
		const auto Entry = std::ranges::find_if(Registry.Entries, [&](const FEntry& Item) { return Matches(Item, Type, Feature); });
		if (Entry != Registry.Entries.end()) Registry.Entries.erase(Entry);
		Broadcast(Registry.Unregistered, Type, Feature);
	}

	auto FModularFeatures::GetModularFeatureImplementationCount(FName Type) -> int32
	{
		auto& Registry = State();
		std::lock_guard Lock(Registry.Mutex);
		return static_cast<int32>(std::ranges::count(Registry.Entries, Type, &FEntry::Type));
	}

	auto FModularFeatures::GetModularFeatureImplementation(FName Type, int32 Index) -> IModularFeature*
	{
		if (Index < 0) return nullptr;
		auto& Registry = State();
		std::lock_guard Lock(Registry.Mutex);
		for (const auto& Entry : Registry.Entries)
			if (Entry.Type == Type && Index-- == 0) return Entry.Implementation;
		return nullptr;
	}

	auto FModularFeatures::ForEachImplementation(FName Type, const std::function<void(IModularFeature*)>& Append) -> void
	{
		auto& Registry = State();
		std::lock_guard Lock(Registry.Mutex);
		for (const auto& Entry : Registry.Entries)
			if (Entry.Type == Type) Append(Entry.Implementation);
	}

	auto IsModularFeatureDispatching() -> bool
	{
		CheckThread();
		std::lock_guard Lock(State().Mutex);
		return State().DispatchDepth != 0 || State().NotificationDepth != 0;
	}

	FScopedModularFeatureAccess::FScopedModularFeatureAccess()
	{
		CheckThread();
		IModularFeatures::Get().LockModularFeatureList();
		++State().DispatchDepth;
	}

	FScopedModularFeatureAccess::~FScopedModularFeatureAccess()
	{
		--State().DispatchDepth;
		IModularFeatures::Get().UnlockModularFeatureList();
	}
}
