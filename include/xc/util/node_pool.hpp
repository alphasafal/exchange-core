#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace xc {

/// A free list for fixed-size blocks, carved out of larger chunks.
///
/// Node-based containers allocate one node per element, which is why the price
/// level maps allocated every time a level was created. Levels at the touch
/// empty and refill continuously in real order flow, so that is not a
/// start-up cost -- it is an allocation on the matching path, at the busiest
/// price in the book.
///
/// The pool learns its block size from the first request and serves every
/// subsequent one from a free list. Anything of a different size falls through
/// to the global allocator, so a container that allocates something unexpected
/// still behaves correctly rather than corrupting memory; those are counted
/// separately, and the tests assert the count stays at zero.
class NodePool {
  public:
    explicit NodePool(std::size_t blocks_per_chunk = 512)
        : blocks_per_chunk_(blocks_per_chunk == 0 ? 1 : blocks_per_chunk) {}

    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;

    void* allocate(std::size_t bytes) {
        if (block_size_ == 0) {
            block_size_ = bytes;
        }
        if (bytes != block_size_) {
            ++foreign_allocations_;
            return ::operator new(bytes);
        }
        if (free_list_ == nullptr) {
            add_chunk();
        }
        void* block = free_list_;
        free_list_ = *reinterpret_cast<void**>(block);
        ++live_;
        return block;
    }

    void deallocate(void* block, std::size_t bytes) {
        if (block == nullptr) {
            return;
        }
        if (bytes != block_size_) {
            ::operator delete(block);
            return;
        }
        // The free-list link lives inside the block itself, so recycling costs
        // no memory beyond the blocks already held.
        *reinterpret_cast<void**>(block) = free_list_;
        free_list_ = block;
        --live_;
    }

    /// Reserves enough blocks of `block_size` to satisfy `count` without
    /// growing. Called before the workload starts so the matching path never
    /// takes a chunk allocation.
    void reserve(std::size_t block_size, std::size_t count) {
        if (block_size_ == 0) {
            block_size_ = block_size;
        }
        while (block_size_ == block_size && available_ < count) {
            add_chunk();
        }
    }

    std::size_t live() const noexcept { return live_; }
    std::size_t block_size() const noexcept { return block_size_; }

    /// Chunks taken from the global allocator. Expected to stop growing once
    /// the workload reaches steady state.
    std::uint64_t chunks() const noexcept { return chunks_.size(); }

    /// Requests for a size other than the pooled one, which went to the global
    /// allocator.
    std::uint64_t foreign_allocations() const noexcept { return foreign_allocations_; }

  private:
    void add_chunk() {
        // Blocks must be able to hold the free-list pointer while they are on
        // it, which is the only constraint the layout has.
        const std::size_t block = block_size_ < sizeof(void*) ? sizeof(void*) : block_size_;
        const std::size_t bytes = block * blocks_per_chunk_;
        auto chunk = std::make_unique<std::byte[]>(bytes);

        std::byte* cursor = chunk.get();
        for (std::size_t i = 0; i < blocks_per_chunk_; ++i) {
            void* slot = cursor + i * block;
            *reinterpret_cast<void**>(slot) = free_list_;
            free_list_ = slot;
        }
        available_ += blocks_per_chunk_;
        chunks_.push_back(std::move(chunk));
    }

    std::vector<std::unique_ptr<std::byte[]>> chunks_;
    void* free_list_ = nullptr;
    std::size_t block_size_ = 0;
    std::size_t blocks_per_chunk_;
    std::size_t live_ = 0;
    std::size_t available_ = 0;
    std::uint64_t foreign_allocations_ = 0;
};

/// Standard-library allocator backed by a NodePool.
///
/// Stateful, so it must be carried by the container it is given to. The pool is
/// borrowed rather than owned: it outlives every container that draws from it,
/// which the order book guarantees by declaring the pool before the maps that
/// use it.
template<typename T>
class PoolAllocator {
  public:
    using value_type = T;

    explicit PoolAllocator(NodePool* pool) noexcept : pool_(pool) {}

    template<typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_(other.pool()) {}

    T* allocate(std::size_t count) {
        if (count != 1 || pool_ == nullptr) {
            // Only single-node requests are pooled. A container asking for an
            // array wants contiguous storage the free list cannot provide.
            return static_cast<T*>(::operator new(count * sizeof(T)));
        }
        return static_cast<T*>(pool_->allocate(sizeof(T)));
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        if (count != 1 || pool_ == nullptr) {
            ::operator delete(pointer);
            return;
        }
        pool_->deallocate(pointer, sizeof(T));
    }

    NodePool* pool() const noexcept { return pool_; }

    template<typename U>
    bool operator==(const PoolAllocator<U>& other) const noexcept {
        return pool_ == other.pool();
    }

  private:
    NodePool* pool_ = nullptr;
};

}  // namespace xc
