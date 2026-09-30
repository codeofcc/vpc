#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <stdexcept>

// ============ Cache Line 检测 ============
#ifdef __cpp_lib_hardware_interference_size
static constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
static constexpr size_t kCacheLine = 64;
#endif

// ============ 跨平台对齐分配（仅动态版使用） ============
namespace detail_ring
{
    inline void *aligned_alloc_safe(size_t align, size_t size) noexcept
    {
        size = (size + align - 1) & ~(align - 1);
#if defined(_MSC_VER)
        return _aligned_malloc(size, align);
#elif defined(__APPLE__) || defined(__ANDROID__)
        void *ptr = nullptr;
        return (posix_memalign(&ptr, align, size) == 0) ? ptr : nullptr;
#else
        return std::aligned_alloc(align, size);
#endif
    }

    inline void aligned_free_safe(void *ptr) noexcept
    {
#if defined(_MSC_VER)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }
} // namespace detail_ring

// ============================================================================
//  StaticAudioRingBuffer — 编译期固定容量，零堆分配
// ============================================================================
/// @tparam T        元素类型（必须平凡可拷贝）
/// @tparam Capacity 编译期容量，必须是2的幂
template <typename T, size_t Capacity>
class StaticAudioRingBuffer
{
    static_assert(Capacity > 0 && ((Capacity & (Capacity - 1)) == 0),
                  "Capacity must be a positive power of 2");

    static constexpr size_t MASK = Capacity - 1;

    // ---- 写端独占 cache line ----
    alignas(kCacheLine) std::atomic<size_t> write_pos_{0};
    char pad_writer_[kCacheLine - sizeof(std::atomic<size_t>)];

    // ---- 读端独占 cache line ----
    alignas(kCacheLine) std::atomic<size_t> read_pos_{0};
    char pad_reader_[kCacheLine - sizeof(std::atomic<size_t>)];

    // ---- 数据区独占 cache line 对齐 ----
    alignas(kCacheLine) T buf_[Capacity];

public:
    StaticAudioRingBuffer() = default;
    ~StaticAudioRingBuffer() = default;

    StaticAudioRingBuffer(const StaticAudioRingBuffer &) = delete;
    StaticAudioRingBuffer &operator=(const StaticAudioRingBuffer &) = delete;
    StaticAudioRingBuffer(StaticAudioRingBuffer &&) = delete;
    StaticAudioRingBuffer &operator=(StaticAudioRingBuffer &&) = delete;

    // ========== 查询接口 ==========
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] size_t available_read() const noexcept
    {
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t w = write_pos_.load(std::memory_order_acquire);
        return (w - r) & MASK;
    }

    [[nodiscard]] size_t available_write() const noexcept
    {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        return (r - w - 1) & MASK;
    }

    [[nodiscard]] bool empty() const noexcept { return available_read() == 0; }
    [[nodiscard]] bool full() const noexcept { return available_write() == 0; }

    // ========== 零拷贝写入 ==========
    [[nodiscard]] T *begin_write(size_t frames) noexcept
    {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        const size_t free_slots = (r - w - 1) & MASK;
        if (frames > free_slots || frames > Capacity - w)
            return nullptr;
        return buf_ + w;
    }

    void end_write(size_t frames) noexcept
    {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        write_pos_.store((w + frames) & MASK, std::memory_order_release);
    }

    // ========== 零拷贝读取 ==========
    [[nodiscard]] const T *begin_read(size_t frames) noexcept
    {
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t used_slots = (w - r) & MASK;
        if (frames > used_slots || frames > Capacity - r)
            return nullptr;
        return buf_ + r;
    }

    void end_read(size_t frames) noexcept
    {
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        read_pos_.store((r + frames) & MASK, std::memory_order_release);
    }

    // ========== 拷贝快捷接口 ==========
    bool write(const T *src, size_t frames) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "write()/read() use memcpy; T must be trivially copyable. "
                      "Use begin_write()/end_write() for non-trivial types.");
        // if (!src || frames == 0) return false;
        T *dst = begin_write(frames);
        if (!dst)
            return false;
        std::memcpy(dst, src, frames * sizeof(T));
        end_write(frames);
        return true;
    }

    bool read(T *dst, size_t frames) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "write()/read() use memcpy; T must be trivially copyable. "
                      "Use begin_write()/end_write() for non-trivial types.");
        // if (!dst || frames == 0) return false;
        const T *src = begin_read(frames);
        if (!src)
            return false;
        std::memcpy(dst, src, frames * sizeof(T));
        end_read(frames);
        return true;
    }

    // ========== 重置 ==========
    void reset() noexcept
    {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        if constexpr (std::is_arithmetic_v<T>)
            std::memset(buf_, 0, Capacity * sizeof(T));
    }
};

// ============================================================================
//  DynamicAudioRingBuffer — 运行时指定容量（2的幂），堆分配
// ============================================================================
/// @tparam T 元素类型（必须平凡可拷贝）
template <typename T>
class DynamicAudioRingBuffer
{
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    // ---- 写端独占 cache line ----
    alignas(kCacheLine) std::atomic<size_t> write_pos_{0};
    char pad_writer_[kCacheLine - sizeof(std::atomic<size_t>)];

    // ---- 读端独占 cache line ----
    alignas(kCacheLine) std::atomic<size_t> read_pos_{0};
    char pad_reader_[kCacheLine - sizeof(std::atomic<size_t>)];

    // ---- 运行时存储 ----
    T *buf_;
    size_t cap_;
    size_t mask_;

public:
    /// @param capacity 必须是正的2的幂
    explicit DynamicAudioRingBuffer(size_t capacity)
    {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("Dynamic capacity must be a positive power of 2");

        buf_ = static_cast<T *>(
            detail_ring::aligned_alloc_safe(kCacheLine, capacity * sizeof(T)));
        if (!buf_)
            throw std::bad_alloc();

        cap_ = capacity;
        mask_ = capacity - 1;
    }

    ~DynamicAudioRingBuffer()
    {
        detail_ring::aligned_free_safe(buf_);
    }

    DynamicAudioRingBuffer(const DynamicAudioRingBuffer &) = delete;
    DynamicAudioRingBuffer &operator=(const DynamicAudioRingBuffer &) = delete;
    DynamicAudioRingBuffer(DynamicAudioRingBuffer &&) = delete;
    DynamicAudioRingBuffer &operator=(DynamicAudioRingBuffer &&) = delete;

    // ========== 查询接口 ==========
    [[nodiscard]] size_t capacity() const noexcept { return cap_; }

    [[nodiscard]] size_t available_read() const noexcept
    {
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t w = write_pos_.load(std::memory_order_acquire);
        return (w - r) & mask_;
    }

    [[nodiscard]] size_t available_write() const noexcept
    {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        return (r - w - 1) & mask_;
    }

    [[nodiscard]] bool empty() const noexcept { return available_read() == 0; }
    [[nodiscard]] bool full() const noexcept { return available_write() == 0; }

    // ========== 零拷贝写入 ==========
    [[nodiscard]] T *begin_write(size_t frames) noexcept
    {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        const size_t free_slots = (r - w - 1) & mask_;
        if (frames > free_slots || frames > cap_ - w)
            return nullptr;
        return buf_ + w;
    }

    void end_write(size_t frames) noexcept
    {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        write_pos_.store((w + frames) & mask_, std::memory_order_release);
    }

    // ========== 零拷贝读取 ==========
    [[nodiscard]] const T *begin_read(size_t frames) noexcept
    {
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t used_slots = (w - r) & mask_;
        if (frames > used_slots || frames > cap_ - r)
            return nullptr;
        return buf_ + r;
    }

    void end_read(size_t frames) noexcept
    {
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        read_pos_.store((r + frames) & mask_, std::memory_order_release);
    }

    // ========== 拷贝快捷接口 ==========
    bool write(const T *src, size_t frames) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "write()/read() use memcpy; T must be trivially copyable. "
                      "Use begin_write()/end_write() for non-trivial types.");
        // if (!src || frames == 0) return false;
        T *dst = begin_write(frames);
        if (!dst)
            return false;
        std::memcpy(dst, src, frames * sizeof(T));
        end_write(frames);
        return true;
    }

    bool read(T *dst, size_t frames) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "write()/read() use memcpy; T must be trivially copyable. "
                      "Use begin_write()/end_write() for non-trivial types.");
        // if (!dst || frames == 0) return false;
        const T *src = begin_read(frames);
        if (!src)
            return false;
        std::memcpy(dst, src, frames * sizeof(T));
        end_read(frames);
        return true;
    }

    // ========== 重置 ==========
    void reset() noexcept
    {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        if constexpr (std::is_arithmetic_v<T>)
            std::memset(buf_, 0, cap_ * sizeof(T));
    }
};

/// Capacity != 0 → Static；Capacity == 0 → Dynamic
template <typename T, size_t Capacity = 0>
using AudioRingBuffer = std::conditional_t<
    (Capacity != 0),
    StaticAudioRingBuffer<T, Capacity>,
    DynamicAudioRingBuffer<T>>;

static constexpr size_t next_pow2(size_t v) noexcept
{
    if (v == 0)
        return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFFULL
    v |= v >> 32;
#endif
    return v + 1;
}