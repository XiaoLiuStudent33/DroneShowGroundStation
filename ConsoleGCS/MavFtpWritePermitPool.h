#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dronegcs::mavftp
{
    struct WritePermitPoolMetrics
    {
        std::uint32_t capacity = 1;
        std::uint32_t outstanding = 0;
        std::uint32_t maxObservedOutstanding = 0;
        std::uint64_t permitsGranted = 0;
        std::uint64_t permitsReleased = 0;
        std::uint64_t permitWaitCount = 0;
        std::uint64_t invalidReleaseCount = 0;
    };

    class IWritePermitPool
    {
    public:
        virtual ~IWritePermitPool() = default;
        virtual void Register(const void* owner) = 0;
        virtual void SetReady(const void* owner, bool ready) = 0;
        virtual bool TryAcquire(const void* owner) = 0;
        virtual void Release(const void* owner) = 0;
        virtual void Unregister(const void* owner) = 0;
    };

    /** Dynamic-N, service-thread-only bounded round-robin Write budget. */
    class WritePermitPool final : public IWritePermitPool
    {
    public:
        explicit WritePermitPool(const std::uint32_t capacity = 1)
        {
            metrics_.capacity = capacity == 0 ? 1 : capacity;
        }

        void Reset(const std::uint32_t capacity)
        {
            owners_.clear(); held_.clear(); ready_.clear(); nextIndex_ = 0;
            metrics_ = {};
            metrics_.capacity = capacity == 0 ? 1 : capacity;
        }

        /** Preserves owners and held permits; a lower cap drains naturally. */
        void SetCapacity(const std::uint32_t capacity)
        {
            metrics_.capacity = capacity == 0 ? 1 : capacity;
        }

        void Register(const void* const owner) override
        {
            if (!owner || held_.find(owner) != held_.end()) return;
            owners_.push_back(owner);
            held_[owner] = 0;
            ready_[owner] = false;
            if (owners_.size() == 1) nextIndex_ = 0;
        }

        void SetReady(const void* const owner, const bool ready) override
        {
            Register(owner);
            const auto item = ready_.find(owner);
            if (item != ready_.end()) item->second = ready;
        }

        bool TryAcquire(const void* const owner) override
        {
            Register(owner);
            if (!owner || metrics_.outstanding >= metrics_.capacity)
            {
                ++metrics_.permitWaitCount;
                return false;
            }
            const std::size_t selected = NextReadyIndex();
            if (selected == owners_.size() || owners_[selected] != owner)
            {
                ++metrics_.permitWaitCount;
                return false;
            }
            ++held_[owner]; ++metrics_.outstanding; ++metrics_.permitsGranted;
            metrics_.maxObservedOutstanding = (std::max)(
                metrics_.maxObservedOutstanding, metrics_.outstanding);
            nextIndex_ = owners_.empty() ? 0 : (selected + 1) % owners_.size();
            return true;
        }

        void Release(const void* const owner) override
        {
            const auto item = held_.find(owner);
            if (item == held_.end() || item->second == 0 ||
                metrics_.outstanding == 0)
            {
                ++metrics_.invalidReleaseCount;
                return;
            }
            --item->second; --metrics_.outstanding; ++metrics_.permitsReleased;
        }

        void Unregister(const void* const owner) override
        {
            const auto held = held_.find(owner);
            if (held != held_.end()) while (held->second > 0) Release(owner);
            const auto item = std::find(owners_.begin(), owners_.end(), owner);
            if (item != owners_.end())
            {
                const std::size_t removed = static_cast<std::size_t>(
                    std::distance(owners_.begin(), item));
                owners_.erase(item);
                if (owners_.empty()) nextIndex_ = 0;
                else if (removed < nextIndex_) --nextIndex_;
                else if (nextIndex_ >= owners_.size()) nextIndex_ = 0;
            }
            held_.erase(owner); ready_.erase(owner);
        }

        const WritePermitPoolMetrics& GetMetrics() const { return metrics_; }
        std::uint32_t GetHeld(const void* const owner) const
        {
            const auto item = held_.find(owner);
            return item == held_.end() ? 0 : item->second;
        }
        std::size_t GetRegisteredOwnerCount() const { return owners_.size(); }

    private:
        std::size_t NextReadyIndex() const
        {
            if (owners_.empty()) return owners_.size();
            for (std::size_t offset = 0; offset < owners_.size(); ++offset)
            {
                const std::size_t index = (nextIndex_ + offset) % owners_.size();
                const auto ready = ready_.find(owners_[index]);
                if (ready != ready_.end() && ready->second) return index;
            }
            return owners_.size();
        }

        std::vector<const void*> owners_;
        std::unordered_map<const void*, std::uint32_t> held_;
        std::unordered_map<const void*, bool> ready_;
        std::size_t nextIndex_ = 0;
        WritePermitPoolMetrics metrics_;
    };
}
