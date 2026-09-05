#include "ProcessRunner.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace ProcessRunner {

std::string executableDirectory()
{
#if defined(__APPLE__)
    std::vector<char> path(4096);
    uint32_t size = (uint32_t)path.size();
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        path.assign(size + 1, '\0');
    }
    if (_NSGetExecutablePath(path.data(), &size) == 0) {
        std::error_code ec;
        fs::path executable = fs::weakly_canonical(fs::path(path.data()), ec);
        return (ec ? fs::path(path.data()) : executable).parent_path().string();
    }
#elif defined(__linux__)
    std::array<char, 4096> path{};
    ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (length > 0) {
        path[(size_t)length] = '\0';
        std::error_code ec;
        fs::path executable = fs::weakly_canonical(fs::path(path.data()), ec);
        return (ec ? fs::path(path.data()) : executable).parent_path().string();
    }
#endif
    return fs::current_path().string();
}

std::optional<std::string> findExecutable(const std::string& name)
{
    fs::path base = executableDirectory();
    for (const auto& candidate : {
             base / "tools" / name,
             base / name,
         }) {
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
    }

    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return std::nullopt;
    }

    std::stringstream stream(path_env);
    std::string directory;
    while (std::getline(stream, directory, ':')) {
        fs::path candidate = fs::path(directory) / name;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
    }

    return std::nullopt;
}

std::optional<std::string> findSystemExecutable(const std::string& name)
{
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return std::nullopt;
    }
    const fs::path base = executableDirectory();
    const fs::path bundled_tool = base / "tools" / name;
    const fs::path bundled_binary = base / name;
    std::stringstream stream(path_env);
    for (std::string directory; std::getline(stream, directory, ':');) {
        const fs::path candidate = fs::path(directory) / name;
        if (candidate == bundled_tool || candidate == bundled_binary ||
            access(candidate.c_str(), X_OK) != 0) {
            continue;
        }
        return candidate.string();
    }
    return std::nullopt;
}

int run(const std::vector<std::string>& args, std::string* output)
{
    if (args.empty()) {
        return -1;
    }

    int pipe_fd[2] = {-1, -1};
    if (output != nullptr && pipe(pipe_fd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (pipe_fd[0] != -1) {
            close(pipe_fd[0]);
            close(pipe_fd[1]);
        }
        return -1;
    }

    if (pid == 0) {
        if (output != nullptr) {
            close(pipe_fd[0]);
            dup2(pipe_fd[1], STDOUT_FILENO);
            close(pipe_fd[1]);

            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (output != nullptr) {
        close(pipe_fd[1]);
        std::array<char, 4096> buffer{};
        ssize_t bytes_read = 0;
        while ((bytes_read = read(pipe_fd[0], buffer.data(), buffer.size())) > 0) {
            output->append(buffer.data(), bytes_read);
        }
        close(pipe_fd[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int runWithCombinedOutput(const std::vector<std::string>& args, std::string* output)
{
    if (args.empty()) {
        return -1;
    }

    int pipe_fd[2] = {-1, -1};
    if (output != nullptr && pipe(pipe_fd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (pipe_fd[0] != -1) {
            close(pipe_fd[0]);
            close(pipe_fd[1]);
        }
        return -1;
    }

    if (pid == 0) {
        if (output != nullptr) {
            close(pipe_fd[0]);
            dup2(pipe_fd[1], STDOUT_FILENO);
            dup2(pipe_fd[1], STDERR_FILENO);
            close(pipe_fd[1]);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (output != nullptr) {
        close(pipe_fd[1]);
        std::array<char, 4096> buffer{};
        ssize_t bytes_read = 0;
        while ((bytes_read = read(pipe_fd[0], buffer.data(), buffer.size())) > 0) {
            output->append(buffer.data(), bytes_read);
        }
        close(pipe_fd[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool launchDetached(const std::vector<std::string>& args)
{
    if (args.empty()) {
        return false;
    }

    pid_t launcher = fork();
    if (launcher < 0) {
        return false;
    }

    if (launcher == 0) {
        pid_t child = fork();
        if (child < 0) {
            _exit(127);
        }
        if (child > 0) {
            _exit(0);
        }

        setsid();
        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            if (dev_null > STDERR_FILENO) {
                close(dev_null);
            }
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    return waitpid(launcher, &status, 0) >= 0 &&
           WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

std::string trim(std::string value)
{
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' ||
            value[start] == '\n' || value[start] == '\r')) {
        start++;
    }
    return value.substr(start);
}

}  // namespace ProcessRunner
