// AnyPS4 shim: texture dumps are not written.
#pragma once
typedef void stbi_write_func(void* context, void* data, int size);
inline int stbi_write_png_to_func(stbi_write_func*, void*, int, int, int, const void*, int) { return 0; }
inline int stbi_write_png(const char*, int, int, int, const void*, int) { return 0; }
