#pragma once
#include <string>

class HexSyncClient {
 public:
  bool self_test(const wchar_t* device_path = L"\\\\.\\HexSyncService");
  bool inject_loadlibrary(unsigned long pid, const std::wstring& dll_path,
                          const wchar_t* device_path = L"\\\\.\\HexSyncService");
  const std::string& last_error() const { return last_error_; }
  unsigned long long magic() const { return magic_; }

 private:
  void set_error(const char* what, unsigned long err);
  std::string last_error_;
  unsigned long long magic_ = 0;
};
