#pragma once
/// @file SmallVector.hpp
/// @brief std::vector with inline capacity: no heap traffic until it outgrows N.
///
/// Why not std::vector?  Engine hot paths build short lists every frame
/// (visible instances, event receivers, sorted lights).  SmallVector keeps
/// those allocation free while remaining a drop-in range.
///
/// Semantics match std::vector for the operations provided.  Iterators and
/// references are invalidated by growth, exactly like std::vector.

#include "local3d/core/Assert.hpp"
#include "local3d/core/Common.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace l3d {

template <typename T, usize N>
class SmallVector {
    static_assert(N > 0, "SmallVector inline capacity must be positive");

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;
    using reference = T&;
    using const_reference = const T&;

    SmallVector() noexcept = default;

    SmallVector(std::initializer_list<T> values) {
        Reserve(values.size());
        for (const T& value : values) {
            PushBack(value);
        }
    }

    explicit SmallVector(usize count) { Resize(count); }

    SmallVector(const SmallVector& other) {
        Reserve(other.size_);
        for (usize i = 0; i < other.size_; ++i) {
            Construct(i, other[i]);
        }
        size_ = other.size_;
    }

    SmallVector(SmallVector&& other) noexcept { MoveFrom(other); }

    /// Destroys the elements and releases the heap buffer if the vector ever
    /// outgrew its inline capacity.  Clear() alone keeps the allocation, which
    /// is what assignment wants; only the destructor gives the memory back.
    ~SmallVector() {
        Clear();
        FreeHeap();
    }

    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            Clear();
            Reserve(other.size_);
            for (usize i = 0; i < other.size_; ++i) {
                Construct(i, other[i]);
            }
            size_ = other.size_;
        }
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            Clear();
            MoveFrom(other);
        }
        return *this;
    }

    void PushBack(const T& value) {
        GrowIfNeeded();
        Construct(size_++, value);
    }

    void PushBack(T&& value) {
        GrowIfNeeded();
        Construct(size_++, std::move(value));
    }

    template <typename... Args>
    T& EmplaceBack(Args&&... args) {
        GrowIfNeeded();
        T* slot = Data() + size_;
        new (slot) T(std::forward<Args>(args)...); // NOLINT
        ++size_;
        return *slot;
    }

    void PopBack() {
        L3D_ASSERT(size_ > 0);
        --size_;
        Data()[size_].~T();
    }

    void Insert(usize index, const T& value) {
        L3D_ASSERT(index <= size_);
        GrowIfNeeded();
        T* data = Data();
        if (index < size_) {
            Construct(size_, std::move(data[size_ - 1]));
            for (usize i = size_ - 1; i > index; --i) {
                data[i] = std::move(data[i - 1]);
            }
            data[index] = value;
        } else {
            Construct(index, value);
        }
        ++size_;
    }

    /// Remove the element at `index`, preserving order.
    void Erase(usize index) {
        L3D_ASSERT(index < size_);
        T* data = Data();
        for (usize i = index; i + 1 < size_; ++i) {
            data[i] = std::move(data[i + 1]);
        }
        --size_;
        data[size_].~T();
    }

    /// Remove by swapping with the last element.  O(1), order not preserved.
    void EraseUnordered(usize index) {
        L3D_ASSERT(index < size_);
        T* data = Data();
        if (index + 1 != size_) {
            data[index] = std::move(data[size_ - 1]);
        }
        --size_;
        data[size_].~T();
    }

    void Resize(usize count) {
        if (count < size_) {
            for (usize i = count; i < size_; ++i) {
                Data()[i].~T();
            }
            size_ = count;
            return;
        }
        Reserve(count);
        for (usize i = size_; i < count; ++i) {
            Construct(i, T{});
        }
        size_ = count;
    }

    void Clear() {
        T* data = Data();
        for (usize i = 0; i < size_; ++i) {
            data[i].~T();
        }
        size_ = 0;
    }

    void Reserve(usize count) {
        if (count <= Capacity()) {
            return;
        }
        usize newCapacity = Capacity() * 2;
        while (newCapacity < count) {
            newCapacity *= 2;
        }
        T* fresh = static_cast<T*>(::operator new(newCapacity * sizeof(T), std::align_val_t{alignof(T)}));
        for (usize i = 0; i < size_; ++i) {
            new (fresh + i) T(std::move(Data()[i])); // NOLINT
            Data()[i].~T();
        }
        if (IsHeap()) {
            ::operator delete(heap_, std::align_val_t{alignof(T)});
        }
        heap_ = fresh;
        capacity_ = newCapacity;
    }

    [[nodiscard]] T* Data() noexcept { return IsHeap() ? heap_ : Inline(); }
    [[nodiscard]] const T* Data() const noexcept { return IsHeap() ? heap_ : Inline(); }

    [[nodiscard]] usize Size() const noexcept { return size_; }
    [[nodiscard]] usize Capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool IsInline() const noexcept { return !IsHeap(); }
    [[nodiscard]] usize MemoryBytes() const noexcept {
        return IsHeap() ? capacity_ * sizeof(T) : 0;
    }

    [[nodiscard]] T& operator[](usize index) noexcept {
        L3D_ASSERT(index < size_);
        return Data()[index];
    }
    [[nodiscard]] const T& operator[](usize index) const noexcept {
        L3D_ASSERT(index < size_);
        return Data()[index];
    }
    [[nodiscard]] T& Front() noexcept { return (*this)[0]; }
    [[nodiscard]] const T& Front() const noexcept { return (*this)[0]; }
    [[nodiscard]] T& Back() noexcept { return (*this)[size_ - 1]; }
    [[nodiscard]] const T& Back() const noexcept { return (*this)[size_ - 1]; }

    [[nodiscard]] iterator begin() noexcept { return Data(); }
    [[nodiscard]] iterator end() noexcept { return Data() + size_; }
    [[nodiscard]] const_iterator begin() const noexcept { return Data(); }
    [[nodiscard]] const_iterator end() const noexcept { return Data() + size_; }

private:
    [[nodiscard]] bool IsHeap() const noexcept { return capacity_ > N; }

    /// Return the heap allocation, if any, and go back to inline storage.
    void FreeHeap() noexcept {
        if (IsHeap()) {
            ::operator delete(heap_, std::align_val_t{alignof(T)});
            heap_ = nullptr;
            capacity_ = N;
        }
    }

    /// Take ownership of `other`'s contents.  When the source is on the heap we
    /// steal the allocation instead of moving element by element; otherwise we
    /// move into our own inline storage (which always has room for N elements).
    void MoveFrom(SmallVector& other) noexcept {
        if (other.IsHeap()) {
            // Release our own heap buffer before taking over the source's, or
            // move assignment over a grown vector would leak it.
            FreeHeap();
            heap_ = other.heap_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            other.heap_ = nullptr;
            other.capacity_ = N;
            other.size_ = 0;
            return;
        }
        for (usize i = 0; i < other.size_; ++i) {
            Construct(i, std::move(other[i]));
            other[i].~T();
        }
        size_ = other.size_;
        other.size_ = 0;
    }
    [[nodiscard]] T* Inline() noexcept { return reinterpret_cast<T*>(&inlineStorage_); } // NOLINT
    [[nodiscard]] const T* Inline() const noexcept {
        return reinterpret_cast<const T*>(&inlineStorage_); // NOLINT
    }

    void GrowIfNeeded() {
        if (size_ == Capacity()) {
            Reserve(Capacity() + 1);
        }
    }

    template <typename... Args>
    void Construct(usize index, Args&&... args) {
        new (Data() + index) T(std::forward<Args>(args)...); // NOLINT
    }

    alignas(T) unsigned char inlineStorage_[sizeof(T) * N]{};
    T* heap_ = nullptr;
    usize capacity_ = N;
    usize size_ = 0;
};

} // namespace l3d
