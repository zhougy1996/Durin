#pragma once

#include "CoreAPI.h"

#include "Math/Vector.h"
#include "Misc/CoreMiscDefines.h"

namespace Durin
{
	struct FColor;

	enum class EGammaSpace : uint8
	{
		/** No gamma correction is applied to this space, the incoming colors are assumed to already be in linear space. */
		Linear,
		/** A simplified sRGB gamma correction is applied, pow(1/2.2). */
		Pow22,
		/** Use the standard sRGB conversion. */
		sRGB,

		Invalid
	};

	#pragma warning(push)
	#pragma warning(disable : 26495) // Disable warning C26495: Variable is uninitialized.

	/**
	 * A linear, 32-bit/component floating point RGBA color.
	 */
	struct FLinearColor
	{
		union
		{
			struct
			{
				float R, G, B, A;
			};

			float RGBA[4];
		};
		static float Pow22OneOver255Table[256];

		static CORE_API float sRGBToLinearTable[256];

		FORCEINLINE FLinearColor() {}

		FORCEINLINE explicit FLinearColor(EForceInit)
			: R(0)
			, G(0)
			, B(0)
			, A(0)
		{
		}

		constexpr FORCEINLINE FLinearColor(float InR, float InG, float InB, float InA = 1.0f)
			: R(InR)
			, G(InG)
			, B(InB)
			, A(InA)
		{
		}

		constexpr FORCEINLINE FLinearColor(const FColor& Color);

		CORE_API FLinearColor(const FVector3f& Vector);
		CORE_API explicit FLinearColor(const FVector3d& Vector); // Warning: keep this explicit, or FVector4f will be implicitly created from FVector3d via FLinearColor

		CORE_API FLinearColor(const FVector4f& Vector);
		CORE_API explicit FLinearColor(const FVector4d& Vector); // Warning: keep this explicit, or FVector4f will be implicitly created from FVector4d via FLinearColor

		FORCEINLINE static FLinearColor FromSRGBColor(const FColor& Color)
		{
			return FLinearColor(Color);
		}

		CORE_API static FLinearColor FromPow22Color(const FColor& Color);

		FORCEINLINE float& Component(int32 Index)
		{
			return RGBA[Index];
		}

		FORCEINLINE const float& Component(int32 Index) const
		{
			return RGBA[Index];
		}

		FORCEINLINE FLinearColor operator+(const FLinearColor& ColorB) const
		{
			return FLinearColor(
				this->R + ColorB.R,
				this->G + ColorB.G,
				this->B + ColorB.B,
				this->A + ColorB.A);
		}
		FORCEINLINE FLinearColor& operator+=(const FLinearColor& ColorB)
		{
			R += ColorB.R;
			G += ColorB.G;
			B += ColorB.B;
			A += ColorB.A;
			return *this;
		}

		FORCEINLINE FLinearColor operator-(const FLinearColor& ColorB) const
		{
			return FLinearColor(
				this->R - ColorB.R,
				this->G - ColorB.G,
				this->B - ColorB.B,
				this->A - ColorB.A);
		}
		FORCEINLINE FLinearColor& operator-=(const FLinearColor& ColorB)
		{
			R -= ColorB.R;
			G -= ColorB.G;
			B -= ColorB.B;
			A -= ColorB.A;
			return *this;
		}

		FORCEINLINE FLinearColor operator*(const FLinearColor& ColorB) const
		{
			return FLinearColor(
				this->R * ColorB.R,
				this->G * ColorB.G,
				this->B * ColorB.B,
				this->A * ColorB.A);
		}
		FORCEINLINE FLinearColor& operator*=(const FLinearColor& ColorB)
		{
			R *= ColorB.R;
			G *= ColorB.G;
			B *= ColorB.B;
			A *= ColorB.A;
			return *this;
		}

		FORCEINLINE FLinearColor operator*(float Scalar) const
		{
			return FLinearColor(
				this->R * Scalar,
				this->G * Scalar,
				this->B * Scalar,
				this->A * Scalar);
		}

		FORCEINLINE FLinearColor& operator*=(float Scalar)
		{
			R *= Scalar;
			G *= Scalar;
			B *= Scalar;
			A *= Scalar;
			return *this;
		}

		FORCEINLINE FLinearColor operator/(const FLinearColor& ColorB) const
		{
			return FLinearColor(
				this->R / ColorB.R,
				this->G / ColorB.G,
				this->B / ColorB.B,
				this->A / ColorB.A);
		}
		FORCEINLINE FLinearColor& operator/=(const FLinearColor& ColorB)
		{
			R /= ColorB.R;
			G /= ColorB.G;
			B /= ColorB.B;
			A /= ColorB.A;
			return *this;
		}

		FORCEINLINE FLinearColor operator/(float Scalar) const
		{
			const float InvScalar = 1.0f / Scalar;
			return FLinearColor(
				this->R * InvScalar,
				this->G * InvScalar,
				this->B * InvScalar,
				this->A * InvScalar);
		}
		FORCEINLINE FLinearColor& operator/=(float Scalar)
		{
			const float InvScalar = 1.0f / Scalar;
			R *= InvScalar;
			G *= InvScalar;
			B *= InvScalar;
			A *= InvScalar;
			return *this;
		}

		// clamped in 0..1 range
		FORCEINLINE FLinearColor GetClamped(float InMin = 0.0f, float InMax = 1.0f) const
		{
			FLinearColor Ret;

			Ret.R = FMath::Clamp(R, InMin, InMax);
			Ret.G = FMath::Clamp(G, InMin, InMax);
			Ret.B = FMath::Clamp(B, InMin, InMax);
			Ret.A = FMath::Clamp(A, InMin, InMax);

			return Ret;
		}

		/** Comparison operators */
		FORCEINLINE bool operator==(const FLinearColor& ColorB) const
		{
			return this->R == ColorB.R && this->G == ColorB.G && this->B == ColorB.B && this->A == ColorB.A;
		}
		FORCEINLINE bool operator!=(const FLinearColor& Other) const
		{
			return this->R != Other.R || this->G != Other.G || this->B != Other.B || this->A != Other.A;
		}

		// Error-tolerant comparison.
		FORCEINLINE bool Equals(const FLinearColor& ColorB, float Tolerance = kKindaSmallNumber) const
		{
			return FMath::Abs(this->R - ColorB.R) < Tolerance && FMath::Abs(this->G - ColorB.G) < Tolerance && FMath::Abs(this->B - ColorB.B) < Tolerance && FMath::Abs(this->A - ColorB.A) < Tolerance;
		}

		FLinearColor CopyWithNewOpacity(float NewOpacicty) const
		{
			FLinearColor NewCopy = *this;
			NewCopy.A = NewOpacicty;
			return NewCopy;
		}

		/**
		 * Euclidean distance between two points.
		 */
		static inline float Dist(const FLinearColor& V1, const FLinearColor& V2)
		{
			return FMath::Sqrt(FMath::Square(V2.R - V1.R) + FMath::Square(V2.G - V1.G) + FMath::Square(V2.B - V1.B) + FMath::Square(V2.A - V1.A));
		}

		/** Quantizes the linear color and returns the result as a FColor with optional sRGB conversion.
		 * Clamps in [0,1] range before conversion.
		 * ToFColor(false) is QuantizeRound
		 */
		FORCEINLINE FColor QuantizeRound() const;

		CORE_API FColor ToFColorSRGB() const;

		FORCEINLINE FColor ToFColor(const bool bSRGB) const;

		/** Computes the perceptually weighted luminance value of a color. */
		inline float GetLuminance() const
		{
			return R * 0.3f + G * 0.59f + B * 0.11f;
		}

		/**
		 * Returns the maximum value in this color structure
		 *
		 * @return The maximum color channel value
		 */
		FORCEINLINE float GetMax() const
		{
			return FMath::Max(FMath::Max(FMath::Max(R, G), B), A);
		}

		/**
		 * Returns the minimum value in this color structure
		 *
		 * @return The minimum color channel value
		 */
		FORCEINLINE float GetMin() const
		{
			return FMath::Min(FMath::Min(FMath::Min(R, G), B), A);
		}

		/**
		 * Helper for pixel format conversions. Clamps to [0,1], mapping NaNs to 0,
		 * for consistency with GPU conversions.
		 *
		 * @param InValue The input value.
		 * @return InValue clamped to [0,1]. NaNs map to 0.
		 */
		static FORCEINLINE float Clamp01NansTo0(float InValue)
		{
			// Write this explicitly instead of using FMath::Clamp because we're particular
			// about what happens with NaNs here.
			const float ClampedLo = (InValue > 0.0f) ? InValue : 0.0f; // Also turns NaNs into 0.
			return (ClampedLo < 1.0f) ? ClampedLo : 1.0f;
		}

		// Common colors.
		static CORE_API const FLinearColor White;
		static CORE_API const FLinearColor Gray;
		static CORE_API const FLinearColor Black;
		static CORE_API const FLinearColor Transparent;
		static CORE_API const FLinearColor Red;
		static CORE_API const FLinearColor Green;
		static CORE_API const FLinearColor Blue;
		static CORE_API const FLinearColor Yellow;
	};

	// Stores an 8-bit-per-channel color in the engine's packed BGRA memory layout.
	struct FColor
	{
		union
		{
			struct
			{
				uint8 B, G, R, A;
			};

			uint32 Bits;
		};

		uint32& DWColor(void) { return Bits; }
		const uint32& DWColor(void) const { return Bits; }

		FORCEINLINE FColor() {}

		FORCEINLINE explicit FColor(EForceInit)
		{
			R = G = B = A = 0;
		}

		constexpr FORCEINLINE FColor(uint8 InR, uint8 InG, uint8 InB, uint8 InA = 255)
			: B(InB)
			, G(InG)
			, R(InR)
			, A(InA)
		{
		}

		FORCEINLINE explicit FColor(uint32 InColor)
		{
			DWColor() = InColor;
		}

		// Operators.
		FORCEINLINE bool operator==(const FColor& C) const
		{
			return DWColor() == C.DWColor();
		}

		FORCEINLINE bool operator!=(const FColor& C) const
		{
			return DWColor() != C.DWColor();
		}

		FORCEINLINE void operator+=(const FColor& C)
		{
			R = (uint8)FMath::Min((int32)R + (int32)C.R, 255);
			G = (uint8)FMath::Min((int32)G + (int32)C.G, 255);
			B = (uint8)FMath::Min((int32)B + (int32)C.B, 255);
			A = (uint8)FMath::Min((int32)A + (int32)C.A, 255);
		}

		/**
		 *	@return a new FColor based of this color with the new alpha value.
		 *	Usage: const FColor& MyColor = FColorList::Green.WithAlpha(128);
		 */
		FColor WithAlpha(uint8 Alpha) const
		{
			return FColor(R, G, B, Alpha);
		}

		/**
		 * Reinterprets the color as a linear color.
		 * This is the correct dequantizer for QuantizeRound.
		 * This matches the GPU spec conversion for U8<->float
		 * @return The linear color representation.
		 */
		FORCEINLINE FLinearColor ReinterpretAsLinear() const
		{
			constexpr float inv255 = 1.f / 255.f;
			return FLinearColor(R * inv255, G * inv255, B * inv255, A * inv255);
		}

		/** Some pre-inited colors, useful for debug code */
		static CORE_API const FColor White;
		static CORE_API const FColor Black;
		static CORE_API const FColor Transparent;
		static CORE_API const FColor Red;
		static CORE_API const FColor Green;
		static CORE_API const FColor Blue;
		static CORE_API const FColor Yellow;
		static CORE_API const FColor Cyan;
		static CORE_API const FColor Magenta;
		static CORE_API const FColor Orange;
		static CORE_API const FColor Purple;
		static CORE_API const FColor Turquoise;
		static CORE_API const FColor Silver;
		static CORE_API const FColor Emerald;
	};

	#pragma warning(pop) // restore warning C26495

	constexpr FORCEINLINE FLinearColor::FLinearColor(const FColor& Color)
		: R(sRGBToLinearTable[Color.R])
		, G(sRGBToLinearTable[Color.G])
		, B(sRGBToLinearTable[Color.B])
		, A(static_cast<float>(Color.A) * (1.0f / 255.0f))
	{
	}

	FORCEINLINE FColor FLinearColor::QuantizeRound() const
	{
		// Avoid FMath::RoundToInt because it calls floor()
		return FColor(
			(uint8)(0.5f + Clamp01NansTo0(R) * 255.f),
			(uint8)(0.5f + Clamp01NansTo0(G) * 255.f),
			(uint8)(0.5f + Clamp01NansTo0(B) * 255.f),
			(uint8)(0.5f + Clamp01NansTo0(A) * 255.f));
	}

	FORCEINLINE FColor FLinearColor::ToFColor(const bool bSRGB) const
	{
		if (bSRGB)
		{
			return ToFColorSRGB();
		}
		else
		{
			return QuantizeRound();
		}
	}
}
