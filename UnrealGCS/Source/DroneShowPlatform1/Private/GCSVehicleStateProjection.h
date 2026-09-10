#pragma once

#include "DroneData.h"
#include "DSPGCSProtocol.h"

// Transport normalization only. Fleet owns the resulting state; consumers use
// FDroneData and Fleet queries/events without depending on this private adapter.
namespace GCSVehicleStateProjection
{
	inline FDroneData FromTransport(const dspgcs::TelemetryDTO& Source, const bool bSelected)
	{
		FDroneData Target;
		Target.DroneID = UTF8_TO_TCHAR(Source.droneId.c_str());
		Target.SysID = static_cast<int32>(Source.sysId);
		Target.EndpointIdentity = UTF8_TO_TCHAR(Source.connection.linkId.c_str());
		Target.GPSLat = Source.gps.latitudeDeg;
		Target.GPSLon = Source.gps.longitudeDeg;
		Target.bGPSValid = Source.gps.valid;
		Target.GPSFixType = static_cast<int32>(Source.gps.fixType);
		Target.GPSSampleTimestampUtcMs = Source.sampleTimestampUtcMs;
		Target.Altitude = Source.altitude.relativeMeters;
		Target.AltitudeAMSL = Source.altitude.amslMeters;
		Target.bAltitudeValid = Source.altitude.valid;
		Target.Battery = Source.battery.valid
			? Source.battery.remainingPercent
			: 0.0f;
		Target.Mode = UTF8_TO_TCHAR(Source.mode.modeName.c_str());
		Target.RSSI = Source.rssi.valid
			? static_cast<int32>(Source.rssi.valueDbm)
			: 0;
		Target.bOnline = Source.connection.online;
		Target.LEDColor = FLinearColor(
			static_cast<float>(Source.led.red) / 255.0f,
			static_cast<float>(Source.led.green) / 255.0f,
			static_cast<float>(Source.led.blue) / 255.0f,
			1.0f);
		Target.bLEDColorValid = Source.led.confirmed;
		Target.bSelected = bSelected;
		return Target;
	}
}
