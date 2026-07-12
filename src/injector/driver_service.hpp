#pragma once
#include <string>

class DriverServiceManager {
 public:
  bool install(const wchar_t* service_name, const wchar_t* driver_path);
  bool start(const wchar_t* service_name);
  bool stop(const wchar_t* service_name);
  bool install_and_start(const wchar_t* service_name, const wchar_t* driver_path);
  bool stop_and_delete(const wchar_t* service_name);
  const std::string& last_error() const { return last_error_; }

 private:
  void set_error(const char* what, unsigned long err);
  std::string last_error_;
};
