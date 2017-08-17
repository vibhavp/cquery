#include "project.h"

#include "compile_commands_loader.h"
#include "match.h"
#include "libclangmm/Utility.h"
#include "platform.h"
#include "serializer.h"
#include "utils.h"

#include <clang-c/CXCompilationDatabase.h>
#include <doctest/doctest.h>
#include <loguru.hpp>

#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <vector>

struct CompileCommandsEntry {
  std::string directory;
  std::string file;
  std::string command;
  std::vector<std::string> args;
};
MAKE_REFLECT_STRUCT(CompileCommandsEntry, directory, file, command, args);

namespace {

#if false
std::vector<Project::Entry> LoadFromDirectoryListing(
    std::unordered_set<std::string>& quote_includes, std::unordered_set<std::string>& angle_includes,
    const std::vector<std::string>& extra_flags, const std::string& project_directory) {
  std::vector<Project::Entry> result;

  std::vector<std::string> args;
  std::cerr << "Using arguments: ";
  for (const std::string& line : ReadLines(project_directory + "/clang_args")) {
    if (line.empty() || StartsWith(line, "#"))
      continue;
    if (!args.empty())
      std::cerr << ", ";
    std::cerr << line;
    args.push_back(line);
  }
  std::cerr << std::endl;


  std::vector<std::string> files = GetFilesInFolder(project_directory, true /*recursive*/, true /*add_folder_to_path*/);
  for (const std::string& file : files) {
    if (EndsWith(file, ".cc") || EndsWith(file, ".cpp") || EndsWith(file, ".c")) {
      CompileCommandsEntry e;
      e.file = NormalizePath(file);
      e.args = args;
      result.push_back(GetCompilationEntryFromCompileCommandEntry(quote_includes, angle_includes, extra_flags, e.file, e.directory, e.args));
    }
  }

  return result;
}

std::vector<Project::Entry> LoadCompilationEntriesFromDirectory(
    std::unordered_set<std::string>& quote_includes, std::unordered_set<std::string>& angle_includes,
    const std::vector<std::string>& extra_flags, const std::string& project_directory) {
  // TODO: Figure out if this function or the clang one is faster.
  //return LoadFromCompileCommandsJson(extra_flags, project_directory);

  std::cerr << "Trying to load compile_commands.json" << std::endl;
  CXCompilationDatabase_Error cx_db_load_error;
  CXCompilationDatabase cx_db = clang_CompilationDatabase_fromDirectory(project_directory.c_str(), &cx_db_load_error);
  if (cx_db_load_error == CXCompilationDatabase_CanNotLoadDatabase) {
    std::cerr << "Unable to load compile_commands.json located at \"" << project_directory << "\"; using directory listing instead." << std::endl;
    return LoadFromDirectoryListing(quote_includes, angle_includes, extra_flags, project_directory);
  }

  CXCompileCommands cx_commands = clang_CompilationDatabase_getAllCompileCommands(cx_db);

  unsigned int num_commands = clang_CompileCommands_getSize(cx_commands);
  std::vector<Project::Entry> result;
  for (unsigned int i = 0; i < num_commands; i++) {
    CXCompileCommand cx_command = clang_CompileCommands_getCommand(cx_commands, i);

    std::string directory = clang::ToString(clang_CompileCommand_getDirectory(cx_command));
    std::string relative_filename = clang::ToString(clang_CompileCommand_getFilename(cx_command));
    std::string absolute_filename = directory + "/" + relative_filename;

    CompileCommandsEntry entry;
    entry.file = NormalizePath(absolute_filename);
    entry.directory = directory;

    unsigned num_args = clang_CompileCommand_getNumArgs(cx_command);
    entry.args.reserve(num_args);
    for (unsigned j = 0; j < num_args; ++j)
      entry.args.push_back(clang::ToString(clang_CompileCommand_getArg(cx_command, j)));

    result.push_back(GetCompilationEntryFromCompileCommandEntry(quote_includes, angle_includes, extra_flags, entry.file, entry.directory, entry.args));
  }

  clang_CompileCommands_dispose(cx_commands);
  clang_CompilationDatabase_dispose(cx_db);

  return result;
}
#endif

// Computes a score based on how well |a| and |b| match. This is used for
// argument guessing.
int ComputeGuessScore(const std::string& a, const std::string& b) {
  const int kMatchPrefixWeight = 100;
  const int kMismatchDirectoryWeight = 100;
  const int kMatchPostfixWeight = 1;

  int score = 0;
  int i = 0;

  // Increase score based on matching prefix.
  for (i = 0; i < a.length() && i < b.length(); ++i) {
    if (a[i] != b[i])
      break;
    score += kMatchPrefixWeight;
  }

  // Reduce score based on mismatched directory distance.
  for (int j = i; j < a.length(); ++j) {
    if (a[j] == '/')
      score -= kMismatchDirectoryWeight;
  }
  for (int j = i; j < b.length(); ++j) {
    if (b[j] == '/')
      score -= kMismatchDirectoryWeight;
  }

  // Increase score based on common ending. Don't increase as much as matching
  // prefix or directory distance.
  for (int offset = 1; offset <= a.length() && offset <= b.length(); ++offset) {
    if (a[a.size() - offset] != b[b.size() - offset])
      break;
    score += kMatchPostfixWeight;
  }

  return score;
}

void WriteEntriesToDisk(const std::vector<Project::Entry>& entries) {
#if false
  std::ofstream file;
  file.open("C:/Users/jacob/Desktop/cquery/foo/project.txt");
  assert(file.good());
  for (const Project::Entry& entry : entries)
    file << entry.filename << ": " << StringJoin(entry.args) << std::endl;
  file.close();
#endif
}

}  // namespace

void Project::Load(const std::vector<std::string>& extra_flags, const std::string& project_directory) {
  std::unordered_set<std::string> unique_quote_includes;
  std::unordered_set<std::string> unique_angle_includes;

  //std::this_thread::sleep_for(std::chrono::seconds(5));

  // Load the project, using either compile_commands.json or a recursive directory listing.
  bool loaded_from_compile_commands = false;
  optional<std::string> compile_commands_content = ReadContent(project_directory + "/compile_commands.json");
  if (compile_commands_content) {
    loaded_from_compile_commands = TryLoadFromCompileCommandsJson(
        &entries,
        &unique_angle_includes, &unique_quote_includes,
        project_directory, extra_flags,
        *compile_commands_content);
  }
  if (!loaded_from_compile_commands) {
    LoadFromDirectoryListing(
        &entries,
        &unique_angle_includes, &unique_quote_includes,
        project_directory, extra_flags);
  }
  LOG_S(INFO) << "Finished loading project spec (used compile_commands=" << loaded_from_compile_commands << "); got " << entries.size() << " entries";

  // Project is loaded. Setup include directories.
  quote_include_directories.assign(unique_quote_includes.begin(), unique_quote_includes.end());
  angle_include_directories.assign(unique_angle_includes.begin(), unique_angle_includes.end());
  for (std::string& path : quote_include_directories) {
    EnsureEndsInSlash(path);
    LOG_S(INFO) << "quote_include_dir: " << path;
  }
  for (std::string& path : angle_include_directories) {
    EnsureEndsInSlash(path);
    LOG_S(INFO) << "angle_include_dir: " << path;
  }

  absolute_path_to_entry_index_.resize(entries.size());
  for (int i = 0; i < entries.size(); ++i)
    absolute_path_to_entry_index_[entries[i].filename] = i;

  WriteEntriesToDisk(entries);
}

Project::Entry Project::FindCompilationEntryForFile(const std::string& filename) {
  auto it = absolute_path_to_entry_index_.find(filename);
  if (it != absolute_path_to_entry_index_.end())
    return entries[it->second];

  // We couldn't find the file. Try to infer it.
  // TODO: Cache inferred file in a separate array (using a lock or similar)
  Entry* best_entry = nullptr;
  int best_score = std::numeric_limits<int>::min();
  for (Entry& entry : entries) {
    int score = ComputeGuessScore(filename, entry.filename);
    if (score > best_score) {
      best_score = score;
      best_entry = &entry;
    }
  }

  Project::Entry result;
  result.is_inferred = true;
  result.filename = filename;
  if (best_entry)
    result.args = best_entry->args;
  return result;
}

void Project::ForAllFilteredFiles(Config* config, std::function<void(int i, const Entry& entry)> action) {
  GroupMatch matcher(config->indexWhitelist, config->indexBlacklist);
  for (int i = 0; i < entries.size(); ++i) {
    const Project::Entry& entry = entries[i];
    std::string failure_reason;
    if (matcher.IsMatch(entry.filename, &failure_reason))
      action(i, entries[i]);
    else {
      if (config->logSkippedPathsForIndex) {
        LOG_S(INFO) << "[" << i + 1 << "/" << entries.size() << "]: Failed " << failure_reason << "; skipping " << entry.filename;
      }
    }
  }
}

TEST_SUITE("Project");

TEST_CASE("Entry inference") {
  Project p;
  {
    Project::Entry e;
    e.args = { "arg1" };
    e.filename = "/a/b/c/d/bar.cc";
    p.entries.push_back(e);
  }
  {
    Project::Entry e;
    e.args = { "arg2" };
    e.filename = "/a/b/c/baz.cc";
    p.entries.push_back(e);
  }

  // Guess at same directory level, when there are parent directories.
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("/a/b/c/d/new.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg1" });
  }

  // Guess at same directory level, when there are child directories.
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("/a/b/c/new.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg2" });
  }

  // Guess at new directory (use the closest parent directory).
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("/a/b/c/new/new.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg2" });
  }
}

TEST_CASE("Entry inference prefers same file endings") {
  Project p;
  {
    Project::Entry e;
    e.args = { "arg1" };
    e.filename = "common/simple_browsertest.cc";
    p.entries.push_back(e);
  }
  {
    Project::Entry e;
    e.args = { "arg2" };
    e.filename = "common/simple_unittest.cc";
    p.entries.push_back(e);
  }
  {
    Project::Entry e;
    e.args = { "arg3" };
    e.filename = "common/a/simple_unittest.cc";
    p.entries.push_back(e);
  }


  // Prefer files with the same ending.
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("my_browsertest.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg1" });
  }
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("my_unittest.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg2" });
  }
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("common/my_browsertest.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg1" });
  }
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("common/my_unittest.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg2" });
  }

  // Prefer the same directory over matching file-ending.
  {
    optional<Project::Entry> entry = p.FindCompilationEntryForFile("common/a/foo.cc");
    REQUIRE(entry.has_value());
    REQUIRE(entry->args == std::vector<std::string>{ "arg3" });
  }
}

TEST_SUITE_END();
