#include "Viewport/ViewportClosePolicy.h"
#include "ViewportClosePolicyPrivate.h"
#include "Threading/RunnableThread.h"

namespace Durin::MonaImGui
{
	namespace
	{
		struct FPolicy
		{
			uint64 Generation = 0;
			std::function<bool()> RequestClose;
		};
		struct FPolicies
		{
			uint64 NextGeneration = 1;
			std::unordered_map<uint32, FPolicy> Entries;
		};
		auto PolicyStorage() -> std::shared_ptr<FPolicies>&
		{
			static std::shared_ptr<FPolicies> State;
			return State;
		}
	}

	FViewportClosePolicyRegistration::~FViewportClosePolicyRegistration() { Reset(); }
	FViewportClosePolicyRegistration::FViewportClosePolicyRegistration(FViewportClosePolicyRegistration&& Other) noexcept
		: Release(std::exchange(Other.Release, {})) {}
	auto FViewportClosePolicyRegistration::operator=(FViewportClosePolicyRegistration&& Other) noexcept -> FViewportClosePolicyRegistration&
	{
		if (this != &Other) { Reset(); Release = std::exchange(Other.Release, {}); }
		return *this;
	}
	auto FViewportClosePolicyRegistration::Reset() -> void
	{
		if (auto Callback = std::exchange(Release, {})) Callback();
	}
	auto RegisterViewportClosePolicy(uint32 ViewportId, std::function<bool()> RequestClose) -> FViewportClosePolicyRegistration
	{
		require(IsInGameThread());
		FViewportClosePolicyRegistration Registration;
		if (!ViewportId || !RequestClose) return Registration;
		auto& Storage = PolicyStorage();
		if (!Storage) Storage = std::make_shared<FPolicies>();
		if (Storage->Entries.contains(ViewportId)) return Registration;
		const uint64 Generation = Storage->NextGeneration++;
		Storage->Entries.emplace(ViewportId, FPolicy{Generation, std::move(RequestClose)});
		Registration.Release = [Weak = std::weak_ptr<FPolicies>(Storage), ViewportId, Generation] {
			if (const auto State = Weak.lock())
			{
				require(IsInGameThread());
				if (const auto Entry = State->Entries.find(ViewportId);
					Entry != State->Entries.end() && Entry->second.Generation == Generation) State->Entries.erase(Entry);
			}
		};
		return Registration;
	}
	auto TryCloseViewport(uint32 ViewportId) -> bool
	{
		const auto State = PolicyStorage();
		if (!State) return true;
		const auto Entry = State->Entries.find(ViewportId);
		if (Entry == State->Entries.end()) return true;
		require(IsInGameThread());
		// Closing can unregister this policy or withdraw the registry while dispatching.
		const auto Callback = Entry->second.RequestClose;
		return Callback();
	}
	auto ResetViewportClosePolicies() -> void { PolicyStorage().reset(); }
}
