/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/extern-compiler.h"

#include <condition_variable>
#include <mutex>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <folly/DynamicConverter.h>
#include <folly/json.h>
#include <folly/FileUtil.h>

#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/zend-strtod.h"
#include "hphp/runtime/vm/repo.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/util/atomic-vector.h"
#include "hphp/util/compression.h"
#include "hphp/util/embedded-data.h"
#include "hphp/util/light-process.h"
#include "hphp/util/logger.h"
#include "hphp/util/match.h"
#include "hphp/util/md5.h"

#include <iostream>

namespace HPHP {

namespace {

struct CompileException : Exception {
  explicit CompileException(const std::string& what) : Exception(what) {}
  template<class... A>
  explicit CompileException(A&&... args)
    : Exception(folly::sformat(std::forward<A>(args)...))
  {}
};

[[noreturn]] void throwErrno(const char* what) {
  throw CompileException("{}: {}", what, folly::errnoStr(errno));
}

struct CompilerOptions {
  bool verboseErrors;
  uint64_t maxRetries;
  uint64_t workers;
  std::string command;
  bool inheritConfig;
};

constexpr int kInvalidPid = -1;

struct ExternCompiler {
  explicit ExternCompiler(const CompilerOptions& options)
      : m_options(options)
    {}
  ExternCompiler(ExternCompiler&&) = default;
  ExternCompiler& operator=(ExternCompiler&&) = default;
  void detach_from_process () {
    // Called from forked processes. Resets inherited pid of compiler process to
    // prevent it being closed in case child process exits
    m_pid = kInvalidPid;
  }
  ~ExternCompiler() { stop(); }

  std::unique_ptr<UnitEmitter> compile(
    const char* filename,
    const MD5& md5,
    folly::StringPiece code,
    AsmCallbacks* callbacks
  ) {
    if (RuntimeOption::EvalHackCompilerReset &&
        m_compilations > RuntimeOption::EvalHackCompilerReset) {
      stop();
    }
    if (!isRunning()) {
      start();
    }

    std::string prog;
    std::unique_ptr<Unit> u;
    try {
      m_compilations++;
      writeProgram(filename, md5, code);
      prog = readProgram();
      return assemble_string(
        prog.data(),
        prog.length(),
        filename,
        md5,
        false /* swallow errors */,
        callbacks
      );
    } catch (CompileException& ex) {
      stop();
      if (m_options.verboseErrors) {
        Logger::FError("ExternCompiler Error: {}", ex.what());
      }
      throw;
    } catch (std::runtime_error& ex) {
      if (m_options.verboseErrors) {
        auto const msg = folly::sformat(
          "{}\n"
          "========== PHP Source ==========\n"
          "{}\n"
          "========== ExternCompiler Result ==========\n"
          "{}\n",
          ex.what(),
          code,
          prog);
        Logger::FError("ExternCompiler Generated a bad unit: {}", msg);

        // Throw the extended message to ensure the fataling unit contains the
        // additional context
        throw std::runtime_error(msg);
      }
      throw;
    }
  }

  std::string getVersionString() {
    if (!isRunning()) start();
    return m_version;
  }

private:
  void start();
  void stop();
  bool isRunning() const { return m_pid != kInvalidPid; }
  void stopLogStderrThread();

  void writeMessage(folly::dynamic& header, folly::StringPiece body);
  void writeConfigs();
  void writeProgram(const char* filename, MD5 md5, folly::StringPiece code);

  std::string readVersion() const;
  std::string readProgram() const;

  pid_t m_pid{kInvalidPid};
  FILE* m_in{nullptr};
  FILE* m_out{nullptr};
  std::string m_version;

  FILE* m_err{nullptr};
  std::thread m_logStderrThread;

  unsigned m_compilations{0};
  const CompilerOptions& m_options;
};

struct CompilerPool {
  explicit CompilerPool(CompilerOptions&& options)
    : m_options(options)
    , m_compilers(options.workers, nullptr)
  {}

  std::pair<size_t, ExternCompiler*> getCompiler();
  void releaseCompiler(size_t id, ExternCompiler* ptr);
  void start();
  void shutdown(bool detach_compilers);
  CompilerResult compile(const char* code,
                         int len,
                         const char* filename,
                         const MD5& md5,
                         AsmCallbacks* callbacks);
  std::string getVersionString() { return m_version; }

 private:
  CompilerOptions m_options;
  std::atomic<size_t> m_freeCount{0};
  std::mutex m_compilerLock;
  std::condition_variable m_compilerCv;
  AtomicVector<ExternCompiler*> m_compilers;
  std::string m_version;
};

struct CompilerGuard final {
  explicit CompilerGuard(CompilerPool& pool)
    : m_pool(pool) {
    std::tie(m_index, m_ptr) = m_pool.getCompiler();
  }

  ~CompilerGuard() {
    m_pool.releaseCompiler(m_index, m_ptr);
  }

  CompilerGuard(CompilerGuard&&) = delete;
  CompilerGuard& operator=(CompilerGuard&&) = delete;

  ExternCompiler* operator->() const { return m_ptr; }

private:
  size_t m_index;
  ExternCompiler* m_ptr;
  CompilerPool& m_pool;
};

std::pair<size_t, ExternCompiler*> CompilerPool::getCompiler() {
  std::unique_lock<std::mutex> l(m_compilerLock);

  m_compilerCv.wait(l, [&] {
    return m_freeCount.load(std::memory_order_relaxed) != 0;
  });
  m_freeCount -= 1;

  for (size_t id = 0; id < m_compilers.size(); ++id) {
    auto ret = m_compilers.exchange(id, nullptr);
    if (ret) return std::make_pair(id, ret);
  }

  not_reached();
}

void CompilerPool::releaseCompiler(size_t id, ExternCompiler* ptr) {
  std::unique_lock<std::mutex> l(m_compilerLock);

  m_compilers[id].store(ptr, std::memory_order_relaxed);
  m_freeCount += 1;

  l.unlock();
  m_compilerCv.notify_one();
}

void CompilerPool::start() {
  auto const nworkers = m_options.workers;
  m_freeCount.store(nworkers, std::memory_order_relaxed);
  for (int i = 0; i < nworkers; ++i) {
    m_compilers[i].store(new ExternCompiler(m_options),
        std::memory_order_relaxed);
  }

  CompilerGuard g(*this);
  m_version = g->getVersionString();
}

void CompilerPool::shutdown(bool detach_compilers) {
  for (int i = 0; i < m_compilers.size(); ++i) {
    auto c = m_compilers.exchange(i, nullptr);
    if (detach_compilers) {
      c->detach_from_process();
    }
    delete c;
  }
}

CompilerResult CompilerPool::compile(const char* code,
                                     int len,
                                     const char* filename,
                                     const MD5& md5,
                                     AsmCallbacks* callbacks
) {
  CompilerGuard compiler(*this);
  std::stringstream err;

  size_t retry = 0;
  const size_t max = std::max<size_t>(
    1, m_options.maxRetries + 1
  );
  while (retry++ < max) {
    try {
      return compiler->compile(filename,
                               md5,
                               folly::StringPiece(code, len),
                               callbacks);
    } catch (CompileException& ex) {
      // Swallow and retry, we return infra errors in bulk once the retry limit
      // is exceeded.
      err << ex.what();
      if (retry < max) err << '\n';
    } catch (std::runtime_error& ex) {
      // Nontransient, don't bother with a retry.
      return ex.what();
    }
  }

  if (m_options.verboseErrors) {
    Logger::Error(
      "ExternCompiler encountered too many communication errors, giving up."
    );
  }

  return err.str();
}

////////////////////////////////////////////////////////////////////////////////

std::string readline(FILE* f) {
  char* line = nullptr;
  size_t mx = 0;
  ssize_t len = 0;
  SCOPE_EXIT { free(line); };

  if ((len = getline(&line, &mx, f)) < 0) {
    throwErrno("error reading line");
  }

  return len ? std::string(line, len - 1) : std::string();
}

std::string ExternCompiler::readVersion() const {
  // Note the utter lack of error handling. We're really expecting the version
  // JSON to be the first thing we get from the compiler daemon, and that it has
  // a "version" field, and that the value at the field is indeed a string...
  const auto line = readline(m_out);
  return folly::parseJson(line).at("version").asString();
}

std::string ExternCompiler::readProgram() const {
  const auto line = readline(m_out);
  const auto header = folly::parseJson(line);
  const std::string type = header.getDefault("type", "").asString();
  const std::size_t bytes = header.getDefault("bytes", 0).asInt();

  if (type == "hhas") {
    std::string program(bytes, '\0');
    if (fread(&program[0], bytes, 1, m_out) != 1) {
      throwErrno("reading input program");
    }
    return program;
  } else if (type == "error") {
    // We don't need to restart the pipe -- the compiler just wasn't able to
    // build this file...
    throw std::runtime_error(
      header.getDefault("error", "[no 'error' field]").asString());
  } else {
    throw std::runtime_error("unknown message type, " + type);
  }

  not_reached();
}

void ExternCompiler::stopLogStderrThread() {
  SCOPE_EXIT { m_err = nullptr; };
  if (m_err) {
    fclose(m_err);   // need to unblock getline()
  }
  if (m_logStderrThread.joinable()) {
    m_logStderrThread.join();
  }
}

void ExternCompiler::writeMessage(
  folly::dynamic& header,
  folly::StringPiece body
) {
  const auto bytes = body.size();
  header["bytes"] = bytes;
  const auto jsonHeader = folly::toJson(header);
  if (
    fprintf(m_in, "%s\n", jsonHeader.data()) == -1 ||
    (bytes > 0 && fwrite(body.begin(), bytes, 1, m_in) != 1)
  ) {
    throwErrno("error writing message");
  }
  fflush(m_in);
}

struct ConfigBuilder {
  template<typename T>
  ConfigBuilder& addField(folly::StringPiece key, const T& data) {
    if (!m_config.isObject()) {
      m_config = folly::dynamic::object();
    }

    m_config[key] = folly::dynamic::object(
      "global_value", folly::toDynamic(data));

    return *this;
  }

  std::string toString() const {
    return m_config.isNull() ? "" : folly::toJson(m_config);
  }

 private:
  folly::dynamic m_config{nullptr};
};

void ExternCompiler::writeConfigs() {
  static const std::string boundConfig = [this] () -> std::string {
    if (m_options.inheritConfig) {
      // necessary to initialize zend-strtod, which is used to serialize
      // boundConfig to JSON (!)
      zend_get_bigint_data();
      return IniSetting::GetAllAsJSON();
    }
    return "";
  }();

  // Some configs, like IncludeRoots, can't easily be Config::Bind(ed), so here
  // we create a place to dump miscellaneous config values HackC might want.
  static const std::string miscConfig = [this] () -> std::string {
    if (m_options.inheritConfig) {
      return ConfigBuilder()
        .addField("hhvm.include_roots", RuntimeOption::IncludeRoots)
        .toString();
    }
    return "";
  }();

  folly::dynamic header = folly::dynamic::object("type", "config");
  writeMessage(header, boundConfig);
  writeMessage(header, miscConfig);
}

void ExternCompiler::writeProgram(
  const char* filename,
  MD5 md5,
  folly::StringPiece code
) {
  folly::dynamic header = folly::dynamic::object
    ("type", "code")
    ("md5", md5.toString())
    ("file", filename)
    ("is_systemlib", !SystemLib::s_inited);
  writeMessage(header, code);
}

struct CompilerManager final {
  int get_delegate() { return m_delegate; }
  std::mutex& get_delegate_lock() { return m_delegateLock; }
  void set_username(const std::string& username) { m_username = username; }
  void ensure_started();
  void shutdown();
  void detach_after_fork();
  bool hackc_enabled() { return (bool)m_hackc_pool; }
  CompilerPool& get_hackc_pool();
private:
  void stop(bool detach_compilers);
  int m_delegate{kInvalidPid};
  std::mutex m_delegateLock;

  std::unique_ptr<CompilerPool> m_hackc_pool;

  std::atomic<bool> m_started{false};
  std::mutex m_compilers_start_lock;
  folly::Optional<std::string> m_username;
} s_manager;

struct UseLightDelegate final {
  UseLightDelegate()
    : m_lock(s_manager.get_delegate_lock())
    , m_prev(LightProcess::setThreadLocalAfdtOverride(s_manager.get_delegate()))
  {}

  UseLightDelegate(UseLightDelegate&&) = delete;
  UseLightDelegate& operator=(UseLightDelegate&&) = delete;

  ~UseLightDelegate() {
    LightProcess::setThreadLocalAfdtOverride(std::move(m_prev));
  }
private:
  std::unique_lock<std::mutex> m_lock;
  std::unique_ptr<LightProcess> m_prev;
};

void ExternCompiler::stop() {
  // This is super-gross: it's possible we're in a forked child -- but fork()
  // doesn't -- can't -- copy over threads, so m_logStderrThread is rubbish --
  // but joinable() in the child. When the child's ~ExternCompiler() destructor
  // is called, it will call m_logStderrThread's destructor, terminating a
  // joinable but unjoined thread, which causes a panic. We really shouldn't be
  // mixing threads with forking, but we should just about get away with it
  // here.
  SCOPE_EXIT {
    stopLogStderrThread();
  };

  if (m_pid == kInvalidPid) return;

  SCOPE_EXIT {
    if (m_in) fclose(m_in);
    if (m_out) fclose(m_out);
    m_in = m_out = nullptr;
    m_pid = kInvalidPid;
  };

  m_compilations = 0;

  auto ret = kill(m_pid, SIGTERM);
  if (ret == -1) {
    Logger::FWarning(
      "ExternCompiler: kill failed: {}, {}",
      errno,
      folly::errnoStr(errno).c_str());
  }

  int status, code;
  {
    UseLightDelegate useDelegate;
    ret = LightProcess::waitpid(m_pid, &status, 0, 2);
    if (ret != m_pid) {
      Logger::FWarning(
        "ExternCompiler: unable to wait for compiler process, return code {},"
        "errno: {}, {}",
        ret,
        errno,
        folly::errnoStr(errno).c_str());
      return;
    }
  }

  if (WIFEXITED(status) && (code = WEXITSTATUS(status)) != 0) {
    Logger::FWarning("ExternCompiler: exited with status code {}", code);
  } else if (WIFSIGNALED(status) && (code = WTERMSIG(status)) != SIGTERM) {
    Logger::FWarning(
      "ExternCompiler: terminated by signal {}{}",
      code,
      WCOREDUMP(status) ? " (code dumped)" : ""
    );
  }
}

struct Pipe final {
  Pipe() {
    if (pipe2(fds, O_CLOEXEC) == -1) throwErrno("unable to open pipe");
  }
  ~Pipe() {
    if (fds[0] != -1) close(fds[0]);
    if (fds[1] != -1) close(fds[1]);
  }
  FILE* detach(const char* mode) {
    auto ret = fdopen(fds[*mode == 'r' ? 0 : 1], mode);
    if (!ret) throwErrno("unable to fdopen pipe");
    close(fds[*mode == 'r' ? 1 : 0]);
    fds[0] = fds[1] = -1;
    return ret;
  }
  int remoteIn() const { return fds[0]; }
  int remoteOut() const { return fds[1]; }
  int fds[2];
};

void ExternCompiler::start() {
  if (m_pid != kInvalidPid) return;

  Pipe in, out, err;
  std::vector<int> created = {in.remoteIn(), out.remoteOut(), err.remoteOut()};
  std::vector<int> wanted = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
  std::vector<std::string> env;

  {
    UseLightDelegate useDelegate;

    m_pid = LightProcess::proc_open(
      m_options.command.c_str(),
      created,
      wanted,
      nullptr /* cwd */,
      env
    );
  }

  if (m_pid == kInvalidPid) {
    const auto msg = folly::to<std::string>(
      "Unable to start external compiler with command: ", m_options.command);
    Logger::Error(msg);
    throw BadCompilerException(msg);
  }

  m_in = in.detach("w");
  m_out = out.detach("r");
  m_err = err.detach("r");

  m_logStderrThread = std::thread([&]() {
      int ret = 0;
      auto pid = m_pid;
      try {
        pollfd pfd[] = {{fileno(m_err), POLLIN, 0}};
        while ((ret = poll(pfd, 1, -1)) != -1) {
          if (ret == 0) continue;
          if (pfd[0].revents & (POLLHUP | POLLNVAL | POLLERR)) {
            throw std::runtime_error("hangup");
          }
          if (pfd[0].revents) {
            const auto line = readline(m_err);
            Logger::FError("[external compiler {}]: {}", pid, line);
          }
        }
      } catch (const std::exception& exc) {
        // The stderr output messes with expected test output, which presumably
        // come from non-server runs.
        if (RuntimeOption::ServerMode) {
          Logger::FVerbose(
            "Ceasing to log stderr from external compiler ({}): {}",
            pid,
            exc.what());
        }
      }
    });

  // Here we expect the very first communication from the external compiler
  // process to be a single line of JSON representing the compiler version.
  try {
    m_version = readVersion();
  } catch (const CompileException& exc) {
    throw BadCompilerException(
      "Couldn't read version message from external compiler");
  }

  // For...reasons...the external compiler process misses the first line of
  // output on the pipe, so we open communications with a single newline.
  if (fprintf(m_in, "\n") == -1) {
    throw BadCompilerException("Couldn't write initial newline");
  }
  fflush(m_in);

  writeConfigs();
}

bool createHackc(const std::string& path, const std::string& binary) {
  if (access(path.c_str(), R_OK|X_OK) == 0) {
    auto const fd = open(path.c_str(), O_RDONLY);
    if (fd != -1) {
      SCOPE_EXIT { close(fd); };
      std::string contents;
      if (folly::readFile(fd, contents) && contents == binary) return true;
    }
  }
  try {
    folly::writeFileAtomic(path, binary, 0755);
  } catch (std::system_error& ex) {
    return false;
  }
  return true;
}

std::string hackcCommand() {
  static const std::string hackc_command = [&] () -> std::string {
    if (!RuntimeOption::EvalHackCompilerUseEmbedded) {
      return RuntimeOption::EvalHackCompilerCommand;
    }

    auto const loc = [&] (const std::string& s) {
      return s + " " + RuntimeOption::EvalHackCompilerArgs;
    };

    auto const trust = RuntimeOption::EvalHackCompilerTrustExtract;
    auto const location = RuntimeOption::HackCompilerExtractPath;
    auto const fallback = RuntimeOption::HackCompilerExtractFallback;
    // As an optimization we can just choose to trust the extracted version
    // without reading it.
    if (trust && access(location.c_str(), X_OK) == 0) return loc(location);
    if (trust && access(fallback.c_str(), X_OK) == 0) return loc(fallback);

    embedded_data desc;
    if (!get_embedded_data("hackc_binary", &desc)) {
      Logger::Error("Embedded hackc binary is missing");
      return RuntimeOption::EvalHackCompilerCommand;
    }
    auto const gz_binary = read_embedded_data(desc);
    int len = safe_cast<int>(gz_binary.size());
    auto const bin_str = gzdecode(gz_binary.data(), len);
    SCOPE_EXIT { free(bin_str); };
    if (!bin_str || !len) {
      Logger::Error("Embedded hackc binary could not be decompressed");
      return RuntimeOption::EvalHackCompilerCommand;
    }

    auto const binary = std::string(bin_str, len);
    if (createHackc(location, binary)) return loc(location);
    if (createHackc(fallback, binary)) return loc(fallback);

    Logger::Error("Failed to write extern hackc binary");
    return RuntimeOption::EvalHackCompilerCommand;
  }();
  return hackc_command;
}

folly::Optional<CompilerOptions> hackcConfiguration() {
  if (hackc_mode() == HackcMode::kNever) {
    return folly::none;
  }

  return CompilerOptions{
    RuntimeOption::EvalHackCompilerVerboseErrors,
    RuntimeOption::EvalHackCompilerMaxRetries,
    RuntimeOption::EvalHackCompilerWorkers,
    hackcCommand(),
    RuntimeOption::EvalHackCompilerInheritConfig,
  };
}

////////////////////////////////////////////////////////////////////////////////
}

HackcMode hackc_mode() {
  if (!RuntimeOption::EvalHackCompilerDefault) {
    return HackcMode::kNever;
  }

  if (hackcCommand() == "" || !RuntimeOption::EvalHackCompilerWorkers) {
    return HackcMode::kNever;
  }

  if (RuntimeOption::EvalHackCompilerFallback) return HackcMode::kFallback;

  return HackcMode::kFatal;
}

void CompilerManager::ensure_started() {
#ifdef __APPLE__
    return;
#endif

  if (m_started.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock<std::mutex> l(m_compilers_start_lock);
  if (m_started.load(std::memory_order_relaxed)) {
    return;
  }
  auto hackConfig = hackcConfiguration();

  if (hackConfig) {
    m_delegate = LightProcess::createDelegate();
  }

  if (hackConfig) {
    m_hackc_pool = std::make_unique<CompilerPool>(std::move(*hackConfig));
  }

  if (m_delegate != kInvalidPid && m_username) {
    std::unique_lock<std::mutex> lock(m_delegateLock);
    LightProcess::ChangeUser(m_delegate, m_username.value());
  }

  if (m_hackc_pool) m_hackc_pool->start();

  m_started.store(true, std::memory_order_release);
}

void CompilerManager::stop(bool detach_compilers) {
  if (m_hackc_pool) {
    m_hackc_pool->shutdown(detach_compilers);
    m_hackc_pool = nullptr;
  }

  close(m_delegate);
  m_delegate = kInvalidPid;
  m_started.store(false, std::memory_order_relaxed);
}

void CompilerManager::shutdown() {
  stop(false);
}

void CompilerManager::detach_after_fork() {
  stop(true);
}

CompilerPool& CompilerManager::get_hackc_pool() {
  ensure_started();
  return *m_hackc_pool;
}

void compilers_start() {
  s_manager.ensure_started();
#if FOLLY_HAVE_PTHREAD_ATFORK
  pthread_atfork(
    nullptr /* prepare */,
    nullptr /* parent */,
    compilers_detach_after_fork /* child */
  );
#endif
}

void compilers_set_user(const std::string& username) {
  s_manager.set_username(username);
}

void compilers_shutdown() {
  s_manager.shutdown();
}

void compilers_detach_after_fork() {
  s_manager.detach_after_fork();
}

CompilerResult hackc_compile(
  const char* code,
  int len,
  const char* filename,
  const MD5& md5,
  AsmCallbacks* callbacks
) {
  return s_manager.get_hackc_pool().compile(code, len, filename,md5, callbacks);
}

std::string hackc_version() {
  return s_manager.get_hackc_pool().getVersionString();
}

bool startsWith(const char* big, const char* small) {
  return strncmp(big, small, strlen(small)) == 0;
}

bool isFileHack(const char* code, size_t codeLen) {
  // if the file starts with a shebang
  if (codeLen > 2 && strncmp(code, "#!", 2) == 0) {
    // reset code to the next char after the shebang line
    const char* loc = reinterpret_cast<const char*>(
        memchr(code, '\n', codeLen));
    if (!loc) {
      return false;
    }

    ptrdiff_t offset = loc - code;
    code = loc + 1;
    codeLen -= offset + 1;
  }

  return codeLen > strlen("<?hh") && startsWith(code, "<?hh");
}

std::unique_ptr<UnitCompiler> UnitCompiler::create(const char* code,
                                                   int codeLen,
                                                   const char* filename,
                                                   const MD5& md5
) {
  s_manager.ensure_started();
  if (SystemLib::s_inited || RuntimeOption::EvalUseExternCompilerForSystemLib) {
    auto const hcMode = hackc_mode();
    if (hcMode != HackcMode::kNever && s_manager.hackc_enabled()) {
      return std::make_unique<HackcUnitCompiler>(
        code, codeLen, filename, md5, hcMode);
    }
  }

  return nullptr;
}

std::unique_ptr<UnitEmitter> HackcUnitCompiler::compile(
  AsmCallbacks* callbacks) const {
  auto res = hackc_compile(m_code,
                           m_codeLen,
                           m_filename,
                           m_md5,
                           callbacks);
  std::unique_ptr<UnitEmitter> unitEmitter;
  match<void>(
    res,
    [&] (std::unique_ptr<UnitEmitter>& ue) {
      unitEmitter = std::move(ue);
    },
    [&] (std::string& err) {
      if (m_hackcMode != HackcMode::kFallback) {
        unitEmitter = createFatalUnit(
          makeStaticString(m_filename),
          m_md5,
          FatalOp::Runtime,
          makeStaticString(err));
      }
    }
  );

  return unitEmitter;
}

////////////////////////////////////////////////////////////////////////////////
}
