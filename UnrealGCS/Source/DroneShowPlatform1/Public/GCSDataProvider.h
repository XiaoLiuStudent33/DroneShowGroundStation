/**
 * @brief 定义协议无关的 UE Fleet 数据源接口。
 *
 * 分类：Production Interface。
 * Production、Mock 和 Acceptance Provider 都通过该接口向 GCSServerAdapter 提交数据。
 */
#pragma once

#include "CoreMinimal.h"

class UGCSServerAdapterSubsystem;

/**
 * Protocol-neutral data source contract.
 *
 * Implementations may represent mock data, a simulator, replay data or a
 * future server connection. The interface itself has no socket or MAVLink
 * dependency.
 */
class DRONESHOWPLATFORM1_API IGCSDataProvider
{
public:
	virtual ~IGCSDataProvider() = default;

	virtual FName GetProviderID() const = 0;
	virtual bool Start(UGCSServerAdapterSubsystem& Adapter) = 0;
	virtual void Stop() = 0;
};
