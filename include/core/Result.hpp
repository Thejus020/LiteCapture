#pragma once

#include "Common.hpp"
#include <string>
#include <variant>

namespace LiteCapture::Core {

enum class StatusCode {
    Success = 0,
    InvalidParameter,
    NotInitialized,
    AlreadyInitialized,
    DeviceLoss,
    EncoderNotFound,
    EncoderConfigFailed,
    CaptureAccessDenied,
    CaptureTargetClosed,
    DiskSpaceFull,
    FileNotFound,
    IoError,
    Timeout,
    InternalError
};

class Status {
public:
    Status() : m_code(StatusCode::Success), m_hr(S_OK) {}
    Status(StatusCode code, std::wstring message = {}, HRESULT hr = S_OK)
        : m_code(code), m_message(std::move(message)), m_hr(hr) {}

    static Status Ok() { return Status(); }
    static Status Error(StatusCode code, std::wstring message = {}, HRESULT hr = S_OK) {
        return Status(code, std::move(message), hr);
    }
    static Status FromHResult(HRESULT hr, std::wstring context = {}) {
        if (SUCCEEDED(hr)) return Status::Ok();
        std::wstring msg = context.empty() ? HResultToString(hr) : context + L": " + HResultToString(hr);
        return Status(StatusCode::InternalError, std::move(msg), hr);
    }

    bool IsOk() const { return m_code == StatusCode::Success; }
    bool IsError() const { return !IsOk(); }
    StatusCode Code() const { return m_code; }
    const std::wstring& Message() const { return m_message; }
    HRESULT HResult() const { return m_hr; }

    std::wstring ToString() const {
        if (IsOk()) return L"Success";
        if (m_hr != S_OK) {
            return std::format(L"Error: {} (0x{:08X})", m_message, static_cast<uint32_t>(m_hr));
        }
        return std::format(L"Error: {}", m_message);
    }

private:
    StatusCode m_code;
    std::wstring m_message;
    HRESULT m_hr;
};

template <typename T>
class Result {
public:
    Result(T value) : m_data(std::move(value)), m_status(Status::Ok()) {}
    Result(Status status) : m_data(std::monostate{}), m_status(std::move(status)) {}

    static Result<T> Success(T value) { return Result<T>(std::move(value)); }
    static Result<T> Fail(Status status) { return Result<T>(std::move(status)); }
    static Result<T> Fail(StatusCode code, std::wstring msg = {}, HRESULT hr = S_OK) {
        return Result<T>(Status::Error(code, std::move(msg), hr));
    }
    static Result<T> FromHResult(HRESULT hr, std::wstring context = {}) {
        return Result<T>(Status::FromHResult(hr, std::move(context)));
    }

    bool IsOk() const { return m_status.IsOk(); }
    bool IsError() const { return m_status.IsError(); }
    const Status& GetStatus() const { return m_status; }

    T& Value() {
        if (!IsOk()) throw std::runtime_error("Attempted to access value of failed Result");
        return std::get<T>(m_data);
    }

    const T& Value() const {
        if (!IsOk()) throw std::runtime_error("Attempted to access value of failed Result");
        return std::get<T>(m_data);
    }

    T ValueOr(T fallback) const {
        if (IsOk()) return std::get<T>(m_data);
        return fallback;
    }

private:
    std::variant<std::monostate, T> m_data;
    Status m_status;
};

} // namespace LiteCapture::Core
