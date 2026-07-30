#pragma once

#include "EngineAPI.h"

#include <cstddef>
#include <iterator>
#include <memory>

namespace Durin
{
	class AActor;
	class DClass;
	class DWorld;

	struct FActorIteratorFilter
	{
		DClass* ActorClass = nullptr;
		bool bRequireCurrentLevel = true;
	};

	class FActorRange;
	class FActorIterator;

	ENGINE_API auto operator==(const FActorIterator& Left, const FActorIterator& Right) -> bool;

	class ENGINE_API FActorIterator
	{
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = AActor*;
		using difference_type = std::ptrdiff_t;
		using pointer = AActor*;
		using reference = AActor*;

		FActorIterator() = default;

		auto operator*() const -> reference;
		auto operator->() const -> pointer;
		auto operator++() -> FActorIterator&;
		auto operator++(int) -> FActorIterator;

		friend ENGINE_API auto operator==(const FActorIterator& Left, const FActorIterator& Right) -> bool;

	private:
		struct FState;

		explicit FActorIterator(std::shared_ptr<FState> InState, size_t InIndex);
		auto Normalize() const -> void;

		std::shared_ptr<FState> State;
		mutable size_t Index = 0;

		friend class FActorRange;
	};

	class ENGINE_API FActorRange
	{
	public:
		explicit FActorRange(DWorld* World, FActorIteratorFilter Filter = {});

		auto begin() const -> FActorIterator;
		auto end() const -> FActorIterator;
		auto GetInitialCandidateCount() const -> size_t;

	private:
		std::shared_ptr<FActorIterator::FState> State;
	};
} // namespace Durin
