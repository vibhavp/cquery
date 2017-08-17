#include "compile_commands_loader.h"

#include "match.h"
#include "libclangmm/Utility.h"
#include "project.h"
#include "platform.h"
#include "serializer.h"
#include "utils.h"

#include <doctest/doctest.h>
#include <loguru/loguru.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace {

bool gEnableTestMode = false;

const char* kBlacklistMulti[] = {
  "-MF",
  "-Xclang"
};

// Blacklisted flags which are always removed from the command line.
const char *kBlacklist[] = {
  "clang++",
  "--param",
  "-M",
  "-MD",
  "-MG",
  "-MM",
  "-MMD",
  "-MP",
  "-MQ",
  "-MT",
  "-Og",
  "-Wa,--32",
  "-Wa,--64",
  "-Wl,--incremental-full",
  "-Wl,--incremental-patch,1",
  "-Wl,--no-incremental",
  "-fbuild-session-file=",
  "-fbuild-session-timestamp=",
  "-fembed-bitcode",
  "-fembed-bitcode-marker",
  "-fmodules-validate-once-per-build-session",
  "-fno-delete-null-pointer-checks",
  "-fno-use-linker-plugin"
  "-fno-var-tracking",
  "-fno-var-tracking-assignments",
  "-fno-enforce-eh-specs",
  "-fvar-tracking",
  "-fvar-tracking-assignments",
  "-fvar-tracking-assignments-toggle",
  "-gcc-toolchain",
  "-march=",
  "-masm=",
  "-mcpu=",
  "-mfpmath=",
  "-mtune=",
  //"-s",

  "-B",
  //"-f",
  //"-pipe",
  //"-W",
  // TODO: make sure we consume includes before stripping all path-like args.
  "/work/goma/gomacc",
  "../../third_party/llvm-build/Release+Asserts/bin/clang++",
  "-Wno-unused-lambda-capture",
  //"/",
  //"..",
  //"-stdlib=libc++"
};

// Arguments which are followed by a potentially relative path. We need to make
// all relative paths absolute, otherwise libclang will not resolve them.
const char* kPathArgs[] = {
  "-I",
  "-iquote",
  "-isystem",
  "--sysroot="
};

const char* kQuoteIncludeArgs[] = {
  "-iquote"
};
const char* kAngleIncludeArgs[] = {
  "-I",
  "-isystem"
};

bool ShouldAddToQuoteIncludes(const std::string& arg) {
  for (const char* flag_type : kQuoteIncludeArgs) {
    if (arg == flag_type)
      return true;
  }
  return false;
}
bool ShouldAddToAngleIncludes(const std::string& arg) {
  for (const char* flag_type : kAngleIncludeArgs) {
    if (StartsWith(arg, flag_type))
      return true;
  }
  return false;
}

// Returns true if we should use the C, not C++, language spec for the given
// file.
bool IsCFile(const std::string& path) {
  return EndsWith(path, ".c");
}

// Clang does not have good hueristics for determining source language, we
// should explicitly specify it.
void AddClangHeuristicArgs(
    const std::string& filename,
    std::vector<std::string>* args) {
  if (!AnyStartsWith(*args, "-x")) {
    if (IsCFile(filename))
      args->push_back("-xc");
    else
      args->push_back("-xc++");
  }
  if (!AnyStartsWith(*args, "-std=")) {
    if (IsCFile(filename))
      args->push_back("-std=c11");
    else
      args->push_back("-std=c++11");
  }
}

void Validate(bool condition, const std::string& error) {
  if (!condition) {
    // TODO: show notification
    std::cerr << "[FATAL]" << error << std::endl;
    exit(1);
  }
}

std::string NormalizePathNotTest(const std::string& path) {
  if (gEnableTestMode)
    return path;

  return NormalizePath(path);
}

void CleanupArguments(
    const std::string& build_directory,
    const std::string& filename,
    std::vector<std::string>* args,
    std::unordered_set<std::string>* angle_includes,
    std::unordered_set<std::string>* quote_includes) {
  assert(EndsWith(build_directory, "/"));

  // TODO: instead of doing a full cleanup here we can just defer cleaning
  // until when we need the args. Overhead cost should be small and it will
  // likely greatly reduce blocked loading time, even though overall time will
  // increase.

  bool make_next_flag_absolute = false;
  bool add_next_flag_quote = false;
  bool add_next_flag_angle = false;

  for (size_t i = 0; i < args->size(); ++i) {
    std::string& arg = (*args)[i];

    // If blacklisted erase the argument.
    if (std::any_of(std::begin(kBlacklistMulti), std::end(kBlacklistMulti), [&arg](const char* value) {
      return StartsWith(arg, value);
    })) {
      args->erase(args->begin() + i, args->begin() + i + 2);
      i -= 1;
      continue;
    }
    if (std::any_of(std::begin(kBlacklist), std::end(kBlacklist), [&arg](const char* value) {
      return StartsWith(arg, value);
    })) {
      args->erase(args->begin() + i);
      i -= 1;
      continue;
    }

    // Cleanup path for previous argument.
    if (make_next_flag_absolute) {
      // Make path absolute if needed. We have to preprend project directory
      // because our working dir is probably not relative to |arg|.
      if (arg.size() > 0 && arg[0] != '/')
        arg = NormalizePathNotTest(build_directory + arg);

      if (add_next_flag_quote)
        quote_includes->insert(arg);
      if (add_next_flag_angle)
        angle_includes->insert(arg);

      make_next_flag_absolute = false;
      add_next_flag_quote = false;
      add_next_flag_angle = false;
    }

    // Update arg if it is a path.
    for (const char* flag_type : kPathArgs) {
      if (arg == flag_type) {
        make_next_flag_absolute = true;
        add_next_flag_quote = ShouldAddToQuoteIncludes(arg);
        add_next_flag_angle = ShouldAddToAngleIncludes(arg);
        break;
      }

      if (StartsWith(arg, flag_type)) {
        std::string path = arg.substr(strlen(flag_type));
        if (path.size() > 0 && path[0] != '/') {
          if (!build_directory.empty())
            path = NormalizePathNotTest(build_directory + path);
          arg = flag_type + path;
        }
        if (ShouldAddToQuoteIncludes(arg))
          quote_includes->insert(path);
        if (ShouldAddToAngleIncludes(arg))
          angle_includes->insert(path);
        break;
      }
    }
  }

#if false
  // We don't do any special processing on user-given extra flags.
  for (const auto& flag : extra_flags)
    result.args.push_back(flag);
#endif

  AddClangHeuristicArgs(filename, args);
}

// value is an array of strings, which are each command line arguments to pass to clang.
std::vector<std::string> ParseArgsFromArgumentsArray(const rapidjson::Value& value) {
  Validate(value.IsArray(), "arguments must be an array");
  assert(false && "arguments array not supported right now");
  return {};
}

std::vector<std::string> ParseArgsFromCommands(
    const rapidjson::Value& value,
    const std::string& build_directory,
    const std::string& filename,
    std::unordered_set<std::string>* angle_includes,
    std::unordered_set<std::string>* quote_includes) {

  Validate(value.IsString(), "arguments must be a string");
  
  std::vector<std::string> result;

  const char* str = value.GetString();
  size_t len = value.GetStringLength();

  size_t i = 0;
  while (i < len) {
    // Skip any whitespace.
    while (i < len && iswspace(str[i])) {
      ++i;
    }

    size_t start = i;
    // Skip until next whitespace.
    while (i < len && !iswspace(str[i]))
      ++i;
    size_t end = i;

    // Insert token into result.
    std::string token;
    token.reserve(end - start);
    for (size_t j = start; j < end; ++j)
      token.push_back(str[j]);
    assert(token.size() == end - start);
    result.push_back(token);
  }

  CleanupArguments(build_directory, filename, &result, angle_includes, quote_includes);

  return result;
}

}  // namespace

bool TryLoadFromCompileCommandsJson(
    std::vector<Project::Entry>* result,
    std::unordered_set<std::string>* angle_includes,
    std::unordered_set<std::string>* quote_includes,
    std::string project_directory,
    const std::vector<std::string>& extra_flags,
    const std::string& compile_commands_content) {
  EnsureEndsInSlash(project_directory);

  rapidjson::Document reader;
  reader.Parse(compile_commands_content.c_str());
  if (reader.HasParseError()) {
    LOG_S(INFO) << "Unable to parse json; error " << reader.GetParseError();
    return {};
  }

  Validate(reader.IsArray(), "compile_commands.json must be an array");

  for (const auto& entry : reader.GetArray()) {
    Validate(entry.IsObject(), "compile_commands.json top-level array entry must be object");
    
    Project::Entry result_entry;

    // directory
    // file
    // command OR args
    // PERF: see if we can avoid std::string allocation
    std::string directory = entry["directory"].GetString();
    EnsureEndsInSlash(directory);

    std::string file = entry["file"].GetString();

    if (StartsWith(file, "/"))
      result_entry.filename = NormalizePathNotTest(file);
    else {
      result_entry.filename = NormalizePathNotTest(directory + file);
    }


    do {
      auto arguments = entry.FindMember("arguments");
      if (arguments != entry.MemberEnd()) {
        result_entry.args = ParseArgsFromArgumentsArray(arguments->value);
        break;
      }

      auto command = entry.FindMember("command");
      if (command != entry.MemberEnd()) {
        result_entry.args = ParseArgsFromCommands(command->value, directory, file, angle_includes, quote_includes);
        break;
      }

      return false;
    } while (false);

    result->push_back(result_entry);
  }

  return true;
}

void LoadFromDirectoryListing(
    std::vector<Project::Entry>* result,
    std::unordered_set<std::string>* angle_includes,
    std::unordered_set<std::string>* quote_includes,
    std::string project_directory,
    const std::vector<std::string>& extra_flags) {
  EnsureEndsInSlash(project_directory);

  // Parses a clang_args file.
  auto parse_clang_args = [project_directory]() {
    std::vector<std::string> result;
    for (const std::string& line : ReadLines(project_directory + "/clang_args")) {
      if (line.empty() || StartsWith(line, "#"))
        continue;
      result.push_back(line);
    }
    return result;
  };

  std::vector<std::string> args = parse_clang_args();
  LOG_S(INFO) << "Using arguments " << StringJoin(args);

  std::vector<std::string> files = GetFilesInFolder(project_directory, true /*recursive*/, true /*add_folder_to_path*/);
  for (const std::string& file : files) {
    // TODO: make set of file extensions customizable
    if (EndsWith(file, ".cc") || EndsWith(file, ".cpp") || EndsWith(file, ".c")) {
      Project::Entry entry;
      entry.filename = NormalizePathNotTest(file);
      entry.args = args;

      // TODO: make it so we only call CleanupArguments once. It cannot be dependent on the filename.
      CleanupArguments(project_directory, entry.filename, &entry.args, angle_includes, quote_includes);

      result->push_back(entry);
    }
  }
}

TEST_SUITE("Project");

TEST_CASE("chromium") {
  gEnableTestMode = true;


  std::string compile_commands = R"(
    [
      {
        "directory": "/work2/chrome/src/out/Release",
        "command": "/work/goma/gomacc ../../third_party/llvm-build/Release+Asserts/bin/clang++ -MMD -MF obj/apps/apps/app_lifetime_monitor.o.d -DV8_DEPRECATION_WARNINGS -DDCHECK_ALWAYS_ON=1 -DUSE_UDEV -DUSE_ASH=1 -DUSE_AURA=1 -DUSE_NSS_CERTS=1 -DUSE_OZONE=1 -DDISABLE_NACL -DFULL_SAFE_BROWSING -DSAFE_BROWSING_CSD -DSAFE_BROWSING_DB_LOCAL -DCHROMIUM_BUILD -DFIELDTRIAL_TESTING_ENABLED -DCR_CLANG_REVISION=\"310694-1\" -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -DCOMPONENT_BUILD -DOS_CHROMEOS -DNDEBUG -DNVALGRIND -DDYNAMIC_ANNOTATIONS_ENABLED=0 -DGL_GLEXT_PROTOTYPES -DUSE_GLX -DUSE_EGL -DANGLE_ENABLE_RELEASE_ASSERTS -DTOOLKIT_VIEWS=1 -DV8_USE_EXTERNAL_STARTUP_DATA -DU_USING_ICU_NAMESPACE=0 -DU_ENABLE_DYLOAD=0 -DICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE -DUCHAR_TYPE=uint16_t -DGOOGLE_PROTOBUF_NO_RTTI -DGOOGLE_PROTOBUF_NO_STATIC_INITIALIZER -DHAVE_PTHREAD -DPROTOBUF_USE_DLLS -DSK_IGNORE_LINEONLY_AA_CONVEX_PATH_OPTS -DSK_HAS_PNG_LIBRARY -DSK_HAS_WEBP_LIBRARY -DSK_HAS_JPEG_LIBRARY -DSKIA_DLL -DGR_GL_IGNORE_ES3_MSAA=0 -DSK_SUPPORT_GPU=1 -DMESA_EGL_NO_X11_HEADERS -DBORINGSSL_SHARED_LIBRARY -DUSING_V8_SHARED -I../.. -Igen -I../../third_party/libwebp/src -I../../third_party/khronos -I../../gpu -I../../third_party/ced/src -I../../third_party/icu/source/common -I../../third_party/icu/source/i18n -I../../third_party/protobuf/src -I../../skia/config -I../../skia/ext -I../../third_party/skia/include/c -I../../third_party/skia/include/config -I../../third_party/skia/include/core -I../../third_party/skia/include/effects -I../../third_party/skia/include/encode -I../../third_party/skia/include/gpu -I../../third_party/skia/include/images -I../../third_party/skia/include/lazy -I../../third_party/skia/include/pathops -I../../third_party/skia/include/pdf -I../../third_party/skia/include/pipe -I../../third_party/skia/include/ports -I../../third_party/skia/include/utils -I../../third_party/skia/third_party/vulkan -I../../third_party/skia/src/gpu -I../../third_party/skia/src/sksl -I../../third_party/mesa/src/include -I../../third_party/libwebm/source -I../../third_party/protobuf/src -Igen/protoc_out -I../../third_party/boringssl/src/include -I../../build/linux/debian_jessie_amd64-sysroot/usr/include/nss -I../../build/linux/debian_jessie_amd64-sysroot/usr/include/nspr -Igen -I../../third_party/WebKit -Igen/third_party/WebKit -I../../v8/include -Igen/v8/include -Igen -I../../third_party/flatbuffers/src/include -Igen -fno-strict-aliasing -Wno-builtin-macro-redefined -D__DATE__= -D__TIME__= -D__TIMESTAMP__= -funwind-tables -fPIC -pipe -B../../third_party/binutils/Linux_x64/Release/bin -pthread -fcolor-diagnostics -m64 -march=x86-64 -Wall -Werror -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-c++11-narrowing -Wno-covered-switch-default -Wno-unneeded-internal-declaration -Wno-inconsistent-missing-override -Wno-undefined-var-template -Wno-nonportable-include-path -Wno-address-of-packed-member -Wno-unused-lambda-capture -Wno-user-defined-warnings -Wno-enum-compare-switch -O2 -fno-ident -fdata-sections -ffunction-sections -fno-omit-frame-pointer -g0 -fvisibility=hidden -Xclang -load -Xclang ../../third_party/llvm-build/Release+Asserts/lib/libFindBadConstructs.so -Xclang -add-plugin -Xclang find-bad-constructs -Xclang -plugin-arg-find-bad-constructs -Xclang check-auto-raw-pointer -Xclang -plugin-arg-find-bad-constructs -Xclang check-ipc -Wheader-hygiene -Wstring-conversion -Wtautological-overlap-compare -Wexit-time-destructors -Wno-header-guard -Wno-exit-time-destructors -std=gnu++14 -fno-rtti -nostdinc++ -isystem../../buildtools/third_party/libc++/trunk/include -isystem../../buildtools/third_party/libc++abi/trunk/include --sysroot=../../build/linux/debian_jessie_amd64-sysroot -fno-exceptions -fvisibility-inlines-hidden -c ../../apps/app_lifetime_monitor.cc -o obj/apps/apps/app_lifetime_monitor.o",
        "file": "../../apps/app_lifetime_monitor.cc"
      }
    ]
  )";

  std::string project_directory = "/work2/chrome/src/";
  std::vector<std::string> extra_flags;

  std::unordered_set<std::string> quote_includes;
  std::unordered_set<std::string> angle_includes;
  std::vector<Project::Entry> result;

  REQUIRE(TryLoadFromCompileCommandsJson(&result, &quote_includes, &angle_includes, project_directory, extra_flags, compile_commands));

  std::vector<std::string> expected_args{
    "-DV8_DEPRECATION_WARNINGS", "-DDCHECK_ALWAYS_ON=1", "-DUSE_UDEV", "-DUSE_ASH=1", "-DUSE_AURA=1", "-DUSE_NSS_CERTS=1", "-DUSE_OZONE=1", "-DDISABLE_NACL", "-DFULL_SAFE_BROWSING", "-DSAFE_BROWSING_CSD", "-DSAFE_BROWSING_DB_LOCAL", "-DCHROMIUM_BUILD", "-DFIELDTRIAL_TESTING_ENABLED", "-DCR_CLANG_REVISION=\"310694-1\"", "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE", "-D_LARGEFILE64_SOURCE", "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS", "-DCOMPONENT_BUILD", "-DOS_CHROMEOS", "-DNDEBUG", "-DNVALGRIND", "-DDYNAMIC_ANNOTATIONS_ENABLED=0", "-DGL_GLEXT_PROTOTYPES", "-DUSE_GLX", "-DUSE_EGL", "-DANGLE_ENABLE_RELEASE_ASSERTS", "-DTOOLKIT_VIEWS=1", "-DV8_USE_EXTERNAL_STARTUP_DATA", "-DU_USING_ICU_NAMESPACE=0", "-DU_ENABLE_DYLOAD=0", "-DICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE", "-DUCHAR_TYPE=uint16_t", "-DGOOGLE_PROTOBUF_NO_RTTI", "-DGOOGLE_PROTOBUF_NO_STATIC_INITIALIZER", "-DHAVE_PTHREAD", "-DPROTOBUF_USE_DLLS", "-DSK_IGNORE_LINEONLY_AA_CONVEX_PATH_OPTS", "-DSK_HAS_PNG_LIBRARY", "-DSK_HAS_WEBP_LIBRARY", "-DSK_HAS_JPEG_LIBRARY", "-DSKIA_DLL", "-DGR_GL_IGNORE_ES3_MSAA=0", "-DSK_SUPPORT_GPU=1", "-DMESA_EGL_NO_X11_HEADERS", "-DBORINGSSL_SHARED_LIBRARY", "-DUSING_V8_SHARED", "-IC:/work2/chrome/src", "-IC:/work2/chrome/src/out/Release/gen", "-IC:/work2/chrome/src/third_party/libwebp/src", "-IC:/work2/chrome/src/third_party/khronos", "-IC:/work2/chrome/src/gpu", "-IC:/work2/chrome/src/third_party/ced/src", "-IC:/work2/chrome/src/third_party/icu/source/common", "-IC:/work2/chrome/src/third_party/icu/source/i18n", "-IC:/work2/chrome/src/third_party/protobuf/src", "-IC:/work2/chrome/src/skia/config", "-IC:/work2/chrome/src/skia/ext", "-IC:/work2/chrome/src/third_party/skia/include/c", "-IC:/work2/chrome/src/third_party/skia/include/config", "-IC:/work2/chrome/src/third_party/skia/include/core", "-IC:/work2/chrome/src/third_party/skia/include/effects", "-IC:/work2/chrome/src/third_party/skia/include/encode", "-IC:/work2/chrome/src/third_party/skia/include/gpu", "-IC:/work2/chrome/src/third_party/skia/include/images", "-IC:/work2/chrome/src/third_party/skia/include/lazy", "-IC:/work2/chrome/src/third_party/skia/include/pathops", "-IC:/work2/chrome/src/third_party/skia/include/pdf", "-IC:/work2/chrome/src/third_party/skia/include/pipe", "-IC:/work2/chrome/src/third_party/skia/include/ports", "-IC:/work2/chrome/src/third_party/skia/include/utils", "-IC:/work2/chrome/src/third_party/skia/third_party/vulkan", "-IC:/work2/chrome/src/third_party/skia/src/gpu", "-IC:/work2/chrome/src/third_party/skia/src/sksl", "-IC:/work2/chrome/src/third_party/mesa/src/include", "-IC:/work2/chrome/src/third_party/libwebm/source", "-IC:/work2/chrome/src/third_party/protobuf/src", "-IC:/work2/chrome/src/out/Release/gen/protoc_out", "-IC:/work2/chrome/src/third_party/boringssl/src/include", "-IC:/work2/chrome/src/build/linux/debian_jessie_amd64-sysroot/usr/include/nss", "-IC:/work2/chrome/src/build/linux/debian_jessie_amd64-sysroot/usr/include/nspr", "-IC:/work2/chrome/src/out/Release/gen", "-IC:/work2/chrome/src/third_party/WebKit", "-IC:/work2/chrome/src/out/Release/gen/third_party/WebKit", "-IC:/work2/chrome/src/v8/include", "-IC:/work2/chrome/src/out/Release/gen/v8/include", "-IC:/work2/chrome/src/out/Release/gen", "-IC:/work2/chrome/src/third_party/flatbuffers/src/include", "-IC:/work2/chrome/src/out/Release/gen", "-fno-strict-aliasing", "-Wno-builtin-macro-redefined", "-D__DATE__=", "-D__TIME__=", "-D__TIMESTAMP__=", "-funwind-tables", "-fPIC", "-pipe", "-pthread", "-fcolor-diagnostics", "-m64", "-Wall", "-Werror", "-Wextra", "-Wno-missing-field-initializers", "-Wno-unused-parameter", "-Wno-c++11-narrowing", "-Wno-covered-switch-default", "-Wno-unneeded-internal-declaration", "-Wno-inconsistent-missing-override", "-Wno-undefined-var-template", "-Wno-nonportable-include-path", "-Wno-address-of-packed-member", "-Wno-user-defined-warnings", "-Wno-enum-compare-switch", "-O2", "-fno-ident", "-fdata-sections", "-ffunction-sections", "-fno-omit-frame-pointer", "-g0", "-fvisibility=hidden", "-Wheader-hygiene", "-Wstring-conversion", "-Wtautological-overlap-compare", "-Wexit-time-destructors", "-Wno-header-guard", "-Wno-exit-time-destructors", "-std=gnu++14", "-fno-rtti", "-nostdinc++", "-isystemC:/work2/chrome/src/buildtools/third_party/libc++/trunk/include", "-isystemC:/work2/chrome/src/buildtools/third_party/libc++abi/trunk/include", "--sysroot=C:/work2/chrome/src/build/linux/debian_jessie_amd64-sysroot", "-fno-exceptions", "-fvisibility-inlines-hidden", "-c", "-o", "obj/apps/apps/app_lifetime_monitor.o", "-xc++"
    //"-DV8_DEPRECATION_WARNINGS"
  };
  std::cout << "Expected - Actual\n\n";
  for (int i = 0; i < std::min(result[0].args.size(), expected_args.size()); ++i) {
    if (result[0].args[i] != expected_args[i])
      std::cout << "mismatch at " << i << "; expected " << expected_args[i] << " but got " << result[0].args[i] << std::endl;
  }
  //std::cout << StringJoin(expected_args) << "\n";
  //std::cout << StringJoin(result[0].args) << "\n";
  REQUIRE(result.size() == 1);
  REQUIRE(result[0].args == expected_args);
}

TEST_CASE("argument parsing") {
  gEnableTestMode = true;

  std::string compile_commands = R"(
    [
      {
        "directory": "/usr/local/google/code/naive/build",
        "command": "clang++    -std=c++14 -Werror -I/usr/include/pixman-1 -I/usr/include/libdrm -I/usr/local/google/code/naive/src    -o CMakeFiles/naive.dir/src/wm/window.cc.o -c /usr/local/google/code/naive/src/wm/window.cc",
        "file": "/usr/local/google/code/naive/src/wm/window.cc"
      }
    ]
  )";

  std::string project_directory = "/usr/local/google/code/naive/src/";
  std::vector<std::string> extra_flags;

  std::unordered_set<std::string> quote_includes;
  std::unordered_set<std::string> angle_includes;
  std::vector<Project::Entry> result;

  REQUIRE(TryLoadFromCompileCommandsJson(&result, &quote_includes, &angle_includes, project_directory, extra_flags, compile_commands));

  std::vector<std::string> expected_args{
      "-std=c++14",
      "-Werror",
      "-I/usr/include/pixman-1",
      "-I/usr/include/libdrm",
      "-I/usr/local/google/code/naive/src",
      "-o",
      "CMakeFiles/naive.dir/src/wm/window.cc.o",
      "-c",
      "/usr/local/google/code/naive/src/wm/window.cc",
      "-xc++"
  };
  REQUIRE(result.size() == 1);
  REQUIRE(result[0].args == expected_args);
}

TEST_CASE("relative directories") {
  gEnableTestMode = true;

  std::string compile_commands = R"(
    [
      {
        "directory": "/build",
        "command": "clang++ -std=c++14 -Werror",
        "file": "bar/foo.cc"
      }
    ]
  )";

  std::string project_directory = "/build/";
  std::vector<std::string> extra_flags;

  std::unordered_set<std::string> quote_includes;
  std::unordered_set<std::string> angle_includes;
  std::vector<Project::Entry> result;

  REQUIRE(TryLoadFromCompileCommandsJson(&result, &quote_includes, &angle_includes, project_directory, extra_flags, compile_commands));
  REQUIRE(result.size() == 1);
  REQUIRE(result[0].filename == "/build/bar/foo.cc");
}

TEST_CASE("Directory extraction") {
  gEnableTestMode = true;

  std::string build_directory = "/base/";
  std::string filename = "foo.cc";
  std::vector<std::string> args = { "-I/absolute1", "-I", "/absolute2", "-Irelative1", "-I", "relative2" };
  std::unordered_set<std::string> angle_includes;
  std::unordered_set<std::string> quote_includes;
  CleanupArguments(build_directory, filename, &args, &angle_includes, &quote_includes);

  std::unordered_set<std::string> expected{ "/absolute1", "/absolute2", "/base/relative1", "/base/relative2" };
  REQUIRE(angle_includes == expected);
}

TEST_SUITE_END();
