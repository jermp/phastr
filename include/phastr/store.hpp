#pragma once

/*
    The records of the keys of a level, partitioned by the upper bits of
    their signature for the level.

    Keys are read once: for each key we store a *record* containing its
    signature for the first level and its second hash, from which we can
    compute its signature for every level. The records of the keys bumped
    from a level are stored for the following level. Records are kept in
    memory or, in external-memory mode, in temporary files.

    On disk, the records of a partition are a sequence of blocks appended to
    one of at most 2^LOG2_FILES temporary files, each containing the blocks
    of a range of consecutive partitions; a table locates the blocks of each
    partition. The length of the blocks grows with the number of records
    written (see GROWTH), so that reading a partition requires few seeks,
    whereas the memory used by the buffers of a writer stays bounded.
*/

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "util.hpp"
#include "geometry.hpp"

namespace phastr {

/* The signature of a key for the first level and its second hash. */
struct record {
    uint64_t h = 0;
    uint64_t second = 0;

    bool operator<(record const& other) const {
        return h != other.h ? h < other.h : second < other.second;
    }
    bool operator==(record const& other) const {
        return h == other.h && second == other.second;
    }
};

/* The base-2 logarithm of the maximum number of partitions of a store,
   which is the number of partitions of the first level (whose number of
   keys is not known in advance). */
static constexpr uint32_t MAX_LOG2_PARTITIONS = 14;

/* The initial number of records of the blocks of a writer. */
static constexpr size_t BLOCK = 1024;

/* The maximum number of records of the blocks of a writer. */
static constexpr size_t MAX_BLOCK = 1 << 16;

/* A writer doubles the size of its blocks (up to MAX_BLOCK records) when it
   has written on average this number of blocks for each partition. */
static constexpr size_t GROWTH = 512;

/* The base-2 logarithm of the maximum number of temporary files of a store
   kept on disk. */
static constexpr uint32_t LOG2_FILES = 4;

/* Returns the base-2 logarithm of the number of partitions of the store of
   a level after the first one with k keys: the total memory of the writers
   and of the sweep buffers is minimized when the number of partitions is
   about sqrt(2k / BLOCK). */
static inline uint32_t log2_partitions_for(uint64_t k) {
    const uint64_t p = static_cast<uint64_t>(
        std::ceil(std::sqrt(2.0 * static_cast<double>(k) / static_cast<double>(BLOCK))));
    return std::min<uint32_t>(ilog2(next_power_of_two(p)), MAX_LOG2_PARTITIONS);
}

/* The signature of a record for a level: its first component for the first
   level, and level_sig with the salt of the level for the following
   ones. */
struct sig_fn {
    bool first_level = true;
    uint64_t salt = 0;

    static sig_fn level0() {
        return sig_fn{true, 0};
    }
    static sig_fn salted(uint64_t salt) {
        return sig_fn{false, salt};
    }

    inline uint64_t sig(record const& r) const {
        return first_level ? r.h : level_sig(r.h, r.second, salt);
    }
};

/* Returns the partition of a signature, given the base-2 logarithm of the
   number of partitions (two shifts, so that a single partition needs no
   special case). */
static inline size_t partition_of(uint64_t sig, uint32_t log2_partitions) {
    return static_cast<size_t>(sig >> 1 >> (63 - log2_partitions));
}

/* An anonymous temporary file in `dir`, opened for reading and writing,
   unlinked at creation. */
struct temp_file {
    int fd = -1;
    uint64_t len = 0; /* length in records */

    temp_file() = default;
    temp_file(temp_file const&) = delete;
    temp_file& operator=(temp_file const&) = delete;
    temp_file(temp_file&& other) noexcept : fd(other.fd), len(other.len) {
        other.fd = -1;
    }
    temp_file& operator=(temp_file&& other) noexcept {
        if (this != &other) {
            close();
            fd = other.fd;
            len = other.len;
            other.fd = -1;
        }
        return *this;
    }
    ~temp_file() {
        close();
    }

    void open(std::string const& dir) {
        std::string templ = (dir.empty() ? std::string(".") : dir) + "/phastr.XXXXXX";
        std::vector<char> buf(templ.begin(), templ.end());
        buf.push_back('\0');
        fd = ::mkstemp(buf.data());
        if (fd < 0) {
            throw std::runtime_error("phastr: could not create a temporary file in '" +
                                     (dir.empty() ? std::string(".") : dir) + "'");
        }
        ::unlink(buf.data());
    }

    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    void append(record const* data, size_t n) {
        size_t bytes = n * sizeof(record);
        char const* p = reinterpret_cast<char const*>(data);
        while (bytes != 0) {
            ssize_t written = ::write(fd, p, bytes);
            if (written <= 0) throw std::runtime_error("phastr: could not write records");
            p += written;
            bytes -= static_cast<size_t>(written);
        }
        len += n;
    }

    void read_at(uint64_t pos, record* data, size_t n) const {
        size_t bytes = n * sizeof(record);
        char* p = reinterpret_cast<char*>(data);
        uint64_t offset = pos * sizeof(record);
        while (bytes != 0) {
            ssize_t got = ::pread(fd, p, bytes, static_cast<off_t>(offset));
            if (got <= 0) throw std::runtime_error("phastr: could not read records");
            p += got;
            offset += static_cast<uint64_t>(got);
            bytes -= static_cast<size_t>(got);
        }
    }
};

/* A block of a partition kept on disk: its position in the file of the
   partition and its length, both in records. */
struct block_ref {
    uint64_t pos = 0;
    uint32_t len = 0;
};

/* The records of the keys of a level, partitioned by the upper bits of
   their signature for the level. */
struct store {
    sig_fn sig;
    uint32_t log2_partitions = 0;
    uint64_t len = 0;
    bool on_disk = false;

    /* In-memory storage: a vector for each partition. */
    std::vector<std::vector<record>> partitions;

    /* On-disk storage: temporary files and the table of the blocks. The
       file of a partition is its index shifted right by file_shift. */
    std::vector<temp_file> files;
    std::vector<std::vector<block_ref>> blocks;
    uint32_t file_shift = 0;

    size_t num_partitions() const {
        return size_t(1) << log2_partitions;
    }

    size_t partition_len(size_t p) const {
        if (!on_disk) return partitions[p].size();
        size_t n = 0;
        for (auto const& b : blocks[p]) n += b.len;
        return n;
    }

    /* Calls f on the records of partition p, using buf as a buffer. */
    template <typename F>
    void for_each(size_t p, std::vector<record>& buf, F const& f) const {
        if (!on_disk) {
            for (auto const& r : partitions[p]) f(r);
            return;
        }
        for (auto const& b : blocks[p]) {
            buf.resize(b.len);
            files[p >> file_shift].read_at(b.pos, buf.data(), b.len);
            for (auto const& r : buf) f(r);
        }
    }
};

/* Writes records to a new store: several threads can write at the same
   time, each using its own store_buffers. */
struct store_writer {
    store st;
    std::mutex mutex;
    /* The expected number of records, if known (0 otherwise): in memory,
       partitions reserve their expected size, so that they do not grow. */
    uint64_t expected = 0;

    /* Creates a writer for a store with the given signature function and
       number of partitions, kept in anonymous temporary files in `dir` if
       on_disk is true, and in memory otherwise. */
    store_writer(sig_fn sig, uint32_t log2_partitions, bool on_disk, std::string const& dir,
                 uint64_t expected_records = 0)
        : expected(expected_records) {
        st.sig = sig;
        st.log2_partitions = log2_partitions;
        st.on_disk = on_disk;
        const size_t np = size_t(1) << log2_partitions;
        if (on_disk) {
            const uint32_t log2_files = std::min(log2_partitions, LOG2_FILES);
            st.files.resize(size_t(1) << log2_files);
            for (auto& f : st.files) f.open(dir);
            st.blocks.assign(np, {});
            st.file_shift = log2_partitions - log2_files;
        } else {
            st.partitions.assign(np, {});
        }
    }

    /* Returns the store, given the number of records written to it. */
    store finish(uint64_t len) {
        st.len = len;
        if (!st.on_disk) {
            for (auto& p : st.partitions) p.shrink_to_fit();
        }
        return std::move(st);
    }
};

/* Returns whether a writer that has written `len` records, using blocks of
   `block` records for each of `partitions` partitions, has written on
   average at least GROWTH blocks for each partition. */
static inline bool store_grows(uint64_t len, size_t partitions, size_t block) {
    return len / (static_cast<uint64_t>(partitions) * block) >= GROWTH;
}

/* The buffers of a thread writing to a store_writer: on disk, a block for
   each partition, which is appended to its file when full; otherwise, the
   records of each partition, which are moved to the store at the end. */
struct store_buffers {
    store_writer* writer = nullptr;
    bool on_disk = false;
    /* On disk, a block for each partition, and the number of records in
       each block. */
    std::vector<record> block_storage;
    std::vector<uint32_t> lens;
    size_t block = BLOCK;
    /* Otherwise, the records of each partition. */
    std::vector<std::vector<record>> parts;
    /* The number of records pushed. */
    uint64_t len = 0;

    explicit store_buffers(store_writer& w) : writer(&w), on_disk(w.st.on_disk) {
        const size_t np = w.st.num_partitions();
        if (on_disk) {
            block_storage.assign(np * BLOCK, record{});
            lens.assign(np, 0);
        } else {
            parts.assign(np, {});
            if (w.expected != 0) {
                /* The expected number of records of a partition, plus six
                   standard deviations of slack. */
                const double mean = static_cast<double>(w.expected) / static_cast<double>(np);
                const size_t capacity =
                    static_cast<size_t>(std::ceil(mean + 6.0 * std::sqrt(mean)));
                for (auto& p : parts) p.reserve(capacity);
            }
        }
    }

    /* Adds a record to the store. */
    inline void push(record const& r) {
        const size_t p = partition_of(writer->st.sig.sig(r), writer->st.log2_partitions);
        len += 1;
        if (!on_disk) {
            parts[p].push_back(r);
            return;
        }
        uint32_t& l = lens[p];
        block_storage[p * block + l] = r;
        l += 1;
        if (l == block) {
            flush(p);
            if (block < MAX_BLOCK && store_grows(len, lens.size(), block)) grow();
        }
    }

    /* Appends the records in the block of partition p to its file. */
    void flush(size_t p) {
        const uint32_t l = lens[p];
        std::lock_guard<std::mutex> guard(writer->mutex);
        auto& st = writer->st;
        const size_t f = p >> st.file_shift;
        auto& file = st.files[f];
        const uint64_t pos = file.len;
        file.append(block_storage.data() + p * block, l);
        st.blocks[p].push_back({pos, l});
        lens[p] = 0;
    }

    /* Doubles the length of the blocks, after flushing the blocks that are
       not empty. */
    void grow() {
        for (size_t p = 0; p != lens.size(); ++p) {
            if (lens[p] != 0) flush(p);
        }
        block *= 2;
        block_storage.clear();
        block_storage.shrink_to_fit();
        block_storage.assign(lens.size() * block, record{});
    }

    /* Moves the remaining records to the store, and returns the number of
       records pushed. */
    uint64_t finish() {
        if (on_disk) {
            for (size_t p = 0; p != lens.size(); ++p) {
                if (lens[p] != 0) flush(p);
            }
        } else {
            std::lock_guard<std::mutex> guard(writer->mutex);
            auto& partitions = writer->st.partitions;
            for (size_t p = 0; p != parts.size(); ++p) {
                if (partitions[p].empty()) {
                    partitions[p] = std::move(parts[p]);
                } else {
                    partitions[p].insert(partitions[p].end(), parts[p].begin(), parts[p].end());
                }
            }
        }
        return len;
    }
};

}  // namespace phastr
