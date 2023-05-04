#pragma once

#include <glog/logging.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace theta {

class QueueOpts {
 public:
  size_t max_size() const { return max_size_; }
  QueueOpts& set_max_size(size_t val) {
    max_size_ = val;
    return *this;
  }

 private:
  size_t max_size_{512};
};

template <typename T>
auto constexpr is_atomic = false;

template <typename T>
auto constexpr is_atomic<std::atomic<T>> = std::atomic<T>::is_always_lock_free;

template <typename T>
auto constexpr can_be_atomic = is_atomic<std::atomic<T>>;

template <typename T>
static constexpr bool memset0_to_bool() {
  T t;
  memset(&t, 0, sizeof(T));
  return static_cast<bool>(t);
}

template <typename T>
concept ZeroableAtomType = requires(T t) {
  can_be_atomic<T>;
  static_cast<bool>(T{}) == false;
  memset0_to_bool<T>() == false;
};

template <ZeroableAtomType T>
class Queue {
 public:
  struct Iterator {
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = T;
    using pointer = value_type*;
    using reference = value_type&;

    Iterator(Queue<T>* queue, size_t index) : queue_(queue), index_(index) {}

    Iterator(const Iterator&) = delete;

    value_type operator*() const {
      value_type v = queue_->buf_[index_].load(std::memory_order::relaxed);
      while (true) {
        if (v) {
          return v;
        }
        v = queue_->buf_[index_].load(std::memory_order::acquire);
      }
    }

    pointer operator->() {
      value_type v = queue_->buf_[index_].load(std::memory_order::relaxed);
      while (true) {
        if (v) {
          return &queue_->buf_[index_].load(std::memory_order::relaxed);
        }
        v = queue_->buf_[index_].load(std::memory_order::acquire);
      }
    }

    // Prefix increment
    Iterator& operator++() {
      index_++;
      if (index_ >= queue_->buf_.size()) {
        index_ = 0;
      }
      return *this;
    }

    friend bool operator==(const Iterator& a, const Iterator& b) {
      return a.queue_ == b.queue_ && a.index_ == b.index_;
    };

    friend bool operator!=(const Iterator& a, const Iterator& b) {
      return !(a == b);
    };

   private:
    Queue<T>* const queue_{nullptr};
    size_t start_index_{0};
    size_t num_reserved_{0};
    size_t index_{0};
  };

  struct Flusher {
    Flusher(Queue<T>* queue) : queue_(queue) {
      start_index_ = queue_->reserve_all_for_read(&num_reserved_);
    }

    ~Flusher() {
      const size_t buf_size = queue_->buf_.size();

      size_t to_clear = std::min(buf_size - start_index_, num_reserved_);
      memset(&queue_->buf_[start_index_], 0, to_clear * sizeof(T));
      if (to_clear < num_reserved_) {
        memset(&queue_->buf_[0], 0, (num_reserved_ - to_clear) * sizeof(T));
      }

      std::atomic_thread_fence(std::memory_order_release);
    }

    Iterator begin() const { return Iterator(queue_, start_index_); }

    Iterator end() const {
      size_t index = start_index_ + num_reserved_;
      if (index >= queue_->buf_.size()) {
        index -= queue_->buf_.size();
      }
      return Iterator(queue_, index);
    }

   private:
    Queue<T>* const queue_;
    size_t start_index_{0};
    size_t num_reserved_{0};
  };

  static constexpr size_t next_pow_2(int v) {
    // TODO(lpe): This fixes a bug shown in one of the unittests, but it also
    // seems to expose odd behavior in some queue when it's left out. Uncomment
    // these lines.
    //if ((v & (v - 1)) == 0) {
    //  return v;
    //}
    int lg_v = 8 * sizeof(v) - __builtin_clz(v);
    return 1 << lg_v;
  }

  Queue(QueueOpts opts)
      : ht_(/*head=*/0, /*tail=*/0), buf_(next_pow_2(opts.max_size())) {}

  ~Queue() {
    while (true) {
      auto v = pop_front();
      if (!v) {
        break;
      }
    }
  }

  bool push_back(T val) {
    return push_back(val, nullptr);
  }

  bool push_back(T val, size_t* num_items) {
    DCHECK(val);
    uint64_t expected = ht_.line.load(std::memory_order::relaxed);
    uint32_t head, tail;
    size_t s = size(expected, buf_.size());
    do {
      if (s == capacity()) {
        if (num_items) {
          *num_items = s;
        }
        return false;
      } else if (num_items) {
        *num_items = s + 1;
      }

      head = HeadTail{expected}.head;
      tail = HeadTail{expected}.tail;

      if (tail == buf_.size() - 1) {
        tail = 0;
      } else {
        tail++;
      }
    } while (!ht_.line.compare_exchange_weak(
        expected, HeadTail{head, tail}.line.load(std::memory_order::relaxed),
        std::memory_order::release, std::memory_order::relaxed));

    uint32_t index = HeadTail{expected}.tail;

    // It is possible that a pop operation has claimed this index but hasn't
    // yet performed its read.
    while (true) {
      T expect_zero{};
      if (buf_[index].compare_exchange_weak(expect_zero, val,
                                            std::memory_order::release,
                                            std::memory_order::relaxed)) {
        break;
      }
    }

    return true;
  }

  T pop_front() {
    auto maybe_index = reserve_for_read(/*num_items=*/1);
    if (!maybe_index.has_value()) {
      return T{};
    }
    auto index = maybe_index.value();

    T t{};
    // It's possible that a push operation has obtained this index but hasn't
    // yet written its value which will cause us to spin.
    do {
      t = buf_[index].exchange(t, std::memory_order::acq_rel);
    } while (!t);

    return t;
  }

  size_t size() const {
    return size(ht_.line.load(std::memory_order::acquire), buf_.size());
  }

  size_t capacity() const { return buf_.size() - 1; }

  Flusher flusher() { return Flusher(this); }

 private:
  // TODO(lpe): It's possible to make this structure naturally fall back to a
  // traditional threadqueue, thereby removing the size limit. This would
  // require 5 index values:
  // head: Index of the next value to pop
  // tail: Index where the next value should push
  // split: If active, a point that splits the queue in half. If this happens,
  //        the head index will always live in one half while the tail index will
  //        always live in the other.
  // fallback_tail: If in fallback mode, this index operates in the same half as the
  //                head. It indicates where the next value popped from the
  //                fallback queue should be placed.
  // fallback_head: If in fallback mode, this indicates where the next value to
  //                be placed in the fallback queue is located.
  //
  // This system requires all 5 values to be read atomically, so the fast-queue size
  // will be limitted to 4096 values (12 bits). This leaves 4 bits, one of which can
  // indicate whether the fallback mode is active.
  //
  // While in fallback mode, creating a Flusher object will need to refill the pop half
  // of the queue. If a push or a pop operation reaches the fallback head/tail,
  // then that operation will need to block while it offloads values into the
  // fallback queue or pulls more values from the fallback queue.
  //
  // Moving from fast-mode to fallback-mode will involve taking the mutex in
  // the push path, declaring the split point, and then moving the elements in
  // the push half into the fallback-queue.
  //
  // Moving from fallback-mode to fast-mode will again involve taking the mutex, but then
  // the two halves of the fast-queue will need to be stitched together by
  // moving elements one half to the other half.
  //
  // It's not clear how thread-friendly the transition from fallback- to fast-mode can be.
  // At the worst, it will be possible to set a bit that flags all operations to block.

  union HeadTail {
    struct {
      uint32_t head;
      uint32_t tail;
    };
    std::atomic<uint64_t> line;

    HeadTail(uint64_t line_) : line(line_) {}
    HeadTail(uint32_t head_, uint32_t tail_) : head(head_), tail(tail_) {}
  } ht_;

  std::vector<std::atomic<T>> buf_;

  static inline constexpr size_t size(uint64_t line, size_t buf_size) {
    uint32_t head = HeadTail(line).head;
    uint32_t tail = HeadTail(line).tail;
    if (tail < head) {
      tail += buf_size;
    }
    return tail - head;
  }

  uint32_t reserve_all_for_read(size_t* num_items) {
    while (true) {
      uint64_t line = ht_.line.load(std::memory_order::relaxed);
      size_t s = size(line, buf_.size());
      auto reserved = reserve_for_read(s, line);
      if (reserved) {
        *num_items = s;
        return reserved.value();
      }
    }
  }

  // If num_items are available, returns the starting index where items my be
  // read. The caller must clear each of these items.
  std::optional<uint32_t> reserve_for_read(size_t num_items) {
    return reserve_for_read(num_items,
                            ht_.line.load(std::memory_order::relaxed));
  }

  std::optional<uint32_t> reserve_for_read(size_t num_items, uint64_t line) {
    uint64_t expected = line;
    uint32_t head, tail;
    do {
      if (size(expected, buf_.size()) < static_cast<size_t>(num_items)) {
        return {};
      }

      head = HeadTail(expected).head;
      tail = HeadTail(expected).tail;

      head += num_items;
      if (head >= buf_.size()) {
        head -= buf_.size();
      }
    } while (!ht_.line.compare_exchange_weak(
        expected,
        HeadTail(head, tail).line.load(
            std::memory_order::relaxed),
        std::memory_order::release, std::memory_order::relaxed));

    return HeadTail(expected).head;
  }
};

}  // namespace theta
