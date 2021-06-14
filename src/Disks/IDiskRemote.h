#pragma once

#if !defined(ARCADIA_BUILD)
#include <Common/config.h>
#endif

#include <atomic>
#include "Disks/DiskFactory.h"
#include "Disks/Executor.h"
#include <utility>
#include <Common/MultiVersion.h>
#include <Common/ThreadPool.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

/// Helper class to collect paths into chunks of maximum size.
/// For diskS3 it is Aws::vector<ObjectIdentifier>, for diskHDFS it is std::vector<std::string>.
/// For diskWEBServer not implemented.
class RemoteFSPathKeeper
{
public:
    RemoteFSPathKeeper(size_t chunk_limit_) : chunk_limit(chunk_limit_) {}

    virtual ~RemoteFSPathKeeper() = default;

    virtual void addPath(const String & path) = 0;

protected:
    size_t chunk_limit;
};

using RemoteFSPathKeeperPtr = std::shared_ptr<RemoteFSPathKeeper>;


/// Base Disk class for remote FS's, which are not posix-compatible.
/// Used to implement disks over s3, hdfs, web-server.
class IDiskRemote : public IDisk
{
friend class DiskRemoteReservation;

public:
    IDiskRemote(
        const String & name_,
        const String & remote_fs_root_path_,
        const String & metadata_path_,
        const String & log_name_,
        size_t thread_pool_size);

    /// Methods to manage local metadata of remote FS objects.

    struct Metadata;

    Metadata readMeta(const String & path) const;

    Metadata createMeta(const String & path) const;

    Metadata readOrCreateMetaForWriting(const String & path, WriteMode mode);

    /// Disk info

    const String & getName() const final override { return name; }

    const String & getPath() const final override { return metadata_path; }

    UInt64 getTotalSpace() const final override { return std::numeric_limits<UInt64>::max(); }

    UInt64 getAvailableSpace() const final override { return std::numeric_limits<UInt64>::max(); }

    UInt64 getUnreservedSpace() const final override { return std::numeric_limits<UInt64>::max(); }

    /// Read-only part

    bool exists(const String & path) const override;

    bool isFile(const String & path) const override;

    size_t getFileSize(const String & path) const override;

    void listFiles(const String & path, std::vector<String> & file_names) override;

    void setReadOnly(const String & path) override;

    bool isDirectory(const String & path) const override;

    DiskDirectoryIteratorPtr iterateDirectory(const String & path) override;

    Poco::Timestamp getLastModified(const String & path) override;

    ReservationPtr reserve(UInt64 bytes) override;

    /// Write and modification part

    void moveFile(const String & from_path, const String & to_path) override;

    void replaceFile(const String & from_path, const String & to_path) override;

    void removeFile(const String & path) override { removeSharedFile(path, false); }

    void removeFileIfExists(const String & path) override;

    void removeRecursive(const String & path) override { removeSharedRecursive(path, false); }

    void removeSharedFile(const String & path, bool keep_in_remote_fs) override;

    void removeSharedRecursive(const String & path, bool keep_in_remote_fs) override;

    void clearDirectory(const String & path) override;

    void moveDirectory(const String & from_path, const String & to_path) override { moveFile(from_path, to_path); }

    void removeDirectory(const String & path) override;

    void setLastModified(const String & path, const Poco::Timestamp & timestamp) override;

    /// Overridden by disks s3 and hdfs.
    virtual void removeFromRemoteFS(RemoteFSPathKeeperPtr /* fs_paths_keeper */)
    {
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk {} does not support removing remote files", getName());
    }

    /// Overridden by disks s3 and hdfs.
    virtual RemoteFSPathKeeperPtr createFSPathKeeper() const
    {
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk {} does not support FS paths keeper", getName());
    }

    /// Create part

    void createFile(const String & path) final override;

    void createDirectory(const String & path) override;

    void createDirectories(const String & path) override;

    void createHardLink(const String & src_path, const String & dst_path) override;

protected:
    Poco::Logger * log;

    /// Disk name
    const String name;
    /// URL + root path to store files in remote FS.
    const String remote_fs_root_path;

    const String metadata_path;

private:
    void removeMeta(const String & path, RemoteFSPathKeeperPtr fs_paths_keeper);

    void removeMetaRecursive(const String & path, RemoteFSPathKeeperPtr fs_paths_keeper);

    bool tryReserve(UInt64 bytes);

    UInt64 reserved_bytes = 0;
    UInt64 reservation_count = 0;
    std::mutex reservation_mutex;
};

using RemoteDiskPtr = std::shared_ptr<IDiskRemote>;


/// Remote FS (S3, HDFS, WEB-server) metadata file layout:
/// FS objects, their number and total size of all FS objects.
/// Each FS object represents a file path in remote FS and its size.

struct IDiskRemote::Metadata
{
    /// Metadata file version.
    static constexpr UInt32 VERSION_ABSOLUTE_PATHS = 1;
    static constexpr UInt32 VERSION_RELATIVE_PATHS = 2;
    static constexpr UInt32 VERSION_READ_ONLY_FLAG = 3;

    using PathAndSize = std::pair<String, size_t>;

    /// Remote FS (S3, HDFS, WEB-server) root path (uri + files directory path).
    const String & remote_fs_root_path;

    /// Disk path.
    const String & disk_path;

    /// Relative path to metadata file on local FS.
    String metadata_file_path;

    /// Total size of all remote FS objects.
    size_t total_size = 0;

    /// Remote FS objects paths and their sizes.
    std::vector<PathAndSize> remote_fs_objects;

    /// Number of references (hardlinks) to this metadata file.
    UInt32 ref_count = 0;

    /// Flag indicates that file is read only.
    bool read_only = false;

    /// Load metadata by path or create empty if `create` flag is set.
    Metadata(const String & remote_fs_root_path_,
            const String & disk_path_,
            const String & metadata_file_path_,
            bool create = false);

    void addObject(const String & path, size_t size);

    /// Fsync metadata file if 'sync' flag is set.
    void save(bool sync = false);

};


class RemoteDiskDirectoryIterator final : public IDiskDirectoryIterator
{
public:
    RemoteDiskDirectoryIterator(const String & full_path, const String & folder_path_) : iter(full_path), folder_path(folder_path_) {}

    void next() override { ++iter; }

    bool isValid() const override { return iter != fs::directory_iterator(); }

    String path() const override
    {
        if (fs::is_directory(iter->path()))
            return folder_path / iter->path().filename().string() / "";
        else
            return folder_path / iter->path().filename().string();
    }

    String name() const override { return iter->path().filename(); }

private:
    fs::directory_iterator iter;
    fs::path folder_path;
};


class DiskRemoteReservation final : public IReservation
{
public:
    DiskRemoteReservation(const RemoteDiskPtr & disk_, UInt64 size_)
        : disk(disk_), size(size_), metric_increment(CurrentMetrics::DiskSpaceReservedForMerge, size_)
    {
    }

    UInt64 getSize() const override { return size; }

    DiskPtr getDisk(size_t i) const override;

    Disks getDisks() const override { return {disk}; }

    void update(UInt64 new_size) override;

    ~DiskRemoteReservation() override;

private:
    RemoteDiskPtr disk;
    UInt64 size;
    CurrentMetrics::Increment metric_increment;
};


/// Runs tasks asynchronously using thread pool.
class AsyncExecutor : public Executor
{
public:
    explicit AsyncExecutor(const String & name_, int thread_pool_size)
        : name(name_)
        , pool(ThreadPool(thread_pool_size)) {}

    std::future<void> execute(std::function<void()> task) override
    {
        auto promise = std::make_shared<std::promise<void>>();
        pool.scheduleOrThrowOnError(
            [promise, task]()
            {
                try
                {
                    task();
                    promise->set_value();
                }
                catch (...)
                {
                    tryLogCurrentException("Failed to run async task");

                    try
                    {
                        promise->set_exception(std::current_exception());
                    }
                    catch (...) {}
                }
            });

        return promise->get_future();
    }

    void setMaxThreads(size_t threads)
    {
        pool.setMaxThreads(threads);
    }

private:
    String name;
    ThreadPool pool;
};

}
