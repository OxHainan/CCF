// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "ccf/ds/logger.h"
#include "ccf/ds/nonstd.h"
#include "consensus/ledger_enclave_types.h"
#include "ds/files.h"
#include "ds/messaging.h"
#include "ds/serialized.h"
#include "kv/kv_types.h"
#include "kv/serialised_entry_format.h"
#include "time_bound_logger.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <list>
#include <map>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <uv.h>
#include <vector>

namespace fs = std::filesystem;

namespace asynchost
{
  static constexpr size_t ledger_max_read_cache_files_default = 5;

  static constexpr auto ledger_committed_suffix = "committed";
  static constexpr auto ledger_start_idx_delimiter = "_";
  static constexpr auto ledger_last_idx_delimiter = "-";
  static constexpr auto ledger_recovery_file_suffix = "recovery";

  static inline size_t get_start_idx_from_file_name(
    const std::string& file_name)
  {
    auto pos = file_name.find(ledger_start_idx_delimiter);
    if (pos == std::string::npos)
    {
      throw std::logic_error(fmt::format(
        "Ledger file name {} does not contain a start seqno", file_name));
    }

    return std::stol(file_name.substr(pos + 1));
  }

  static inline std::optional<size_t> get_last_idx_from_file_name(
    const std::string& file_name)
  {
    auto pos = file_name.find(ledger_last_idx_delimiter);
    if (pos == std::string::npos)
    {
      // Non-committed file names do not contain a last idx
      return std::nullopt;
    }

    return std::stol(file_name.substr(pos + 1));
  }

  static inline bool is_ledger_file_committed(const std::string& file_name)
  {
    return nonstd::ends_with(file_name, ledger_committed_suffix);
  }

  static inline bool is_ledger_file_name_recovery(const std::string& file_name)
  {
    return nonstd::ends_with(file_name, ledger_recovery_file_suffix);
  }

  static inline fs::path remove_recovery_suffix(const std::string& file_name)
  {
    return nonstd::remove_suffix(
      file_name, fmt::format(".{}", ledger_recovery_file_suffix));
  }

  static std::optional<std::string> get_file_name_with_idx(
    const std::string& dir, size_t idx, bool allow_recovery_files)
  {
    std::optional<std::string> match = std::nullopt;
    for (auto const& f : fs::directory_iterator(dir))
    {
      // If any file, based on its name, contains idx. Only committed
      // (i.e. those with a last idx) are considered here.
      auto f_name = f.path().filename();
      if (!allow_recovery_files && is_ledger_file_name_recovery(f_name))
      {
        continue;
      }

      size_t start_idx = 0;
      std::optional<size_t> last_idx = std::nullopt;
      try
      {
        start_idx = get_start_idx_from_file_name(f_name);
        last_idx = get_last_idx_from_file_name(f_name);
      }
      catch (const std::exception& e)
      {
        // Ignoring invalid ledger file
        continue;
      }
      if (idx >= start_idx && last_idx.has_value() && idx <= last_idx.value())
      {
        match = f_name;
        break;
      }
    }

    return match;
  }

  class LedgerFile
  {
  private:
    using positions_offset_header_t = size_t;
    static constexpr auto file_name_prefix = "ledger";

    const fs::path dir;
    fs::path file_name;

    // This uses C stdio instead of fstream because an fstream
    // cannot be truncated.
    FILE* file = nullptr;
    std::mutex file_lock;

    size_t start_idx = 1;
    size_t total_len = 0; // Points to end of last written entry
    std::vector<uint32_t> positions;

    bool completed = false;
    bool committed = false;

    bool recovery = false;

  public:
    // Used when creating a new (empty) ledger file
    LedgerFile(const fs::path& dir, size_t start_idx, bool recovery = false) :
      dir(dir),
      file_name(fmt::format("{}_{}", file_name_prefix, start_idx)),
      start_idx(start_idx),
      recovery(recovery)
    {
      if (recovery)
      {
        file_name =
          fmt::format("{}.{}", file_name.string(), ledger_recovery_file_suffix);
      }

      auto file_path = dir / file_name;
      file = fopen(file_path.c_str(), "w+b");
      if (!file)
      {
        throw std::logic_error(fmt::format(
          "Unable to open ledger file {}: {}", file_path, strerror(errno)));
      }

      // Header reserved for the offset to the position table
      fseeko(file, sizeof(positions_offset_header_t), SEEK_SET);
      total_len = sizeof(positions_offset_header_t);
    }

    // Used when recovering an existing ledger file
    LedgerFile(const std::string& dir, const std::string& file_name_) :
      dir(dir),
      file_name(file_name_),
      completed(false)
    {
      auto file_path = (fs::path(dir) / fs::path(file_name));
      file = fopen(file_path.c_str(), "r+b");
      if (!file)
      {
        throw std::logic_error(fmt::format(
          "Unable to open ledger file {}: {}", file_path, strerror(errno)));
      }

      committed = is_ledger_file_committed(file_name);
      start_idx = get_start_idx_from_file_name(file_name);

      // First, get full size of file
      fseeko(file, 0, SEEK_END);
      size_t total_file_size = ftello(file);

      // Second, read offset to header table
      fseeko(file, 0, SEEK_SET);
      positions_offset_header_t table_offset = 0;
      if (fread(&table_offset, sizeof(positions_offset_header_t), 1, file) != 1)
      {
        throw std::logic_error(fmt::format(
          "Failed to read positions offset from ledger file {}", file_path));
      }

      if (table_offset != 0)
      {
        // If the chunk was completed, read positions table from file directly
        total_len = table_offset;
        fseeko(file, table_offset, SEEK_SET);

        if (table_offset > total_file_size)
        {
          throw std::logic_error(fmt::format(
            "Invalid table offset {} greater than total file size {}",
            table_offset,
            total_file_size));
        }

        positions.resize(
          (total_file_size - table_offset) / sizeof(positions.at(0)));

        if (
          fread(
            positions.data(),
            sizeof(positions.at(0)),
            positions.size(),
            file) != positions.size())
        {
          throw std::logic_error(fmt::format(
            "Failed to read positions table from ledger file {}", file_path));
        }
        completed = true;
      }
      else
      {
        // If the chunk was not completed, read all entries to reconstruct
        // positions table
        total_len = sizeof(positions_offset_header_t);
        auto len = total_file_size - total_len;

        kv::SerialisedEntryHeader entry_header = {};
        size_t current_idx = start_idx;
        while (len >= kv::serialised_entry_header_size)
        {
          if (
            fread(&entry_header, kv::serialised_entry_header_size, 1, file) !=
            1)
          {
            LOG_FAIL_FMT(
              "Failed to read entry header from ledger file {} at seqno {}",
              file_path,
              current_idx);
            return;
          }

          len -= kv::serialised_entry_header_size;

          const auto& entry_size = entry_header.size;
          if (len < entry_size)
          {
            LOG_FAIL_FMT(
              "Malformed incomplete ledger file {} at seqno {} (expecting "
              "entry of size "
              "{}, remaining {})",
              file_path,
              current_idx,
              entry_size,
              len);

            return;
          }

          fseeko(file, entry_size, SEEK_CUR);
          len -= entry_size;
          current_idx++;

          LOG_TRACE_FMT(
            "Recovered one entry of size {} at seqno {}",
            entry_size,
            current_idx);

          positions.push_back(total_len);
          total_len += (kv::serialised_entry_header_size + entry_size);
        }
        completed = false;
      }
    }

    ~LedgerFile()
    {
      if (file)
      {
        fclose(file);
      }
    }

    size_t get_start_idx() const
    {
      return start_idx;
    }

    size_t get_last_idx() const
    {
      return start_idx + positions.size() - 1;
    }

    size_t get_current_size() const
    {
      return total_len;
    }

    bool is_committed() const
    {
      return committed;
    }

    bool is_complete() const
    {
      return completed;
    }

    bool is_recovery() const
    {
      return recovery;
    }

    size_t write_entry(const uint8_t* data, size_t size, bool committable)
    {
      fseeko(file, total_len, SEEK_SET);

      if (fwrite(data, size, 1, file) != 1)
      {
        throw std::logic_error("Failed to write entry to ledger");
      }

      // Committable entries get flushed straight away
      if (committable && fflush(file) != 0)
      {
        throw std::logic_error(
          fmt::format("Failed to flush entry to ledger: {}", strerror(errno)));
      }

      positions.push_back(total_len);
      total_len += size;

      return get_last_idx();
    }

    size_t entries_size(size_t from, size_t to) const
    {
      if ((from < start_idx) || (to < from) || (to > get_last_idx()))
      {
        return 0;
      }

      if (to == get_last_idx())
      {
        return total_len - positions.at(from - start_idx);
      }
      else
      {
        return positions.at(to - start_idx + 1) -
          positions.at(from - start_idx);
      }
    }

    std::optional<std::vector<uint8_t>> read_entries(size_t from, size_t to)
    {
      if ((from < start_idx) || (to > get_last_idx()) || (to < from))
      {
        LOG_FAIL_FMT("Unknown entries range: {} - {}", from, to);
        return std::nullopt;
      }

      std::unique_lock<std::mutex> guard(file_lock);
      auto size = entries_size(from, to);
      std::vector<uint8_t> entries(size);
      fseeko(file, positions.at(from - start_idx), SEEK_SET);

      if (fread(entries.data(), size, 1, file) != 1)
      {
        throw std::logic_error(fmt::format(
          "Failed to read entry range {} - {} from file", from, to));
      }

      return entries;
    }

    bool truncate(size_t idx)
    {
      if (
        committed || (idx < start_idx - 1) ||
        (completed && idx >= get_last_idx()))
      {
        return false;
      }

      if (idx == start_idx - 1)
      {
        // Truncating everything triggers file deletion
        if (!fs::remove(dir / file_name))
        {
          throw std::logic_error(
            fmt::format("Could not remove file {}", file_name));
        }
        LOG_TRACE_FMT(
          "Removed ledger file {} on truncation at {}", file_name, idx);
        return true;
      }

      // Reset positions offset header
      fseeko(file, 0, SEEK_SET);
      positions_offset_header_t table_offset = 0;
      if (fwrite(&table_offset, sizeof(table_offset), 1, file) != 1)
      {
        throw std::logic_error("Failed to reset positions table offset");
      }

      completed = false;
      if (idx != get_last_idx())
      {
        total_len = positions.at(idx - start_idx + 1);
        positions.resize(idx - start_idx + 1);
      }

      if (fflush(file) != 0)
      {
        throw std::logic_error(
          fmt::format("Failed to flush ledger file: {}", strerror(errno)));
      }

      if (ftruncate(fileno(file), total_len))
      {
        throw std::logic_error(
          fmt::format("Failed to truncate ledger: {}", strerror(errno)));
      }

      fseeko(file, total_len, SEEK_SET);
      LOG_TRACE_FMT("Truncated ledger file {} at seqno {}", file_name, idx);
      return false;
    }

    void complete()
    {
      if (completed)
      {
        return;
      }

      fseeko(file, total_len, SEEK_SET);
      size_t table_offset = ftello(file);

      if (
        fwrite(
          reinterpret_cast<uint8_t*>(positions.data()),
          sizeof(positions.at(0)),
          positions.size(),
          file) != positions.size())
      {
        throw std::logic_error("Failed to write positions table to ledger");
      }

      // Write positions table offset at start of file
      if (fseeko(file, 0, SEEK_SET) != 0)
      {
        throw std::logic_error("Failed to set file offset to 0");
      }

      if (fwrite(&table_offset, sizeof(table_offset), 1, file) != 1)
      {
        throw std::logic_error("Failed to write positions table to ledger");
      }

      if (fflush(file) != 0)
      {
        throw std::logic_error(
          fmt::format("Failed to flush ledger file: {}", strerror(errno)));
      }

      LOG_TRACE_FMT("Completed ledger file {}", file_name);

      completed = true;
    }

    bool rename(const std::string& new_file_name)
    {
      auto file_path = dir / file_name;
      auto new_file_path = dir / new_file_name;

      try
      {
        files::rename(file_path, new_file_path);
      }
      catch (const std::exception& e)
      {
        // If the file cannot be renamed (e.g. file was removed), report an
        // error and continue
        LOG_FAIL_FMT("Error renaming ledger file: {}", e.what());
      }
      file_name = new_file_name;
      return true;
    }

    void open()
    {
      auto new_file_name = remove_recovery_suffix(file_name);
      rename(new_file_name);
      recovery = false;
      LOG_DEBUG_FMT("Open recovery ledger file {}", new_file_name);
    }

    bool commit(size_t idx)
    {
      if (!completed || committed || (idx != get_last_idx()))
      {
        // No effect if commit idx is not last idx
        return false;
      }

      if (fflush(file) != 0)
      {
        throw std::logic_error(
          fmt::format("Failed to flush ledger file: {}", strerror(errno)));
      }

      auto committed_file_name = fmt::format(
        "{}_{}-{}.{}",
        file_name_prefix,
        start_idx,
        get_last_idx(),
        ledger_committed_suffix);

      if (recovery)
      {
        committed_file_name = fmt::format(
          "{}.{}", committed_file_name, ledger_recovery_file_suffix);
      }

      if (!rename(committed_file_name))
      {
        return false;
      }

      committed = true;
      LOG_DEBUG_FMT("Committed ledger file {}", file_name);

      // Committed recovery files stay in the list of active files until the
      // ledger is open
      return !recovery;
    }
  };

  class Ledger
  {
  private:
    static constexpr size_t max_chunk_threshold_size =
      std::numeric_limits<uint32_t>::max(); // 4GB

    ringbuffer::WriterPtr to_enclave;

    // Main ledger directory (write and read)
    const fs::path ledger_dir;

    // Ledger directories (read-only)
    std::vector<fs::path> read_ledger_dirs;

    // Keep tracks of all ledger files for writing.
    // Current ledger file is always the last one
    std::list<std::shared_ptr<LedgerFile>> files;

    // Cache of ledger files for reading
    size_t max_read_cache_files;
    std::list<std::shared_ptr<LedgerFile>> files_read_cache;
    std::mutex read_cache_lock;

    const size_t chunk_threshold;
    size_t last_idx = 0;
    size_t committed_idx = 0;

    size_t end_of_committed_files_idx = 0;

    // True if a new file should be created when writing an entry
    bool require_new_file;

    // Set during recovery to mark files as temporary until the recovery is
    // complete
    std::optional<size_t> recovery_start_idx = std::nullopt;

    auto get_it_contains_idx(size_t idx) const
    {
      if (idx == 0)
      {
        return files.end();
      }

      auto f = std::upper_bound(
        files.begin(),
        files.end(),
        idx,
        [](size_t idx, const std::shared_ptr<LedgerFile>& f) {
          return (idx <= f->get_last_idx());
        });

      return f;
    }

    std::shared_ptr<LedgerFile> get_file_from_cache(size_t idx)
    {
      if (idx == 0)
      {
        return nullptr;
      }

      {
        std::unique_lock<std::mutex> guard(read_cache_lock);

        // First, try to find file from read cache
        for (auto const& f : files_read_cache)
        {
          if (f->get_start_idx() <= idx && idx <= f->get_last_idx())
          {
            return f;
          }
        }
      }

      // If the file is not in the cache, find the file from the ledger
      // directories, inspecting the main ledger directory first
      // Note: reading recovery chunks from main ledger directory is
      // acceptable and in fact required to complete private recovery.
      std::string ledger_dir_;
      auto match = get_file_name_with_idx(ledger_dir, idx, true);
      if (match.has_value())
      {
        ledger_dir_ = ledger_dir;
      }
      else
      {
        for (auto const& dir : read_ledger_dirs)
        {
          match = get_file_name_with_idx(dir, idx, false);
          if (match.has_value())
          {
            ledger_dir_ = dir;
            break;
          }
        }
      }

      if (!match.has_value())
      {
        return nullptr;
      }

      // Emplace file in the max-sized read cache, replacing the oldest entry if
      // the read cache is full
      std::shared_ptr<LedgerFile> match_file = nullptr;
      try
      {
        match_file = std::make_shared<LedgerFile>(ledger_dir_, match.value());
      }
      catch (const std::exception& e)
      {
        LOG_FAIL_FMT(
          "Could not open ledger file {} to read seqno {}", match.value(), idx);
        return nullptr;
      }

      {
        std::unique_lock<std::mutex> guard(read_cache_lock);

        files_read_cache.emplace_back(match_file);
        if (files_read_cache.size() > max_read_cache_files)
        {
          files_read_cache.erase(files_read_cache.begin());
        }
      }

      return match_file;
    }

    std::shared_ptr<LedgerFile> get_file_from_idx(
      size_t idx, bool read_cache_only = false)
    {
      if (idx == 0)
      {
        return nullptr;
      }

      if (!read_cache_only)
      {
        // First, check if the file is in the list of files open for writing
        auto f = std::upper_bound(
          files.rbegin(),
          files.rend(),
          idx,
          [](size_t idx, const std::shared_ptr<LedgerFile>& f) {
            return idx >= f->get_start_idx();
          });

        if (f != files.rend())
        {
          return *f;
        }
      }

      // Otherwise, return file from read cache
      return get_file_from_cache(idx);
    }

    std::shared_ptr<LedgerFile> get_latest_file() const
    {
      if (files.empty())
      {
        return nullptr;
      }
      return files.back();
    }

    std::optional<std::vector<uint8_t>> read_entries_range(
      size_t from, size_t to, bool read_cache_only = false)
    {
      if ((from <= 0) || (to > last_idx) || (to < from))
      {
        return std::nullopt;
      }

      std::vector<uint8_t> entries;
      size_t idx = from;
      while (idx <= to)
      {
        auto f_from = get_file_from_idx(idx, read_cache_only);
        if (f_from == nullptr)
        {
          return std::nullopt;
        }
        auto to_ = std::min(f_from->get_last_idx(), to);
        auto v = f_from->read_entries(idx, to_);
        if (!v.has_value())
        {
          return std::nullopt;
        }
        entries.insert(
          entries.end(),
          std::make_move_iterator(v->begin()),
          std::make_move_iterator(v->end()));
        idx = to_ + 1;
      }

      return entries;
    }

  public:
    Ledger(
      const fs::path& ledger_dir,
      ringbuffer::AbstractWriterFactory& writer_factory,
      size_t chunk_threshold,
      size_t max_read_cache_files = ledger_max_read_cache_files_default,
      const std::vector<std::string>& read_ledger_dirs_ = {}) :
      to_enclave(writer_factory.create_writer_to_inside()),
      ledger_dir(ledger_dir),
      max_read_cache_files(max_read_cache_files),
      chunk_threshold(chunk_threshold)
    {
      if (chunk_threshold == 0 || chunk_threshold > max_chunk_threshold_size)
      {
        throw std::logic_error(fmt::format(
          "Error: Ledger chunk threshold should be between 1-{}",
          max_chunk_threshold_size));
      }

      // Recover last idx from read-only ledger directories
      for (const auto& read_dir : read_ledger_dirs_)
      {
        LOG_INFO_FMT("Recovering read-only ledger directory \"{}\"", read_dir);
        if (!fs::is_directory(read_dir))
        {
          throw std::logic_error(
            fmt::format("{} read-only ledger is not a directory", read_dir));
        }

        read_ledger_dirs.emplace_back(read_dir);

        for (auto const& f : fs::directory_iterator(read_dir))
        {
          auto last_idx_ = get_last_idx_from_file_name(f.path().filename());
          if (
            !last_idx_.has_value() ||
            !is_ledger_file_committed(f.path().filename()))
          {
            LOG_DEBUG_FMT(
              "Read-only ledger file {} is ignored as not committed",
              f.path().filename());
            continue;
          }

          if (last_idx_.value() > last_idx)
          {
            last_idx = last_idx_.value();
            committed_idx = last_idx;
            end_of_committed_files_idx = last_idx;
          }
        }
      }

      if (last_idx > 0)
      {
        LOG_INFO_FMT(
          "Recovered read-only ledger directories up to {}, committed up to "
          "{} ",
          last_idx,
          committed_idx);
      }

      if (fs::is_directory(ledger_dir))
      {
        // If the ledger directory exists, recover ledger files from it
        LOG_INFO_FMT("Recovering main ledger directory {}", ledger_dir);

        for (auto const& f : fs::directory_iterator(ledger_dir))
        {
          auto file_name = f.path().filename();

          if (is_ledger_file_name_recovery(file_name))
          {
            LOG_INFO_FMT(
              "Deleting recovery ledger file {} in main ledger directory",
              file_name);
            fs::remove(f);
            continue;
          }

          std::shared_ptr<LedgerFile> ledger_file = nullptr;
          try
          {
            ledger_file = std::make_shared<LedgerFile>(ledger_dir, file_name);

            // Truncate file to latest recovered index to cleanup entries that
            // may have been corrupted (no-op if file isn't corrupted)
            if (ledger_file->truncate(ledger_file->get_last_idx()))
            {
              // If truncation of corrupted entries removes file, file is not
              // recovered
              LOG_FAIL_FMT("Removed ledger file {}", file_name);
              continue;
            }
          }
          catch (const std::exception& e)
          {
            LOG_FAIL_FMT(
              "Error reading ledger file {}: {}", file_name, e.what());
            continue;
          }

          LOG_DEBUG_FMT(
            "Recovering file from main ledger directory: {}", file_name);
          files.emplace_back(std::move(ledger_file));
        }

        if (files.empty())
        {
          LOG_INFO_FMT(
            "Main ledger directory {} is empty: no ledger file to "
            "recover",
            ledger_dir);
          require_new_file = true;
          return;
        }

        files.sort([](
                     const std::shared_ptr<LedgerFile>& a,
                     const std::shared_ptr<LedgerFile>& b) {
          return a->get_last_idx() < b->get_last_idx();
        });

        auto main_ledger_dir_last_idx = get_latest_file()->get_last_idx();
        if (main_ledger_dir_last_idx < last_idx)
        {
          throw std::logic_error(fmt::format(
            "Main ledger directory last seqno ({}) is less than read-only "
            "ledger directories last seqno ({})",
            main_ledger_dir_last_idx,
            last_idx));
        }
        last_idx = main_ledger_dir_last_idx;

        // Remove committed files from list of writable files
        for (auto f = files.begin(); f != files.end();)
        {
          if ((*f)->is_committed())
          {
            const auto f_last_idx = (*f)->get_last_idx();
            committed_idx = f_last_idx;
            end_of_committed_files_idx = f_last_idx;
            f = files.erase(f);
          }
          else
          {
            f++;
          }
        }

        // Continue writing at the end of last file only if that file is not
        // complete
        if (!files.empty() && !files.back()->is_complete())
        {
          require_new_file = false;
        }
        else
        {
          require_new_file = true;
        }
      }
      else
      {
        if (!fs::create_directory(ledger_dir))
        {
          throw std::logic_error(fmt::format(
            "Error: Could not create ledger directory: {}", ledger_dir));
        }
        require_new_file = true;
      }

      LOG_INFO_FMT(
        "Recovered ledger entries up to {}, committed to {}",
        last_idx,
        committed_idx);
    }

    Ledger(const Ledger& that) = delete;

    void init(size_t idx, size_t recovery_start_idx_ = 0)
    {
      TimeBoundLogger log_if_slow(
        fmt::format("Initing ledger - seqno={}", idx));

      // Used to initialise the ledger when starting from a non-empty state,
      // i.e. snapshot. It is assumed that idx is included in a committed
      // ledger file

      // As it is possible that some ledger files containing indices later
      // than snapshot index already exist (e.g. to verify the snapshot
      // evidence), delete those so that ledger can restart neatly.
      bool has_deleted = false;
      for (auto const& f : fs::directory_iterator(ledger_dir))
      {
        auto file_name = f.path().filename();
        if (get_start_idx_from_file_name(file_name) > idx)
        {
          LOG_INFO_FMT(
            "Deleting ledger file {} it is later than init seqno {}",
            file_name,
            idx);
          fs::remove(f);
          has_deleted = true;
        }
      }

      if (has_deleted)
      {
        files.clear();
        require_new_file = true;
      }

      last_idx = idx;
      committed_idx = idx;
      if (recovery_start_idx_ > 0)
      {
        // Do not set recovery idx and create recovery chunks
        // if the ledger is initialised from 0 (i.e. genesis)
        recovery_start_idx = recovery_start_idx_;
      }

      LOG_INFO_FMT(
        "Set last known/commit seqno to {}, recovery seqno to {}",
        idx,
        recovery_start_idx_);
    }

    void complete_recovery()
    {
      // When the recovery is completed (i.e. service is open), temporary
      // recovery ledger chunks are renamed as they can now be recovered.
      // Note: this operation cannot be rolled back.
      LOG_INFO_FMT("Ledger complete recovery");

      for (auto it = files.begin(); it != files.end();)
      {
        auto& f = *it;
        if (f->is_recovery())
        {
          f->open();

          // Recovery files are kept in the list of active files when committed
          // so that they can be renamed in a stable order when the service is
          // open. Once this is done, they can be removed from the list of
          // active files.
          if (f->is_committed())
          {
            it = files.erase(it);
            continue;
          }
        }
        ++it;
      }

      recovery_start_idx.reset();
    }

    size_t get_last_idx() const
    {
      return last_idx;
    }

    void set_recovery_start_idx(size_t idx)
    {
      recovery_start_idx = idx;
    }

    std::optional<std::vector<uint8_t>> read_entry(size_t idx)
    {
      TimeBoundLogger log_if_slow(
        fmt::format("Reading ledger entry at {}", idx));

      return read_entries_range(idx, idx);
    }

    std::optional<std::vector<uint8_t>> read_entries(size_t from, size_t to)
    {
      TimeBoundLogger log_if_slow(
        fmt::format("Reading ledger entries from {} to {}", from, to));

      return read_entries_range(from, to);
    }

    size_t write_entry(
      const uint8_t* data,
      size_t size,
      bool committable,
      bool force_chunk_after)
    {
      TimeBoundLogger log_if_slow(fmt::format(
        "Writing ledger entry - {} bytes, committable={}, "
        "force_chunk_after={}, "
        "require_new_file={}",
        size,
        committable,
        force_chunk_after,
        require_new_file));

      auto header = serialized::peek<kv::SerialisedEntryHeader>(data, size);

      if (header.flags & kv::EntryFlags::FORCE_LEDGER_CHUNK_BEFORE)
      {
        LOG_TRACE_FMT(
          "Forcing ledger chunk before entry as required by the entry header "
          "flags");

        auto f = get_latest_file();
        if (f != nullptr)
        {
          f->complete();
          require_new_file = true;
          LOG_DEBUG_FMT("Ledger chunk completed at {}", f->get_last_idx());
        }
      }

      bool force_chunk_after_in_header =
        header.flags & kv::EntryFlags::FORCE_LEDGER_CHUNK_AFTER;
      if (force_chunk_after_in_header)
      {
        if (!committable)
        {
          throw std::logic_error(
            "Ledger chunks cannot end in a non-committable transaction");
        }
        LOG_TRACE_FMT(
          "Forcing ledger chunk after entry as required by the entry header "
          "flags");
      }
      force_chunk_after |= force_chunk_after_in_header;

      if (require_new_file)
      {
        size_t start_idx = last_idx + 1;
        bool is_recovery = recovery_start_idx.has_value() &&
          start_idx > recovery_start_idx.value();

        files.push_back(
          std::make_shared<LedgerFile>(ledger_dir, last_idx + 1, is_recovery));
        require_new_file = false;
      }

      auto f = get_latest_file();
      last_idx = f->write_entry(data, size, committable);

      LOG_TRACE_FMT(
        "Wrote entry at {} [committable: {}, force chunk after: {}]",
        last_idx,
        committable,
        force_chunk_after);

      if (
        committable &&
        (force_chunk_after || f->get_current_size() >= chunk_threshold))
      {
        f->complete();
        require_new_file = true;
        LOG_DEBUG_FMT("Ledger chunk completed at {}", last_idx);
      }

      return last_idx;
    }

    void truncate(size_t idx)
    {
      TimeBoundLogger log_if_slow(fmt::format("Truncating ledger at {}", idx));

      LOG_DEBUG_FMT("Ledger truncate: {}/{}", idx, last_idx);

      if (idx >= last_idx || idx < committed_idx)
      {
        return;
      }

      require_new_file = true;

      auto f_from = get_it_contains_idx(idx + 1);
      auto f_to = get_it_contains_idx(last_idx);
      auto f_end = std::next(f_to);

      for (auto it = f_from; it != f_end;)
      {
        // Truncate the first file to the truncation index while the more
        // recent files are deleted entirely
        auto truncate_idx = (it == f_from) ? idx : (*it)->get_start_idx() - 1;
        if ((*it)->truncate(truncate_idx))
        {
          it = files.erase(it);
        }
        else
        {
          // A new file will not be required on the next written entry if the
          // file is _not_ deleted entirely
          require_new_file = false;
          it++;
        }
      }

      last_idx = idx;
    }

    void commit(size_t idx)
    {
      TimeBoundLogger log_if_slow(
        fmt::format("Committing ledger entry {}", idx));

      LOG_DEBUG_FMT("Ledger commit: {}/{}", idx, last_idx);

      if (idx <= committed_idx)
      {
        return;
      }

      auto f_from = (committed_idx == 0) ? get_it_contains_idx(1) :
                                           get_it_contains_idx(committed_idx);
      auto f_to = get_it_contains_idx(idx);
      auto f_end = std::next(f_to);

      for (auto it = f_from; it != f_end;)
      {
        // Commit all previous file to their latest index while the latest
        // file is committed to the committed index
        const auto last_idx_in_file = (*it)->get_last_idx();
        auto commit_idx = (it == f_to) ? idx : last_idx_in_file;
        if (
          (*it)->commit(commit_idx) &&
          (it != f_to || (idx == last_idx_in_file)))
        {
          end_of_committed_files_idx = last_idx_in_file;
          it = files.erase(it);
        }
        else
        {
          it++;
        }
      }

      committed_idx = idx;
    }

    bool is_in_committed_file(size_t idx)
    {
      return idx <= end_of_committed_files_idx;
    }

    struct AsyncLedgerGet
    {
      // Filled on construction
      Ledger* ledger;
      size_t from_idx;
      size_t to_idx;

      // First argument is ledger entries (or nullopt if not found)
      // Second argument is uv status code, which may indicate a cancellation
      using ResultCallback =
        std::function<void(std::optional<std::vector<uint8_t>>&&, int)>;
      ResultCallback result_cb;

      // Final result
      std::optional<std::vector<uint8_t>> entries = std::nullopt;
    };

    static void on_ledger_get_async(uv_work_t* req)
    {
      auto data = static_cast<AsyncLedgerGet*>(req->data);

      data->entries =
        data->ledger->read_entries_range(data->from_idx, data->to_idx, true);
    }

    static void on_ledger_get_async_complete(uv_work_t* req, int status)
    {
      auto data = static_cast<AsyncLedgerGet*>(req->data);

      data->result_cb(std::move(data->entries), status);

      delete data;
      delete req;
    }

    void write_ledger_get_range_response(
      size_t from_idx,
      size_t to_idx,
      std::optional<std::vector<uint8_t>>&& entries,
      consensus::LedgerRequestPurpose purpose)
    {
      if (entries.has_value())
      {
        RINGBUFFER_WRITE_MESSAGE(
          consensus::ledger_entry_range,
          to_enclave,
          from_idx,
          to_idx,
          purpose,
          entries.value());
      }
      else
      {
        RINGBUFFER_WRITE_MESSAGE(
          consensus::ledger_no_entry_range,
          to_enclave,
          from_idx,
          to_idx,
          purpose);
      }
    }

    void register_message_handlers(
      messaging::Dispatcher<ringbuffer::Message>& disp)
    {
      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, consensus::ledger_init, [this](const uint8_t* data, size_t size) {
          auto idx = serialized::read<consensus::Index>(data, size);
          auto recovery_start_idx =
            serialized::read<consensus::Index>(data, size);
          init(idx, recovery_start_idx);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp,
        consensus::ledger_append,
        [this](const uint8_t* data, size_t size) {
          auto committable = serialized::read<bool>(data, size);
          auto force_chunk_after = serialized::read<bool>(data, size);
          write_entry(data, size, committable, force_chunk_after);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp,
        consensus::ledger_truncate,
        [this](const uint8_t* data, size_t size) {
          auto idx = serialized::read<consensus::Index>(data, size);
          auto recovery_mode = serialized::read<bool>(data, size);
          truncate(idx);
          if (recovery_mode)
          {
            set_recovery_start_idx(idx);
          }
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp,
        consensus::ledger_commit,
        [this](const uint8_t* data, size_t size) {
          auto idx = serialized::read<consensus::Index>(data, size);
          commit(idx);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, consensus::ledger_open, [this](const uint8_t*, size_t) {
          complete_recovery();
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp,
        consensus::ledger_get_range,
        [&](const uint8_t* data, size_t size) {
          auto [from_idx, to_idx, purpose] =
            ringbuffer::read_message<consensus::ledger_get_range>(data, size);

          if (is_in_committed_file(to_idx))
          {
            // Start an asynchronous job to do this, since it is committed and
            // can be accessed independently (and in parallel)
            uv_work_t* work_handle = new uv_work_t;

            {
              auto job = new AsyncLedgerGet;
              job->ledger = this;
              job->from_idx = from_idx;
              job->to_idx = to_idx;
              job->result_cb = [this,
                                from_idx = from_idx,
                                to_idx = to_idx,
                                purpose = purpose](auto&& entry, int status) {
                // NB: Even if status is cancelled (and entry is empty), we
                // want to write this result back to the enclave
                write_ledger_get_range_response(
                  from_idx, to_idx, read_entries(from_idx, to_idx), purpose);
              };

              work_handle->data = job;
            }

            uv_queue_work(
              uv_default_loop(),
              work_handle,
              &on_ledger_get_async,
              &on_ledger_get_async_complete);
          }
          else
          {
            // Read synchronously, since this accesses uncommitted state and
            // must accurately reflect changing files
            write_ledger_get_range_response(
              from_idx, to_idx, read_entries(from_idx, to_idx), purpose);
          }
        });
    }
  };
}
