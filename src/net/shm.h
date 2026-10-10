// shm.h — 跨平台命名共享内存段（用于「多进程分片」demo 的 router↔shard 通道）
//
// 为什么用「命名」共享内存：
//   本 demo 由一个 router 进程 fork/exec 出若干 shard 子进程。子进程需要访问父进程
//   创建的同一块内存。用**命名**段，子进程只需按名字 open 即可，不依赖句柄继承
//   （fork 后子进程立即 exec，继承的 fd 也没意义）。
//
// 为什么不用 socket 做 router↔shard：
//   共享内存 + 无锁 SPSC 环是「零拷贝、零系统调用」的进程间通道，最能体现
//   「多进程分片」相比「多进程 + 网络转发」的工程量差异——这也是本项目选它的原因。
//
// 平台：
//   POSIX  : shm_open + ftruncate + mmap  （段落在 /dev/shm/<name>）
//   Windows: CreateFileMapping + MapViewOfFile（页文件后备，按名字跨进程打开）
//
// 复用并发.h 里的 SpmcRing<T,N>（单生产者单消费者无锁环）作为通道内核——
// 每个 shard 两条环：to_shard（router 生产 / shard 消费）、from_shard（shard 生产 /
// router 消费）。每条环都只有唯一生产者与唯一消费者，故 SPSC 不变式天然成立，
// 跨进程原子量落在同一物理页上，无需任何锁。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace synq {
namespace net {

class Shm {
 public:
  Shm() : ptr_(nullptr), size_(0) {}
  ~Shm() { close(); }

  // 移动语义：把所有权（句柄 + 映射视图）转移给目标，源置空，使其析构变为 no-op。
  // 【为什么必须手写】早期用 vector<Shm>::push_back(std::move(shm)) 触发隐式按成员移动
  // （标量按位拷贝），原对象析构时 close() 把「同一份映射」释放掉，导致 vector
  // 里那个元素指向已解映射/已关闭的段 —— 子进程 OpenFileMapping 失败、router 写已解映射
  // 地址段错误。手写 move 后，源被置空，析构安全。
  Shm(Shm&& o) noexcept : ptr_(o.ptr_), size_(o.size_) {
#ifdef _WIN32
    handle_ = o.handle_;
#else
    fd_ = o.fd_;
#endif
    o.ptr_ = nullptr;
    o.size_ = 0;
#ifdef _WIN32
    o.handle_ = nullptr;
#else
    o.fd_ = -1;
#endif
  }
  Shm& operator=(Shm&& o) noexcept {
    if (this != &o) {
      close();
      ptr_ = o.ptr_;
      size_ = o.size_;
#ifdef _WIN32
      handle_ = o.handle_;
#else
      fd_ = o.fd_;
#endif
      o.ptr_ = nullptr;
      o.size_ = 0;
#ifdef _WIN32
      o.handle_ = nullptr;
#else
      o.fd_ = -1;
#endif
    }
    return *this;
  }
  Shm(const Shm&) = delete;
  Shm& operator=(const Shm&) = delete;

  // 创建并清零（router 侧）。size 任意字节数。
  bool create(const std::string& name, std::size_t size) {
#ifdef _WIN32
    handle_ = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   static_cast<DWORD>(size >> 32),
                                   static_cast<DWORD>(size & 0xFFFFFFFFu),
                                   name.c_str());
    if (handle_ == nullptr) return false;
    ptr_ = ::MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    fd_ = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) return false;
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    ptr_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr_ == MAP_FAILED) {
      ::close(fd_);
      fd_ = -1;
      ptr_ = nullptr;
      return false;
    }
#endif
    if (ptr_ == nullptr) return false;
    size_ = size;
    std::memset(ptr_, 0, size);  // 段内原子量/环显式清零（POSIX 通常已零，双保险）
    return true;
  }

  // 打开已存在的段（shard 侧）。size 必须与 create 时一致。
  bool open(const std::string& name, std::size_t size) {
#ifdef _WIN32
    handle_ = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (handle_ == nullptr) return false;
    ptr_ = ::MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    fd_ = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd_ < 0) return false;
    ptr_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr_ == MAP_FAILED) {
      ::close(fd_);
      fd_ = -1;
      ptr_ = nullptr;
      return false;
    }
#endif
    if (ptr_ == nullptr) return false;
    size_ = size;
    return true;
  }

  void* ptr() const { return ptr_; }
  std::size_t size() const { return size_; }

  void close() {
    if (ptr_ != nullptr) {
#ifdef _WIN32
      ::UnmapViewOfFile(ptr_);
#else
      ::munmap(ptr_, size_);
#endif
      ptr_ = nullptr;
    }
#ifdef _WIN32
    if (handle_ != nullptr) {
      ::CloseHandle(handle_);
      handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

  // 仅 creator 在退出前调用：删掉名字（Linux 下会删除 /dev/shm 文件）。
  // Windows 的命名段在最后一个视图关闭后由系统回收，无需 unlink。
  void unlink(const std::string& name) {
#ifdef _WIN32
    (void)name;
#else
    ::shm_unlink(name.c_str());
#endif
  }

 private:
#ifdef _WIN32
  HANDLE handle_ = nullptr;
#else
  int fd_ = -1;
#endif
  void* ptr_;
  std::size_t size_;
};

}  // namespace net
}  // namespace synq
