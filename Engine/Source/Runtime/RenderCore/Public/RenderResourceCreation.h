#pragma once

#include "Misc/CoreTypes.h"

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

	struct FRenderResourceGeneration
	{
		uint64 Shader = 0;
		uint64 Device = 0;
		uint64 Manual = 0;

		auto operator==(const FRenderResourceGeneration&) const -> bool = default;
	};

	struct FRenderResourceCreateError
	{
		ERenderResourceCreateErrorCategory Category =
			ERenderResourceCreateErrorCategory::InvalidConfiguration;
		std::string Context;
		std::string Identity;
		std::string Message;
		ERenderResourceGenerationDependency RetryDependencies =
			ERenderResourceGenerationDependency::Manual;
		FRenderResourceGeneration AttemptedGeneration;
		bool bRetainedFallback = false;
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

	// Stage-0 contract scaffold. Stage 1 replaces the one-shot behavior below
	// with generation-aware retry and transactional refresh semantics.
	template <typename PayloadType>
	class TRenderResourceCreationSlot
	{
	public:
		explicit TRenderResourceCreationSlot(
			ERenderResourceGenerationDependency InPayloadDependencies)
			: PayloadDependencies(InPayloadDependencies)
		{
		}

		template <typename FactoryType, typename DiagnosticReporterType>
		auto Resolve(
			const FRenderResourceGeneration& Generation,
			FactoryType&& Factory,
			DiagnosticReporterType&& ReportDiagnostic) -> PayloadType*
		{
			if (bHasAttempt)
			{
				return Payload ? &*Payload : nullptr;
			}

			bHasAttempt = true;
			Availability = ERenderResourceAvailability::Creating;
			auto Result = std::forward<FactoryType>(Factory)();
			if (Result.HasPayload())
			{
				Payload.emplace(Result.TakePayload());
				PayloadGeneration = Generation;
				Availability = ERenderResourceAvailability::Ready;
				return &*Payload;
			}

			Failure.emplace(Result.TakeError());
			Failure->AttemptedGeneration = Generation;
			Availability = ERenderResourceAvailability::Failed;
			std::forward<DiagnosticReporterType>(ReportDiagnostic)(
				FRenderResourceCreateDiagnostic{
					.Kind = ERenderResourceCreateDiagnosticKind::Failure,
					.Error = Failure,
				});
			return nullptr;
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

		auto Reset() -> void
		{
			Payload.reset();
			Failure.reset();
			PayloadGeneration = {};
			bHasAttempt = false;
			Availability = ERenderResourceAvailability::Uninitialized;
		}

	private:
		ERenderResourceGenerationDependency PayloadDependencies;
		std::optional<PayloadType> Payload;
		std::optional<FRenderResourceCreateError> Failure;
		FRenderResourceGeneration PayloadGeneration;
		bool bHasAttempt = false;
		ERenderResourceAvailability Availability =
			ERenderResourceAvailability::Uninitialized;
	};
}
