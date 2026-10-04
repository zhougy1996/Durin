#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"

namespace Durin
{
	// Replay-owned portable access state; native synchronization uses tracked Metal resources.
	class FMetalAccessStateTracker
	{
	public:
		explicit FMetalAccessStateTracker(uint64 Size) : TotalSize(Size), Intervals{{0, Size, ERHIAccess::None}} {}
		auto Validate(uint64 Offset, uint64 Size, ERHIAccess Expected, ERHIAccess& Tracked) const -> bool
		{
			if (!Size || Offset > TotalSize || Size > TotalSize - Offset) return false;
			bool Found = false;
			for (const auto& Interval : Intervals)
			{
				if (Interval.Offset >= Offset + Size || Offset >= Interval.Offset + Interval.Size) continue;
				Tracked = Interval.Access;
				Found = true;
				if (Expected != ERHIAccess::Discard && Tracked != Expected) return false;
			}
			return Found;
		}
		auto Apply(uint64 Offset, uint64 Size, ERHIAccess Access) -> void
		{
			require(Size && Offset <= TotalSize && Size <= TotalSize - Offset && Access != ERHIAccess::Discard);
			std::vector<FInterval> Result;
			const uint64 UpdateEnd = Offset + Size;
			for (const auto& Interval : Intervals)
			{
				const uint64 End = Interval.Offset + Interval.Size;
				if (End <= Offset || Interval.Offset >= UpdateEnd) { Result.push_back(Interval); continue; }
				if (Interval.Offset < Offset) Result.push_back({Interval.Offset, Offset - Interval.Offset, Interval.Access});
				const uint64 Begin = std::max(Interval.Offset, Offset);
				Result.push_back({Begin, std::min(End, UpdateEnd) - Begin, Access});
				if (End > UpdateEnd) Result.push_back({UpdateEnd, End - UpdateEnd, Interval.Access});
			}
			Intervals.clear();
			for (const auto& Interval : Result)
			{
				if (!Intervals.empty() && Intervals.back().Access == Interval.Access
					&& Intervals.back().Offset + Intervals.back().Size == Interval.Offset)
					Intervals.back().Size += Interval.Size;
				else Intervals.push_back(Interval);
			}
		}
	private:
		struct FInterval { uint64 Offset, Size; ERHIAccess Access; };
		uint64 TotalSize;
		std::vector<FInterval> Intervals;
	};

	// Preflight the entire batch before publishing any new access states.
	auto ApplyMetalBufferTransitions(std::span<const FRHIBufferTransition> Transitions)
		-> std::optional<std::string>;
	auto ApplyMetalTextureTransitions(std::span<const FRHITextureTransition> Transitions)
		-> std::optional<std::string>;
	auto GetMetalCanonicalBufferAccess(EBufferUsageFlags Usage) -> ERHIAccess;
}
