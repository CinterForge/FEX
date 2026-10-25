/*
$info$
tags: thunklibs|GL
desc: Handles glXGetProcAddress
$end_info$
*/

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xproto.h>
#include <GL/gl.h>
#include <GL/glext.h>

#undef GL_ARB_viewport_array
#include "glcorearb.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <unordered_map>

#include "common/Guest.h"

#include "thunkgen_guest_libGL.inl"

typedef void voidFunc();

// Maps OpenGL API function names to the address of a guest function which is
// linked to the corresponding host function pointer
const std::unordered_map<std::string_view, uintptr_t /* guest function address */> HostPtrInvokers = std::invoke([]() {
#define PAIR(name, unused) Ret[#name] = reinterpret_cast<uintptr_t>(GetCallerForHostFunction(name));
  std::unordered_map<std::string_view, uintptr_t> Ret;
  FOREACH_internal_SYMBOL(PAIR);
  return Ret;
#undef PAIR
});

extern "C" {
voidFunc* glXGetProcAddress(const GLubyte* procname) {
  auto Ret = fexfn_pack_glXGetProcAddress(procname);
  if (!Ret) {
    return nullptr;
  }

  auto TargetFuncIt = HostPtrInvokers.find(reinterpret_cast<const char*>(procname));
  if (TargetFuncIt == HostPtrInvokers.end()) {
    std::string_view procname_s {reinterpret_cast<const char*>(procname)};
    // If glXGetProcAddress is querying itself, then we can just return itself.
    // Some games do this for unknown reasons.
    if (procname_s == "glXGetProcAddress" || procname_s == "glXGetProcAddressARB") {
      return reinterpret_cast<voidFunc*>(glXGetProcAddress);
    }

    // Extension found in host but not in our interface definition => Not fatal but warn about it
    // Some games query leaked GLES symbols but don't use them
    // glFrustrumf : ES 1.x function
    //  - Papers, Please
    //  - Dicey Dungeons
    fprintf(stderr, "glXGetProcAddress: not found %s\n", procname);
    return nullptr;
  }

  LinkAddressToFunction((uintptr_t)Ret, TargetFuncIt->second);
  return Ret;
}

voidFunc* glXGetProcAddressARB(const GLubyte* procname) {
  return glXGetProcAddress(procname);
}
}

// Wrapper around malloc() without noexcept specifiers
static void* malloc_wrapper(size_t size) {
  return malloc(size);
}

// Errors originating on the host GLX display are not packets from the
// guest display's X11 transport.  Never feed them to guest _XError: that
// routine processes incoming wire packets and mutates guest sequence/reply
// state.  In particular, synthesizing a sequence number here can make the
// next guest XSync wait indefinitely for a reply that will never arrive.
//
// Deliver an application-level XErrorEvent to the currently installed guest
// Xlib error handler instead.  XSetErrorHandler stores that pointer in the
// exported libX11 _XErrorFunction variable.  This is an Xlib-internal ABI
// dependency: resolve it explicitly and fail loudly if unavailable.
static int GuestDeliverXError(uintptr_t GuestDisplay, uint64_t HostSerial, uint64_t ResourceId,
                              uint32_t ErrorCode, uint32_t RequestCode, uint32_t MinorCode) {
  using XErrorHandlerPointer = int (*)(Display*, XErrorEvent*);
  static auto HandlerSlot = reinterpret_cast<XErrorHandlerPointer*>(dlsym(RTLD_DEFAULT, "_XErrorFunction"));
  static auto DefaultHandler = reinterpret_cast<XErrorHandlerPointer>(dlsym(RTLD_DEFAULT, "_XDefaultError"));
  if (!HandlerSlot || !DefaultHandler) {
    fprintf(stderr, "FEX libGL: guest Xlib error handler ABI is unavailable\n");
    std::abort();
  }

  auto* DisplayConnection = reinterpret_cast<Display*>(GuestDisplay);
  XErrorEvent Error {};
  Error.type = X_Error;
  Error.display = DisplayConnection;
  // The host request serial has no counterpart in the guest connection.
  (void)HostSerial;
  Error.serial = LastKnownRequestProcessed(DisplayConnection);
  Error.resourceid = static_cast<XID>(ResourceId);
  Error.error_code = static_cast<unsigned char>(ErrorCode);
  Error.request_code = static_cast<unsigned char>(RequestCode);
  Error.minor_code = static_cast<unsigned char>(MinorCode);
  auto Handler = *HandlerSlot;
  return Handler ? Handler(DisplayConnection, &Error) : DefaultHandler(DisplayConnection, &Error);
}

static void OnInit() {
  fexfn_pack_GL_SetGuestMalloc((uintptr_t)malloc_wrapper, (uintptr_t)CallbackUnpack<decltype(malloc_wrapper)>::Unpack);
  fexfn_pack_GL_SetGuestXSync((uintptr_t)XSync, (uintptr_t)CallbackUnpack<decltype(XSync)>::Unpack);
  fexfn_pack_GL_SetGuestXGetVisualInfo((uintptr_t)XGetVisualInfo, (uintptr_t)CallbackUnpack<decltype(XGetVisualInfo)>::Unpack);
  fexfn_pack_GL_SetGuestXDisplayString((uintptr_t)XDisplayString, (uintptr_t)CallbackUnpack<decltype(XDisplayString)>::Unpack);
  fexfn_pack_GL_SetGuestXError((uintptr_t)GuestDeliverXError, (uintptr_t)CallbackUnpack<decltype(GuestDeliverXError)>::Unpack);
}

// libGL.so must pull in libX11.so as a dependency. Referencing some libX11
// symbol here prevents the linker from optimizing away the unused dependency
auto implicit_libx11_dependency = XSetErrorHandler;

LOAD_LIB_INIT(libGL, OnInit)
