#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace itch {

// RAII read-only memory map of a file; the parser walks the mapped bytes
// directly, no heap copy on the hot path.
class MmapFile {
public:
    explicit MmapFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("open failed: " + path);

        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error("fstat failed: " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (p == MAP_FAILED) {
                ::close(fd_);
                throw std::runtime_error("mmap failed: " + path);
            }
            data_ = static_cast<const unsigned char*>(p);
            ::madvise(const_cast<void*>(p), size_, MADV_SEQUENTIAL);
        }
    }

    ~MmapFile() {
        if (data_) ::munmap(const_cast<unsigned char*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    const unsigned char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    int fd_{-1};
    const unsigned char* data_{nullptr};
    std::size_t size_{0};
};

}  // namespace itch
