#include "Modules/ModularFeature.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"

namespace Durin
{
	namespace
	{
		struct FEntry
		{
			uint64 Identity;
			FModularFeatureIdentity FeatureIdentity;
			IModularFeature* Implementation;
		};
		struct FRegistryState
		{
			uint32 StartupThreadId = FPlatformLTS::GetCurrentThreadId();
			uint32 DispatchDepth = 0;
			uint64 NextIdentity = 1;
			uint64 PublicationRevision = 0;
			std::vector<FEntry> Entries;
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
			CheckThread();
			requiref(State().DispatchDepth == 0, "Feature registration changes require a safe point outside dispatch.");
		}
	}

	Detail::FModularFeatureDispatchScope::FModularFeatureDispatchScope() { FModularFeatureRegistry::Get().BeginDispatch(); }
	Detail::FModularFeatureDispatchScope::~FModularFeatureDispatchScope() { FModularFeatureRegistry::Get().EndDispatch(); }

	FModularFeatureRegistration::FModularFeatureRegistration(FModularFeatureRegistration&& Other) noexcept
		: RegistrationIdentity(std::exchange(Other.RegistrationIdentity, 0)) {}
	auto FModularFeatureRegistration::operator=(FModularFeatureRegistration&& Other) noexcept -> FModularFeatureRegistration&
	{
		if (this != &Other) { Reset(); RegistrationIdentity = std::exchange(Other.RegistrationIdentity, 0); }
		return *this;
	}
	FModularFeatureRegistration::~FModularFeatureRegistration() { Reset(); }
	auto FModularFeatureRegistration::IsValid() const -> bool { return RegistrationIdentity != 0; }
	auto FModularFeatureRegistration::Reset() -> void
	{
		if (RegistrationIdentity == 0) return;
		FModularFeatureRegistry::Get().Unregister(RegistrationIdentity);
		RegistrationIdentity = 0;
	}
	auto FModularFeatureRegistry::Get() -> FModularFeatureRegistry&
	{
		(void)State();
		static FModularFeatureRegistry Registry;
		return Registry;
	}
	auto FModularFeatureRegistry::GetPublicationRevision() const -> uint64
	{
		CheckThread();
		return State().PublicationRevision;
	}
	auto FModularFeatureRegistry::IsDispatching() const -> bool
	{
		CheckThread();
		return State().DispatchDepth != 0;
	}
	auto FModularFeatureRegistry::BeginDispatch() -> void { CheckThread(); ++State().DispatchDepth; }
	auto FModularFeatureRegistry::EndDispatch() -> void { CheckThread(); require(State().DispatchDepth != 0); --State().DispatchDepth; }
	auto FModularFeatureRegistry::Register(FModularFeatureIdentity Identity, IModularFeature& Implementation) -> FModularFeatureRegistration
	{
		CheckMutation();
		if (Identity.Name.IsNone() || Identity.Version == 0) return {};
		auto& Registry = State();
		const auto Id = Registry.NextIdentity++;
		Registry.Entries.push_back({Id, std::move(Identity), &Implementation});
		++Registry.PublicationRevision;
		return FModularFeatureRegistration(Id);
	}
	auto FModularFeatureRegistry::FindEntries(const FModularFeatureIdentity& Identity, uint64 RegistrationIdentity) const -> std::vector<FEntryView>
	{
		CheckThread();
		std::vector<FEntryView> Result;
		for (const auto& Entry : State().Entries)
			if (Entry.FeatureIdentity == Identity && (RegistrationIdentity == 0 || Entry.Identity == RegistrationIdentity))
				Result.push_back({Entry.Identity, Entry.Implementation});
		return Result;
	}
	auto FModularFeatureRegistry::Unregister(uint64 Identity) -> void
	{
		CheckMutation();
		auto& Registry = State();
		if (std::erase_if(Registry.Entries, [Identity](const auto& Entry) { return Entry.Identity == Identity; }) != 0)
			++Registry.PublicationRevision;
	}
}
