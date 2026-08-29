#include "Renderers/SimpleElement/SimpleElementCollector.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr double ClipEpsilon = 1.e-8;

		struct FClippedSegment
		{
			FVector4 Start{0.0};
			FVector4 End{0.0};
			double StartT = 0.0;
			double EndT = 1.0;
		};

		auto IsFinite(const FVector4& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		auto ClipSegment(FVector4 Start, FVector4 End)
			-> std::optional<FClippedSegment>
		{
			if (!IsFinite(Start) || !IsFinite(End))
				return std::nullopt;
			FClippedSegment Result{Start, End};
			auto ClipPlane = [&Result](auto PlaneDistance, double Minimum) {
				const double StartDistance = PlaneDistance(Result.Start);
				const double EndDistance = PlaneDistance(Result.End);
				if (StartDistance < Minimum && EndDistance < Minimum)
					return false;
				if (StartDistance < Minimum || EndDistance < Minimum)
				{
					const double LocalT = (Minimum - StartDistance)
						/ (EndDistance - StartDistance);
					const FVector4 Intersection =
						Math::Lerp(Result.Start, Result.End, LocalT);
					const double SourceT = Result.StartT
						+ (Result.EndT - Result.StartT) * LocalT;
					if (StartDistance < Minimum)
					{
						Result.Start = Intersection;
						Result.StartT = SourceT;
					}
					else
					{
						Result.End = Intersection;
						Result.EndT = SourceT;
					}
				}
				return true;
			};
			if (!ClipPlane([](const FVector4& Value) { return Value.w; },
					ClipEpsilon)
				|| !ClipPlane(
					[](const FVector4& Value) { return Value.z; }, 0.0))
			{
				return std::nullopt;
			}
			return Result;
		}

		auto ApplyDepthBias(FVector4 Position, float DepthBias,
			ESceneDepthConvention Convention) -> FVector4
		{
			const double Direction = Convention == ESceneDepthConvention::ReversedZ
				? 1.0 : -1.0;
			Position.z += Direction * static_cast<double>(DepthBias) * Position.w;
			return Position;
		}

		auto MakeClipPosition(const FVector4& Clip, const FVector2& Offset)
			-> FVector4f
		{
			return {
				static_cast<float>(Clip.x + Offset.x * Clip.w),
				static_cast<float>(Clip.y + Offset.y * Clip.w),
				static_cast<float>(Clip.z), static_cast<float>(Clip.w)};
		}

		auto MakeKey(const FSimpleElement& Element, const FSceneView& View,
			RenderTargetLayouts::EViewportOutput Output)
			-> FSimpleElementBatchKey
		{
			FSimpleElementBatchKey Key{
				.ShaderClass = Element.Type == ESimpleElementType::Sprite
					? ESimpleElementShaderClass::Textured
					: ESimpleElementShaderClass::Untextured,
				.BlendMode = Element.BlendMode,
				.DepthPriorityGroup = Element.DepthPriorityGroup,
				.DepthConvention = View.DepthConvention,
				.Output = Output,
			};
			if (Element.Type == ESimpleElementType::Sprite)
				Key.Texture = std::get<FSimpleElementSprite>(Element.Value).Texture;
			return Key;
		}

		auto AddQuad(FPreparedSimpleElementBatch& Batch,
			const std::array<FSimpleElementVertex, 4>& Vertices) -> void
		{
			const uint32 Base = static_cast<uint32>(Batch.Vertices.size());
			Batch.Vertices.insert(Batch.Vertices.end(),
				Vertices.begin(), Vertices.end());
			Batch.Indices.insert(Batch.Indices.end(), {
				Base, Base + 1, Base + 2, Base + 2, Base + 1, Base + 3});
		}

		auto AppendLine(const FSceneView& View,
			const FSimpleElementLine& Line,
			FPreparedSimpleElementBatch& Batch) -> bool
		{
			const FVector4 OriginalStart = ApplyDepthBias(
				View.ViewProjectionMatrix * FVector4(Line.Start, 1.0),
				Line.Style.DepthBias, View.DepthConvention);
			const FVector4 OriginalEnd = ApplyDepthBias(
				View.ViewProjectionMatrix * FVector4(Line.End, 1.0),
				Line.Style.DepthBias, View.DepthConvention);
			const std::optional<FClippedSegment> Clipped =
				ClipSegment(OriginalStart, OriginalEnd);
			if (!Clipped)
				return false;
			const FVector2 NdcStart = FVector2(Clipped->Start) / Clipped->Start.w;
			const FVector2 NdcEnd = FVector2(Clipped->End) / Clipped->End.w;
			const FVector2f PixelDelta{
				static_cast<float>((NdcEnd.x - NdcStart.x) * 0.5
					* View.ViewportWidth),
				static_cast<float>((NdcEnd.y - NdcStart.y) * 0.5
					* View.ViewportHeight)};
			const float PixelLength = Math::Length(PixelDelta);
			if (!std::isfinite(PixelLength) || PixelLength <= 0.001f)
				return false;
			const FVector2f PixelNormal{
				-PixelDelta.y / PixelLength, PixelDelta.x / PixelLength};
			const float HalfWidth = std::max(0.5f,
				Line.Style.WidthPixels * 0.5f);
			const FVector2 NdcOffset{
				static_cast<double>(PixelNormal.x * HalfWidth * 2.0f
					/ std::max(1u, View.ViewportWidth)),
				static_cast<double>(PixelNormal.y * HalfWidth * 2.0f
					/ std::max(1u, View.ViewportHeight))};
			const float PatternPeriod =
				Line.Style.Pattern == ESimpleElementLinePattern::Dashed
				? std::max(2.0f, Line.Style.PatternPeriodPixels) : 0.0f;
			const double VisibleFraction = Clipped->EndT - Clipped->StartT;
			const float OriginalPixelLength = VisibleFraction > ClipEpsilon
				? PixelLength / static_cast<float>(VisibleFraction) : PixelLength;
			const float PatternStart = OriginalPixelLength
				* static_cast<float>(Clipped->StartT);
			AddQuad(Batch, {{
				{MakeClipPosition(Clipped->Start, NdcOffset), {}, Line.Color,
					{PatternStart, PatternPeriod}},
				{MakeClipPosition(Clipped->Start, -NdcOffset), {}, Line.Color,
					{PatternStart, PatternPeriod}},
				{MakeClipPosition(Clipped->End, NdcOffset), {}, Line.Color,
					{PatternStart + PixelLength, PatternPeriod}},
				{MakeClipPosition(Clipped->End, -NdcOffset), {}, Line.Color,
					{PatternStart + PixelLength, PatternPeriod}},
			}});
			return true;
		}

		auto AppendBillboard(const FSceneView& View, const FVector3& Position,
			const FVector2f& SizePixels, const FVector2f& MinUV,
			const FVector2f& MaxUV, const FVector4f& Color, float DepthBias,
			FPreparedSimpleElementBatch& Batch) -> bool
		{
			const FVector4 Clip = ApplyDepthBias(
				View.ViewProjectionMatrix * FVector4(Position, 1.0),
				DepthBias, View.DepthConvention);
			if (!IsFinite(Clip) || Clip.w <= ClipEpsilon || Clip.z < 0.0)
				return false;
			const double HalfNdcX = static_cast<double>(SizePixels.x)
				/ std::max(1u, View.ViewportWidth);
			const double HalfNdcY = static_cast<double>(SizePixels.y)
				/ std::max(1u, View.ViewportHeight);
			AddQuad(Batch, {{
				{MakeClipPosition(Clip, {-HalfNdcX, -HalfNdcY}), MinUV, Color, {}},
				{MakeClipPosition(Clip, {HalfNdcX, -HalfNdcY}),
					{MaxUV.x, MinUV.y}, Color, {}},
				{MakeClipPosition(Clip, {-HalfNdcX, HalfNdcY}),
					{MinUV.x, MaxUV.y}, Color, {}},
				{MakeClipPosition(Clip, {HalfNdcX, HalfNdcY}), MaxUV, Color, {}},
			}});
			return true;
		}

		auto AppendElement(const FSceneView& View,
			const FSimpleElement& Element,
			FPreparedSimpleElementBatch& Batch) -> bool
		{
			switch (Element.Type)
			{
			case ESimpleElementType::Line:
				return AppendLine(View,
					std::get<FSimpleElementLine>(Element.Value), Batch);
			case ESimpleElementType::Point:
			{
				const auto& Point =
					std::get<FSimpleElementPoint>(Element.Value);
				return AppendBillboard(View, Point.Position,
					FVector2f(Point.SizePixels), {}, {}, Point.Color,
					Point.DepthBias, Batch);
			}
			case ESimpleElementType::Sprite:
			{
				const auto& Sprite =
					std::get<FSimpleElementSprite>(Element.Value);
				return AppendBillboard(View, Sprite.Position, Sprite.SizePixels,
					Sprite.MinUV, Sprite.MaxUV, Sprite.Color,
					Sprite.DepthBias, Batch);
			}
			}
			return false;
		}

	} // namespace

	auto FSimpleElementCollector::Collect(const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output)
		-> FPreparedSimpleElements
	{
		FPreparedSimpleElements Prepared;
		std::vector<FSimpleElement> Elements(
			View.SimpleElements.GetElements().begin(),
			View.SimpleElements.GetElements().end());
		Prepared.Statistics.SubmittedElementCount =
			static_cast<uint32>(Elements.size());

		static constexpr std::array DepthOrder{
			ESceneDepthPriorityGroup::Foreground,
			ESceneDepthPriorityGroup::World};
		static constexpr std::array BlendOrder{
			ESimpleElementBlendMode::Opaque,
			ESimpleElementBlendMode::Translucent};
		static constexpr std::array ShaderOrder{
			ESimpleElementShaderClass::Untextured,
			ESimpleElementShaderClass::Textured};
		uint64 TotalBytes = 0;
		for (const ESceneDepthPriorityGroup Depth : DepthOrder)
		{
			for (const ESimpleElementBlendMode Blend : BlendOrder)
			{
				for (const ESimpleElementShaderClass ShaderClass : ShaderOrder)
				{
					for (const FSimpleElement& Element : Elements)
					{
						const FSimpleElementBatchKey Key =
							MakeKey(Element, View, Output);
						if (Key.DepthPriorityGroup != Depth
							|| Key.BlendMode != Blend
							|| Key.ShaderClass != ShaderClass)
							continue;
						FPreparedSimpleElementBatch Candidate;
						Candidate.Key = Key;
						Candidate.FirstSubmissionOrder = Element.SubmissionOrder;
						if (!AppendElement(View, Element, Candidate))
						{
							++Prepared.Statistics.DroppedElementCount;
							continue;
						}
						Candidate.SourceElementCount = 1;
						Candidate.VertexBytes = Candidate.Vertices.size()
							* sizeof(FSimpleElementVertex);
						Candidate.IndexBytes = Candidate.Indices.size()
							* sizeof(uint32);
						const uint64 CandidateBytes = Candidate.VertexBytes
							+ Candidate.IndexBytes;
						if (CandidateBytes > MaxPreparedBytes - TotalBytes)
						{
							++Prepared.Statistics.DroppedElementCount;
							continue;
						}
						TotalBytes += CandidateBytes;
						++Prepared.Statistics.AcceptedElementCount;
						if (!Prepared.Batches.empty()
							&& Prepared.Batches.back().Key == Candidate.Key)
						{
							auto& Batch = Prepared.Batches.back();
							const uint32 VertexBase =
								static_cast<uint32>(Batch.Vertices.size());
							Batch.Vertices.insert(Batch.Vertices.end(),
								Candidate.Vertices.begin(), Candidate.Vertices.end());
							for (const uint32 Index : Candidate.Indices)
								Batch.Indices.push_back(VertexBase + Index);
							++Batch.SourceElementCount;
							Batch.VertexBytes += Candidate.VertexBytes;
							Batch.IndexBytes += Candidate.IndexBytes;
						}
						else
						{
							Prepared.Batches.push_back(std::move(Candidate));
						}
					}
				}
			}
		}
		Prepared.Statistics.BatchCount =
			static_cast<uint32>(Prepared.Batches.size());
		for (const FPreparedSimpleElementBatch& Batch : Prepared.Batches)
		{
			Prepared.Statistics.VertexCount +=
				static_cast<uint32>(Batch.Vertices.size());
			Prepared.Statistics.IndexCount +=
				static_cast<uint32>(Batch.Indices.size());
			Prepared.Statistics.VertexBytes += Batch.VertexBytes;
			Prepared.Statistics.IndexBytes += Batch.IndexBytes;
		}
		return Prepared;
	}
} // namespace Durin
