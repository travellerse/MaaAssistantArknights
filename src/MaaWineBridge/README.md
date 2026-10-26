# MAA Wine Bridge

## Getting Started

```console
$ cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=winegcc.cmake
$ cmake --build build
$ cmake --install build --prefix build
```

Place `build/MaaCore.dll` next to `MAA.exe`. When is gets loaded by Wine, it will load `libMaaCore.so` from the same directory and forward calls to it.

## Known limitations

- `AsstDestroy` must not be called from inside an `Asst*` callback, nor from a thread that a callback is synchronously waiting on (e.g. a callback that blocks on the UI thread which then destroys the instance). The destructor joins the message thread, and the message thread is waiting for that very callback to return. Such a call is ignored and logged: the instance is stopped but **not destroyed**, and the handle stays valid. A caller that still holds the handle can destroy again once the callback returns; a caller that has already dropped it leaks the instance. This is a guard against misuse, not a supported shutdown path.
- Win32 window attach (`AsstAttachWindow` / `AsstAsyncAttachWindow`) has no Linux counterpart. The shim exports stubs that report failure instead of missing symbols, so the GUI shows a normal connection error.

