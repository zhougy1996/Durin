#pragma once

#include "CoreMinimal.h"


namespace Durin::MeshStreamPrivate
{
	// Resource storage may be mutable or retain a validated native allocation.
	template<typename T>
	auto Read(const std::vector<T>& Mutable, const FSharedByteBuffer& Shared) -> std::span<const T>
	{
		if (auto View = Shared.GetNativeView<T>()) return *View;
		return Mutable;
	}

	template<typename T>
	auto Detach(std::vector<T>& Mutable, FSharedByteBuffer& Shared) -> std::vector<T>&
	{
		if (auto View = Shared.GetNativeView<T>())
		{
			std::vector<T> Candidate(View->begin(), View->end());
			Mutable = std::move(Candidate);
			Shared = {};
		}
		return Mutable;
	}

	template<typename T>
	auto Retain(std::vector<T>& Mutable, FSharedByteBuffer& Shared, FSharedByteBuffer Candidate) -> bool
	{
		if (!Candidate.GetNativeView<T>()) return false;
		std::vector<T>().swap(Mutable);
		Shared = std::move(Candidate);
		return true;
	}

	// Retains existing shared storage or moves the mutable allocation into it.
	template<typename T>
	auto Freeze(std::vector<T>& Mutable, FSharedByteBuffer& Shared) -> FSharedByteBuffer
	{
		if (!Shared.GetNativeView<T>()) Shared = FSharedByteBuffer::TakeNative(std::move(Mutable));
		return Shared;
	}

	template<typename T>
	auto Capacity(const std::vector<T>& Mutable, const FSharedByteBuffer& Shared) -> size_t
	{
		if (Shared.GetNativeView<T>()) return Shared.GetRetainedCapacityBytes() / sizeof(T);
		return Mutable.capacity();
	}
}
