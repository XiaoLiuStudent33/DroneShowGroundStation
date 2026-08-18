#pragma once

#include "Wpl110Core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace dronegcs::route_mission
{
	using Byte = std::uint8_t;

	inline constexpr std::size_t MissionItemRecordBytesV1 = 38;
	inline constexpr std::size_t LegacyMissionFtpContainerBytesV1 = 10;
	inline constexpr std::uint16_t LegacyMissionFtpMagicV1 = 0x763d;
	inline constexpr std::uint16_t LegacyMissionTypeV1 = 0;
	inline constexpr std::uint16_t FirstContainerOptionsV1 = 0;
	inline constexpr std::uint16_t SubsequentContainerOptionsV1 = 1;
	inline constexpr std::uint32_t MaximumLogicalRouteRowsV1 = 65536;

	enum class ErrorCode : std::uint8_t
	{
		None,
		InvalidArgument,
		SequenceOutOfRange,
		RouteRowCountOutOfRange,
		RouteRowCountMismatch,
		SequenceMismatch,
		SizeOverflow,
		AltitudeOverflow,
		BufferTooSmall,
		ConsumerRejected,
		ContainerFieldOutOfRange,
		EmptyContainerRejected,
		ContainerRangeOverflow
	};

	struct Error
	{
		ErrorCode Code = ErrorCode::None;
		std::int32_t RowIndex = -1;
		std::string Detail;
	};

	inline bool Fail(
		Error& OutError,
		const ErrorCode Code,
		const char* const Detail,
		const std::int32_t RowIndex = -1)
	{
		OutError.Code = Code;
		OutError.RowIndex = RowIndex;
		OutError.Detail = Detail ? Detail : "";
		return false;
	}

	inline void WriteU16LittleEndian(Byte* const Out, const std::uint16_t Value)
	{
		Out[0] = static_cast<Byte>(Value);
		Out[1] = static_cast<Byte>(Value >> 8);
	}

	inline void WriteU32LittleEndian(Byte* const Out, const std::uint32_t Value)
	{
		Out[0] = static_cast<Byte>(Value);
		Out[1] = static_cast<Byte>(Value >> 8);
		Out[2] = static_cast<Byte>(Value >> 16);
		Out[3] = static_cast<Byte>(Value >> 24);
	}

	inline int MostSignificantBit(const std::uint64_t Value)
	{
		int Result = -1;
		std::uint64_t Remaining = Value;
		while (Remaining != 0)
		{
			++Result;
			Remaining >>= 1;
		}
		return Result;
	}

	inline std::uint64_t UnsignedMagnitude(const std::int64_t Value)
	{
		return Value >= 0
			? static_cast<std::uint64_t>(Value)
			: static_cast<std::uint64_t>(-(Value + 1)) + 1;
	}

	/**
	 * Converts signed E7 meters to IEEE-754 binary32 bits using only integer
	 * arithmetic. The quotient is rounded to nearest, ties to even. This does
	 * not read or change the process floating-point environment.
	 */
	inline bool AltitudeE7ToBinary32Bits(
		const std::int64_t AltitudeE7,
		std::uint32_t& OutBits,
		Error& OutError)
	{
		OutBits = 0;
		constexpr std::uint64_t Denominator = 10000000;
		const std::uint64_t Magnitude = UnsignedMagnitude(AltitudeE7);
		if (Magnitude == 0)
		{
			OutError = {};
			return true;
		}

		int Exponent = MostSignificantBit(Magnitude) - MostSignificantBit(Denominator);
		bool LessThanPower = false;
		if (Exponent >= 0)
		{
			const int DenominatorBits = MostSignificantBit(Denominator) + 1;
			LessThanPower = Exponent >= 64 - DenominatorBits
				|| Magnitude < (Denominator << Exponent);
		}
		else
		{
			const int LeftShift = -Exponent;
			LessThanPower = LeftShift >= 64 - (MostSignificantBit(Magnitude) + 1)
				? false
				: (Magnitude << LeftShift) < Denominator;
		}
		if (LessThanPower) --Exponent;

		const int ScaleShift = 23 - Exponent;
		std::uint64_t Quotient = 0;
		std::uint64_t Remainder = 0;
		std::uint64_t RoundingDenominator = Denominator;
		if (ScaleShift >= 0)
		{
			Quotient = Magnitude / Denominator;
			Remainder = Magnitude % Denominator;
			for (int Index = 0; Index < ScaleShift; ++Index)
			{
				Quotient <<= 1;
				Remainder <<= 1;
				if (Remainder >= Denominator)
				{
					++Quotient;
					Remainder -= Denominator;
				}
			}
		}
		else
		{
			const int DenominatorShift = -ScaleShift;
			if (DenominatorShift >= 64 - (MostSignificantBit(Denominator) + 1))
				return Fail(OutError, ErrorCode::AltitudeOverflow,
					"Altitude rational scaling overflowed.");
			RoundingDenominator = Denominator << DenominatorShift;
			Quotient = Magnitude / RoundingDenominator;
			Remainder = Magnitude % RoundingDenominator;
		}

		const std::uint64_t TwiceRemainder = Remainder << 1;
		if (TwiceRemainder > RoundingDenominator
			|| (TwiceRemainder == RoundingDenominator && (Quotient & 1U) != 0))
		{
			++Quotient;
		}
		if (Quotient == (std::uint64_t{1} << 24))
		{
			Quotient >>= 1;
			++Exponent;
		}

		const int BiasedExponent = Exponent + 127;
		if (BiasedExponent <= 0 || BiasedExponent >= 255
			|| Quotient < (std::uint64_t{1} << 23)
			|| Quotient >= (std::uint64_t{1} << 24))
		{
			return Fail(OutError, ErrorCode::AltitudeOverflow,
				"Altitude is not representable as finite normalized binary32.");
		}

		OutBits = (AltitudeE7 < 0 ? 0x80000000U : 0U)
			| (static_cast<std::uint32_t>(BiasedExponent) << 23)
			| (static_cast<std::uint32_t>(Quotient) & 0x007fffffU);
		OutError = {};
		return true;
	}

	inline std::uint32_t Rgb8ToBinary32Bits(const std::uint8_t Value)
	{
		if (Value == 0) return 0;
		const int Exponent = MostSignificantBit(Value);
		const std::uint32_t Significand = static_cast<std::uint32_t>(Value) << (23 - Exponent);
		return (static_cast<std::uint32_t>(Exponent + 127) << 23)
			| (Significand & 0x007fffffU);
	}

	inline bool EncodeMissionItemRecordV1(
		const wpl110::Row& Row,
		std::array<Byte, MissionItemRecordBytesV1>& Out,
		Error& OutError)
	{
		Out.fill(0);
		if (Row.Sequence < 0 || Row.Sequence > 65535)
			return Fail(OutError, ErrorCode::SequenceOutOfRange,
				"Mission sequence exceeds uint16.", Row.Sequence);
		std::uint32_t AltitudeBits = 0;
		if (!AltitudeE7ToBinary32Bits(Row.AltitudeE7, AltitudeBits, OutError))
		{
			OutError.RowIndex = Row.Sequence;
			return false;
		}

		WriteU32LittleEndian(Out.data() + 0, Rgb8ToBinary32Bits(Row.Red));
		WriteU32LittleEndian(Out.data() + 4, Rgb8ToBinary32Bits(Row.Green));
		WriteU32LittleEndian(Out.data() + 8, Rgb8ToBinary32Bits(Row.Blue));
		WriteU32LittleEndian(Out.data() + 12, 0); // Param4 is positive zero.
		WriteU32LittleEndian(Out.data() + 16, static_cast<std::uint32_t>(Row.LatitudeE7));
		WriteU32LittleEndian(Out.data() + 20, static_cast<std::uint32_t>(Row.LongitudeE7));
		WriteU32LittleEndian(Out.data() + 24, AltitudeBits);
		WriteU16LittleEndian(Out.data() + 28, static_cast<std::uint16_t>(Row.Sequence));
		WriteU16LittleEndian(Out.data() + 30, 16); // MAV_CMD_NAV_WAYPOINT compatibility v1.
		Out[32] = 0; // target_system
		Out[33] = 0; // target_component
		Out[34] = 3; // MAV_FRAME_GLOBAL_RELATIVE_ALT compatibility v1.
		Out[35] = 0; // current
		Out[36] = 1; // autocontinue
		Out[37] = 0; // mission_type
		OutError = {};
		return true;
	}

	inline bool CheckedMissionPayloadSize(
		const std::uint32_t RowCount,
		std::size_t& OutBytes,
		Error& OutError)
	{
		OutBytes = 0;
		if (RowCount == 0 || RowCount > MaximumLogicalRouteRowsV1)
			return Fail(OutError, ErrorCode::RouteRowCountOutOfRange,
				"Logical route row count must be in 1..65536.");
		if (RowCount > (std::numeric_limits<std::size_t>::max)() / MissionItemRecordBytesV1)
			return Fail(OutError, ErrorCode::SizeOverflow,
				"Mission payload size overflowed.");
		OutBytes = static_cast<std::size_t>(RowCount) * MissionItemRecordBytesV1;
		OutError = {};
		return true;
	}

	using ByteConsumer = bool (*)(const Byte* Bytes, std::size_t Size, void* Context);

	class WholeRouteEncoderV1 final
	{
	public:
		bool Begin(const std::uint32_t ExpectedRows, Error& OutError)
		{
			Bytes.clear();
			Rows = 0;
			std::size_t Required = 0;
			if (!CheckedMissionPayloadSize(ExpectedRows, Required, OutError)) return false;
			Expected = ExpectedRows;
			Bytes.reserve(Required);
			return true;
		}

		bool Consume(const wpl110::Row& Row, Error& OutError)
		{
			if (Rows >= Expected || Row.Sequence != static_cast<std::int32_t>(Rows))
				return Fail(OutError, ErrorCode::SequenceMismatch,
					"Typed Mission rows must be contiguous from sequence zero.", Row.Sequence);
			std::array<Byte, MissionItemRecordBytesV1> Encoded{};
			if (!EncodeMissionItemRecordV1(Row, Encoded, OutError)) return false;
			Bytes.insert(Bytes.end(), Encoded.begin(), Encoded.end());
			++Rows;
			return true;
		}

		bool Finish(Error& OutError) const
		{
			if (Rows != Expected)
				return Fail(OutError, ErrorCode::RouteRowCountMismatch,
					"Mission row count differs from the declared route count.");
			OutError = {};
			return true;
		}

		const std::vector<Byte>& GetBytes() const { return Bytes; }
		std::uint32_t GetRowsEncoded() const { return Rows; }

	private:
		std::vector<Byte> Bytes;
		std::uint32_t Expected = 0;
		std::uint32_t Rows = 0;
	};

	/** Caller-owned buffer enables chunked or fixed small-buffer streaming. */
	class BufferedRouteEncoderV1 final
	{
	public:
		bool Begin(
			const std::uint32_t ExpectedRows,
			Byte* const Buffer,
			const std::size_t BufferCapacity,
			const ByteConsumer Consumer,
			void* const ConsumerContext,
			Error& OutError)
		{
			std::size_t Ignored = 0;
			if (!CheckedMissionPayloadSize(ExpectedRows, Ignored, OutError)) return false;
			if (!Buffer || BufferCapacity < MissionItemRecordBytesV1 || !Consumer)
				return Fail(OutError, ErrorCode::BufferTooSmall,
					"Mission output buffer must hold at least one complete record.");
			Expected = ExpectedRows;
			Rows = 0;
			Storage = Buffer;
			Capacity = BufferCapacity - BufferCapacity % MissionItemRecordBytesV1;
			Used = 0;
			OnBytes = Consumer;
			Context = ConsumerContext;
			return true;
		}

		bool Consume(const wpl110::Row& Row, Error& OutError)
		{
			if (Rows >= Expected || Row.Sequence != static_cast<std::int32_t>(Rows))
				return Fail(OutError, ErrorCode::SequenceMismatch,
					"Typed Mission rows must be contiguous from sequence zero.", Row.Sequence);
			if (Capacity - Used < MissionItemRecordBytesV1 && !Flush(OutError)) return false;
			std::array<Byte, MissionItemRecordBytesV1> Encoded{};
			if (!EncodeMissionItemRecordV1(Row, Encoded, OutError)) return false;
			for (std::size_t Index = 0; Index < Encoded.size(); ++Index)
				Storage[Used + Index] = Encoded[Index];
			Used += Encoded.size();
			++Rows;
			return true;
		}

		bool Finish(Error& OutError)
		{
			if (Rows != Expected)
				return Fail(OutError, ErrorCode::RouteRowCountMismatch,
					"Mission row count differs from the declared route count.");
			return Flush(OutError);
		}

		std::uint32_t GetRowsEncoded() const { return Rows; }

	private:
		bool Flush(Error& OutError)
		{
			if (Used == 0)
			{
				OutError = {};
				return true;
			}
			if (!OnBytes(Storage, Used, Context))
				return Fail(OutError, ErrorCode::ConsumerRejected,
					"Mission byte consumer rejected output.");
			Used = 0;
			OutError = {};
			return true;
		}

		Byte* Storage = nullptr;
		std::size_t Capacity = 0;
		std::size_t Used = 0;
		ByteConsumer OnBytes = nullptr;
		void* Context = nullptr;
		std::uint32_t Expected = 0;
		std::uint32_t Rows = 0;
	};

	inline bool EncodeLegacyMissionFtpContainerV1(
		const std::uint32_t StartSequence,
		const std::uint32_t ItemCount,
		const std::uint32_t Options,
		std::array<Byte, LegacyMissionFtpContainerBytesV1>& Out,
		Error& OutError)
	{
		Out.fill(0);
		if (StartSequence > 65535 || ItemCount > 65535 || Options > 65535)
			return Fail(OutError, ErrorCode::ContainerFieldOutOfRange,
				"Legacy container fields must fit uint16.");
		if (ItemCount == 0)
			return Fail(OutError, ErrorCode::EmptyContainerRejected,
				"Production container API rejects zero-item containers.");
		if (ItemCount - 1 > 65535 - StartSequence)
			return Fail(OutError, ErrorCode::ContainerRangeOverflow,
				"Legacy container sequence range exceeds uint16.");

		WriteU16LittleEndian(Out.data() + 0, LegacyMissionFtpMagicV1);
		WriteU16LittleEndian(Out.data() + 2, LegacyMissionTypeV1);
		WriteU16LittleEndian(Out.data() + 4, static_cast<std::uint16_t>(Options));
		WriteU16LittleEndian(Out.data() + 6, static_cast<std::uint16_t>(StartSequence));
		WriteU16LittleEndian(Out.data() + 8, static_cast<std::uint16_t>(ItemCount));
		OutError = {};
		return true;
	}
}
