// The test files: tests/data/<name>, read whole into memory.
#pragma once
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/types.h"
#include <stdio.h>
#include <string.h>

namespace kvtest {

struct File {
    kv::u8* data = nullptr;
    kv::u32 len  = 0;
    kv::Span<const kv::u8> bytes() const { return kv::Span<const kv::u8>(data, len); }
};

// tests/data/<name>, beside this header.
inline File read_data(const char* name) {
    char path[1024];
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    kv::format(path, sizeof path, "%.*s/data/%s", slash ? (int)(slash - here) : 0, here, name);
    File f;
    FILE* fp = fopen(path, "rb");
    if (!fp) { KV_ERROR("test", "cannot open %s", path); return f; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f.data = (kv::u8*)kv::mem_alloc((kv::usize)n + 1);
    f.len  = (kv::u32)fread(f.data, 1, (kv::usize)n, fp);
    fclose(fp);
    return f;
}

inline void drop(File& f) { kv::mem_free(f.data); f = File{}; }

} // namespace kvtest
