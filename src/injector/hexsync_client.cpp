#include "hexsync_client.hpp"

#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
constexpr DWORD IOCTL_HEXSYNC_READ_MEMORY = 0x222000;
constexpr DWORD IOCTL_HEXSYNC_WRITE_MEMORY = 0x222004;
constexpr DWORD IOCTL_HEXSYNC_GET_MODULE_BASE = 0x222008;
constexpr DWORD IOCTL_HEXSYNC_SELF_TEST = 0x22200C;
constexpr DWORD IOCTL_HEXSYNC_ALLOCATE_MEMORY = 0x222018;
constexpr DWORD IOCTL_HEXSYNC_CREATE_THREAD = 0x222020;
constexpr unsigned long long HEXSYNC_SELF_TEST_MAGIC_QWORD = 0x200008421ULL;

#pragma pack(push, 1)
struct HexSyncReadReq {
  unsigned int pid;
  unsigned int padding;
  void* remote_address;
  unsigned long long size;
};
struct HexSyncWriteReq {
  unsigned int pid;
  unsigned int padding;
  void* remote_address;
  unsigned long long size;
};
struct HexSyncModuleReq {
  unsigned int pid;
  unsigned int padding;
  wchar_t module_name[MAX_PATH];
  void* out_base;
};
struct HexSyncAllocReq {
  unsigned int pid;
  unsigned int padding;
  unsigned long long size;
  unsigned int alloc_type;
  unsigned int protect;
  void* address;
};
struct HexSyncThreadReq {
  unsigned int pid;
  unsigned int padding;
  void* start_routine;
  void* parameter;
  void* process_handle;
};
struct HexSyncSelfTest {
  unsigned int magic_low;
  unsigned long long magic_high;
};
#pragma pack(pop)

static_assert(sizeof(HexSyncReadReq) == 24);
static_assert(sizeof(HexSyncWriteReq) == 24);
static_assert(sizeof(HexSyncModuleReq) == 536);
static_assert(sizeof(HexSyncAllocReq) == 32);
static_assert(sizeof(HexSyncThreadReq) == 32);
static_assert(sizeof(HexSyncSelfTest) == 12);

struct HandleGuard {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~HandleGuard() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
};

void Stage(const char* message) {
  wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const auto path = std::filesystem::path(exe).parent_path() / L"CS2LuaInjector-kernel.log";
  std::ofstream out(path, std::ios::app);
  SYSTEMTIME t{}; GetLocalTime(&t);
  out << t.wHour << ':' << t.wMinute << ':' << t.wSecond << "  " << message << '\n';
  out.flush();
}

}  // namespace

void HexSyncClient::set_error(const char* what, unsigned long err) {
  char buf[512]{};
  std::snprintf(buf, sizeof(buf), "%s failed: %lu", what, err);
  last_error_ = buf;
}

bool HexSyncClient::self_test(const wchar_t* device_path) {
  last_error_.clear();
  magic_ = 0;

  HandleGuard dev{CreateFileW(device_path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (dev.h == INVALID_HANDLE_VALUE) {
    set_error("CreateFileW(\\\\.\\HexSyncService)", GetLastError());
    return false;
  }

  HexSyncSelfTest st{};
  DWORD ret = 0;
  if (!DeviceIoControl(dev.h, IOCTL_HEXSYNC_SELF_TEST, &st, sizeof(st), &st, sizeof(st), &ret, nullptr)) {
    set_error("DeviceIoControl(SELF_TEST)", GetLastError());
    return false;
  }

  magic_ = (static_cast<unsigned long long>(st.magic_high) << 32) | st.magic_low;
  if (magic_ != HEXSYNC_SELF_TEST_MAGIC_QWORD) {
    char buf[256]{};
    std::snprintf(buf, sizeof(buf), "SELF_TEST magic mismatch: got 0x%llX expected 0x%llX", magic_, HEXSYNC_SELF_TEST_MAGIC_QWORD);
    last_error_ = buf;
    return false;
  }
  return true;
}

bool HexSyncClient::inject_loadlibrary(unsigned long pid, const std::wstring& dll_path, const wchar_t* device_path) {
  Stage("inject begin");
  last_error_.clear();
  magic_ = 0;

  HandleGuard dev{CreateFileW(device_path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (dev.h == INVALID_HANDLE_VALUE) {
    set_error("CreateFileW(\\\\.\\HexSyncService)", GetLastError());
    return false;
  }
  Stage("device opened");

  HexSyncSelfTest st{};
  DWORD ret = 0;
  if (!DeviceIoControl(dev.h, IOCTL_HEXSYNC_SELF_TEST, &st, sizeof(st), &st, sizeof(st), &ret, nullptr)) {
    set_error("DeviceIoControl(SELF_TEST)", GetLastError());
    return false;
  }
  Stage("self test returned");
  magic_ = (static_cast<unsigned long long>(st.magic_high) << 32) | st.magic_low;
  if (magic_ != HEXSYNC_SELF_TEST_MAGIC_QWORD) {
    last_error_ = "SELF_TEST magic mismatch";
    return false;
  }

  const size_t path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);

  HexSyncAllocReq alloc{};
  alloc.pid = pid;
  alloc.size = path_bytes;
  alloc.alloc_type = MEM_COMMIT | MEM_RESERVE;
  alloc.protect = PAGE_READWRITE;
  alloc.address = nullptr;
  if (!DeviceIoControl(dev.h, IOCTL_HEXSYNC_ALLOCATE_MEMORY, &alloc, sizeof(alloc), &alloc, sizeof(alloc), &ret, nullptr)) {
    set_error("DeviceIoControl(ALLOCATE_MEMORY)", GetLastError());
    return false;
  }
  Stage("allocate returned");
  if (!alloc.address) {
    last_error_ = "ALLOCATE_MEMORY returned null address";
    return false;
  }

  std::vector<unsigned char> write_buf(sizeof(HexSyncWriteReq) + path_bytes);
  auto* wreq = reinterpret_cast<HexSyncWriteReq*>(write_buf.data());
  wreq->pid = pid;
  wreq->remote_address = alloc.address;
  wreq->size = path_bytes;
  memcpy(write_buf.data() + sizeof(HexSyncWriteReq), dll_path.c_str(), path_bytes);
  if (!DeviceIoControl(dev.h, IOCTL_HEXSYNC_WRITE_MEMORY, write_buf.data(), static_cast<DWORD>(write_buf.size()),
                       nullptr, 0, &ret, nullptr)) {
    set_error("DeviceIoControl(WRITE_MEMORY)", GetLastError());
    return false;
  }
  Stage("write returned");

  HexSyncReadReq rreq{};
  rreq.pid = pid;
  rreq.remote_address = alloc.address;
  rreq.size = path_bytes;
  std::vector<unsigned char> read_back(path_bytes);
  if (!DeviceIoControl(dev.h, IOCTL_HEXSYNC_READ_MEMORY, &rreq, sizeof(rreq), read_back.data(), static_cast<DWORD>(read_back.size()), &ret, nullptr)) {
    set_error("DeviceIoControl(READ_MEMORY)", GetLastError());
    return false;
  }
  Stage("read returned");
  if (memcmp(read_back.data(), dll_path.c_str(), path_bytes) != 0) {
    last_error_ = "READ_MEMORY verification mismatch";
    return false;
  }

  HexSyncModuleReq mod{};
  mod.pid = pid;
  wcscpy_s(mod.module_name, L"kernel32.dll");
  if (!DeviceIoControl(dev.h, IOCTL_HEXSYNC_GET_MODULE_BASE, &mod, sizeof(mod), &mod, sizeof(mod), &ret, nullptr)) {
    set_error("DeviceIoControl(GET_MODULE_BASE)", GetLastError());
    return false;
  }
  Stage("get module returned");
  if (!mod.out_base) {
    last_error_ = "GET_MODULE_BASE returned null kernel32 base";
    return false;
  }

  HMODULE local_k32 = GetModuleHandleW(L"kernel32.dll");
  auto local_loadlibrary = reinterpret_cast<unsigned char*>(GetProcAddress(local_k32, "LoadLibraryW"));
  if (!local_loadlibrary) {
    set_error("GetProcAddress(LoadLibraryW)", GetLastError());
    return false;
  }
  auto remote_loadlibrary = reinterpret_cast<void*>(
      reinterpret_cast<unsigned char*>(mod.out_base) + (local_loadlibrary - reinterpret_cast<unsigned char*>(local_k32)));

  HexSyncThreadReq treq{};
  treq.pid = pid;
  treq.start_routine = remote_loadlibrary;
  treq.parameter = alloc.address;
  if (!DeviceIoControl(dev.h, IOCTL_HEXSYNC_CREATE_THREAD, &treq, sizeof(treq), &treq, sizeof(treq), &ret, nullptr)) {
    set_error("DeviceIoControl(CREATE_THREAD)", GetLastError());
    return false;
  }
  if (treq.process_handle) {
    using NtCreateThreadExFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, void*, HANDLE, void*, void*, ULONG, SIZE_T, SIZE_T, SIZE_T, void*);
    const auto nt_create_thread = reinterpret_cast<NtCreateThreadExFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx"));
    HANDLE thread = nullptr;
    const LONG status = nt_create_thread ? nt_create_thread(&thread, THREAD_ALL_ACCESS, nullptr,
        static_cast<HANDLE>(treq.process_handle), remote_loadlibrary, alloc.address, 0, 0, 0, 0, nullptr) : static_cast<LONG>(0xC000007AL);
    CloseHandle(static_cast<HANDLE>(treq.process_handle));
    if (status < 0 || !thread) { char buf[128]{}; std::snprintf(buf, sizeof(buf), "NtCreateThreadEx fallback failed: 0x%08lX", status); last_error_=buf; return false; }
    CloseHandle(thread);
    Stage("driver handle + NtCreateThreadEx fallback returned");
  }
  Stage("create thread returned; injection complete");
  return true;
}
