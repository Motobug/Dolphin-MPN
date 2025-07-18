// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/ResourcePack/ResourcePack.h"

#include <algorithm>
#include <memory>

#include <mz_zip.h>
#include <mz_os.h>
#include <mz_zip_rw.h>
#include <mz_strm.h>
#include <mz_zip_rw.h>

#include "Common/CommonPaths.h"
#include "Common/Contains.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/MinizipUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"

#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/Manifest.h"

namespace ResourcePack
{
constexpr char TEXTURE_PATH[] = HIRES_TEXTURES_DIR DIR_SEP;

ResourcePack::ResourcePack(const std::string& path) : m_path(path)
{
  void* zip_reader = mz_zip_reader_create();
  if (!zip_reader)
  {
    m_valid = false;
    m_error = "Failed to create zip reader";
    return;
  }

  Common::ScopeGuard file_guard{[&] { mz_zip_reader_delete(&zip_reader); }};

  if (mz_zip_reader_open_file(zip_reader, path.c_str()) != MZ_OK)
  {
    m_valid = false;
    m_error = "Failed to open resource pack";
    return;
  }

  if (mz_zip_reader_locate_entry(zip_reader, "manifest.json", 0) != MZ_OK)
  {
    m_valid = false;
    m_error = "Resource pack is missing a manifest.";
    return;
  }

  mz_zip_file* manifest_info;
  if (mz_zip_reader_entry_get_info(zip_reader, &manifest_info) != MZ_OK)
  {
    m_valid = false;
    m_error = "Failed to access manifest.json";
    return;
  }

  std::string manifest_contents(manifest_info->uncompressed_size, '\0');
  if (!Common::ReadFileFromZip(zip_reader, &manifest_contents))
  {
    m_valid = false;
    m_error = "Failed to read manifest.json";
    return;
  }

  m_manifest = std::make_shared<Manifest>(manifest_contents);
  if (!m_manifest->IsValid())
  {
    m_valid = false;
    m_error = "Manifest error: " + m_manifest->GetError();
    return;
  }

  if (mz_zip_reader_locate_entry(zip_reader, "logo.png", 0) == MZ_OK)
  {
    mz_zip_file* logo_info;
    mz_zip_reader_entry_get_info(zip_reader, &logo_info);

    m_logo_data.resize(logo_info->uncompressed_size);

    if (!Common::ReadFileFromZip(zip_reader, &m_logo_data))
    {
      m_valid = false;
      m_error = "Failed to read logo.png";
      return;
    }
  }

  mz_zip_reader_goto_first_entry(zip_reader);

  do
  {
    std::string filename(256, '\0');

    mz_zip_file* texture_info;
    mz_zip_reader_entry_get_info(zip_reader, &texture_info);

    if (!filename.starts_with("textures/") || texture_info->uncompressed_size == 0)
      continue;

    // If a texture is compressed and the manifest doesn't state that, abort.
    if (!m_manifest->IsCompressed() && texture_info->compression_method != 0)
    {
      m_valid = false;
      m_error = "Texture " + filename + " is compressed!";
      return;
    }

    m_textures.push_back(filename.substr(9));
  } while (mz_zip_reader_goto_next_entry(zip_reader) != MZ_END_OF_LIST);
}

bool ResourcePack::IsValid() const
{
  return m_valid;
}

const std::vector<char>& ResourcePack::GetLogo() const
{
  return m_logo_data;
}

const std::string& ResourcePack::GetPath() const
{
  return m_path;
}

const std::string& ResourcePack::GetError() const
{
  return m_error;
}

const Manifest* ResourcePack::GetManifest() const
{
  return m_manifest.get();
}

const std::vector<std::string>& ResourcePack::GetTextures() const
{
  return m_textures;
}

bool ResourcePack::Install(const std::string& path)
{
  if (!IsValid())
  {
    m_error = "Invalid pack";
    return false;
  }

  void* zip_reader = mz_zip_reader_create();
  if (!zip_reader)
  {
    m_valid = false;
    m_error = "Failed to create zip reader";
    return false;
  }

  Common::ScopeGuard file_guard{[&] { mz_zip_reader_delete(&zip_reader); }};

  if (mz_zip_reader_open_file(zip_reader, m_path.c_str()) != MZ_OK)
  {
    m_valid = false;
    m_error = "Failed to open resource pack";
    return false;
  }

  if (mz_zip_reader_goto_first_entry(zip_reader) != MZ_OK)
    return false;

  do
  {
    mz_zip_file* texture_info{};
    if (mz_zip_reader_entry_get_info(zip_reader, &texture_info) != MZ_OK)
    {
      return false;
    }

    const std::string texture_zip_path = texture_info->filename;

    const std::string texture_zip_path_prefix = "textures/";
    if (!texture_zip_path.starts_with(texture_zip_path_prefix))
      continue;
    const std::string texture_name = texture_zip_path.substr(texture_zip_path_prefix.size());

    auto texture_it = std::ranges::find_if(m_textures, [&texture_name](const std::string& texture) {
      return mz_path_compare_wc(texture.c_str(), texture_name.c_str(), 1) == MZ_OK;
    });
    if (texture_it == m_textures.cend())
      continue;
    const auto texture = *texture_it;

    // Check if a higher priority pack already provides a given texture, don't overwrite it
    bool provided_by_other_pack = false;
    for (const auto& pack : GetHigherPriorityPacks(*this))
    {
      if (Common::Contains(pack->GetTextures(), texture))
      {
        provided_by_other_pack = true;
        break;
      }
    }
    if (provided_by_other_pack)
      continue;

    const std::string texture_path = path + TEXTURE_PATH + texture;
    std::string texture_full_dir;
    if (!SplitPath(texture_path, &texture_full_dir, nullptr, nullptr))
      continue;

    if (!File::CreateFullPath(texture_full_dir))
    {
      m_error = "Failed to create full path " + texture_full_dir;
      return false;
    }

    const size_t data_size = static_cast<size_t>(texture_info->uncompressed_size);
    auto data = std::make_unique<u8[]>(data_size);
    if (!Common::ReadFileFromZip(zip_reader, data.get(), data_size))
    {
      m_error = "Failed to read texture " + texture;
      return false;
    }

    File::IOFile out(texture_path, "wb");
    if (!out)
    {
      m_error = "Failed to open " + texture;
      return false;
    }
    if (!out.WriteBytes(data.get(), data_size))
    {
      m_error = "Failed to write " + texture;
      return false;
    }

  } while (mz_zip_reader_goto_next_entry(zip_reader) == MZ_OK);

  SetInstalled(*this, true);
  return true;
}

bool ResourcePack::Uninstall(const std::string& path)
{
  if (!IsValid())
  {
    m_error = "Invalid pack";
    return false;
  }

  auto lower = GetLowerPriorityPacks(*this);

  SetInstalled(*this, false);

  for (const auto& texture : m_textures)
  {
    bool provided_by_other_pack = false;

    // Check if a higher priority pack already provides a given texture, don't delete it
    for (const auto& pack : GetHigherPriorityPacks(*this))
    {
      if (::ResourcePack::IsInstalled(*pack) && Common::Contains(pack->GetTextures(), texture))
      {
        provided_by_other_pack = true;
        break;
      }
    }

    if (provided_by_other_pack)
      continue;

    // Check if a lower priority pack provides a given texture - if so, install it.
    for (auto& pack : lower)
    {
      if (::ResourcePack::IsInstalled(*pack) && Common::Contains(pack->GetTextures(), texture))
      {
        pack->Install(path);

        provided_by_other_pack = true;
        break;
      }
    }

    if (provided_by_other_pack)
      continue;

    const std::string texture_path = path + TEXTURE_PATH + texture;
    if (File::Exists(texture_path) && !File::Delete(texture_path))
    {
      m_error = "Failed to delete texture " + texture;
      return false;
    }

    // Recursively delete empty directories

    std::string dir;
    SplitPath(texture_path, &dir, nullptr, nullptr);

    while (dir.length() > (path + TEXTURE_PATH).length())
    {
      auto is_empty = Common::DoFileSearch({dir}).empty();

      if (is_empty)
        File::DeleteDir(dir);

      SplitPath(dir.substr(0, dir.size() - 2), &dir, nullptr, nullptr);
    }
  }

  return true;
}

bool ResourcePack::operator==(const ResourcePack& pack) const
{
  return pack.GetPath() == m_path;
}

}  // namespace ResourcePack
