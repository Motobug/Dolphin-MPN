// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "Common/CommonTypes.h"
#include "Common/ScopeGuard.h"

#ifdef __unix__
#include <sys/stat.h>
#endif

namespace Common
{
// Reads all of the current file entry. destination must be big enough to fit the whole file.
// 'zip_reader' is a pointer created by mz_zip_reader_create and opened with mz_zip_reader_open_file or similar.
// The entry must be located (mz_zip_reader_locate_entry) before calling this.
inline bool ReadFileFromZip(void* zip_reader, u8* destination, u64 len
#ifdef __unix__
, mode_t permissions = 0755
#endif
)
{
  const u64 MAX_BUFFER_SIZE = 65535;

  // Open the current entry for reading
  if (mz_zip_reader_entry_open(zip_reader) != MZ_OK)
    return false;

  Common::ScopeGuard guard{[&] { mz_zip_reader_entry_close(zip_reader); }};

  u64 bytes_to_go = len;
  while (bytes_to_go > 0)
  {
    const u32 read_len = static_cast<u32>(std::min(bytes_to_go, MAX_BUFFER_SIZE));
    int rv = mz_zip_reader_entry_read(zip_reader, destination, read_len);
    if (rv < 0)
      return false;

    const u32 bytes_read = static_cast<u32>(rv);
    if (bytes_read == 0)
      break; // End of file

    bytes_to_go -= bytes_read;
    destination += bytes_read;
  }

#ifdef __unix__
  // Set the file permissions after reading
  if (chmod(reinterpret_cast<const char*>(destination), permissions) != 0)
    return false;
#endif

  // There is no direct equivalent to unzEndOfFile, but if we read all requested bytes, it's OK.
  return true;
}

template <typename ContiguousContainer>
bool ReadFileFromZip(void* zip_reader, ContiguousContainer* destination)
{
  return ReadFileFromZip(zip_reader, reinterpret_cast<u8*>(destination->data()), destination->size()
#ifdef __unix__
         , 0755
#endif
  );
}
}  // namespace Common
