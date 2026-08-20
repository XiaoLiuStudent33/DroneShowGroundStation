#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dronegcs::wpl110
{
	using Byte = std::uint8_t;

	inline constexpr char Header[] = "QGC WPL 110";
	inline constexpr std::size_t HeaderLength = sizeof(Header) - 1;
	inline constexpr std::int64_t DecimalScale = 10000000;
	inline constexpr int FractionDigits = 7;
	inline constexpr std::size_t FieldCount = 12;
	inline constexpr std::size_t MaxRowBytes = 80;

	enum class ErrorCode : std::uint8_t
	{
		None,
		UnexpectedBom,
		HeaderMismatch,
		HeaderLineEnding,
		RowLineEnding,
		UnexpectedFinalNewline,
		RowTooLong,
		WrongFieldCount,
		EmptyField,
		InvalidInteger,
		IntegerOverflow,
		NonCanonicalInteger,
		InvalidDecimal,
		DecimalOverflow,
		NonCanonicalDecimal,
		FixedFieldMismatch,
		SequenceMismatch,
		RgbOutOfRange,
		LatitudeOutOfRange,
		LongitudeOutOfRange,
		RowCountMismatch,
		ConsumerRejected
	};

	struct Error
	{
		ErrorCode Code = ErrorCode::None;
		std::int32_t RowIndex = -1;
		std::int32_t FieldIndex = -1;
	};

	struct Row
	{
		std::int32_t Sequence = 0;
		std::uint8_t Red = 0;
		std::uint8_t Green = 0;
		std::uint8_t Blue = 0;
		std::int32_t LatitudeE7 = 0;
		std::int32_t LongitudeE7 = 0;
		std::int64_t AltitudeE7 = 0;
	};

	struct FormattedRow
	{
		std::array<Byte, MaxRowBytes> Bytes{};
		std::size_t Size = 0;
	};

	struct RgbSaturationDiagnostics
	{
		std::uint8_t ClampedChannelCount = 0;
	};

	inline std::uint64_t UnsignedMagnitude(const std::int64_t Value)
	{
		return Value >= 0
			? static_cast<std::uint64_t>(Value)
			: static_cast<std::uint64_t>(-(Value + 1)) + 1;
	}

	inline bool AppendByte(FormattedRow& Out, const Byte Value)
	{
		if (Out.Size >= Out.Bytes.size())
		{
			return false;
		}
		Out.Bytes[Out.Size++] = Value;
		return true;
	}

	inline bool AppendUnsigned(FormattedRow& Out, std::uint64_t Value)
	{
		Byte Digits[20];
		std::size_t Count = 0;
		do
		{
			Digits[Count++] = static_cast<Byte>('0' + Value % 10);
			Value /= 10;
		}
		while (Value != 0);
		while (Count > 0)
		{
			if (!AppendByte(Out, Digits[--Count]))
			{
				return false;
			}
		}
		return true;
	}

	inline bool AppendScaled(FormattedRow& Out, const std::int64_t Value)
	{
		const std::uint64_t Magnitude = UnsignedMagnitude(Value);
		if (Value < 0 && !AppendByte(Out, '-'))
		{
			return false;
		}
		if (!AppendUnsigned(Out, Magnitude / static_cast<std::uint64_t>(DecimalScale))
			|| !AppendByte(Out, '.'))
		{
			return false;
		}
		std::uint64_t Fraction = Magnitude % static_cast<std::uint64_t>(DecimalScale);
		if (Fraction == 0)
		{
			return AppendByte(Out, '0');
		}
		Byte Digits[FractionDigits];
		for (int Index = FractionDigits - 1; Index >= 0; --Index)
		{
			Digits[Index] = static_cast<Byte>('0' + Fraction % 10);
			Fraction /= 10;
		}
		int Last = FractionDigits - 1;
		while (Last > 0 && Digits[Last] == '0')
		{
			--Last;
		}
		for (int Index = 0; Index <= Last; ++Index)
		{
			if (!AppendByte(Out, Digits[Index]))
			{
				return false;
			}
		}
		return true;
	}

	inline bool FormatScaledDecimal(
		const std::int64_t Value,
		std::array<Byte, 32>& OutBytes,
		std::size_t& OutSize)
	{
		FormattedRow Buffer;
		if (!AppendScaled(Buffer, Value) || Buffer.Size > OutBytes.size())
		{
			OutSize = 0;
			return false;
		}
		std::copy_n(Buffer.Bytes.begin(), Buffer.Size, OutBytes.begin());
		OutSize = Buffer.Size;
		return true;
	}

	inline bool FormatRow(const Row& Value, FormattedRow& Out)
	{
		Out = FormattedRow{};
		if (Value.Sequence < 0)
		{
			return false;
		}
		auto Tab = [&Out]() { return AppendByte(Out, '\t'); };
		return AppendUnsigned(Out, static_cast<std::uint64_t>(Value.Sequence)) && Tab()
			&& AppendByte(Out, '0') && Tab()
			&& AppendByte(Out, '3') && Tab()
			&& AppendByte(Out, '1') && AppendByte(Out, '6') && Tab()
			&& AppendUnsigned(Out, Value.Red) && Tab()
			&& AppendUnsigned(Out, Value.Green) && Tab()
			&& AppendUnsigned(Out, Value.Blue) && Tab()
			&& AppendByte(Out, '0') && Tab()
			&& AppendScaled(Out, Value.LatitudeE7) && Tab()
			&& AppendScaled(Out, Value.LongitudeE7) && Tab()
			&& AppendScaled(Out, Value.AltitudeE7) && Tab()
			&& AppendByte(Out, '1');
	}

	inline bool QuantizeGeodeticE7(
		const double Degrees,
		const double Minimum,
		const double Maximum,
		std::int32_t& OutE7)
	{
		OutE7 = 0;
		if (!std::isfinite(Degrees) || Degrees < Minimum || Degrees > Maximum)
		{
			return false;
		}
		const double Scaled = Degrees * static_cast<double>(DecimalScale);
		if (!std::isfinite(Scaled)
			|| Scaled < static_cast<double>(std::numeric_limits<std::int32_t>::min())
			|| Scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
		{
			return false;
		}
		OutE7 = static_cast<std::int32_t>(Scaled);
		return true;
	}

	inline bool QuantizeAltitudeE7(const double Meters, std::int64_t& OutE7)
	{
		OutE7 = 0;
		if (!std::isfinite(Meters))
		{
			return false;
		}
		const double Scaled = Meters * static_cast<double>(DecimalScale);
		constexpr double Int64Limit = 9223372036854775808.0;
		if (!std::isfinite(Scaled) || Scaled < -Int64Limit || Scaled >= Int64Limit)
		{
			return false;
		}
		OutE7 = static_cast<std::int64_t>(Scaled);
		return true;
	}

	inline bool QuantizeRgb(
		const float Red,
		const float Green,
		const float Blue,
		const float Alpha,
		std::uint8_t& OutRed,
		std::uint8_t& OutGreen,
		std::uint8_t& OutBlue,
		RgbSaturationDiagnostics* const OutDiagnostics = nullptr)
	{
		OutRed = OutGreen = OutBlue = 0;
		if (OutDiagnostics)
		{
			*OutDiagnostics = RgbSaturationDiagnostics{};
		}
		if (!std::isfinite(Red) || !std::isfinite(Green)
			|| !std::isfinite(Blue) || !std::isfinite(Alpha))
		{
			return false;
		}

		auto SaturateAndQuantize = [OutDiagnostics](
			const float Value,
			std::uint8_t& OutValue)
		{
			const float Saturated = std::clamp(Value, 0.0f, 1.0f);
			if (OutDiagnostics && Saturated != Value)
			{
				++OutDiagnostics->ClampedChannelCount;
			}
			OutValue = static_cast<std::uint8_t>(Saturated * 255.0f);
		};
		SaturateAndQuantize(Red, OutRed);
		SaturateAndQuantize(Green, OutGreen);
		SaturateAndQuantize(Blue, OutBlue);
		return true;
	}

	using RowConsumer = bool (*)(const Row&, void*);

	class StreamingParser final
	{
	public:
		StreamingParser(
			const std::int32_t ExpectedRows,
			const RowConsumer Consumer = nullptr,
			void* const ConsumerContext = nullptr)
			: ExpectedRowCount(ExpectedRows)
			, OnRow(Consumer)
			, OnRowContext(ConsumerContext)
		{
		}

		bool Consume(const Byte* Data, const std::size_t Size)
		{
			if (StateValue == State::Failed || StateValue == State::Finished
				|| (Size > 0 && Data == nullptr))
			{
				return false;
			}
			for (std::size_t Index = 0; Index < Size; ++Index)
			{
				const Byte Value = Data[Index];
				switch (StateValue)
				{
				case State::Header:
					if (HeaderIndex == 0 && Value == 0xef)
					{
						return Fail(ErrorCode::UnexpectedBom);
					}
					if (HeaderIndex >= HeaderLength || Value != static_cast<Byte>(Header[HeaderIndex]))
					{
						return Fail(ErrorCode::HeaderMismatch);
					}
					if (++HeaderIndex == HeaderLength)
					{
						StateValue = State::HeaderCr;
					}
					break;
				case State::HeaderCr:
					if (Value != '\r') return Fail(ErrorCode::HeaderLineEnding);
					StateValue = State::HeaderLf;
					break;
				case State::HeaderLf:
					if (Value != '\n') return Fail(ErrorCode::HeaderLineEnding);
					StateValue = State::Row;
					break;
				case State::Row:
					if (Value == '\r')
					{
						StateValue = State::RowLf;
					}
					else if (Value == '\n')
					{
						return Fail(ErrorCode::RowLineEnding);
					}
					else
					{
						EndedWithSeparator = false;
						if (RowSize >= RowBytes.size()) return Fail(ErrorCode::RowTooLong);
						RowBytes[RowSize++] = Value;
					}
					break;
				case State::RowLf:
					if (Value != '\n') return Fail(ErrorCode::RowLineEnding);
					if (!ParseAndDispatchRow()) return false;
					RowSize = 0;
					EndedWithSeparator = true;
					StateValue = State::Row;
					break;
				default:
					return false;
				}
			}
			return true;
		}

		bool Finish()
		{
			if (StateValue == State::Failed || StateValue == State::Finished)
			{
				return false;
			}
			if (StateValue == State::Header || StateValue == State::HeaderCr
				|| StateValue == State::HeaderLf)
			{
				return Fail(ErrorCode::HeaderMismatch);
			}
			if (StateValue == State::RowLf)
			{
				return Fail(ErrorCode::RowLineEnding);
			}
			if (EndedWithSeparator)
			{
				return Fail(ErrorCode::UnexpectedFinalNewline);
			}
			if (RowSize == 0 || !ParseAndDispatchRow())
			{
				if (StateValue != State::Failed) return Fail(ErrorCode::RowCountMismatch);
				return false;
			}
			RowSize = 0;
			if (ExpectedRowCount < 0 || ParsedRowCount != ExpectedRowCount)
			{
				return Fail(ErrorCode::RowCountMismatch);
			}
			StateValue = State::Finished;
			return true;
		}

		const Error& GetError() const { return ErrorValue; }
		std::int32_t GetParsedRowCount() const { return ParsedRowCount; }

	private:
		enum class State : std::uint8_t { Header, HeaderCr, HeaderLf, Row, RowLf, Failed, Finished };

		bool Fail(const ErrorCode Code, const std::int32_t Field = -1)
		{
			if (StateValue != State::Failed)
			{
				ErrorValue.Code = Code;
				ErrorValue.RowIndex = ParsedRowCount;
				ErrorValue.FieldIndex = Field;
				StateValue = State::Failed;
			}
			return false;
		}

		static bool ParseUnsigned(
			const Byte* Begin,
			const std::size_t Length,
			const std::uint64_t Maximum,
			std::uint64_t& Out,
			ErrorCode& OutError)
		{
			Out = 0;
			if (Length == 0)
			{
				OutError = ErrorCode::EmptyField;
				return false;
			}
			if (Length > 1 && Begin[0] == '0')
			{
				OutError = ErrorCode::NonCanonicalInteger;
				return false;
			}
			for (std::size_t Index = 0; Index < Length; ++Index)
			{
				if (Begin[Index] < '0' || Begin[Index] > '9')
				{
					OutError = ErrorCode::InvalidInteger;
					return false;
				}
				const std::uint64_t Digit = Begin[Index] - '0';
				if (Out > (Maximum - Digit) / 10)
				{
					OutError = ErrorCode::IntegerOverflow;
					return false;
				}
				Out = Out * 10 + Digit;
			}
			return true;
		}

		static bool ParseScaled(
			const Byte* Begin,
			const std::size_t Length,
			std::int64_t& Out,
			ErrorCode& OutError)
		{
			Out = 0;
			if (Length == 0)
			{
				OutError = ErrorCode::EmptyField;
				return false;
			}
			std::size_t Offset = 0;
			const bool Negative = Begin[0] == '-';
			if (Begin[0] == '+')
			{
				OutError = ErrorCode::InvalidDecimal;
				return false;
			}
			if (Negative && ++Offset == Length)
			{
				OutError = ErrorCode::InvalidDecimal;
				return false;
			}
			std::size_t Dot = Offset;
			while (Dot < Length && Begin[Dot] != '.') ++Dot;
			if (Dot == Offset || Dot == Length || Dot + 1 == Length)
			{
				OutError = ErrorCode::InvalidDecimal;
				return false;
			}
			for (std::size_t Index = Dot + 1; Index < Length; ++Index)
			{
				if (Begin[Index] == '.')
				{
					OutError = ErrorCode::InvalidDecimal;
					return false;
				}
			}
			const std::size_t IntegerLength = Dot - Offset;
			const std::size_t FractionLength = Length - Dot - 1;
			if (IntegerLength > 1 && Begin[Offset] == '0')
			{
				OutError = ErrorCode::NonCanonicalDecimal;
				return false;
			}
			if (FractionLength > FractionDigits
				|| (FractionLength > 1 && Begin[Length - 1] == '0'))
			{
				OutError = ErrorCode::NonCanonicalDecimal;
				return false;
			}
			const std::uint64_t Limit = Negative
				? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1
				: static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
			std::uint64_t Integer = 0;
			if (!ParseUnsigned(Begin + Offset, IntegerLength, Limit / DecimalScale, Integer, OutError))
			{
				if (OutError == ErrorCode::IntegerOverflow) OutError = ErrorCode::DecimalOverflow;
				return false;
			}
			std::uint64_t Fraction = 0;
			for (std::size_t Index = Dot + 1; Index < Length; ++Index)
			{
				if (Begin[Index] < '0' || Begin[Index] > '9')
				{
					OutError = ErrorCode::InvalidDecimal;
					return false;
				}
				Fraction = Fraction * 10 + (Begin[Index] - '0');
			}
			for (std::size_t Index = FractionLength; Index < FractionDigits; ++Index) Fraction *= 10;
			const std::uint64_t Magnitude = Integer * DecimalScale + Fraction;
			if (Magnitude > Limit)
			{
				OutError = ErrorCode::DecimalOverflow;
				return false;
			}
			if (Negative && Magnitude == 0)
			{
				OutError = ErrorCode::NonCanonicalDecimal;
				return false;
			}
			Out = Negative
				? (Magnitude == Limit ? std::numeric_limits<std::int64_t>::min()
					: -static_cast<std::int64_t>(Magnitude))
				: static_cast<std::int64_t>(Magnitude);
			return true;
		}

		bool ParseAndDispatchRow()
		{
			std::array<std::size_t, FieldCount + 1> Offsets{};
			std::size_t Field = 0;
			Offsets[0] = 0;
			for (std::size_t Index = 0; Index < RowSize; ++Index)
			{
				if (RowBytes[Index] == '\t')
				{
					if (++Field >= FieldCount) return Fail(ErrorCode::WrongFieldCount);
					Offsets[Field] = Index + 1;
				}
			}
			if (Field + 1 != FieldCount) return Fail(ErrorCode::WrongFieldCount);
			Offsets[FieldCount] = RowSize + 1;
			auto Begin = [this, &Offsets](const std::size_t Index) { return RowBytes.data() + Offsets[Index]; };
			auto Length = [&Offsets](const std::size_t Index)
			{
				return Offsets[Index + 1] - Offsets[Index] - 1;
			};
			for (std::size_t Index = 0; Index < FieldCount; ++Index)
			{
				if (Length(Index) == 0) return Fail(ErrorCode::EmptyField, static_cast<std::int32_t>(Index));
			}
			auto Fixed = [this, &Begin, &Length](const std::size_t Index, const char* Text)
			{
				const std::size_t Size = std::strlen(Text);
				return Length(Index) == Size && std::memcmp(Begin(Index), Text, Size) == 0;
			};
			if (!Fixed(1, "0")) return Fail(ErrorCode::FixedFieldMismatch, 1);
			if (!Fixed(2, "3")) return Fail(ErrorCode::FixedFieldMismatch, 2);
			if (!Fixed(3, "16")) return Fail(ErrorCode::FixedFieldMismatch, 3);
			if (!Fixed(7, "0")) return Fail(ErrorCode::FixedFieldMismatch, 7);
			if (!Fixed(11, "1")) return Fail(ErrorCode::FixedFieldMismatch, 11);

			ErrorCode ParseError = ErrorCode::None;
			std::uint64_t Unsigned = 0;
			Row Parsed;
			if (!ParseUnsigned(Begin(0), Length(0), std::numeric_limits<std::int32_t>::max(), Unsigned, ParseError))
				return Fail(ParseError, 0);
			Parsed.Sequence = static_cast<std::int32_t>(Unsigned);
			if (Parsed.Sequence != ParsedRowCount) return Fail(ErrorCode::SequenceMismatch, 0);
			std::uint8_t* Rgb[] = { &Parsed.Red, &Parsed.Green, &Parsed.Blue };
			for (std::size_t Index = 0; Index < 3; ++Index)
			{
				if (!ParseUnsigned(Begin(4 + Index), Length(4 + Index), 255, Unsigned, ParseError))
					return Fail(ParseError == ErrorCode::IntegerOverflow
						? ErrorCode::RgbOutOfRange : ParseError, static_cast<std::int32_t>(4 + Index));
				*Rgb[Index] = static_cast<std::uint8_t>(Unsigned);
			}
			std::int64_t Scaled = 0;
			if (!ParseScaled(Begin(8), Length(8), Scaled, ParseError)) return Fail(ParseError, 8);
			if (Scaled < -900000000LL || Scaled > 900000000LL) return Fail(ErrorCode::LatitudeOutOfRange, 8);
			Parsed.LatitudeE7 = static_cast<std::int32_t>(Scaled);
			if (!ParseScaled(Begin(9), Length(9), Scaled, ParseError)) return Fail(ParseError, 9);
			if (Scaled < -1800000000LL || Scaled > 1800000000LL) return Fail(ErrorCode::LongitudeOutOfRange, 9);
			Parsed.LongitudeE7 = static_cast<std::int32_t>(Scaled);
			if (!ParseScaled(Begin(10), Length(10), Parsed.AltitudeE7, ParseError)) return Fail(ParseError, 10);

			FormattedRow Canonical;
			if (!FormatRow(Parsed, Canonical)
				|| Canonical.Size != RowSize
				|| std::memcmp(Canonical.Bytes.data(), RowBytes.data(), RowSize) != 0)
			{
				return Fail(ErrorCode::NonCanonicalDecimal);
			}
			if (OnRow && !OnRow(Parsed, OnRowContext)) return Fail(ErrorCode::ConsumerRejected);
			++ParsedRowCount;
			return true;
		}

		State StateValue = State::Header;
		std::size_t HeaderIndex = 0;
		std::array<Byte, MaxRowBytes> RowBytes{};
		std::size_t RowSize = 0;
		std::int32_t ExpectedRowCount = 0;
		std::int32_t ParsedRowCount = 0;
		RowConsumer OnRow = nullptr;
		void* OnRowContext = nullptr;
		bool EndedWithSeparator = false;
		Error ErrorValue;
	};
}
