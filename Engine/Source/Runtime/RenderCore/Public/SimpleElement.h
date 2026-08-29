#pragma once

#include "Math/DurinMath.h"
#include "RHIResources.h"

namespace Durin
{
	class FViewPrimitiveDrawInterface;

	// Selects whether a simple primitive is rejected by scene depth.
	enum class ESceneDepthPriorityGroup : uint8
	{
		World,
		Foreground,
		Count,
	};

	// Selects solid or distance-patterned rendering for a simple line.
	enum class ESimpleElementLinePattern : uint8
	{
		Solid,
		Dashed,
	};

	// Carries Durin-specific pixel-width and dash policy for one line.
	struct FSimpleElementLineStyle
	{
		float WidthPixels = 1.0f;
		ESimpleElementLinePattern Pattern = ESimpleElementLinePattern::Solid;
		float PatternPeriodPixels = 12.0f;
		float DepthBias = 0.0f;

		auto operator==(const FSimpleElementLineStyle&) const -> bool = default;
	};

	// Retains a stable sprite resource identity without retaining an Engine asset.
	struct FSimpleElementTexture
	{
		enum class EKind : uint8
		{
			Invalid,
			TextureReference,
			EditorIconAtlas,
		};

		static auto FromTextureReference(FRHITextureReferenceRef InReference)
			-> FSimpleElementTexture
		{
			return {.Kind = InReference != nullptr ? EKind::TextureReference
				: EKind::Invalid, .TextureReference = std::move(InReference)};
		}

		static auto EditorIconAtlas() -> FSimpleElementTexture
		{
			return {.Kind = EKind::EditorIconAtlas};
		}

		auto IsValid() const -> bool { return Kind != EKind::Invalid; }
		auto operator==(const FSimpleElementTexture&) const -> bool = default;

		EKind Kind = EKind::Invalid;
		FRHITextureReferenceRef TextureReference;
	};

	enum class ESimpleElementType : uint8
	{
		Line,
		Point,
		Sprite,
	};

	enum class ESimpleElementBlendMode : uint8
	{
		Opaque,
		Translucent,
	};

	struct FSimpleElementLine
	{
		FVector3 Start{0.0};
		FVector3 End{0.0};
		FVector4f Color{1.0f};
		FSimpleElementLineStyle Style;
	};

	struct FSimpleElementPoint
	{
		FVector3 Position{0.0};
		FVector4f Color{1.0f};
		float SizePixels = 1.0f;
		float DepthBias = 0.0f;
	};

	struct FSimpleElementSprite
	{
		FVector3 Position{0.0};
		FVector2f SizePixels{1.0f};
		FVector2f MinUV{0.0f};
		FVector2f MaxUV{1.0f};
		FVector4f Color{1.0f};
		FSimpleElementTexture Texture;
		float DepthBias = 0.0f;
	};

	// Owns one copied primitive and its explicit ordering/depth classification.
	struct FSimpleElement
	{
		ESimpleElementType Type = ESimpleElementType::Line;
		ESimpleElementBlendMode BlendMode = ESimpleElementBlendMode::Opaque;
		ESceneDepthPriorityGroup DepthPriorityGroup =
			ESceneDepthPriorityGroup::World;
		uint64 SubmissionOrder = 0;
		std::variant<FSimpleElementLine, FSimpleElementPoint,
			FSimpleElementSprite> Value = FSimpleElementLine{};
	};

	// Transports a bounded immutable primitive list after producer-side sealing.
	class FSimpleElementViewSubmission
	{
	public:
		static constexpr uint32 MaxElementCount = 65'536;
		static constexpr uint64 MaxPayloadBytes = 8ull * 1024ull * 1024ull;

		auto GetElements() const -> std::span<const FSimpleElement>
		{
			return Elements;
		}

		auto GetPayloadBytes() const -> uint64 { return PayloadBytes; }
		auto GetDroppedElementCount() const -> uint32
		{
			return DroppedElementCount;
		}
		auto IsSealed() const -> bool { return bSealed; }

	private:
		friend class FViewPrimitiveDrawInterface;

		std::vector<FSimpleElement> Elements;
		uint64 PayloadBytes = 0;
		uint64 NextSubmissionOrder = 0;
		uint32 DroppedElementCount = 0;
		bool bSealed = false;
	};
} // namespace Durin
