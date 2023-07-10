// MIT license (https://opensource.org/license/mit/)
// Initial Authors: Daniel Anderson and Guy Blelloch
// Developed as part of cmuparlay/parlaylib

// A concurrent allocator for blocks of a fixed size
// And for fixed types.  e.g.
//   using long_allocator = type_allocator<long>;
//   long* foo = long_allocator::New(23);
//   long_allocator::Delete(foo);
//
// Keeps a local pool per thread, and grabs list_length elements from a
// global pool if empty, and returns list_length elements to the global
// pool when local pool=2*list_length.
//
// Keeps track of number of allocated elements. Much more efficient
// than a general purpose allocator.
//
// Not generally intended for users. Users should use "type_allocator"
// which is a convenient wrapper around block_allocator that helps
// to manage memory for a specific type

#ifndef PARLAY_ALLOCATORS_TYPE_ALLOCATOR_H_
#define PARLAY_ALLOCATORS_TYPE_ALLOCATOR_H_

#include <cassert>
#include <cstddef>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <new>
#include <optional>

#include "hazptr_stack.h"

//#include "get_memory_size.h"

// IWYU pragma: no_include <vector>

namespace parlay {
namespace allocators {

struct block_allocator {
 private:

  static inline constexpr size_t default_list_bytes = (1 << 18) - 64;  // in bytes
  static inline constexpr size_t min_alignment = 128;  // for cache line padding

  struct block {
    block* next;
  };

  struct alignas(128) local_list {
    size_t sz;
    block* head;
    block* mid;
    local_list() : sz(0), head(nullptr), mid(nullptr) {};
  };

  const size_t max_thread_count;
  hazptr_stack<std::byte*> allocated_buffers;
  hazptr_stack<block*> global_stack;
  std::unique_ptr<local_list[]> local_lists;

  size_t block_size;
  std::align_val_t block_align;
  size_t list_length;
  size_t max_blocks;
  std::atomic<size_t> blocks_allocated;

  block* get_block(std::byte* buffer, size_t i) const {
    // Since block is an aggregate type, it has implicit lifetime, so the following code
    // is defined behaviour in C++20 even if we haven't yet called a constructor of block.
    // It is still UB prior to C++20, but this is hard to avoid without losing performance.
    return from_bytes<block>(buffer + i * block_size);
  }

 public:
  block_allocator(const block_allocator&) = delete;
  block_allocator(block_allocator&&) = delete;
  block_allocator& operator=(const block_allocator&) = delete;
  block_allocator& operator=(block_allocator&&) = delete;

  size_t get_block_size() const { return block_size; }
  size_t num_allocated_blocks() const { return blocks_allocated.load(); }

  // Allocate a new list of list_length elements

  auto initialize_list(std::byte* buffer) const -> block* {
    for (size_t i=0; i < list_length - 1; i++) 
      new (buffer + i * block_size) block{get_block(buffer, i+1)};
    new (buffer + (list_length - 1) * block_size) block{nullptr};
    return get_block(buffer, 0);
  }

  size_t num_used_blocks() {
    size_t free_blocks = global_stack.size()*list_length;
    for (size_t i = 0; i < max_thread_count; ++i)
      free_blocks += local_lists[i].sz;
    return blocks_allocated.load() - free_blocks;
  }

  auto allocate_blocks(size_t num_blocks) -> std::byte* {
    auto buffer = static_cast<std::byte*>(::operator new(num_blocks * block_size, block_align));
    assert(buffer != nullptr);

    blocks_allocated.fetch_add(num_blocks);
    assert(blocks_allocated.load() <= max_blocks);

    allocated_buffers.push(buffer); // keep track so can free later
    return buffer;
  }

  // Either grab a list from the global pool, or if there is none
  // then allocate a new list
  auto get_list() -> block* {
    std::optional<block*> rem = global_stack.pop();
    if (rem) return *rem;
    std::byte* buffer = allocate_blocks(list_length);
    return initialize_list(buffer);
  }

  // Currently a noop
  void reserve(size_t n) {
    // size_t num_lists = thread_count + (n + list_length - 1) / list_length;
    // std::byte* start = allocate_blocks(list_length*num_lists);
    // parallel_for(0, num_lists, [&] (size_t i) {
    //   std::byte* offset = start + i * list_length * block_size;
    //   global_stack.push(initialize_list(offset));
    // }, 1, true);
  }

  void print_stats() {
    size_t used = num_used_blocks();
    size_t allocated = num_allocated_blocks();
    size_t sz = get_block_size();
    std::cout << "Used: " << used << ", allocated: " << allocated
	      << ", block size: " << sz
	      << ", bytes: " << sz*allocated << std::endl;
  }

  explicit block_allocator(size_t block_size_,
    size_t block_align_ = alignof(std::max_align_t),
    size_t reserved_blocks = 0,
    size_t list_length_ = 0,
    size_t max_blocks_ = 0) :
    max_thread_count(thread_id.max_id),
    local_lists(std::make_unique<local_list[]>(max_thread_count)),                     // Each block needs to be at least
    block_size(std::max<size_t>(block_size_, sizeof(block))),    // <------------- // large enough to hold the struct
    block_align(std::align_val_t{std::max<size_t>(block_align_, min_alignment)}),  // representing a free block.
    list_length(list_length_ == 0 ? (default_list_bytes + block_size + 1) / block_size : list_length_),
    max_blocks(1000000000000ul / block_size), //max_blocks_ == 0 ? (3 * getMemorySize() / block_size) / 4 : max_blocks_),
    blocks_allocated(0) {

    reserve(reserved_blocks);
  }

  // Clears all memory ever allocated by this allocator. All allocated blocks
  // must be returned before calling this function.
  //
  // This operation is not safe to perform concurrently with any other operation
  //
  // Returns false if there exists blocks that haven't been returned, in which case
  // the operation fails and nothing is cleared. Returns true if no lingering
  // blocks were not freed, in which case the operation is successful
  bool clear() {
    if (num_used_blocks() > 0) {
      return false;
    }
    else {
      // clear lists
      for (size_t i = 0; i < max_thread_count; ++i)
        local_lists[i].sz = 0;

      // throw away all allocated memory
      std::optional<std::byte*> x;
      while ((x = allocated_buffers.pop())) ::operator delete(*x, block_align);
      global_stack.clear();
      blocks_allocated.store(0);
      return true;
    }
  }

  ~block_allocator() {
    clear();
  }

  void free(void* ptr) {
    size_t id = thread_id.get();

    if (local_lists[id].sz == list_length+1) {
      local_lists[id].mid = local_lists[id].head;
    } else if (local_lists[id].sz == 2*list_length) {
      global_stack.push(local_lists[id].mid->next);
      local_lists[id].mid->next = nullptr;
      local_lists[id].sz = list_length;
    }

    assert(id == thread_id.get());
    auto new_node = new (ptr) block{local_lists[id].head};
    local_lists[id].head = new_node;
    local_lists[id].sz++;
  }

  inline void* alloc() {
    size_t id = thread_id.get();

    if (local_lists[id].sz == 0)  {
      auto new_list = get_list();

      // !! Critical problem !! If this task got stolen during get_list(),
      // the worker id may have changed, so we can't assume we are looking
      // at the same local list, so we have to check the (possibly different)
      // local list of the (possibly changed) worker id
      id = thread_id.get();

      if (local_lists[id].sz == 0) {
        local_lists[id].head = new_list;
        local_lists[id].sz = list_length;
      }
      else {
        // Looks like the task got stolen and the new thread already had a
        // non-empty local list, so we can push the new one into the global
        // poo for someone else to use in the future
        global_stack.push(new_list);
      }
    }

    assert(id == thread_id.get());
    block* p = local_lists[id].head;
    local_lists[id].head = local_lists[id].head->next;
    local_lists[id].sz--;

    // Note: block is trivial, so it is legal to not call its destructor
    // before returning it and allowing its storage to be reused
    return static_cast<void*>(p);
  }

};

// A static allocator for allocating storage for single objects of a fixed
// type.   It is headerless and fast.
//
// Can be used to allocate raw uninitialized storage via alloc()/free(ptr),
// or to perform combined allocation and construction with create(args...)
// followed by destroy(ptr) to destroy and deallocate.
//
// alloc() -> T*         : returns uninitialized storage for an object of type T
// free(T*)              : deallocates storage obtained by alloc
//
// New(args...) -> T* : allocates storage for and constructs a T using args...
// Delete(T*)         : destructs and deallocates a T obtained from create(...)
//
// All members are static, so it is not required to create an instance of
// type_allocator<T> to use it.
//

template<size_t Size, size_t Align>
extern inline block_allocator& get_block_allocator() {
  static block_allocator a(Size, Align);
  return a;
}

template <typename T>
class type_allocator {

private:
  static block_allocator& get_allocator() {
    return get_block_allocator<sizeof(T), alignof(T)>();
  }

public:

  // Allocate uninitialized storage appropriate for storing an object of type T
  static T* alloc() {
    void* buffer = get_allocator().alloc();
    assert(reinterpret_cast<uintptr_t>(buffer) % alignof(T) == 0);
    return static_cast<T*>(buffer);
  }

  // Free storage obtained by alloc()
  static void free(T* ptr) {
    assert(ptr != nullptr);
    assert(reinterpret_cast<uintptr_t>(ptr) % alignof(T) == 0);
    get_allocator().free(static_cast<void*>(ptr));
  }

  // Allocate storage for and then construct an object of type T using args...
  template<typename... Args>
  static T* New(Args... args) {
    static_assert(std::is_constructible_v<T, Args...>);
    return new (alloc()) T(std::forward<Args>(args)...);
  }

  // Destroy an object obtained by create(...) and deallocate its storage
  static void Delete(T* ptr) {
    assert(ptr != nullptr);
    ptr->~T();
    free(ptr);
  }

  static void reserve(size_t n) { get_allocator().reserve(n); }
  static void finish() { get_allocator().clear(); }
  static size_t block_size () { return get_allocator().get_block_size(); }
  static size_t num_allocated_blocks() { return get_allocator().num_allocated_blocks(); }
  static size_t num_used_blocks() { return get_allocator().num_used_blocks(); }
  static size_t num_used_bytes() { return num_used_blocks() * block_size(); }
  static void print_stats() { get_allocator().print_stats(); }
};

  }  // namespace allocators
}  // namespace parlay

#endif  // PARLAY_ALLOCATORS_TYPE_ALLOCATOR_H_
