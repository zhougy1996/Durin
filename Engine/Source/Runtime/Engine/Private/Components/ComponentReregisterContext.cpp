#include "ComponentReregisterContext.h"

#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Logging/LogMacros.h"

namespace Durin
{
	FComponentReregisterContext::FComponentReregisterContext(DActorComponent* InComponent)
	{
		if (!InComponent || !InComponent->IsRegistered()) return;
		if (!IsValid(InComponent) || InComponent->IsBeingDestroyed()) { InComponent->UnregisterComponent(); return; }
		Component = InComponent;
		Owner = InComponent->GetOwner();
		bTickRegistered = InComponent->GetPrimaryComponentTick().IsTickFunctionRegistered();
		bInitialized = InComponent->HasBeenInitialized();
		bPending = true;
		try { InComponent->UnregisterComponent(); }
		catch (...)
		{
			if (auto Result = ReRegister(); !Result) DURIN_ERROR("Reregistration cleanup failed: {}", Result.error());
			throw;
		}
	}
	FComponentReregisterContext::~FComponentReregisterContext()
	{
		if (auto Result = ReRegister(); !Result) DURIN_ERROR("Component reregistration failed: {}", Result.error());
	}
	auto FComponentReregisterContext::ReRegister() -> std::expected<void, std::string>
	{
		if (!std::exchange(bPending, false)) return {};
		auto* Value = Component.Get();
		if (!IsValid(Value) || Value->IsBeingDestroyed()) return {}; // Retirement never resurrects a component.
		if (Value->GetOwner() != Owner.Get() || (Owner.IsValid() && !Owner.Get()->OwnsComponent(Value)))
			return std::unexpected("Component reregistration owner changed.");
		if (Owner.IsValid() && Owner.Get()->IsBeingDestroyed()) return {};
		try
		{
			Value->RegisterComponentInternal(bInitialized);
			if (!IsValid(Value) || Value->IsBeingDestroyed()) return {};
			if (!Value->IsRegistered()) return std::unexpected("Component reregistration did not settle.");
			auto Tick = Value->GetPrimaryComponentTick().CaptureState();
			Tick.bRegistered = bTickRegistered;
			return Value->GetPrimaryComponentTick().RestoreState(Tick, Value->GetOwner() ? Cast<DLevel>(Value->GetOwner()->GetOuter()) : nullptr);
		}
		catch (const std::exception& Error) { return std::unexpected(Error.what()); }
		catch (...) { return std::unexpected("Component registration callback threw."); }
	}
}
