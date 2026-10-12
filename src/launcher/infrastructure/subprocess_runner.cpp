// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/subprocess_runner.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ttrally::launcher {

namespace {
constexpr std::size_t kReadChunk = 4096;
} // namespace

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        size);
    return wide;
}

/// Quotes an argument the way the Microsoft C runtime splits command lines.
std::wstring quoted(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
        } else {
            result.append(backslashes, L'\\');
        }
        backslashes = 0;
        result += c;
    }
    result.append(backslashes * 2, L'\\');
    return result + L"\"";
}

} // namespace

struct SubprocessRunner::Process {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE output = nullptr; ///< read end of the pipe

    ~Process() {
        for (HANDLE handle : {process, job, output}) {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    }
};

void SubprocessRunner::start(const Invocation& invocation) {
    if (running_) {
        throw std::logic_error("a program is already running");
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
        throw std::runtime_error("cannot create a pipe");
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = quoted(widen(invocation.program));
    for (const std::string& argument : invocation.arguments) {
        command += L" " + quoted(widen(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    const std::wstring directory = invocation.working_directory.wstring();
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &info);
    CloseHandle(write_end);
    if (!created) {
        CloseHandle(read_end);
        throw std::runtime_error("cannot start " + invocation.program);
    }
    process_ = std::make_unique<Process>();
    process_->process = info.hProcess;
    process_->output = read_end;
    process_->job = CreateJobObjectW(nullptr, nullptr); // so cancel also ends child processes
    if (process_->job != nullptr) {
        AssignProcessToJobObject(process_->job, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    {
        const std::lock_guard lock(mutex_);
        output_.clear();
        exit_code_.reset();
    }
    running_ = true;
    reader_ = std::thread([this] { read_until_end(); });
}

void SubprocessRunner::read_until_end() {
    std::array<char, kReadChunk> buffer{};
    DWORD read = 0;
    while (ReadFile(process_->output, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                    nullptr) &&
           read > 0) {
        const std::lock_guard lock(mutex_);
        output_.append(buffer.data(), read);
    }
    WaitForSingleObject(process_->process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_->process, &code);
    {
        const std::lock_guard lock(mutex_);
        exit_code_ = static_cast<int>(code);
    }
    running_ = false;
}

void SubprocessRunner::cancel() {
    if (running_ && process_) {
        if (process_->job != nullptr) {
            TerminateJobObject(process_->job, 1);
        } else {
            TerminateProcess(process_->process, 1);
        }
    }
}

std::filesystem::path current_executable() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

#else // POSIX

struct SubprocessRunner::Process {
    pid_t pid = -1;
    int output = -1; ///< read end of the pipe

    ~Process() {
        if (output >= 0) {
            close(output);
        }
    }
};

void SubprocessRunner::start(const Invocation& invocation) {
    if (running_) {
        throw std::logic_error("a program is already running");
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    std::array<int, 2> pipe_ends{};
    if (pipe(pipe_ends.data()) != 0) {
        throw std::runtime_error("cannot create a pipe");
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
    const std::string directory = invocation.working_directory.string();
    if (!directory.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP); // own group: cancel all of it
    posix_spawnattr_setpgroup(&attributes, 0);

    std::vector<std::string> words{invocation.program};
    words.insert(words.end(), invocation.arguments.begin(), invocation.arguments.end());
    std::vector<char*> argv;
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int result = posix_spawnp(&pid, invocation.program.c_str(), &actions, &attributes,
                                    argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipe_ends[1]);
    if (result != 0) {
        close(pipe_ends[0]);
        throw std::runtime_error("cannot start " + invocation.program + ": " +
                                 std::strerror(result));
    }
    process_ = std::make_unique<Process>();
    process_->pid = pid;
    process_->output = pipe_ends[0];
    {
        const std::lock_guard lock(mutex_);
        output_.clear();
        exit_code_.reset();
    }
    running_ = true;
    reader_ = std::thread([this] { read_until_end(); });
}

void SubprocessRunner::read_until_end() {
    std::array<char, kReadChunk> buffer{};
    for (;;) {
        const ssize_t count = read(process_->output, buffer.data(), buffer.size());
        if (count <= 0) {
            break;
        }
        const std::lock_guard lock(mutex_);
        output_.append(buffer.data(), static_cast<std::size_t>(count));
    }
    int status = 0;
    waitpid(process_->pid, &status, 0);
    {
        const std::lock_guard lock(mutex_);
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    }
    running_ = false;
}

void SubprocessRunner::cancel() {
    if (running_ && process_) {
        kill(-process_->pid, SIGTERM); // the whole process group
    }
}

std::filesystem::path current_executable() {
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path("ttrally") : path;
}

#endif

SubprocessRunner::SubprocessRunner() = default;

SubprocessRunner::~SubprocessRunner() {
    cancel();
    if (reader_.joinable()) {
        reader_.join();
    }
}

bool SubprocessRunner::running() const { return running_; }

std::string SubprocessRunner::take_output() {
    const std::lock_guard lock(mutex_);
    return std::exchange(output_, {});
}

std::optional<int> SubprocessRunner::exit_code() const {
    const std::lock_guard lock(mutex_);
    return exit_code_;
}

} // namespace ttrally::launcher
