#include "Renderers/SimpleElement/EditorIconAtlas.h"

namespace Durin
{
	auto BuildEditorIconAtlasPixels()
		-> std::array<uint8, FEditorIconAtlasLayout::PixelByteCount>
	{
		constexpr uint32 Size = FEditorIconAtlasLayout::IconExtent;
		constexpr uint32 SamplesPerAxis = 4;
		constexpr uint32 AtlasWidth = FEditorIconAtlasLayout::Width;
		std::array<uint8, FEditorIconAtlasLayout::PixelByteCount> Pixels{};
		auto InsideCircle = [](float X, float Y, float CenterX,
			float CenterY, float Radius) {
			const float DX = X - CenterX;
			const float DY = Y - CenterY;
			return DX * DX + DY * DY <= Radius * Radius;
		};
		auto InsideLens = [](float X, float Y) {
			if (X < 42.0f || X > 57.0f)
				return false;
			const float T = (X - 42.0f) / 15.0f;
			const float Top = 29.0f * (1.0f - T) + 20.0f * T;
			const float Bottom = 43.0f * (1.0f - T) + 52.0f * T;
			return Y >= Top && Y <= Bottom;
		};
		auto InsideTriangle = [](float X, float Y, const FVector2f& A,
			const FVector2f& B, const FVector2f& C) {
			auto Edge = [](const FVector2f& Start, const FVector2f& End,
				const FVector2f& Point) {
				return (Point.x - Start.x) * (End.y - Start.y)
					- (Point.y - Start.y) * (End.x - Start.x);
			};
			const FVector2f Point{X, Y};
			const float AB = Edge(A, B, Point);
			const float BC = Edge(B, C, Point);
			const float CA = Edge(C, A, Point);
			return (AB >= 0.0f && BC >= 0.0f && CA >= 0.0f)
				|| (AB <= 0.0f && BC <= 0.0f && CA <= 0.0f);
		};
		for (uint32 Y = 0; Y < Size; ++Y)
		{
			for (uint32 X = 0; X < Size; ++X)
			{
				uint32 CoveredSamples = 0;
				for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
				{
					for (uint32 SampleX = 0; SampleX < SamplesPerAxis; ++SampleX)
					{
						const float PX = static_cast<float>(X)
							+ (static_cast<float>(SampleX) + 0.5f) / SamplesPerAxis;
						const float PY = static_cast<float>(Y)
							+ (static_cast<float>(SampleY) + 0.5f) / SamplesPerAxis;
						const bool bBody = PX >= 9.0f && PX <= 44.0f
							&& PY >= 27.0f && PY <= 49.0f;
						const bool bReels =
							InsideCircle(PX, PY, 19.0f, 21.0f, 10.0f)
							|| InsideCircle(PX, PY, 37.0f, 20.0f, 9.0f);
						const bool bReelHole =
							InsideCircle(PX, PY, 19.0f, 21.0f, 3.5f)
							|| InsideCircle(PX, PY, 37.0f, 20.0f, 3.0f);
						if ((bBody || bReels || InsideLens(PX, PY)) && !bReelHole)
							++CoveredSamples;
					}
				}
				const size_t Offset =
					(static_cast<size_t>(Y) * AtlasWidth + X) * 4;
				Pixels[Offset + 0] = 255;
				Pixels[Offset + 1] = 255;
				Pixels[Offset + 2] = 255;
				Pixels[Offset + 3] = static_cast<uint8>(CoveredSamples * 255
					/ (SamplesPerAxis * SamplesPerAxis));
			}
		}
		for (uint32 Y = 0; Y < Size; ++Y)
		{
			for (uint32 X = 0; X < Size; ++X)
			{
				uint32 CoveredSamples = 0;
				for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
				{
					for (uint32 SampleX = 0; SampleX < SamplesPerAxis; ++SampleX)
					{
						const float PX = static_cast<float>(X)
							+ (static_cast<float>(SampleX) + 0.5f) / SamplesPerAxis
							- 32.0f;
						const float PY = static_cast<float>(Y)
							+ (static_cast<float>(SampleY) + 0.5f) / SamplesPerAxis
							- 32.0f;
						const float Radius = std::sqrt(PX * PX + PY * PY);
						const float RayAxisDistance =
							std::abs(std::sin(std::atan2(PY, PX) * 4.0f)) * Radius;
						if (Radius <= 12.0f
							|| (Radius >= 17.0f && Radius <= 27.0f
								&& RayAxisDistance <= 2.2f))
						{
							++CoveredSamples;
						}
					}
				}
				const size_t Offset =
					(static_cast<size_t>(Y) * AtlasWidth + Size + X) * 4;
				Pixels[Offset + 0] = 255;
				Pixels[Offset + 1] = 255;
				Pixels[Offset + 2] = 255;
				Pixels[Offset + 3] = static_cast<uint8>(CoveredSamples * 255
					/ (SamplesPerAxis * SamplesPerAxis));
			}
		}
		for (uint32 Y = 0; Y < Size; ++Y)
		{
			for (uint32 X = 0; X < Size; ++X)
			{
				uint32 CoveredSamples = 0;
				for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
				{
					for (uint32 SampleX = 0; SampleX < SamplesPerAxis; ++SampleX)
					{
						const float PX = static_cast<float>(X)
							+ (static_cast<float>(SampleX) + 0.5f) / SamplesPerAxis;
						const float PY = static_cast<float>(Y)
							+ (static_cast<float>(SampleY) + 0.5f) / SamplesPerAxis;
						const bool bPole = PX >= 19.0f && PX <= 24.0f
							&& PY >= 9.0f && PY <= 56.0f;
						const bool bFlag = InsideTriangle(PX, PY,
							{23.0f, 12.0f}, {56.0f, 23.5f}, {23.0f, 35.0f});
						const bool bFinial =
							InsideCircle(PX, PY, 21.5f, 8.5f, 3.5f);
						const bool bFoot = PX >= 14.0f && PX <= 29.0f
							&& PY >= 54.0f && PY <= 58.0f;
						if (bPole || bFlag || bFinial || bFoot)
							++CoveredSamples;
					}
				}
				const size_t Offset =
					(static_cast<size_t>(Y) * AtlasWidth + Size * 2 + X) * 4;
				Pixels[Offset + 0] = 255;
				Pixels[Offset + 1] = 255;
				Pixels[Offset + 2] = 255;
				Pixels[Offset + 3] = static_cast<uint8>(CoveredSamples * 255
					/ (SamplesPerAxis * SamplesPerAxis));
			}
		}
		return Pixels;
	}
} // namespace Durin
