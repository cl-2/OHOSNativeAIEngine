// RingBuffer.h - 线程安全的多生产者单消费者环形缓冲区
// 用于音频采集线程→ASR推理线程的零拷贝传输
// 基于无锁 atomic 设计，避免 mutex 竞争导致的延迟抖动

#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OHOS_RingBuffer"

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : m_capacity(capacity)
        , m_buffer(new T[capacity])
        , m_head(0)
        , m_tail(0)
        , m_size(0)
    {
        // 确保 capacity 是 2 的幂，方便取模优化
        // 如果不是，对齐到下一个 2 的幂
        if ((capacity & (capacity - 1)) != 0) {
            size_t nextPow2 = 1;
            while (nextPow2 < capacity) nextPow2 <<= 1;
            m_capacity = nextPow2;
            m_buffer.reset(new T[m_capacity]);
        }
        m_mask = m_capacity - 1;
    }

    ~RingBuffer() = default;

    // 禁止拷贝
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 写入数据（生产者线程调用）
    // 返回实际写入的元素数
    size_t Write(const T* data, size_t count) {
        size_t written = 0;
        while (written < count) {
            size_t currentHead = m_head.load(std::memory_order_acquire);
            size_t currentTail = m_tail.load(std::memory_order_relaxed);
            
            if (currentHead == currentTail) {
                // 缓冲区为空，快速路径
                size_t toWrite = count - written;
                size_t space = m_capacity - m_size.load(std::memory_order_relaxed);
                if (toWrite > space) toWrite = space;
                if (toWrite == 0) break;

                for (size_t i = 0; i < toWrite; i++) {
                    m_buffer[(currentHead + i) & m_mask] = data[written + i];
                }
                m_head.store((currentHead + toWrite) & m_mask, std::memory_order_release);
                m_size.fetch_add(toWrite, std::memory_order_release);
                written += toWrite;
            } else {
                // 非空，直接写入
                size_t toWrite = count - written;
                size_t space = m_capacity - m_size.load(std::memory_order_relaxed);
                if (toWrite > space) toWrite = space;
                if (toWrite == 0) break;

                for (size_t i = 0; i < toWrite; i++) {
                    m_buffer[(currentHead + i) & m_mask] = data[written + i];
                }
                m_head.store((currentHead + toWrite) & m_mask, std::memory_order_release);
                m_size.fetch_add(toWrite, std::memory_order_release);
                written += toWrite;
            }
        }
        return written;
    }

    // 读取数据（消费者线程调用）
    // 返回实际读取的元素数
    size_t Read(T* data, size_t count) {
        size_t currentTail = m_tail.load(std::memory_order_acquire);
        size_t currentHead = m_head.load(std::memory_order_acquire);
        
        if (currentTail == currentHead) {
            return 0; // 缓冲区为空
        }

        size_t available = m_size.load(std::memory_order_acquire);
        size_t toRead = count < available ? count : available;
        if (toRead == 0) return 0;

        for (size_t i = 0; i < toRead; i++) {
            data[i] = m_buffer[(currentTail + i) & m_mask];
        }
        m_tail.store((currentTail + toRead) & m_mask, std::memory_order_release);
        m_size.fetch_sub(toRead, std::memory_order_release);
        return toRead;
    }

    // 批量读取到 vector（零拷贝适配，减少一次拷贝）
    size_t ReadToVector(std::vector<T>& vec, size_t count) {
        size_t currentTail = m_tail.load(std::memory_order_acquire);
        size_t currentHead = m_head.load(std::memory_order_acquire);
        
        if (currentTail == currentHead) {
            return 0;
        }

        size_t available = m_size.load(std::memory_order_acquire);
        size_t toRead = count < available ? count : available;
        if (toRead == 0) return 0;

        vec.reserve(vec.size() + toRead);
        for (size_t i = 0; i < toRead; i++) {
            vec.push_back(m_buffer[(currentTail + i) & m_mask]);
        }
        m_tail.store((currentTail + toRead) & m_mask, std::memory_order_release);
        m_size.fetch_sub(toRead, std::memory_order_release);
        return toRead;
    }

    // 直接追加到目标指针（零拷贝，避免 vector 二次拷贝）
    size_t ReadToPointer(T* dest, size_t count) {
        size_t currentTail = m_tail.load(std::memory_order_acquire);
        size_t currentHead = m_head.load(std::memory_order_acquire);
        
        if (currentTail == currentHead) {
            return 0;
        }

        size_t available = m_size.load(std::memory_order_acquire);
        size_t toRead = count < available ? count : available;
        if (toRead == 0) return 0;

        for (size_t i = 0; i < toRead; i++) {
            dest[i] = m_buffer[(currentTail + i) & m_mask];
        }
        m_tail.store((currentTail + toRead) & m_mask, std::memory_order_release);
        m_size.fetch_sub(toRead, std::memory_order_release);
        return toRead;
    }

    // 查看当前元素数（非精确，用于调试）
    size_t Size() const {
        return m_size.load(std::memory_order_acquire);
    }

    // 清空缓冲区（消费者线程调用）
    void Clear() {
        m_tail.store(m_head.load(std::memory_order_acquire), std::memory_order_release);
        m_size.store(0, std::memory_order_release);
    }

    // 获取容量
    size_t Capacity() const { return m_capacity; }

private:
    size_t m_capacity;
    size_t m_mask;
    std::unique_ptr<T[]> m_buffer;
    std::atomic<size_t> m_head;   // 生产者写入位置
    std::atomic<size_t> m_tail;   // 消费者读取位置
    std::atomic<size_t> m_size;   // 当前元素数
};
