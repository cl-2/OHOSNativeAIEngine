#ifndef CANCEL_SCOPE_H
#define CANCEL_SCOPE_H

#include <atomic>
#include <cstdint>

/**
 * @brief 线程安全的中断作用域 — 用生成计数器替代布尔标志
 *
 * 为什么需要这个？为什么不能用 bool？
 * ===================================
 * 用 std::atomic<bool> 做中断标志有 ABA 问题：
 *   Thread A: 检测到唤醒 → 设 flag=true
 *   Thread B: 处理完唤醒 → 设 flag=false
 *   Thread A: 检测到新唤醒 → 设 flag=true
 *   Thread B: 执行旧逻辑 → 设 flag=false  ← 清掉了 Thread A 的新中断！
 *
 * 用 generation counter 解决：
 *   Cancel() 每次递增计数器，不重置
 *   消费者在操作开始时保存 Get()，操作中检查 IsStale(saved)
 *   如果别人 Cancel() 过，saved 必然小于最新值 → 无条件过期
 *   不存在"旧信号覆盖新信号"的问题
 *
 * 使用方式：
 *   CancelScope scope;
 *   // 线程 A（生产者）：
 *   scope.Cancel();  // 通知所有进行中的操作取消
 *
 *   // 线程 B（消费者）：
 *   auto gen = scope.Get();  // 在开始操作前保存
 *   while (...) {
 *       if (scope.IsStale(gen)) break;  // 有人 Cancel 了
 *       // 继续工作...
 *   }
 */
class CancelScope {
public:
    CancelScope() = default;
    ~CancelScope() = default;

    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;

    /// 取消当前所有操作。每次调用递增计数器，所有之前保存的 generation 都过期。
    void Cancel() {
        m_generation.fetch_add(1, std::memory_order_release);
    }

    /// 获取当前 generation。在开始一个操作前调用并保存返回值。
    uint64_t Get() const {
        return m_generation.load(std::memory_order_acquire);
    }

    /// 检查 savedGen 是否过时（即：有人在我保存之后 Cancel 过）。
    bool IsStale(uint64_t savedGen) const {
        return savedGen != m_generation.load(std::memory_order_acquire);
    }

private:
    std::atomic<uint64_t> m_generation{0};
};

#endif // CANCEL_SCOPE_H
