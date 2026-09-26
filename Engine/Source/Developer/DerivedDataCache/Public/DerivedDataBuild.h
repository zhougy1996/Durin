#pragma once

#include "DerivedDataBuildDefinition.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <optional>

namespace Durin::DerivedData
{
	enum class EBuildPhase : uint8 { Lookup, Decode, Resolve, Build, Validate, Encode, Store, Count };
	enum class EBuildFailure : uint8 { InvalidRequest, FunctionMismatch, InputMismatch, Cancelled };
	enum class EBuildOrigin : uint8 { Rebuilt, CacheHit };

	struct FBuildExecutionPolicy
	{
		bool bReadCache = true;
		bool bWriteCache = true;
		bool bForceRebuild = false;
		bool bFailOnEncodeError = false;
		uint64 MaximumValueBytes = 0;
	};

	// Borrowed for the synchronous call. The caller owns admission, reservations and DLL lifetime.
	struct FBuildExecutionContext
	{
		std::function<bool()> ShouldCancel;
		std::function<void(EBuildPhase)> OnPhase;
	};

	template<typename TError>
	struct TBuildObservations
	{
		EBuildOrigin Origin = EBuildOrigin::Rebuilt;
		std::optional<FCacheError> ReadError;
		std::optional<FCacheError> WriteError;
		std::optional<TError> DecodeError;
		std::optional<TError> EncodeError;
		std::array<uint64, static_cast<size_t>(EBuildPhase::Count)> Nanoseconds{};
		uint64 ReadBytes = 0;
		uint64 WrittenBytes = 0;
	};

	// Optional diagnostic consumption, independent of the build result and timing counters.
	// Each operation is visited once; normal misses are never reported as problems.
	template<typename TError, typename TVisitor>
	auto VisitBuildIssues(const TBuildObservations<TError>& Observations, TVisitor&& Visit) -> void
	{
		if (Observations.ReadError && Observations.ReadError->Code != ECacheError::Miss)
			Visit(EBuildPhase::Lookup, *Observations.ReadError);
		if (Observations.DecodeError) Visit(EBuildPhase::Decode, *Observations.DecodeError);
		if (Observations.EncodeError) Visit(EBuildPhase::Encode, *Observations.EncodeError);
		if (Observations.WriteError) Visit(EBuildPhase::Store, *Observations.WriteError);
	}

	// Adapter owns immutable source bindings and normalized settings for Definition.
	// Required hooks: GetFunction, GetInputs, ValidateBindings, Resolve, Build,
	// Validate, Decode, Encode, MakeError, IsCancelled. Hook results are expected;
	// decoded/prepared values own every buffer borrowed by the synchronous recipe.
	// No tasks, object application, function registry or family payload erasure here.
	template<typename TAdapter>
	auto ExecuteBuild(const FBuildDefinition& Definition, TAdapter& Adapter,
		const FBuildExecutionPolicy& Policy, const FBuildExecutionContext& Context,
		TBuildObservations<typename TAdapter::FError>& Observations)
		-> std::expected<typename TAdapter::FProduct, typename TAdapter::FError>
	{
		Observations = {};
		bool bCancelled = false;
		const auto Cancelled = [&] {
			bCancelled = bCancelled || (Context.ShouldCancel && Context.ShouldCancel());
			return bCancelled;
		};
		const auto CancelError = [&] { return std::unexpected(Adapter.MakeError(EBuildFailure::Cancelled)); };
		const auto Phase = [&](EBuildPhase Value) {
			if (Context.OnPhase) Context.OnPhase(Value);
			return !Cancelled();
		};
		const auto Measure = [&](EBuildPhase Value, auto&& Operation) {
			const auto Start = std::chrono::steady_clock::now();
			auto Result = Operation();
			Observations.Nanoseconds[static_cast<size_t>(Value)] += static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - Start).count());
			return Result;
		};
		if (Cancelled()) return CancelError();
		if (!Policy.MaximumValueBytes || !Definition.GetKey().IsValid())
			return std::unexpected(Adapter.MakeError(EBuildFailure::InvalidRequest));
		if (Definition.GetFunction() != Adapter.GetFunction())
			return std::unexpected(Adapter.MakeError(EBuildFailure::FunctionMismatch));
		if (!Definition.MatchesInputs(Adapter.GetInputs()))
			return std::unexpected(Adapter.MakeError(EBuildFailure::InputMismatch));
		if (auto Valid = Adapter.ValidateBindings(Definition); !Valid)
			return std::unexpected(std::move(Valid.error()));

		if (Policy.bReadCache && !Policy.bForceRebuild)
		{
			if (!Phase(EBuildPhase::Lookup)) return CancelError();
			auto Cached = Measure(EBuildPhase::Lookup, [&] {
				return GetCache().Get({Definition.GetKey(), Policy.MaximumValueBytes});
			});
			if (Cancelled()) return CancelError();
			if (Cached)
			{
				Observations.ReadBytes = Cached->GetSize();
				if (!Phase(EBuildPhase::Decode)) return CancelError();
				auto Decoded = Measure(EBuildPhase::Decode, [&] { return Adapter.Decode(*Cached); });
				if (Cancelled()) return CancelError();
				if (Decoded)
				{
					if (!Phase(EBuildPhase::Validate)) return CancelError();
					auto Valid = Measure(EBuildPhase::Validate, [&] { return Adapter.Validate(*Decoded); });
					if (Cancelled()) return CancelError();
					if (Valid)
					{
						Observations.Origin = EBuildOrigin::CacheHit;
						return std::move(*Decoded);
					}
					if (Adapter.IsCancelled(Valid.error())) return std::unexpected(std::move(Valid.error()));
					Observations.DecodeError = std::move(Valid.error());
				}
				else
				{
					if (Adapter.IsCancelled(Decoded.error())) return std::unexpected(std::move(Decoded.error()));
					Observations.DecodeError = std::move(Decoded.error());
				}
			}
			else if (Cached.error().Code != ECacheError::Miss)
				Observations.ReadError = std::move(Cached.error());
		}

		if (!Phase(EBuildPhase::Resolve)) return CancelError();
		auto Prepared = Measure(EBuildPhase::Resolve, [&] { return Adapter.Resolve(); });
		if (Cancelled()) return CancelError();
		if (!Prepared) return std::unexpected(std::move(Prepared.error()));
		if (!Phase(EBuildPhase::Build)) return CancelError();
		auto Built = Measure(EBuildPhase::Build, [&] { return Adapter.Build(*Prepared); });
		if (Cancelled()) return CancelError();
		if (!Built) return std::unexpected(std::move(Built.error()));
		if (!Phase(EBuildPhase::Validate)) return CancelError();
		auto Valid = Measure(EBuildPhase::Validate, [&] { return Adapter.Validate(*Built); });
		if (Cancelled()) return CancelError();
		if (!Valid) return std::unexpected(std::move(Valid.error()));
		if (Policy.bWriteCache)
		{
			if (!Phase(EBuildPhase::Encode)) return CancelError();
			auto Encoded = Measure(EBuildPhase::Encode, [&] { return Adapter.Encode(*Built); });
			if (Cancelled()) return CancelError();
			if (!Encoded)
			{
				if (Adapter.IsCancelled(Encoded.error()) || Policy.bFailOnEncodeError)
					return std::unexpected(std::move(Encoded.error()));
				Observations.EncodeError = std::move(Encoded.error());
			}
			else
			{
				if (!Phase(EBuildPhase::Store)) return CancelError();
				auto Stored = Measure(EBuildPhase::Store, [&] {
					return GetCache().Put({Definition.GetKey(), *Encoded, Policy.MaximumValueBytes});
				});
				if (Stored) Observations.WrittenBytes = Encoded->size();
				else Observations.WriteError = std::move(Stored.error());
			}
		}
		if (Cancelled()) return CancelError();
		return std::move(*Built);
	}
}
