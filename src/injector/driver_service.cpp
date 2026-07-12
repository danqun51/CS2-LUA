#include "driver_service.hpp"

#include <windows.h>
#include <winsvc.h>
#include <cstdio>

void DriverServiceManager::set_error(const char* what, unsigned long err) {
  char buf[512]{};
  std::snprintf(buf, sizeof(buf), "%s failed: %lu", what, err);
  last_error_ = buf;
}

bool DriverServiceManager::install_and_start(const wchar_t* service_name, const wchar_t* driver_path) {
  if (!install(service_name, driver_path)) return false;
  return start(service_name);
}

bool DriverServiceManager::install(const wchar_t* service_name, const wchar_t* driver_path) {
  last_error_.clear();

  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
  if (!scm) { set_error("OpenSCManagerW", GetLastError()); return false; }

  SC_HANDLE svc = CreateServiceW(
      scm, service_name, service_name,
      SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS,
      SERVICE_KERNEL_DRIVER,
      SERVICE_DEMAND_START,
      SERVICE_ERROR_NORMAL,
      driver_path,
      nullptr, nullptr, nullptr, nullptr, nullptr);

  if (!svc) {
    DWORD err = GetLastError();
    if (err == ERROR_SERVICE_EXISTS) {
      svc = OpenServiceW(scm, service_name, SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
      if (!svc) { set_error("OpenServiceW(existing)", GetLastError()); CloseServiceHandle(scm); return false; }
      if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                driver_path, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        set_error("ChangeServiceConfigW", GetLastError());
        CloseServiceHandle(svc); CloseServiceHandle(scm); return false;
      }
    } else {
      set_error("CreateServiceW", err);
      CloseServiceHandle(scm);
      return false;
    }
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return true;
}

bool DriverServiceManager::start(const wchar_t* service_name) {
  last_error_.clear();
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) { set_error("OpenSCManagerW", GetLastError()); return false; }
  SC_HANDLE svc = OpenServiceW(scm, service_name, SERVICE_START | SERVICE_QUERY_STATUS);
  if (!svc) { set_error("OpenServiceW", GetLastError()); CloseServiceHandle(scm); return false; }
  BOOL ok = StartServiceW(svc, 0, nullptr);
  if (!ok) {
    DWORD err = GetLastError();
    if (err != ERROR_SERVICE_ALREADY_RUNNING) {
      set_error("StartServiceW", err);
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return false;
    }
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return true;
}

bool DriverServiceManager::stop(const wchar_t* service_name) {
  last_error_.clear();
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) { set_error("OpenSCManagerW", GetLastError()); return false; }
  SC_HANDLE svc = OpenServiceW(scm, service_name, SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (!svc) { set_error("OpenServiceW", GetLastError()); CloseServiceHandle(scm); return false; }
  SERVICE_STATUS_PROCESS ssp{}; DWORD bytes = 0;
  if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes)) {
    set_error("QueryServiceStatusEx", GetLastError()); CloseServiceHandle(svc); CloseServiceHandle(scm); return false;
  }
  if (ssp.dwCurrentState != SERVICE_STOPPED) {
    SERVICE_STATUS ss{};
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &ss) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
      set_error("ControlService(STOP)", GetLastError()); CloseServiceHandle(svc); CloseServiceHandle(scm); return false;
    }
    for (int i = 0; i < 50; ++i) {
      Sleep(100);
      if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes) || ssp.dwCurrentState == SERVICE_STOPPED) break;
    }
  }
  CloseServiceHandle(svc); CloseServiceHandle(scm); return true;
}

bool DriverServiceManager::stop_and_delete(const wchar_t* service_name) {
  last_error_.clear();

  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) { set_error("OpenSCManagerW", GetLastError()); return false; }

  SC_HANDLE svc = OpenServiceW(scm, service_name, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
  if (!svc) {
    DWORD err = GetLastError();
    if (err == ERROR_SERVICE_DOES_NOT_EXIST) { CloseServiceHandle(scm); return true; }
    set_error("OpenServiceW", err);
    CloseServiceHandle(scm);
    return false;
  }

  SERVICE_STATUS_PROCESS ssp{};
  DWORD bytes = 0;
  if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes)) {
    if (ssp.dwCurrentState != SERVICE_STOPPED && ssp.dwCurrentState != SERVICE_STOP_PENDING) {
      SERVICE_STATUS ss{};
      ControlService(svc, SERVICE_CONTROL_STOP, &ss);
      for (int i = 0; i < 50; ++i) {
        Sleep(100);
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes)) break;
        if (ssp.dwCurrentState == SERVICE_STOPPED) break;
      }
    }
  }

  BOOL del_ok = DeleteService(svc);
  DWORD del_err = GetLastError();
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);

  if (!del_ok && del_err != ERROR_SERVICE_MARKED_FOR_DELETE) {
    set_error("DeleteService", del_err);
    return false;
  }
  return true;
}
