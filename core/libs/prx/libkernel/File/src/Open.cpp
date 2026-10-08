#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "SceTypes.hpp"

#include <cerrno>
#include <cstdarg>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int APS5_VABI fcntl_nid_postfix(int descriptor, int command, ...);
extern "C" int* APS5_VABI __error_nid_postfix();

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::_wopen(p.wstring().c_str(), nativeFlags, static_cast<int>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::_lseeki64(fd, offset, whence);
}
static int NativeRead(int fd, void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelRead: nbytes exceeds platform limit");
    }
    return ::_read(fd, buf, static_cast<unsigned int>(n));
}
static int NativeWrite(int fd, const void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        throw std::runtime_error("sceKernelWrite: nbytes exceeds platform limit");
    }
    return ::_write(fd, buf, static_cast<unsigned int>(n));
}
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
static void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
static int NativeClose(int fd) {
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const int result = ::_close(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    return result;
}
static int NativeUnlink(const std::filesystem::path& p) {
    return ::_wunlink(p.wstring().c_str());
}
static int NativeGetDescriptorFlags(int fd) {
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    DWORD flags = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetHandleInformation(handle, &flags)) {
        errno = EBADF;
        return -1;
    }
    return (flags & HANDLE_FLAG_INHERIT) != 0 ? 0 : 1;
}
static int NativeSetDescriptorFlags(int fd, int flags) {
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE
        || !SetHandleInformation(handle, HANDLE_FLAG_INHERIT, (flags & 1) != 0 ? 0 : HANDLE_FLAG_INHERIT)) {
        errno = EBADF;
        return -1;
    }
    return 0;
}
static int NativeDuplicate(int fd, int minimum, bool closeOnExec) {
    if (minimum < 0) {
        errno = EINVAL;
        return -1;
    }
    if (File::DirectoryDescriptorPath(fd)) {
        throw std::runtime_error(std::string("sceKernelFcntl: duplicating a directory descriptor is not supported on this host, fd=") + std::to_string(fd));
    }
    std::vector<int> below;
    int duplicate = ::_dup(fd);
    while (duplicate >= 0 && duplicate < minimum) {
        below.push_back(duplicate);
        duplicate = ::_dup(fd);
    }
    const int error = errno;
    for (const int d : below) ::_close(d);
    if (duplicate < 0) {
        errno = error;
        return -1;
    }
    if (NativeSetDescriptorFlags(duplicate, closeOnExec ? 1 : 0) != 0) {
        ::_close(duplicate);
        return -1;
    }
    return duplicate;
}
static int NativeGetStatusFlags(int fd) {
    throw std::runtime_error(std::string("sceKernelFcntl: F_GETFL is not supported on this host, fd=") + std::to_string(fd));
}
static int NativeSetStatusFlags(int fd, int sceFlags) {
    throw std::runtime_error(std::string("sceKernelFcntl: F_SETFL is not supported on this host, fd=") + std::to_string(fd) + ", flags=" + std::to_string(sceFlags));
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= _O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= _O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= _O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= _O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= _O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= _O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= _O_EXCL;
    f |= _O_BINARY;
    return f;
}
#else
#include <fcntl.h>
#include <unistd.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::open(p.c_str(), nativeFlags, static_cast<mode_t>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::lseek(fd, static_cast<off_t>(offset), whence);
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) {
    return ::read(fd, buf, n);
}
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) {
    return ::write(fd, buf, n);
}
static int NativeClose(int fd) { return ::close(fd); }
static int NativeUnlink(const std::filesystem::path& p) {
    return ::unlink(p.c_str());
}
static int NativeGetDescriptorFlags(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : (flags & FD_CLOEXEC) != 0 ? 1 : 0;
}
static int NativeSetDescriptorFlags(int fd, int flags) {
    return ::fcntl(fd, F_SETFD, (flags & 1) != 0 ? FD_CLOEXEC : 0);
}
static int NativeDuplicate(int fd, int minimum, bool closeOnExec) {
    return ::fcntl(fd, closeOnExec ? F_DUPFD_CLOEXEC : F_DUPFD, minimum);
}
static int NativeGetStatusFlags(int fd) {
    const int native = ::fcntl(fd, F_GETFL);
    if (native < 0) return -1;
    int f = native & O_ACCMODE;
    if (native & O_APPEND) f |= SCE_KERNEL_O_APPEND;
    if (native & O_NONBLOCK) f |= SCE_KERNEL_O_NONBLOCK;
    if ((native & O_SYNC) == O_SYNC) f |= SCE_KERNEL_O_SYNC;
    else if (native & O_DSYNC) f |= SCE_KERNEL_O_DSYNC;
#ifdef O_DIRECT
    if (native & O_DIRECT) f |= SCE_KERNEL_O_DIRECT;
#endif
    return f;
}
static int NativeSetStatusFlags(int fd, int sceFlags) {
    int f = 0;
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_NONBLOCK) f |= O_NONBLOCK;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_DSYNC) f |= O_DSYNC;
#ifdef O_DIRECT
    if (sceFlags & SCE_KERNEL_O_DIRECT) f |= O_DIRECT;
#endif
    return ::fcntl(fd, F_SETFL, f);
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= O_EXCL;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_DIRECTORY) f |= O_DIRECTORY;
    return f;
}
#endif

static int SceErrorFromErrno(int error) {
    constexpr int GuestEio = 5;
    const int guest = error > 0 && error <= 34 ? error : GuestEio;
    return static_cast<int>(0x80020000u | static_cast<unsigned>(guest));
}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    APS5_LOG_OUT("path=%s flags=0x%X nativeFlags=0x%X mode=0%o", path, flags, MapFlags(flags), mode);
    auto native = ResolvePath_nid_no_patch(path);
    int fd = NativeOpen(native, MapFlags(flags), mode);
#ifdef _WIN32
    if (fd < 0 && errno != ENOENT) {
        std::error_code error;
        if (std::filesystem::is_directory(native, error)) fd = File::OpenDirectoryDescriptor(native);
    }
#endif
    if (fd < 0) {
        return SceErrorFromErrno(errno);
    }
    return fd;
}

int APS5_VABI sceKernelClose(int d) {
#ifdef _WIN32
    File::ForgetDirectoryDescriptor(d);
#endif
    if (NativeClose(d) != 0) {
        if (errno == EBADF) return SCE_KERNEL_ERROR_EBADF;
        throw std::runtime_error(std::string(__func__) + ": close failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return 0;
}

std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes) {
    if (buf == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": buf is null");
    }
    const GuestArena::HostWrite destination(buf, nbytes);
    if (!destination.Open()) errno = EFAULT;
    auto n = destination.Open() ? NativeRead(d, buf, nbytes) : -1;
    if (n < 0) {
        throw std::runtime_error(std::string(__func__) + ": read failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes) {
    if (buf == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": buf is null");
    }
    auto n = NativeWrite(d, buf, nbytes);
    if (n < 0) {
        throw std::runtime_error(std::string(__func__) + ": write failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (whence < 0 || whence > 2) {
        throw std::invalid_argument(std::string(__func__) + ": invalid whence=" + std::to_string(whence));
    }
    std::int64_t result = NativeLseek(d, offset, whence);
    if (result < 0) {
        throw std::runtime_error(std::string(__func__) + ": lseek failed, fd=" + std::to_string(d) + ", errno=" + std::to_string(errno));
    }
    return result;
}

int APS5_VABI sceKernelStat(const char* path, FileStat* sb) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    if (sb == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": sb is null");
    }
    const auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!std::filesystem::exists(native, error)) {
        return SceErrorFromErrno(2);
    }
    File::FillFileStat(native, sb);
    return 0;
}

int APS5_VABI sceKernelUnlink(const char* path) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    auto native = ResolvePath_nid_no_patch(path);
    if (NativeUnlink(native) != 0) {
        return SceErrorFromErrno(errno);
    }
    return 0;
}

int APS5_VABI sceKernelFcntl(int d, int cmd, ...) {
    constexpr int FcntlDupfd = 0;
    constexpr int FcntlGetfd = 1;
    constexpr int FcntlSetfd = 2;
    constexpr int FcntlGetfl = 3;
    constexpr int FcntlSetfl = 4;
    constexpr int FcntlDupfdCloexec = 17;
    const bool takesArgument = cmd == FcntlDupfd || cmd == FcntlSetfd || cmd == FcntlSetfl || cmd == FcntlDupfdCloexec;
    int argument = 0;
    if (takesArgument) {
#ifdef _WIN32
        __builtin_sysv_va_list arguments;
        __builtin_sysv_va_start(arguments, cmd);
        argument = __builtin_va_arg(arguments, int);
        __builtin_sysv_va_end(arguments);
#else
        std::va_list arguments;
        va_start(arguments, cmd);
        argument = va_arg(arguments, int);
        va_end(arguments);
#endif
    }
    if (d >= GuestSockets::FirstDescriptor) {
        if (!GuestSockets::IsOpen(d)) return SCE_KERNEL_ERROR_EBADF;
        const int result = takesArgument ? fcntl_nid_postfix(d, cmd, argument) : fcntl_nid_postfix(d, cmd);
        return result < 0 ? SceKernelError(*__error_nid_postfix()) : result;
    }
    int result = 0;
    switch (cmd) {
        case FcntlDupfd: result = NativeDuplicate(d, argument, false); break;
        case FcntlDupfdCloexec: result = NativeDuplicate(d, argument, true); break;
        case FcntlGetfd: result = NativeGetDescriptorFlags(d); break;
        case FcntlSetfd: result = NativeSetDescriptorFlags(d, argument); break;
        case FcntlGetfl: result = NativeGetStatusFlags(d); break;
        case FcntlSetfl: result = NativeSetStatusFlags(d, argument); break;
        default:
            throw std::runtime_error(std::string(__func__) + ": unsupported command " + std::to_string(cmd) + ", fd=" + std::to_string(d));
    }
    return result < 0 ? SceErrorFromErrno(errno) : result;
}

}
