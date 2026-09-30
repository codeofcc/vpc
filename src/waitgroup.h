#pragma once
#include <atomic>
#include <functional>
#include "libgo.h"

class WaitGroup {
public:
    WaitGroup() = default;
    ~WaitGroup() = default;

    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;
    WaitGroup(WaitGroup&& other) noexcept;

    void Add(int delta);
    void Done();
    void Wait();
    void Go(std::function<void()> f); // 不再 inline

private:
    std::atomic<int> counter_{ 0 }; // 类内默认初始化，构造函数无需显式赋0
    co_mutex mtx_;
    co_condition_variable cv_;
};