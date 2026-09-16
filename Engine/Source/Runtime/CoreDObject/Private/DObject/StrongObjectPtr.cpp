#include "DObject/StrongObjectPtr.h"

#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
auto GetStrongReferences() -> std::unordered_map<FObjectKey, uint32, FObjectKeyHash>&
		{
			static std::unordered_map<FObjectKey, uint32, FObjectKeyHash> References;
			return References;
		}

		auto AcquireStrongReference(FObjectKey Handle) -> FObjectKey
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			DObject* Object = ResolveObjectKey(Handle);
			if (!IsValid(Object)) return nullptr;
			++GetStrongReferences()[Handle];
			return Handle;
		}

		auto ReleaseStrongReference(FObjectKey Handle) -> void
		{
			if (IsObjectKeyNull(Handle)) return;
			if (GIsGameThreadIdInitialized) CheckGameThread();
			auto& References = GetStrongReferences();
			const auto It = References.find(Handle);
			check(It != References.end() && It->second > 0);
			if (--It->second == 0) References.erase(It);
		}
	}

	FStrongObjectPtr::FStrongObjectPtr(DObject* InObject)
		: Handle(AcquireStrongReference(FObjectKey(InObject)))
	{
	}

	FStrongObjectPtr::FStrongObjectPtr(FObjectKey InHandle)
		: Handle(AcquireStrongReference(InHandle))
	{
	}

	FStrongObjectPtr::~FStrongObjectPtr()
	{
		ReleaseStrongReference(Handle);
	}

	FStrongObjectPtr::FStrongObjectPtr(const FStrongObjectPtr& Other)
		: Handle(AcquireStrongReference(Other.Handle))
	{
	}

	auto FStrongObjectPtr::operator=(const FStrongObjectPtr& Other) -> FStrongObjectPtr&
	{
		if (this == &Other) return *this;
		FStrongObjectPtr Copy(Other);
		return *this = std::move(Copy);
	}

	FStrongObjectPtr::FStrongObjectPtr(FStrongObjectPtr&& Other) noexcept
		: Handle(Other.Handle)
	{
		Other.Handle = nullptr;
	}

	auto FStrongObjectPtr::operator=(FStrongObjectPtr&& Other) noexcept -> FStrongObjectPtr&
	{
		if (this == &Other) return *this;
		ReleaseStrongReference(Handle);
		Handle = Other.Handle;
		Other.Handle = nullptr;
		return *this;
	}

	auto FStrongObjectPtr::Get() const -> DObject*
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		DObject* Object = ResolveObjectKey(Handle);
		return Durin::IsValid(Object) ? Object : nullptr;
	}

	auto FStrongObjectPtr::Reset() -> void
	{
		ReleaseStrongReference(Handle);
		Handle = nullptr;
	}

	namespace Private
	{
		auto GetStrongObjectReferenceCount(FObjectKey Handle) -> uint32
		{
			const auto It = GetStrongReferences().find(Handle);
			return It == GetStrongReferences().end() ? 0 : It->second;
		}

		auto AddStrongObjectReferences(FReferenceCollector& Collector) -> void
		{
			for (const auto& [Handle, Count] : GetStrongReferences())
			{
				(void)Count;
				DObject* Object = ResolveObjectKey(Handle);
				if (IsValid(Object)) Collector.AddReferencedObject(Object);
			}
		}
	}
}
