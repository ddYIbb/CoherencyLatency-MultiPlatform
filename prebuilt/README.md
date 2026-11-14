# Prebuilt binary

`CoherencyLatency` is a Universal 2 Mach-O executable for Intel and Apple Silicon Macs.

- Architecture: universal (x86_64 + arm64)
- Minimum macOS: Intel 10.7 Lion; Apple Silicon 11.0 Big Sur
- Code signature: arm64 slice ad-hoc signed
- Built with: Zig 0.14.1 from the same `src/main.cpp`

If macOS quarantine blocks it, run:

```bash
xattr -dr com.apple.quarantine CoherencyLatency
codesign --force --deep --sign - CoherencyLatency
```
