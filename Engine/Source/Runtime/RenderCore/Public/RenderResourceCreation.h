#pragma once

#include "Misc/CoreTypes.h"
#include "Misc/AssertionMacros.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace Durin
{
	enum class ERenderResourceCreateErrorCategory : uint8
	{
		ShaderOptions,
		ShaderCompile,
		ShaderBinding,
		RHIResource,
		GraphicsPipeline,
		InvalidConfiguration,
	};

	// Identifies control-flow-relevant causes independently from diagnostic text.
	enum class ERenderResourceCreateErrorReason : uint8
	{
		Unspecified,
		GlobalShaderUnavailable,
	};

	enum class ERenderResourceGenerationDependency : uint8
	{
		None = 0,
		Shader = 1 << 0,
		Device = 1 << 1,
		Manual = 1 << 2,
	};

	constexpr auto operator|(
		ERenderResourceGenerationDependency Left,
		ERenderResourceGenerationDependency Right)
		-> ERenderResourceGenerationDependency
	{
		return static_cast<ERenderResourceGenerationDependency>(
			static_cast<uint8>(Left) | static_cast<uint8>(Right));
	}

	constexpr auto operator&(
		ERenderResourceGenerationDependency Left,
		ERenderResourceGenerationDependency Right)
		-> ERenderResourceGenerationDependency
	{
		return static_cast<ERenderResourceGenerationDependency>(
			static_cast<uint8>(Left) & static_cast<uint8>(Right));
	}

	constexpr auto HasRenderResourceGenerationDependency(
		ERenderResourceGenerationDependency Dependencies,
		ERenderResourceGenerationDependency Dependency) -> bool
	{
		return (Dependencies & Dependency)
			!= ERenderResourceGenerationDependency::None;
	}

	struct FRenderResourceGeneration
	{
		uint64 Shader = 0;
		uint64 Device = 0;
		uint64 Manual = 0;

		auto operator==(const FRenderResourceGeneration&) const -> bool = default;

		auto Advance(ERenderResourceGenerationDependency Dependency) -> void
		{
			uint64* Counter = nullptr;
			switch (Dependency)
			{
			case ERenderResourceGenerationDependency::Shader:
				Counter = &Shader;
				break;
			case ERenderResourceGenerationDependency::Device:
				Counter = &Device;
				break;
			case ERenderResourceGenerationDependency::Manual:
				Counter = &Manual;
				break;
			default:
				checkf(
					false,
					"Exactly one renderer resource generation dependency must be advanced.");
				return;
			}
			checkf(
				*Counter != std::numeric_limits<uint64>::max(),
				"Renderer resource generation overflowed.");
			++*Counter;
		}
	};

	constexpr auto HasSelectedRenderResourceGenerationChanged(
		const FRenderResourceGeneration& Earlier,
		const FRenderResourceGeneration& Later,
		ERenderResourceGenerationDependency Dependencies) -> bool
	{
		return (
				   HasRenderResourceGenerationDependency(
					   Dependencies,
					   ERenderResourceGenerationDependency::Shader)
				   && Earlier.Shader != Later.Shader)
			|| (
				HasRenderResourceGenerationDependency(
					Dependencies,
					ERenderResourceGenerationDependency::Device)
				&& Earlier.Device != Later.Device)
			|| (
				HasRenderResourceGenerationDependency(
					Dependencies,
					ERenderResourceGenerationDependency::Manual)
				&& Earlier.Manual != Later.Manual);
	}

	struct FRenderResourceCreateError
	{
		ERenderResourceCreateErrorCategory Category =
			ERenderResourceCreateErrorCategory::InvalidConfiguration;
		ERenderResourceCreateErrorReason Reason =
			ERenderResourceCreateErrorReason::Unspecified;
		std::string Context;
		std::string Identity;
		std::string Message;
		ERenderResourceGenerationDependency RetryDependencies =
			ERenderResourceGenerationDependency::Manual;
		FRenderResourceGeneration AttemptedGeneration;
		bool bRetainedFallback = false;

		auto GetFingerprint() const -> size_t
		{
			size_t Fingerprint = static_cast<size_t>(Category);
			auto Combine = [&Fingerprint](size_t Value) {
				Fingerprint ^= Value + 0x9e3779b9 + (Fingerprint << 6)
					+ (Fingerprint >> 2);
			};
			Combine(static_cast<size_t>(Reason));
			Combine(std::hash<std::string>{}(Context));
			Combine(std::hash<std::string>{}(Identity));
			Combine(std::hash<std::string>{}(Message));
			Combine(static_cast<size_t>(RetryDependencies));
			Combine(static_cast<size_t>(bRetainedFallback));
			return Fingerprint;
		}
	};

	enum class ERenderResourceAvailability : uint8
	{
		Uninitialized,
		Creating,
		Ready,
		Refreshing,
		Failed,
		StaleReady,
	};

	enum class ERenderResourceCreateDiagnosticKind : uint8
	{
		Failure,
		Recovery,
	};

	struct FRenderResourceCreateDiagnostic
	{
		ERenderResourceCreateDiagnosticKind Kind =
			ERenderResourceCreateDiagnosticKind::Failure;
		std::optional<FRenderResourceCreateError> Error;
	};

	template <typename PayloadType>
	class TRenderResourceCreateResult
	{
	public:
		static auto Success(PayloadType Payload) -> TRenderResourceCreateResult
		{
			TRenderResourceCreateResult Result;
			Result.Payload.emplace(std::move(Payload));
			return Result;
		}

		static auto Failure(FRenderResourceCreateError Error)
			-> TRenderResourceCreateResult
		{
			TRenderResourceCreateResult Result;
			Result.Error.emplace(std::move(Error));
			return Result;
		}

		auto HasPayload() const -> bool { return Payload.has_value(); }
		auto TakePayload() -> PayloadType { return std::move(*Payload); }
		auto TakeError() -> FRenderResourceCreateError
		{
			return std::move(*Error);
		}

	private:
		std::optional<PayloadType> Payload;
		std::optional<FRenderResourceCreateError> Error;
	};

	// Owns one synchronously created renderer payload and its generation-scoped
	// attempt state. The factory must return a complete candidate or an owned
	// failure; it never mutates the live payload.
	template <typename PayloadType>
	class TRenderResourceCreationSlot
	{
	public:
		explicit TRenderResourceCreationSlot(
			ERenderResourceGenerationDependency InPayloadDependencies =
				ERenderResourceGenerationDependency::None)
			: PayloadDependencies(InPayloadDependencies)
		{
		}

		template <typename FactoryType, typename DiagnosticReporterType>
		auto Resolve(
			const FRenderResourceGeneration& Generation,
			FactoryType&& Factory,
			DiagnosticReporterType&& ReportDiagnostic) -> PayloadType*
		{
			if (bResolving)
			{
				return Payload ? &*Payload : nullptr;
			}

			ApplyDeviceGeneration(Generation);
			if (ShouldSuppressAttempt(Generation))
			{
				return Payload ? &*Payload : nullptr;
			}

			bResolving = true;
			Availability = Payload
				? ERenderResourceAvailability::Refreshing
				: ERenderResourceAvailability::Creating;
			auto Result = std::forward<FactoryType>(Factory)();
			bResolving = false;
			bHasAttempt = true;
			AttemptedGeneration = Generation;
			if (Result.HasPayload())
			{
				std::optional<PayloadType> Candidate;
				Candidate.emplace(Result.TakePayload());
				Payload.swap(Candidate);
				PayloadGeneration = Generation;
				const bool bReportRecovery = bFailureReported;
				std::optional<FRenderResourceCreateError> RecoveredError =
					Failure;
				Failure.reset();
				FailureFingerprint.reset();
				Availability = ERenderResourceAvailability::Ready;
				bFailureReported = false;
				if (bReportRecovery)
				{
					std::forward<DiagnosticReporterType>(ReportDiagnostic)(
						FRenderResourceCreateDiagnostic{
							.Kind =
								ERenderResourceCreateDiagnosticKind::Recovery,
							.Error = std::move(RecoveredError),
						});
				}
				return &*Payload;
			}

			Failure.emplace(Result.TakeError());
			Failure->AttemptedGeneration = Generation;
			Failure->bRetainedFallback = Payload.has_value();
			FailureFingerprint = Failure->GetFingerprint();
			Availability = Payload
				? ERenderResourceAvailability::StaleReady
				: ERenderResourceAvailability::Failed;
			std::forward<DiagnosticReporterType>(ReportDiagnostic)(
				FRenderResourceCreateDiagnostic{
					.Kind = ERenderResourceCreateDiagnosticKind::Failure,
					.Error = Failure,
				});
			bFailureReported = true;
			return Payload ? &*Payload : nullptr;
		}

		auto GetPayload() -> PayloadType*
		{
			return Payload ? &*Payload : nullptr;
		}

		auto GetPayload() const -> const PayloadType*
		{
			return Payload ? &*Payload : nullptr;
		}

		auto GetFailure() const -> const FRenderResourceCreateError*
		{
			return Failure ? &*Failure : nullptr;
		}

		auto GetAvailability() const -> ERenderResourceAvailability
		{
			return Availability;
		}

		auto GetPayloadGeneration() const -> const FRenderResourceGeneration&
		{
			return PayloadGeneration;
		}

		auto GetAttemptedGeneration() const -> const FRenderResourceGeneration&
		{
			return AttemptedGeneration;
		}

		auto GetFailureFingerprint() const -> std::optional<size_t>
		{
			return FailureFingerprint;
		}

		auto Reset() -> void
		{
			Payload.reset();
			Failure.reset();
			FailureFingerprint.reset();
			PayloadGeneration = {};
			AttemptedGeneration = {};
			ObservedDeviceGeneration = 0;
			bHasObservedDeviceGeneration = false;
			bHasAttempt = false;
			bResolving = false;
			bFailureReported = false;
			Availability = ERenderResourceAvailability::Uninitialized;
		}

	private:
		auto ApplyDeviceGeneration(
			const FRenderResourceGeneration& Generation) -> void
		{
			if (!HasRenderResourceGenerationDependency(
					PayloadDependencies,
					ERenderResourceGenerationDependency::Device))
			{
				return;
			}
			if (!bHasObservedDeviceGeneration)
			{
				ObservedDeviceGeneration = Generation.Device;
				bHasObservedDeviceGeneration = true;
				return;
			}
			if (ObservedDeviceGeneration == Generation.Device)
			{
				return;
			}

			Payload.reset();
			Failure.reset();
			FailureFingerprint.reset();
			PayloadGeneration = {};
			AttemptedGeneration = {};
			ObservedDeviceGeneration = Generation.Device;
			bHasAttempt = false;
			bFailureReported = false;
			Availability = ERenderResourceAvailability::Uninitialized;
		}

		auto ShouldSuppressAttempt(
			const FRenderResourceGeneration& Generation) const -> bool
		{
			if (!bHasAttempt)
			{
				return false;
			}
			const ERenderResourceGenerationDependency RetryDependencies =
				Failure
				? Failure->RetryDependencies
				: PayloadDependencies;
			return !HasSelectedRenderResourceGenerationChanged(
				AttemptedGeneration,
				Generation,
				RetryDependencies);
		}

		ERenderResourceGenerationDependency PayloadDependencies;
		std::optional<PayloadType> Payload;
		std::optional<FRenderResourceCreateError> Failure;
		std::optional<size_t> FailureFingerprint;
		FRenderResourceGeneration PayloadGeneration;
		FRenderResourceGeneration AttemptedGeneration;
		uint64 ObservedDeviceGeneration = 0;
		bool bHasObservedDeviceGeneration = false;
		bool bHasAttempt = false;
		bool bResolving = false;
		bool bFailureReported = false;
		ERenderResourceAvailability Availability =
			ERenderResourceAvailability::Uninitialized;
	};
}
