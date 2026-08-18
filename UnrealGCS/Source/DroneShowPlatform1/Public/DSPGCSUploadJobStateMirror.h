#pragma once

#include "CoreMinimal.h"
#include "DSPGCSUploadJobProtocol.h"

#include <unordered_map>

/** Value-only UE mirror of Console-owned Upload Job authority. */
class DRONESHOWPLATFORM1_API FDSPGCSUploadJobStateMirror final
{
public:
	void Reset();
	void SetPendingCommandID(const std::string& ClientCommandID);
	bool ApplyCommandAck(const dspgcs::UploadJobCommandAckDTO& Ack);
	bool ApplySnapshot(const dspgcs::UploadJobSnapshotDTO& Snapshot);
	bool ApplyRoutePage(const dspgcs::UploadJobRouteStatePageDTO& Page);
	bool ApplyRouteDelta(
		const dspgcs::UploadJobRouteStateDeltaDTO& Delta,
		bool& bOutDuplicateIgnored);
	void MarkTransportReconnected();
	void MarkPageOpened();
	void ClearRecoveryRequired();

	bool HasAuthoritativeSnapshot() const { return bHasSnapshot; }
	bool IsRecoveryRequired() const { return bRecoveryRequired; }
	bool IsRoutePageComplete() const { return bRoutePageComplete; }
	const dspgcs::UploadJobSnapshotDTO* GetSnapshot() const
	{
		return bHasSnapshot ? &CurrentSnapshot : nullptr;
	}
	const dspgcs::UploadRouteStateDTO* FindRoute(int32 RouteSlotIndex) const;
	int32 GetRouteStateCount() const { return static_cast<int32>(Routes.size()); }
	std::uint32_t GetNextRoutePageStart() const { return NextRoutePageStart; }
	const std::string& GetPendingCommandID() const { return PendingCommandID; }

private:
	dspgcs::UploadJobSnapshotDTO CurrentSnapshot;
	std::unordered_map<std::int32_t, dspgcs::UploadRouteStateDTO> Routes;
	std::string PendingCommandID;
	std::uint64_t LastDeltaSequence = 0;
	std::uint32_t NextRoutePageStart = 0;
	bool bHasSnapshot = false;
	bool bRecoveryRequired = false;
	bool bRoutePageComplete = false;
};
