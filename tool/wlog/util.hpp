/**
 * @file
 * @brief Utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2012 Cybozu Labs, Inc.
 */
#ifndef UTIL_HPP
#define UTIL_HPP

#include <cassert>
#include <stdexcept>
#include <sstream>
#include <cstdarg>
#include <algorithm>
#include <vector>
#include <queue>
#include <mutex>
#include <cstring>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>


#define RT_ERR(fmt, args...)                        \
    std::runtime_error(formatString(fmt, ##args))

namespace walb {
namespace util {

/**
 * Create a std::string using printf() like formatting.
 */
std::string formatString(const char * format, ...)
{
    char *p = nullptr;

    va_list args;
    va_start(args, format);
    int ret = ::vasprintf(&p, format, args);
    va_end(args);
    if (ret < 0) {
        ::free(p);
        throw std::runtime_error("vasprintf failed.");
    }
    std::string st(p, ret);
    ::free(p);
    return st;
}

/**
 * formatString() test.
 */
void testFormatString()
{
    {
        std::string st(formatString("%s%c%s", "012", (char)0, "345"));
        for (size_t i = 0; i < st.size(); i++) {
            printf("%0x ", st[i]);
        }
        printf("\n size %zu\n", st.size());
        assert(st.size() == 7);
    }

    {
        std::string st(formatString(""));
        ::printf("%s %zu\n", st.c_str(), st.size());
    }

    {
        try {
            std::string st(formatString(nullptr));
            assert(false);
        } catch (std::runtime_error& e) {
        }
    }

    {
        std::string st(formatString("%s%s", "0123456789", "0123456789"));
        ::printf("%s %zu\n", st.c_str(), st.size());
        assert(st.size() == 20);
    }
}

#if 0
/**
 * Each IO log.
 */
struct IoLog
{
    const unsigned int threadId;
    const IoType type;
    const size_t blockId;
    const double startTime; /* unix time [second] */
    const double response; /* [second] */

    IoLog(unsigned int threadId_, IoType type_, size_t blockId_,
          double startTime_, double response_)
        : threadId(threadId_)
        , type(type_)
        , blockId(blockId_)
        , startTime(startTime_)
        , response(response_) {}

    IoLog(const IoLog& log)
        : threadId(log.threadId)
        , type(log.type)
        , blockId(log.blockId)
        , startTime(log.startTime)
        , response(log.response) {}

    void print() {
        ::printf("threadId %d type %d blockId %10zu startTime %.06f response %.06f\n",
                 threadId, (int)type, blockId, startTime, response);
    }
};
#endif

static inline double getTime()
{
    struct timeval tv;
    double t;

    ::gettimeofday(&tv, NULL);

    t = static_cast<double>(tv.tv_sec) +
        static_cast<double>(tv.tv_usec) / 1000000.0;
    return t;
}

class BlockDevice
{
private:
    std::string name_;
    int openFlags_;
    int fd_;
    size_t deviceSize_;

    std::once_flag close_flag_;

public:
    BlockDevice(const std::string& name, int flags)
        : name_(name)
        , openFlags_(flags)
        , fd_(openDevice(name, flags))
        , deviceSize_(getDeviceSizeFirst(fd_)) {
#if 0
        ::printf("device %s size %zu isWrite %d isDirect %d\n",
                 name_.c_str(), deviceSize_,
                 openFlags_ & O_RDWR != 0, openFlags_ & O_DIRECT != 0);
#endif
    }
    explicit BlockDevice(BlockDevice&& rhs)
        : name_(std::move(rhs.name_))
        , openFlags_(rhs.openFlags_)
        , fd_(rhs.fd_)
        , deviceSize_(rhs.deviceSize_) {

        rhs.fd_ = -1;
    }
    BlockDevice& operator=(BlockDevice&& rhs) {

        name_ = std::move(rhs.name_);
        openFlags_ = rhs.openFlags_;
        fd_ = rhs.fd_; rhs.fd_ = -1;
        deviceSize_= rhs.deviceSize_;
        return *this;
    }

    ~BlockDevice() {
        close();
    }

    void close() {
        std::call_once(close_flag_, [&]() {
                if (fd_ > 0) {
                    if (::close(fd_) < 0) {
                        ::fprintf(::stderr, "close() failed.\n");
                    }
                    fd_ = -1;
                }
            });
    }

    /**
     * Get device size [byte].
     */
    size_t getDeviceSize() const {

        return deviceSize_;
    }

    class EofError : public std::exception {};

    /**
     * Read data and fill a buffer.
     */
    void read(off_t oft, size_t size, char* buf) {

        if (deviceSize_ < oft + size) { throw EofError(); }
        ::lseek(fd_, oft, SEEK_SET);
        size_t s = 0;
        while (s < size) {
            ssize_t ret = ::read(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw RT_ERR("read failed: %s.", ::strerror(errno));
            }
            s += ret;
        }
    }

    /**
     * Write data of a buffer.
     */
    void write(off_t oft, size_t size, char* buf) {

        if (deviceSize_ < oft + size) { throw EofError(); }
        ::lseek(fd_, oft, SEEK_SET);
        size_t s = 0;
        while (s < size) {
            ssize_t ret = ::write(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw RT_ERR("write failed: %s.", ::strerror(errno));
            }
            s += ret;
        }
    }

    /**
     * fdatasync.
     */
    void fdatasync() {

        int ret = ::fdatasync(fd_);
        if (ret) {
            throw RT_ERR("fdsync failed: %s.", ::strerror(errno));
        }
    }

    /**
     * fsync.
     */
    void fsync() {

        int ret = ::fsync(fd_);
        if (ret) {
            throw RT_ERR("fsync failed: %s.", ::strerror(errno));
        }
    }

    int getFlags() const { return openFlags_; }
    int getFd() const { return fd_; }

    /**
     * Get physical block device.
     */
    unsigned int getPhysicalBlockSize() const {

        unsigned int pbs;

        if (::ioctl(fd_, BLKPBSZGET, &pbs) < 0) {
            throw RT_ERR("Getting physical block size failed.");
        }
        assert(pbs > 0);

        return pbs;
    }

private:

    /**
     * Helper function for constructor.
     */
    static int openDevice(const std::string& name, int flags) {

        /* Open */
        int fd = ::open(name.c_str(), flags);
        if (fd < 0) {
            throw RT_ERR("open %s failed: %s.",
                         name.c_str(), ::strerror(errno));
        }

        /* Check the file is the block device. */
        struct stat sb;
        if (::stat(name.c_str(), &sb) < 0) {
            throw RT_ERR("stat failed: %s.", ::strerror(errno));
        }

        if ((sb.st_mode & S_IFMT) != S_IFBLK) {
            throw RT_ERR("%s is not a block device.", name.c_str());
        }

        return fd;
    }

    /**
     * Helper function for constructor.
     * Get device size in bytes.
     */
    static size_t getDeviceSizeFirst(int fd) {

        size_t ret;
        struct stat s;
        if (::fstat(fd, &s) < 0) {
            throw RT_ERR("fstat failed: %s.", ::strerror(errno));
        }
        if ((s.st_mode & S_IFMT) == S_IFBLK) {
            size_t size;
            if (::ioctl(fd, BLKGETSIZE64, &size) < 0) {
                throw RT_ERR("ioctl failed: %s.", ::strerror(errno));
            }
            ret = size;
        } else {
            ret = s.st_size;
        }
#if 0
        std::cout << "devicesize: " << ret << std::endl; //debug
#endif
        return ret;
    }
};

/**
 * Calculate access range.
 */
static inline size_t calcAccessRange(
    size_t accessRange, size_t blockSize, const BlockDevice& dev) {

    return (accessRange == 0) ? (dev.getDeviceSize() / blockSize) : accessRange;
}


class PerformanceStatistics
{
private:
    double total_;
    double max_;
    double min_;
    size_t count_;

public:
    PerformanceStatistics()
        : total_(0), max_(-1.0), min_(-1.0), count_(0) {}
    PerformanceStatistics(double total, double max, double min, size_t count)
        : total_(total), max_(max), min_(min), count_(count) {}

    void updateRt(double rt) {

        if (max_ < 0 || min_ < 0) {
            max_ = rt; min_ = rt;
        } else if (max_ < rt) {
            max_ = rt;
        } else if (min_ > rt) {
            min_ = rt;
        }
        total_ += rt;
        count_++;
    }

    double getMax() const { return max_; }
    double getMin() const { return min_; }
    double getTotal() const { return total_; }
    size_t getCount() const { return count_; }

    double getAverage() const { return total_ / (double)count_; }

    void print() const {
        ::printf("total %.06f count %zu avg %.06f max %.06f min %.06f\n",
                 getTotal(), getCount(), getAverage(),
                 getMax(), getMin());
    }
};

template<typename T> //T is iterator type of PerformanceStatistics.
static inline PerformanceStatistics mergeStats(const T begin, const T end)
{
    double total = 0;
    double max = -1.0;
    double min = -1.0;
    size_t count = 0;

    std::for_each(begin, end, [&](PerformanceStatistics& stat) {

            total += stat.getTotal();
            if (max < 0 || max < stat.getMax()) { max = stat.getMax(); }
            if (min < 0 || min > stat.getMin()) { min = stat.getMin(); }
            count += stat.getCount();
        });

    return PerformanceStatistics(total, max, min, count);
}

/**
 * Convert throughput data to string.
 */
static inline
std::string getDataThroughputString(double throughput)
{
    const double GIGA = static_cast<double>(1000ULL * 1000ULL * 1000ULL);
    const double MEGA = static_cast<double>(1000ULL * 1000ULL);
    const double KILO = static_cast<double>(1000ULL);

    std::stringstream ss;
    if (throughput > GIGA) {
        throughput /= GIGA;
        ss << throughput << " GB/sec";
    } else if (throughput > MEGA) {
        throughput /= MEGA;
        ss << throughput << " MB/sec";
    } else if (throughput > KILO) {
        throughput /= KILO;
        ss << throughput << " KB/sec";
    } else {
        ss << throughput << " B/sec";
    }

    return ss.str();
}

/**
 * Print throughput data.
 * @blockSize block size [bytes].
 * @nio Number of IO executed.
 * @periodInSec Elapsed time [second].
 */
static inline
void printThroughput(size_t blockSize, size_t nio, double periodInSec)
{
    double throughput = static_cast<double>(blockSize * nio) / periodInSec;
    double iops = static_cast<double>(nio) / periodInSec;
    ::printf("Throughput: %.3f B/s %s %.3f iops.\n",
             throughput, getDataThroughputString(throughput).c_str(), iops);
}

/**
 * Ring buffer for block data.
 */
class BlockBuffer
{
private:
    const size_t nr_;
    std::vector<char *> bufArray_;
    size_t idx_;

public:
    BlockBuffer(size_t nr, size_t blockSize)
        : nr_(nr)
        , bufArray_(nr)
        , idx_(0) {

        assert(blockSize % 512 == 0);
        for (size_t i = 0; i < nr; i++) {
            char *p = nullptr;
            int ret = ::posix_memalign((void **)&p, 512, blockSize);
            assert(ret == 0);
            assert(p != nullptr);
            bufArray_[i] = p;
        }
    }

    ~BlockBuffer() {

        for (size_t i = 0; i < nr_; i++) {
            ::free(bufArray_[i]);
        }
    }

    char* next() {

        char *ret = bufArray_[idx_];
        idx_ = (idx_ + 1) % nr_;
        return ret;
    }
};

} //namespace util
} //namespace walb

#endif /* UTIL_HPP */
