#include "RoadNet/RoadSurface.h"
#include "Math/Operations.h"

namespace Durin::RoadNet
{
	namespace
	{
		constexpr double PositionTolerance = 1.e-3;
		constexpr double FrameTolerance = 1.e-3;

		auto SurfaceUp(const FRoadSurface& Surface, const FVector3& Position, FVector3& Up) -> bool
		{
			return Math::TryNormalize(Surface.Mode == ERoadSurfaceMode::Sphere
				? Position - Surface.Origin : Surface.Normal, Up, 1.e-18);
		}

		auto FrameAt(const FRoadSurface& Surface, const FVector3& Position,
			const FVector3& Derivative, FSplineMeshFrame& Frame) -> bool
		{
			FVector3 ReferenceUp;
			if (!SurfaceUp(Surface, Position, ReferenceUp)
				|| !Math::TryNormalize(Derivative, Frame.Forward, 1.e-18)
				|| !Math::TryNormalize(Math::Cross(ReferenceUp, Frame.Forward), Frame.Side, 1.e-18)) return false;
			Frame.Up = Math::Cross(Frame.Forward, Frame.Side);
			return std::abs(Math::Dot(Frame.Forward, Frame.Up)) <= 1.e-8
				&& std::abs(Math::Length(Frame.Up) - 1.0) <= 1.e-8;
		}

		auto Project(const FRoadSurface& Surface, FSplineSample& Sample) -> bool
		{
			if (Surface.Mode == ERoadSurfaceMode::Plane)
			{
				const auto N = Math::Normalize(Surface.Normal);
				Sample.Position += N * (Surface.ElevationMeters - Math::Dot(Sample.Position - Surface.Origin, N));
				Sample.FirstDerivative -= N * Math::Dot(Sample.FirstDerivative, N);
			}
			else if (Surface.Mode == ERoadSurfaceMode::Sphere)
			{
				const auto V = Sample.Position - Surface.Origin;
				const double Length = Math::Length(V);
				if (!std::isfinite(Length) || Length <= 1.e-6) return false;
				const auto N = V / Length;
				const double Radius = Surface.RadiusMeters + Surface.ElevationMeters;
				Sample.Position = Surface.Origin + Radius * N;
				Sample.FirstDerivative = (Sample.FirstDerivative - N * Math::Dot(N, Sample.FirstDerivative)) * (Radius / Length);
			}
			return Math::IsFinite(Sample.Position) && Math::IsFinite(Sample.FirstDerivative)
				&& Math::Length(Sample.FirstDerivative) > 1.e-9;
		}

		auto ValidateSurface(const FRoadSurface& Surface, std::string& Error) -> bool
		{
			FVector3 N;
			if (Surface.Mode > ERoadSurfaceMode::Sphere
				|| (Surface.Mode != ERoadSurfaceMode::Unconstrained && (!Math::IsFinite(Surface.Origin)
				|| std::max({std::abs(Surface.Origin.x), std::abs(Surface.Origin.y), std::abs(Surface.Origin.z)}) > RoadCoordinateLimit
				|| !std::isfinite(Surface.ElevationMeters) || std::abs(Surface.ElevationMeters) > RoadCoordinateLimit))
				|| (Surface.Mode != ERoadSurfaceMode::Sphere && !Math::TryNormalize(Surface.Normal, N, 1.e-18))
				|| (Surface.Mode == ERoadSurfaceMode::Sphere && (!std::isfinite(Surface.RadiusMeters) || Surface.RadiusMeters < 1.0
					|| Surface.RadiusMeters > RoadCoordinateLimit || Surface.RadiusMeters + Surface.ElevationMeters < 1.0
					|| Surface.RadiusMeters + Surface.ElevationMeters > RoadCoordinateLimit)))
			{
				Error = "Road surface has unsupported mode, coordinates, normal or radius.";
				return false;
			}
			return true;
		}
	}

	auto FRoadSurface::HasSameGenerationConfig(const FRoadSurface& Other) const -> bool
	{
		if (Mode != Other.Mode) return false;
		switch (Mode)
		{
		case ERoadSurfaceMode::Unconstrained:
			return Normal == Other.Normal;
		case ERoadSurfaceMode::Plane:
			return Origin == Other.Origin && Normal == Other.Normal && ElevationMeters == Other.ElevationMeters;
		case ERoadSurfaceMode::Sphere:
			return Origin == Other.Origin && RadiusMeters == Other.RadiusMeters && ElevationMeters == Other.ElevationMeters;
		default:
			return false;
		}
	}

	auto ValidateRoadPlacement(const FTransform& Placement, std::string& OutError) -> bool
	{
		if (Placement.Scale3D != FVector3(1.0) || !Math::IsFinite(Placement.Translation)
			|| !std::isfinite(Placement.Rotation.w) || !std::isfinite(Placement.Rotation.x)
			|| !std::isfinite(Placement.Rotation.y) || !std::isfinite(Placement.Rotation.z)
			|| std::abs(Placement.Rotation.w * Placement.Rotation.w + Placement.Rotation.x * Placement.Rotation.x
				+ Placement.Rotation.y * Placement.Rotation.y + Placement.Rotation.z * Placement.Rotation.z - 1.0) > 1.e-8)
		{
			OutError = "Road placement requires finite translation, a unit rotation and identity scale.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto FRoadAlignment::Build(const FRoad& Road, const FRoadSurface& InSurface,
		uint64 InAssetRevision, std::shared_ptr<const FRoadAlignment>& OutSnapshot, std::string& OutError) -> bool
	{
		OutError.clear();
		if (!ValidateSurface(InSurface, OutError)) return false;
		// Reuse authored validation without requiring callers to construct the enclosing network.
		FDefinition Definition;
		Definition.Roads.push_back(Road);
		if (Road.ReferenceLine.GetNumPoints() < 2)
		{
			OutError = "Road alignment requires two or more source points.";
			return false;
		}
		Definition.Nodes.push_back({.Id = Road.StartNodeId, .Position = Road.ReferenceLine.GetPoints().front().Position});
		if (Road.EndNodeId != Road.StartNodeId)
			Definition.Nodes.push_back({.Id = Road.EndNodeId, .Position = Road.ReferenceLine.GetPoints().back().Position});
		if (!ValidateDefinition(Definition, OutError)) return false;
		auto Result = std::make_shared<FRoadAlignment>();
		Result->Surface = InSurface;
		Result->AssetRevision = InAssetRevision;
		Result->Sections = Road.LaneSections;
		const auto Source = Road.ReferenceLine.BuildEvaluationData();
		auto Fail = [&](std::string_view Reason) {
			OutError = std::format("Road '{}' surface fit: {}", Road.Id.ToString(), Reason);
			return false;
		};
		for (uint32 Segment = 0; Segment < Source->GetNumSegments(); ++Segment)
		{
			std::function<bool(double, double, uint32, uint32)> Fit;
			Fit = [&](double A, double B, uint32 Depth, uint32 Key) -> bool {
				auto Start = Source->Evaluate({Segment, A});
				auto End = Source->Evaluate({Segment, B});
				if (!Project(InSurface, Start) || !Project(InSurface, End)) return Fail("singular projection or derivative.");
				FSplineMeshFrame StartFrame, EndFrame;
				if (!FrameAt(InSurface, Start.Position, Start.FirstDerivative, StartFrame)
					|| !FrameAt(InSurface, End.Position, End.FirstDerivative, EndFrame)) return Fail("degenerate reference up.");
				FSplineMeshParams Params;
				Params.StartPosition = Start.Position;
				Params.EndPosition = End.Position;
				Params.StartTangent = Start.FirstDerivative * (B - A);
				Params.EndTangent = End.FirstDerivative * (B - A);
				Params.SplineUpDirection = StartFrame.Up;
				const auto BaseEnd = FSplineMeshDeformer::Evaluate(Params, 1.0);
				Params.EndRollRadians = std::atan2(Math::Dot(EndFrame.Forward, Math::Cross(BaseEnd.Frame.Up, EndFrame.Up)),
					Math::Dot(BaseEnd.Frame.Up, EndFrame.Up));
				bool Split = Math::Dot(StartFrame.Forward, EndFrame.Forward) <= 0.0 || Math::Dot(StartFrame.Up, EndFrame.Up) <= 0.0;
				FVector3 Previous = Start.Position;
				double PolylineLength = 0.0;
				std::array<double, 5> Distances{};
				double SpeedIntegral = Math::Length(Start.FirstDerivative) + Math::Length(End.FirstDerivative);
				for (int Index = 1; Index <= 4; ++Index)
				{
					const double T = Index * 0.25;
					auto Exact = Source->Evaluate({Segment, A + (B - A) * T});
					if (!Project(InSurface, Exact)) return Fail("singular interior projection or derivative.");
					FSplineMeshFrame ExactFrame;
					if (!FrameAt(InSurface, Exact.Position, Exact.FirstDerivative, ExactFrame)) return Fail("singular interior reference up.");
					const auto Approx = FSplineMeshDeformer::Evaluate(Params, T);
					Split |= Math::Length(Approx.Position - Exact.Position) > PositionTolerance
						|| Math::Dot(Approx.Frame.Up, ExactFrame.Up) < std::cos(FrameTolerance)
						|| Math::Dot(Approx.Frame.Forward, ExactFrame.Forward) < std::cos(FrameTolerance);
					PolylineLength += Math::Length(Exact.Position - Previous);
					Distances[Index] = PolylineLength;
					Previous = Exact.Position;
					if (Index < 4) SpeedIntegral += (Index == 2 ? 2.0 : 4.0) * Math::Length(Exact.FirstDerivative);
				}
				SpeedIntegral *= (B - A) / 12.0;
				// Distance-table interpolation must also resolve nonuniform speed on straight cubics.
				Split |= std::abs(Distances[1] - Distances[2] * 0.5) > 1.e-5
					|| std::abs(Distances[3] - (Distances[2] + Distances[4]) * 0.5) > 1.e-5;
				// Reserve headroom for the spline foundation's two-chord distance leaves.
				Split |= std::abs(SpeedIntegral - PolylineLength) > 1.e-5 * (B - A) / Source->GetNumSegments() + 1.e-7 * SpeedIntegral;
				if (Split)
				{
					if (Depth == 16) return Fail("maximum subdivision depth exceeded.");
					const double Mid = (A + B) * 0.5;
					return Fit(A, Mid, Depth + 1, Key * 2) && Fit(Mid, B, Depth + 1, Key * 2 + 1);
				}
				if (Result->Intervals.size() == 65536) return Fail("maximum interval count exceeded.");
				Result->Intervals.push_back({Road.ReferenceLine.GetPoints()[Segment].Id, Key, Params});
				return true;
			};
			if (!Fit(0.0, 1.0, 0, 1)) return false;
		}
		FSplineCurve FinalCurve;
		std::vector<FSplinePoint> Points;
		for (const auto& Interval : Result->Intervals)
		{
			FSplinePoint Point(Interval.Params.StartPosition);
			Point.TangentMode = ESplineTangentMode::ManualBroken;
			Point.LeaveTangent = Interval.Params.StartTangent;
			Point.ArriveTangent = Points.empty() ? Point.LeaveTangent : Result->Intervals[Points.size() - 1].Params.EndTangent;
			Points.push_back(Point);
		}
		FSplinePoint Last(Result->Intervals.back().Params.EndPosition);
		Last.TangentMode = ESplineTangentMode::ManualBroken;
		Last.ArriveTangent = Result->Intervals.back().Params.EndTangent;
		Last.LeaveTangent = Last.ArriveTangent;
		Points.push_back(Last);
		FinalCurve.SetPoints(std::move(Points));
		Result->Evaluation = FinalCurve.BuildEvaluationData();
		const double Length = Result->GetLengthMeters();
		if (!std::isfinite(Length) || Length <= 1.e-6) return Fail("degenerate final length.");
		if (std::abs(Result->Sections.back().EndDistanceMeters - Length) > std::max(1.e-4, 1.e-6 * Length))
			return Fail(std::format("explicit lane stations end at {} but final evaluated length is {}; update the complete candidate.", Result->Sections.back().EndDistanceMeters, Length));
		OutSnapshot = std::move(Result);
		return true;
	}

	auto FRoadAlignment::Sample(double DistanceMeters, FRoadSample& OutSample, std::string& OutError) const -> bool
	{
		if (!std::isfinite(DistanceMeters) || DistanceMeters < 0.0 || DistanceMeters > GetLengthMeters())
		{
			OutError = "Road station is outside the final evaluated interval.";
			return false;
		}
		const auto Value = Evaluation->EvaluateAtLocalDistance(DistanceMeters);
		FRoadSample Result;
		Result.DistanceMeters = DistanceMeters;
		Result.Position = Value.Position;
		if (!FrameAt(Surface, Value.Position, Value.FirstDerivative, Result.Frame))
		{
			OutError = "Road station has a degenerate frame.";
			return false;
		}
		const FLaneSection* Section = &Sections.back();
		for (const auto& Candidate : Sections)
			if (DistanceMeters < Candidate.EndDistanceMeters) { Section = &Candidate; break; }
		std::vector<const FLane*> Lanes;
		for (const auto& Lane : Section->Lanes) Lanes.push_back(&Lane);
		std::ranges::sort(Lanes, [](const auto* A, const auto* B) {
			return std::abs(static_cast<int64>(A->Index)) < std::abs(static_cast<int64>(B->Index));
		});
		double Left = 0.0, Right = 0.0;
		for (const auto* Lane : Lanes)
		{
			if (Lane->Index > 0)
			{
				Result.Lanes.push_back({Lane->Id, Right, Right + Lane->WidthMeters});
				Right += Lane->WidthMeters;
			}
			else
			{
				Result.Lanes.push_back({Lane->Id, Left - Lane->WidthMeters, Left});
				Left -= Lane->WidthMeters;
			}
		}
		OutSample = std::move(Result);
		OutError.clear();
		return true;
	}

	auto FRoadAlignment::SampleWorld(double DistanceMeters, const FTransform& Placement,
		FRoadSample& OutSample, std::string& OutError) const -> bool
	{
		if (!ValidateRoadPlacement(Placement, OutError)) return false;
		FRoadSample Result;
		if (!Sample(DistanceMeters, Result, OutError)) return false;
		Result.Position = Placement.Rotation * Result.Position + Placement.Translation;
		Result.Frame.Forward = Placement.Rotation * Result.Frame.Forward;
		Result.Frame.Side = Placement.Rotation * Result.Frame.Side;
		Result.Frame.Up = Placement.Rotation * Result.Frame.Up;
		OutSample = std::move(Result);
		return true;
	}
}
