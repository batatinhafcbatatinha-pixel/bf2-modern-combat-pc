#pragma once
#ifdef _WIN32
#include <wrl/client.h>
#else
#include <unknwn.h>
#include <utility>
#include <memory>

namespace Microsoft::WRL {
// The subset of WRL used by the renderers, with DXVK native's COM ABI.
template <class T> class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) {}
  ComPtr(T* value) : pointer_(value) { if (pointer_) pointer_->AddRef(); }
  ComPtr(const ComPtr& other) : ComPtr(other.pointer_) {}
  ComPtr(ComPtr&& other) noexcept : pointer_(other.Detach()) {}
  ~ComPtr() { Reset(); }
  ComPtr& operator=(const ComPtr& other) {
    if (this != std::addressof(other)) { ComPtr copy(other); Swap(copy); }
    return *this;
  }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != std::addressof(other)) { Reset(); pointer_ = other.Detach(); }
    return *this;
  }
  ComPtr& operator=(T* value) { ComPtr copy(value); Swap(copy); return *this; }
  ComPtr& operator=(std::nullptr_t) { Reset(); return *this; }
  T* Get() const { return pointer_; }
  T* operator->() const { return pointer_; }
  explicit operator bool() const { return pointer_ != nullptr; }
  bool operator==(std::nullptr_t) const { return pointer_ == nullptr; }
  bool operator!=(std::nullptr_t) const { return pointer_ != nullptr; }
  T** GetAddressOf() { return &pointer_; }
  T* const* GetAddressOf() const { return &pointer_; }
  T** ReleaseAndGetAddressOf() { Reset(); return &pointer_; }
  T** operator&() { return ReleaseAndGetAddressOf(); }
  ULONG Reset() { T* old = std::exchange(pointer_, nullptr); return old ? old->Release() : 0; }
  void Attach(T* value) { Reset(); pointer_ = value; }
  T* Detach() { return std::exchange(pointer_, nullptr); }
  void Swap(ComPtr& other) noexcept { std::swap(pointer_, other.pointer_); }
  template <class U> HRESULT As(U** result) const {
    if (!result) return E_POINTER;
    *result = nullptr;
    return pointer_ ? pointer_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(result)) : E_NOINTERFACE;
  }
  template <class U> HRESULT As(ComPtr<U>* result) const {
    return result ? As(result->ReleaseAndGetAddressOf()) : E_POINTER;
  }
 private:
  T* pointer_ = nullptr;
};
}
#endif
