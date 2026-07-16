#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace itch {

// A slab + free-list object pool. Hands out fixed-size slots from contiguous
// slabs and recycles freed ones through an intrusive free list (the link lives
// in the dead slot's own storage), so steady-state add/remove does zero heap
// allocation.
//
// Requires sizeof(T) >= sizeof(void*). Slot pointers stay stable for the pool's
// lifetime, so the order index and FIFO nodes can hold raw pointers into it.
template <class T>
class ObjectPool {
    static_assert(sizeof(T) >= sizeof(void*), "slot must hold a free-list link");
    static_assert(alignof(T) >= alignof(void*), "slot must be aligned for the link");

public:
    ObjectPool() = default;
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    T* allocate() {
        if (free_) {
            void* slot = free_;
            free_ = free_->next;  // read link before we construct over it
            return new (slot) T();
        }
        if (idx_ == kSlab) new_slab();
        return new (&cur_[idx_++]) T();
    }

    void deallocate(T* p) {
        p->~T();
        auto* node = reinterpret_cast<FreeNode*>(p);
        node->next = free_;
        free_ = node;
    }

    std::size_t slab_count() const noexcept { return slabs_.size(); }

private:
    struct FreeNode {
        FreeNode* next;
    };
    struct alignas(T) Slot {
        unsigned char bytes[sizeof(T)];
    };

    static constexpr std::size_t kSlab = 8192;

    void new_slab() {
        slabs_.push_back(std::make_unique<Slot[]>(kSlab));
        cur_ = slabs_.back().get();
        idx_ = 0;
    }

    std::vector<std::unique_ptr<Slot[]>> slabs_;
    Slot* cur_ = nullptr;
    std::size_t idx_ = kSlab;  // forces a fresh slab on the first allocate()
    FreeNode* free_ = nullptr;
};

}  // namespace itch
