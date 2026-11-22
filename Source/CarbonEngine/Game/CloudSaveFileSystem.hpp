/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

// Private native filesystem operations for Steam Cloud. Keep the public filesystem API unchanged.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#ifdef WINDOWS
    #include <windows.h>
#else
    #include <cerrno>
    #include <dirent.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

namespace Carbon
{
namespace CloudSaveFileSystem
{

using Bytes = std::vector<unsigned char>;
// Steam's synchronous file API is limited to 100 MiB per object. Reserve space for the version header.
static const size_t HeaderSize = 24;
static const size_t MaxObjectSize = 100u * 1024u * 1024u;
static const char* const Prefix = "carbon-save-v1/";
static const char* const TempPrefix = ".carbon-cloud-";

enum class Kind { Missing, File, Directory, Unsafe };
struct Info
{
    Kind kind = Kind::Unsafe;
    uint64_t size = 0;
    uint64_t seconds = 0;
    uint32_t nanos = 0;
};

inline bool sameVersion(const Info& a, const Info& b)
{
    return a.kind == b.kind && a.size == b.size && a.seconds == b.seconds && a.nanos == b.nanos;
}

inline bool newer(const Info& a, const Info& b)
{
    return a.seconds > b.seconds || (a.seconds == b.seconds && a.nanos > b.nanos);
}

inline bool validTime(const Info& info)
{
    return info.seconds > 0 && info.seconds <= 253402300799ULL && info.nanos < 1000000000u;
}

inline std::string lowerASCII(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// Only portable path components are accepted, never absolute paths, dot traversal, Windows device names,
// alternate data streams, or our own temporary files. Non-ASCII names are deliberately left local for now:
// cross-platform Unicode case folding/normalization is not part of this initial sync implementation.
inline bool validRelativePath(const std::string& path)
{
    if (path.empty() || path.size() > 240 || path.front() == '/' || path.back() == '/')
        return false;
    size_t start = 0;
    while (start < path.size())
    {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part.front() == '.' || part.front() == ' ' || part.back() == '.' || part.back() == ' ')
            return false;
        for (const auto c : part)
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127 ||
                std::string("\\:*?\"<>|~").find(c) != std::string::npos)
                return false;
        auto base = lowerASCII(part.substr(0, part.find('.')));
        while (!base.empty() && base.back() == ' ')
            base.pop_back();
        if (lowerASCII(part) == "desktop.ini" || lowerASCII(part) == "thumbs.db" || base == "con" ||
            base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$" ||
            base == "clock$" ||
            (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) &&
             base[3] >= '0' && base[3] <= '9'))
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

inline std::string join(const std::string& root, const std::string& relative)
{
    return root + "/" + relative;
}

inline std::string parent(const std::string& path)
{
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0)
        return {};
    return path.substr(0, pos);
}

#ifdef WINDOWS
inline std::wstring wide(const std::string& path)
{
    auto native = path;
    std::replace(native.begin(), native.end(), '/', '\\');
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, native.c_str(), -1, nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, native.c_str(), -1, &result[0], count))
        return {};
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

inline std::string utf8(const wchar_t* name)
{
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, &result[0], count, nullptr, nullptr))
        return {};
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

inline Info windowsInfo(DWORD attributes, DWORD sizeHigh, DWORD sizeLow, FILETIME time)
{
    Info result;
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
        return result;
    result.kind = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::Directory : Kind::File;
    result.size = (uint64_t(sizeHigh) << 32) | sizeLow;
    const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ULL;
    if (ticks >= epoch)
    {
        result.seconds = (ticks - epoch) / 10000000ULL;
        result.nanos = static_cast<uint32_t>((ticks - epoch) % 10000000ULL) * 100u;
    }
    return result;
}

inline Info inspect(const std::string& path)
{
    Info result;
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    const auto name = wide(path);
    if (name.empty())
        return result;
    if (!GetFileAttributesExW(name.c_str(), GetFileExInfoStandard, &data))
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            result.kind = Kind::Missing;
        return result;
    }
    return windowsInfo(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime);
}

inline bool exactEntry(const std::string& path)
{
    const auto name = wide(path);
    WIN32_FIND_DATAW data = {};
    if (name.empty())
        return false;
    const auto handle = FindFirstFileW(name.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    FindClose(handle);
    return utf8(data.cFileName) == path.substr(path.find_last_of('/') + 1);
}

inline bool makeDirectory(const std::string& path)
{
    const auto name = wide(path);
    return !name.empty() && (CreateDirectoryW(name.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) &&
           inspect(path).kind == Kind::Directory;
}
#else
inline Info posixInfo(const struct stat& data)
{
    Info result;
    if (S_ISDIR(data.st_mode))
        result.kind = Kind::Directory;
    else if (S_ISREG(data.st_mode) && data.st_size >= 0)
    {
        result.kind = Kind::File;
        result.size = static_cast<uint64_t>(data.st_size);
    }
    if (data.st_mtime > 0)
        result.seconds = static_cast<uint64_t>(data.st_mtime);
#if defined(__APPLE__)
    result.nanos = static_cast<uint32_t>(data.st_mtimespec.tv_nsec);
#else
    result.nanos = static_cast<uint32_t>(data.st_mtim.tv_nsec);
#endif
    return result;
}

inline Info inspect(const std::string& path)
{
    struct stat data = {};
    if (lstat(path.c_str(), &data) == 0)
        return posixInfo(data);
    Info result;
    if (errno == ENOENT)
        result.kind = Kind::Missing;
    return result;
}

inline bool exactEntry(const std::string& path)
{
    const auto p = parent(path);
    if (p.empty())
        return false;
    const auto name = path.substr(path.find_last_of('/') + 1);
    const auto handle = opendir(p.c_str());
    if (!handle)
        return false;
    bool found = false;
    while (const auto entry = readdir(handle))
        if (name == entry->d_name)
        {
            found = true;
            break;
        }
    closedir(handle);
    return found;
}

inline bool makeDirectory(const std::string& path)
{
    return (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) && inspect(path).kind == Kind::Directory;
}
#endif

// The application-data directory may not yet exist on a fresh installation.
inline bool ensureRoot(const std::string& path)
{
    const auto kind = inspect(path).kind;
    if (kind == Kind::Directory)
        return true;
    if (kind != Kind::Missing)
        return false;
    const auto p = parent(path);
    return !p.empty() && ensureRoot(p) && makeDirectory(path);
}

// Check every ancestor below the save root without following symlinks/junctions.
// Missing ancestors may be created only while restoring a file.
inline bool checkParents(const std::string& root, const std::string& relative, bool create)
{
    if (!validRelativePath(relative))
        return false;
    if (create ? !ensureRoot(root) : inspect(root).kind != Kind::Directory)
        return false;
    size_t end = relative.find('/');
    while (end != std::string::npos)
    {
        const auto path = join(root, relative.substr(0, end));
        const auto kind = inspect(path).kind;
        if (kind != Kind::Directory && (!create || kind != Kind::Missing || !makeDirectory(path)))
            return false;
        if (!exactEntry(path))
            return false;
        end = relative.find('/', end + 1);
    }
    return true;
}

// A missing directory is distinct from an unsafe or inaccessible ancestor.
inline Info inspectRelative(const std::string& root, const std::string& relative)
{
    if (!validRelativePath(relative))
        return {};
    auto info = inspect(root);
    if (info.kind != Kind::Directory)
        return info.kind == Kind::Missing ? info : Info();
    size_t end = relative.find('/');
    while (end != std::string::npos)
    {
        const auto path = join(root, relative.substr(0, end));
        info = inspect(path);
        if (info.kind != Kind::Directory)
            return info.kind == Kind::Missing ? info : Info();
        if (!exactEntry(path))
            return {};
        end = relative.find('/', end + 1);
    }
    const auto path = join(root, relative);
    info = inspect(path);
    if (info.kind != Kind::Missing && !exactEntry(path))
        return {};
    return info;
}

// Capture a complete list before syncing. On enumeration failure, fail closed rather than use a partial list.
// Nonportable, dot-prefixed, special and linked paths are intentionally not followed.
inline bool enumerate(const std::string& root, const std::string& relative, std::vector<std::string>& files)
{
    const auto directory = relative.empty() ? root : join(root, relative);
    const auto kind = inspect(directory).kind;
    if (relative.empty() && kind == Kind::Missing)
        return true;
    if (kind != Kind::Directory)
        return false;
    std::vector<std::string> names;
#ifdef WINDOWS
    WIN32_FIND_DATAW data = {};
    const auto pattern = wide(join(directory, "*"));
    if (pattern.empty())
        return false;
    const auto handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    do
    {
        if (!(data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
            names.push_back(utf8(data.cFileName));
    } while (FindNextFileW(handle, &data));
    const auto error = GetLastError();
    FindClose(handle);
    if (error != ERROR_NO_MORE_FILES)
        return false;
#else
    auto handle = opendir(directory.c_str());
    if (!handle)
        return false;
    int error = 0;
    while (true)
    {
        errno = 0;
        const auto entry = readdir(handle);
        if (!entry)
        {
            error = errno;
            break;
        }
        names.emplace_back(entry->d_name);
    }
    closedir(handle);
    if (error)
        return false;
#endif
    for (const auto& name : names)
    {
        if (name.empty() || name == "." || name == "..")
            continue;
        const auto rel = relative.empty() ? name : join(relative, name);
        if (!validRelativePath(rel))
            continue;
        switch (inspect(join(root, rel)).kind)
        {
            case Kind::Directory:
                if (!enumerate(root, rel, files))
                    return false;
                break;
            case Kind::File:
                files.push_back(rel);
                break;
            case Kind::Missing:
                // A concurrent directory modification makes the snapshot unreliable.
                return false;
            default:
                break;
        }
    }
    return true;
}

// A case-sensitive source may contain names that alias on a Windows or default macOS volume.
// Reject the entire ambiguous branch, including directory-component collisions.
class PathIndex
{
public:
    void add(const std::string& relative)
    {
        size_t end = 0;
        do
        {
            end = relative.find('/', end);
            const auto part = relative.substr(0, end);
            const auto key = lowerASCII(part);
            const auto inserted = paths_.emplace(key, part);
            if (!inserted.second && inserted.first->second != part)
                conflicts_.insert(key);
            // The same component cannot be a file in one inventory and a directory in the other.
            if (end == std::string::npos)
            {
                files_.insert(key);
                if (directories_.count(key))
                    conflicts_.insert(key);
            }
            else
            {
                directories_.insert(key);
                if (files_.count(key))
                    conflicts_.insert(key);
            }
            if (end != std::string::npos)
                ++end;
        } while (end != std::string::npos);
    }

    bool accepts(const std::string& relative) const
    {
        size_t end = 0;
        do
        {
            end = relative.find('/', end);
            if (conflicts_.count(lowerASCII(relative.substr(0, end))))
                return false;
            if (end != std::string::npos)
                ++end;
        } while (end != std::string::npos);
        return true;
    }
private:
    std::map<std::string, std::string> paths_;
    std::set<std::string> conflicts_;
    std::set<std::string> files_;
    std::set<std::string> directories_;
};

// The caller must have quiesced its save files. Check metadata before and after IO, as an extra guard.
inline bool read(const std::string& path, const Info& expected, Bytes& bytes)
{
    if (expected.kind != Kind::File || expected.size > MaxObjectSize - HeaderSize || !validTime(expected))
        return false;
    bytes.resize(static_cast<size_t>(expected.size));
#ifdef WINDOWS
    const auto name = wide(path);
    if (name.empty())
        return false;
    const auto handle = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION data = {};
    DWORD count = 0;
    bool ok = GetFileInformationByHandle(handle, &data) &&
              sameVersion(expected, windowsInfo(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime));
    if (ok && !bytes.empty())
        ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
    ok = ok && GetFileInformationByHandle(handle, &data) &&
         sameVersion(expected, windowsInfo(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime));
    CloseHandle(handle);
#else
    const auto fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat data = {};
    bool ok = fstat(fd, &data) == 0 && sameVersion(expected, posixInfo(data));
    size_t offset = 0;
    while (ok && offset < bytes.size())
    {
        const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            ok = false;
        else
            offset += static_cast<size_t>(count);
    }
    ok = ok && fstat(fd, &data) == 0 && sameVersion(expected, posixInfo(data));
    close(fd);
#endif
    return ok && sameVersion(expected, inspect(path));
}

// Commit only a fully written temporary file in the target directory. Preserve its original source timestamp.
// If the destination did not exist, use a no-replace operation so a newly created local save cannot be clobbered.
inline bool replace(const std::string& root, const std::string& relative, const Info& expected,
                    const Info& version, const unsigned char* bytes, size_t size)
{
    if (!validTime(version) || size > MaxObjectSize - HeaderSize ||
        (expected.kind != Kind::Missing && expected.kind != Kind::File) || !checkParents(root, relative, true))
        return false;
    const auto path = join(root, relative);
    if (!sameVersion(expected, inspectRelative(root, relative)))
        return false;
    const auto directory = parent(path);
#ifdef WINDOWS
    const auto target = wide(path);
    if (target.empty())
        return false;
    std::wstring temporary;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned int i = 0; i < 128 && handle == INVALID_HANDLE_VALUE; ++i)
    {
        temporary = wide(join(directory, std::string(TempPrefix) + std::to_string(GetCurrentProcessId()) + "-" +
                                         std::to_string(GetTickCount64()) + "-" + std::to_string(i)));
        if (temporary.empty())
            return false;
        handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = !size || (WriteFile(handle, bytes, static_cast<DWORD>(size), &written, nullptr) && written == size);
    const auto ticks = 116444736000000000ULL + version.seconds * 10000000ULL + version.nanos / 100u;
    FILETIME time = {};
    time.dwLowDateTime = static_cast<DWORD>(ticks);
    time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    ok = ok && SetFileTime(handle, nullptr, nullptr, &time) && FlushFileBuffers(handle);
    if (!CloseHandle(handle))
        ok = false;
    if (ok && checkParents(root, relative, false) && sameVersion(expected, inspectRelative(root, relative)))
        ok = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH |
                         (expected.kind == Kind::File ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
    else
        ok = false;
    if (!ok)
        DeleteFileW(temporary.c_str());
#else
    auto pattern = join(directory, std::string(TempPrefix) + "XXXXXX");
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    const auto fd = mkstemp(temporary.data());
    if (fd < 0)
        return false;
    size_t offset = 0;
    bool ok = true;
    while (ok && offset < size)
    {
        const auto count = write(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            ok = false;
        else
            offset += static_cast<size_t>(count);
    }
    struct timeval times[2] = {};
    times[0].tv_sec = times[1].tv_sec = static_cast<time_t>(version.seconds);
    times[0].tv_usec = times[1].tv_usec = static_cast<suseconds_t>(version.nanos / 1000u);
    ok = ok && static_cast<uint64_t>(times[1].tv_sec) == version.seconds && futimes(fd, times) == 0 && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok && checkParents(root, relative, false) && sameVersion(expected, inspectRelative(root, relative)))
    {
        if (expected.kind == Kind::Missing)
            ok = link(temporary.data(), path.c_str()) == 0;
        else
            ok = rename(temporary.data(), path.c_str()) == 0;
    }
    else
        ok = false;
    unlink(temporary.data());
#endif
    return ok;
}

// Store the source modification time alongside the file payload in a single versioned cloud object.
// Steam's cache modification time reflects the cloud write, not necessarily the original save's age.
// A nonempty header also makes an empty game file distinguishable from a failed Steam FileRead.
inline void putInteger(Bytes& data, size_t offset, uint64_t value, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        data[offset + i] = static_cast<unsigned char>(value >> (8u * i));
}

inline uint64_t getInteger(const Bytes& data, size_t offset, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= uint64_t(data[offset + i]) << (8u * i);
    return value;
}

inline uint32_t checksum(const unsigned char* data, size_t count)
{
    // FNV-1a detects accidental corruption; it is not an authentication mechanism.
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < count; ++i)
        value = (value ^ data[i]) * 16777619u;
    return value;
}

inline Bytes encode(const Info& version, const Bytes& payload)
{
    Bytes data(HeaderSize + payload.size());
    const std::string magic = "CSAVE001";
    std::copy(magic.begin(), magic.end(), data.begin());
    putInteger(data, 8, version.seconds, 8);
    putInteger(data, 16, version.nanos, 4);
    putInteger(data, 20, checksum(payload.data(), payload.size()) ^ checksum(data.data(), 20), 4);
    std::copy(payload.begin(), payload.end(), data.begin() + HeaderSize);
    return data;
}

inline bool decode(const Bytes& data, Info& version)
{
    const std::string magic = "CSAVE001";
    if (data.size() < HeaderSize || data.size() > MaxObjectSize || !std::equal(magic.begin(), magic.end(), data.begin()))
        return false;
    version.kind = Kind::File;
    version.seconds = getInteger(data, 8, 8);
    version.nanos = static_cast<uint32_t>(getInteger(data, 16, 4));
    version.size = data.size() - HeaderSize;
    return validTime(version) && getInteger(data, 20, 4) == (checksum(data.data() + HeaderSize, static_cast<size_t>(version.size)) ^
                                                          checksum(data.data(), 20));
}

inline bool samePayload(const Bytes& local, const Bytes& cloud, const Info& version)
{
    return local.size() == version.size && std::equal(local.begin(), local.end(), cloud.begin() + HeaderSize);
}

}
}
