#include "DObject/StrongObjectPtr.h"

#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FObjectHandleHash
		{
			auto operator()(FObjectHandle Handle) const noexcept -> size_t
			{
				return std::hash<uint32>{}(Handle.Index)
					^ (std::hash<uint32>{}(Handle.Generation) << 1);
			}
		};

		auto GetStrongReferences() -> std::unordered_map<FObjectHandle, uint32, FObjectHandleHash>&
		{
			static std::unordered_map<FObjectHandle, uint32, FObjectHandleHash> References;
			return References;
		}

		auto AcquireStrongReference(FObjectHandle Handle) -> FObjectHandle
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			DObject* Object = ResolveObjectHandle(Handle);
			if (!IsValid(Object)) return nullptr;
			++GetStrongReferences()[Handle];
			return Handle;
		}

		auto ReleaseStrongReference(FObjectHandle Handle) -> void
		{
			if (IsObjectHandleNull(Handle)) return;
			if (GIsGameThreadIdInitialized) CheckGameThread();
			auto& References = GetStrongReferences();
			const auto It = References.find(Handle);
			check(It != References.end() && It->second > 0);
			if (--It->second == 0) References.erase(It);
		}
	}

	FStrongObjectPtr::FStrongObjectPtr(DObject* InObject)
		: Handle(AcquireStrongReference(MakeObjectHandle(InObject)))
	{
	}

	FStrongObjectPtr::FStrongObjectPtr(FObjectHandle InHandle)
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
		DObject* Object = ResolveObjectHandle(Handle);
		return Durin::IsValid(Object) ? Object : nullptr;
	}

	auto FStrongObjectPtr::Reset() -> void
	{
		ReleaseStrongReference(Handle);
		Handle = nullptr;
	}

	namespace Private
	{
		auto AddStrongObjectReferences(FReferenceCollector& Collector) -> void
		{
			for (const auto& [Handle, Count] : GetStrongReferences())
			{
				(void)Count;
				DObject* Object = ResolveObjectHandle(Handle);
				if (IsValid(Object)) Collector.AddReferencedObject(Object);
			}
		}
	}
}
