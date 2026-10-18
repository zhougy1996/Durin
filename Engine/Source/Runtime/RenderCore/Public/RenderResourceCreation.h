#pragma once

#include "CoreMinimal.h"

#include "Misc/CoreTypes.h"
#include "Misc/AssertionMacros.h"
#include "RenderPipelineCreation.h"
#include "RenderResourcePreparation.h"
#include "Shader/ShaderDiagnostics.h"
#include "Shader/ShaderMapPreparation.h"

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
		ShaderFailure,
		ShaderCreationFailed,
		ResourceCreationFailed,
		PipelineCreationFailed,
		SamplerCreationFailed,
		FullscreenGeometryUnavailable,
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

	using FRenderResourceCreateCause = std::variant<std::monostate, FShaderError, std::string>;

	struct FRenderResourceCreateError
	{
		ERenderResourceCreateErrorCategory Category =
			ERenderResourceCreateErrorCategory::InvalidConfiguration;
		ERenderResourceCreateErrorReason Reason =
			ERenderResourceCreateErrorReason::Unspecified;
		std::string Context;
		std::string Identity;
		FRenderResourceCreateCause Cause;
		ERenderResourceGenerationDependency RetryDependencies =
			ERenderResourceGenerationDependency::Manual;
		FRenderResourceGeneration AttemptedGeneration;

		RENDERCORE_API auto GetFingerprint() const -> size_t;
	};

	RENDERCORE_API auto FormatRenderResourceCreateError(const FRenderResourceCreateError& Error) -> std::string;

	enum class ERenderResourceAvailability : uint8
	{
		Uninitialized,
		Pending,
		Ready,
		Failed,
		Canceled,
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

		static auto Canceled() -> TRenderResourceCreateResult { TRenderResourceCreateResult Result; Result.bCanceled = true; return Result; }
		auto IsCanceled() const -> bool { return bCanceled; }
		auto HasPayload() const -> bool { return Payload.has_value(); }
		auto TakePayload() -> PayloadType { return std::move(*Payload); }
		auto TakeError() -> FRenderResourceCreateError
		{
			return std::move(*Error);
		}

	private:
		bool bCanceled = false;
		std::optional<PayloadType> Payload;
		std::optional<FRenderResourceCreateError> Error;
	};

	// Owns one complete renderer payload and its generation-scoped attempt state.
	// Shader preparation and pipeline requests may defer an attempt.
	// Only Ready exposes a payload;
	// a new attempt invalidates the previous generation before invoking its factory.
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
				return GetPayload();
			}

			ApplyDeviceGeneration(Generation);
			if (ShouldSuppressAttempt(Generation))
			{
				return GetPayload();
			}

			bResolving = true;
			struct FResolveGuard { bool& Flag; ~FResolveGuard() { Flag = false; } } ResolveGuard{bResolving};
			Payload.reset();
			PayloadGeneration = {};
			Availability = ERenderResourceAvailability::Pending;
			FRenderPipelineRequestScope PipelineScope(PipelineRequests, Generation);
			const uint64 PendingShadersBefore = FPreparedShaderMapScope::GetPendingRequestCount();
			auto Result = std::forward<FactoryType>(Factory)();
			bResolving = false;
			if (PipelineScope.HasPending()
				|| FPreparedShaderMapScope::GetPendingRequestCount() != PendingShadersBefore)
			{
				bHasAttempt = false;
				return nullptr;
			}
			if (Result.IsCanceled()) { PipelineRequests.Reset(); bHasAttempt = false; Failure.reset(); FailureFingerprint.reset(); bFailureReported = false; Availability = ERenderResourceAvailability::Canceled; return nullptr; }
			bHasAttempt = true;
			AttemptedGeneration = Generation;
			if (Result.HasPayload())
			{
				auto Candidate = std::make_shared<PayloadType>(Result.TakePayload());
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
				return GetPayload();
			}

			PipelineRequests.Reset();
			const auto PreviousFingerprint = FailureFingerprint;
			Failure.emplace(Result.TakeError());
			Failure->AttemptedGeneration = Generation;
			FailureFingerprint = Failure->GetFingerprint();
			Availability = ERenderResourceAvailability::Failed;
			if (PreviousFingerprint != FailureFingerprint)
			{
				std::forward<DiagnosticReporterType>(ReportDiagnostic)(
					FRenderResourceCreateDiagnostic{
						.Kind = ERenderResourceCreateDiagnosticKind::Failure,
						.Error = Failure,
					});
			}
			bFailureReported = true;
			return nullptr;
		}

		auto GetPayload() -> PayloadType*
		{
			if (Availability != ERenderResourceAvailability::Ready || !Payload) return nullptr;
			PipelineRequests.RetainForPreparation();
			FRenderResourcePreparationScope::Retain(Payload, sizeof(PayloadType));
			return Payload.get();
		}

		auto GetPayload() const -> const PayloadType*
		{
			if (Availability != ERenderResourceAvailability::Ready || !Payload) return nullptr;
			PipelineRequests.RetainForPreparation();
			FRenderResourcePreparationScope::Retain(Payload, sizeof(PayloadType));
			return Payload.get();
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
			PipelineRequests.Reset();
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
			if (Failure && (Failure->Category == ERenderResourceCreateErrorCategory::RHIResource
				|| Failure->Category == ERenderResourceCreateErrorCategory::GraphicsPipeline
				|| Failure->Reason == ERenderResourceCreateErrorReason::ShaderCreationFailed
				|| Failure->Reason == ERenderResourceCreateErrorReason::SamplerCreationFailed))
				return false;
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
		FRenderPipelineRequests PipelineRequests;
		std::shared_ptr<PayloadType> Payload;
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
