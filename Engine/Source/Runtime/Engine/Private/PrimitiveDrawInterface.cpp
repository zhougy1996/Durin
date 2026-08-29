#include "PrimitiveDrawInterface.h"

#include "HAL/PlatformLTS.h"
#include "Math/Operations.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		auto IsFinite(const FVector4f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		auto IsValidStyle(const FSimpleElementLineStyle& Style) -> bool
		{
			return std::isfinite(Style.WidthPixels)
				&& Style.WidthPixels > 0.0f
				&& std::isfinite(Style.PatternPeriodPixels)
				&& Style.PatternPeriodPixels > 0.0f
				&& std::isfinite(Style.DepthBias);
		}
	} // namespace

	FPrimitiveDrawInterface::~FPrimitiveDrawInterface() = default;

	FViewPrimitiveDrawInterface::FViewPrimitiveDrawInterface(FSceneView& InView)
		: Submission(&InView.SimpleElements)
		, ProducerThreadId(FPlatformLTS::GetCurrentThreadId())
	{
	}

	FViewPrimitiveDrawInterface::~FViewPrimitiveDrawInterface() = default;

	auto FViewPrimitiveDrawInterface::CanAdmit(
		ESceneDepthPriorityGroup DepthPriorityGroup) -> bool
	{
		checkf(ProducerThreadId == FPlatformLTS::GetCurrentThreadId(),
			"FViewPrimitiveDrawInterface must be used on its producer thread");
		if (Submission == nullptr || Submission->bSealed
			|| DepthPriorityGroup >= ESceneDepthPriorityGroup::Count)
		{
			return false;
		}
		return true;
	}

	auto FViewPrimitiveDrawInterface::Add(
		FSimpleElement Element, uint64 PayloadBytes) -> void
	{
		if (Submission->Elements.size()
				>= FSimpleElementViewSubmission::MaxElementCount
			|| PayloadBytes > FSimpleElementViewSubmission::MaxPayloadBytes
				- Submission->PayloadBytes)
		{
			++Submission->DroppedElementCount;
			return;
		}
		Element.SubmissionOrder = Submission->NextSubmissionOrder++;
		Submission->PayloadBytes += PayloadBytes;
		Submission->Elements.push_back(std::move(Element));
	}

	auto FViewPrimitiveDrawInterface::DrawLine(const FVector3& Start,
		const FVector3& End, const FVector4f& Color,
		ESceneDepthPriorityGroup DepthPriorityGroup,
		const FSimpleElementLineStyle& Style) -> void
	{
		if (!CanAdmit(DepthPriorityGroup) || !Math::IsFinite(Start)
			|| !Math::IsFinite(End) || !IsFinite(Color)
			|| !IsValidStyle(Style)
			|| Math::LengthSquared(End - Start) <= kSmallNumber)
		{
			if (Submission != nullptr && !Submission->bSealed
				&& ProducerThreadId == FPlatformLTS::GetCurrentThreadId())
				++Submission->DroppedElementCount;
			return;
		}
		FVector4f OpaqueColor = Color;
		OpaqueColor.w = 1.0f;
		Add({.Type = ESimpleElementType::Line,
			.BlendMode = ESimpleElementBlendMode::Opaque,
			.DepthPriorityGroup = DepthPriorityGroup,
			.Value = FSimpleElementLine{Start, End, OpaqueColor, Style}},
			sizeof(FSimpleElementLine));
	}

	auto FViewPrimitiveDrawInterface::DrawTranslucentLine(
		const FVector3& Start, const FVector3& End, const FVector4f& Color,
		ESceneDepthPriorityGroup DepthPriorityGroup,
		const FSimpleElementLineStyle& Style) -> void
	{
		if (!CanAdmit(DepthPriorityGroup) || !Math::IsFinite(Start)
			|| !Math::IsFinite(End) || !IsFinite(Color)
			|| !IsValidStyle(Style) || Color.w <= 0.0f
			|| Math::LengthSquared(End - Start) <= kSmallNumber)
		{
			if (Submission != nullptr && !Submission->bSealed
				&& ProducerThreadId == FPlatformLTS::GetCurrentThreadId())
				++Submission->DroppedElementCount;
			return;
		}
		Add({.Type = ESimpleElementType::Line,
			.BlendMode = ESimpleElementBlendMode::Translucent,
			.DepthPriorityGroup = DepthPriorityGroup,
			.Value = FSimpleElementLine{Start, End, Color, Style}},
			sizeof(FSimpleElementLine));
	}

	auto FViewPrimitiveDrawInterface::DrawPoint(const FVector3& Position,
		const FVector4f& Color, float PointSizePixels,
		ESceneDepthPriorityGroup DepthPriorityGroup, float DepthBias) -> void
	{
		if (!CanAdmit(DepthPriorityGroup) || !Math::IsFinite(Position)
			|| !IsFinite(Color) || !std::isfinite(PointSizePixels)
			|| PointSizePixels <= 0.0f || !std::isfinite(DepthBias))
		{
			if (Submission != nullptr && !Submission->bSealed
				&& ProducerThreadId == FPlatformLTS::GetCurrentThreadId())
				++Submission->DroppedElementCount;
			return;
		}
		const ESimpleElementBlendMode BlendMode = Color.w < 1.0f
			? ESimpleElementBlendMode::Translucent
			: ESimpleElementBlendMode::Opaque;
		Add({.Type = ESimpleElementType::Point, .BlendMode = BlendMode,
			.DepthPriorityGroup = DepthPriorityGroup,
			.Value = FSimpleElementPoint{
				Position, Color, PointSizePixels, DepthBias}},
			sizeof(FSimpleElementPoint));
	}

	auto FViewPrimitiveDrawInterface::DrawSprite(const FVector3& Position,
		const FVector2f& SizePixels, const FSimpleElementTexture& Texture,
		const FVector2f& MinUV, const FVector2f& MaxUV,
		const FVector4f& Color,
		ESceneDepthPriorityGroup DepthPriorityGroup, float DepthBias) -> void
	{
		if (!CanAdmit(DepthPriorityGroup) || !Math::IsFinite(Position)
			|| !std::isfinite(SizePixels.x) || !std::isfinite(SizePixels.y)
			|| SizePixels.x <= 0.0f || SizePixels.y <= 0.0f
			|| !std::isfinite(MinUV.x) || !std::isfinite(MinUV.y)
			|| !std::isfinite(MaxUV.x) || !std::isfinite(MaxUV.y)
			|| MinUV.x >= MaxUV.x || MinUV.y >= MaxUV.y
			|| !IsFinite(Color) || Color.w <= 0.0f || !Texture.IsValid()
			|| !std::isfinite(DepthBias))
		{
			if (Submission != nullptr && !Submission->bSealed
				&& ProducerThreadId == FPlatformLTS::GetCurrentThreadId())
				++Submission->DroppedElementCount;
			return;
		}
		Add({.Type = ESimpleElementType::Sprite,
			.BlendMode = ESimpleElementBlendMode::Translucent,
			.DepthPriorityGroup = DepthPriorityGroup,
			.Value = FSimpleElementSprite{Position, SizePixels, MinUV, MaxUV,
				Color, Texture, DepthBias}}, sizeof(FSimpleElementSprite));
	}

	auto FViewPrimitiveDrawInterface::Seal() -> void
	{
		checkf(ProducerThreadId == FPlatformLTS::GetCurrentThreadId(),
			"FViewPrimitiveDrawInterface must be sealed on its producer thread");
		if (Submission == nullptr)
			return;
		Submission->bSealed = true;
	}
} // namespace Durin
