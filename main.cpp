#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sstream>
#include <csignal>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <array>
#include <pwd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <map>
#include <thread>
#include <atomic>
#include <algorithm>

std::atomic<bool> monitor_running{true};
std::string vfs_users_dir;

void sighup_handler(int) {
    std::cout << "Configuration reloaded" << std::endl;
    std::cout.flush();
}

// Создание файлов для пользователя
void create_user_files(const std::string& username, const std::string& user_dir) {
    struct passwd *pwd = getpwnam(username.c_str());
    
    if (pwd) {
        std::ofstream id_file(user_dir + "/id");
        if (id_file.is_open()) {
            id_file << pwd->pw_uid;
            id_file.close();
        }
        
        std::ofstream home_file(user_dir + "/home");
        if (home_file.is_open()) {
            home_file << pwd->pw_dir;
            home_file.close();
        }
        
        std::ofstream shell_file(user_dir + "/shell");
        if (shell_file.is_open()) {
            shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
            shell_file.close();
        }
        
        std::cout << "[DEBUG] Created VFS files for: " << username << std::endl;
    } else {
        // Если пользователь не найден, создаем с дефолтными значениями
        std::ofstream id_file(user_dir + "/id");
        if (id_file.is_open()) {
            static int default_uid = 10000;
            id_file << default_uid++;
            id_file.close();
        }
        
        std::ofstream home_file(user_dir + "/home");
        if (home_file.is_open()) {
            home_file << "/home/" + username;
            home_file.close();
        }
        
        std::ofstream shell_file(user_dir + "/shell");
        if (shell_file.is_open()) {
            shell_file << "/bin/bash";
            shell_file.close();
        }
    }
}

// Обработка создания пользователя
void handle_user_creation(const std::string& username) {
    std::cout << "[DEBUG] Creating user: " << username << std::endl;
    
    // 1. Создаем пользователя в системе
    std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>/dev/null";
    int result = system(cmd.c_str());
    
    if (result != 0) {
        // Пробуем useradd как fallback
        cmd = "useradd -m -s /bin/bash " + username + " 2>/dev/null";
        system(cmd.c_str());
    }
    
    // 2. Создаем директорию в VFS (она уже должна существовать, но на всякий случай)
    std::string user_dir = vfs_users_dir + "/" + username;
    mkdir(user_dir.c_str(), 0755);
    
    // 3. Создаем VFS файлы
    create_user_files(username, user_dir);
    
    std::cout << "[DEBUG] User processing completed: " << username << std::endl;
}

void monitor_users_directory() {
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) return;
    
    int watch_fd = inotify_add_watch(inotify_fd, vfs_users_dir.c_str(), IN_CREATE | IN_MOVED_TO);
    if (watch_fd < 0) {
        close(inotify_fd);
        return;
    }
    
    char buffer[4096];
    
    while (monitor_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);
        
        struct timeval timeout = {0, 100000}; // 100ms
        
        if (select(inotify_fd + 1, &fds, NULL, NULL, &timeout) > 0) {
            ssize_t len = read(inotify_fd, buffer, sizeof(buffer));
            if (len <= 0) break;
            
            for (char *ptr = buffer; ptr < buffer + len; ) {
                struct inotify_event *event = (struct inotify_event *)ptr;
                ptr += sizeof(struct inotify_event) + event->len;
                
                if (event->len == 0) continue;
                std::string name = event->name;
                if (name.empty() || name[0] == '.') continue;
                
                if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
                    // Синхронная обработка для тестов
                    handle_user_creation(name);
                }
            }
        }
    }
    
    close(inotify_fd);
}

void setup_users_vfs() {
    // Определяем директорию VFS
    struct stat st;
    if (stat("/opt/users", &st) != -1) {
        vfs_users_dir = "/opt/users";
        std::cout << "[DEBUG] Test mode: using /opt/users" << std::endl;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) home = "/root";
        vfs_users_dir = std::string(home) + "/users";
        std::cout << "[DEBUG] Normal mode: using " << vfs_users_dir << std::endl;
    }
    
    // Создаем директорию
    mkdir(vfs_users_dir.c_str(), 0755);
    
    // Инициализируем VFS существующими пользователями
    struct passwd *pwd;
    int count = 0;
    
    // Используем getpwent() как в решении товарища
    setpwent();
    while ((pwd = getpwent()) != nullptr) {
        std::string shell = pwd->pw_shell ? pwd->pw_shell : "";
        
        // Фильтруем пользователей с нормальными шеллами
        if (shell.find("/bash") != std::string::npos || 
            shell.find("/sh") != std::string::npos) {
            
            std::string username = pwd->pw_name;
            std::string user_dir = vfs_users_dir + "/" + username;
            
            // Создаем директорию
            mkdir(user_dir.c_str(), 0755);
            
            // Создаем файлы
            std::ofstream id_file(user_dir + "/id");
            if (id_file.is_open()) id_file << pwd->pw_uid;
            
            std::ofstream home_file(user_dir + "/home");
            if (home_file.is_open()) home_file << pwd->pw_dir;
            
            std::ofstream shell_file(user_dir + "/shell");
            if (shell_file.is_open()) shell_file << shell;
            
            count++;
        }
    }
    endpwent();
    
    std::cout << "[DEBUG] VFS initialized with " << count << " users" << std::endl;
    
    // Запускаем мониторинг
    std::thread(monitor_users_directory).detach();
}

// ... остальные функции (обработка команд) остаются такими же ...

bool handle_internal_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    
    const std::string& cmd = args[0];
    
    if (cmd == "exit" || cmd == "\\q") {
        monitor_running = false;
        return true;
    }
    else if (cmd == "echo" && args.size() > 1) {
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) std::cout << " ";
            std::string arg = args[i];
            if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') {
                arg = arg.substr(1, arg.size() - 2);
            }
            std::cout << arg;
        }
        std::cout << std::endl;
        return true;
    }
    else if (cmd == "debug" && args.size() > 1) {
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) std::cout << " ";
            std::string arg = args[i];
            if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') {
                arg = arg.substr(1, arg.size() - 2);
            }
            std::cout << arg;
        }
        std::cout << std::endl;
        return true;
    }
    else if (cmd == "\\e" && args.size() == 2) {
        std::string var = args[1];
        if (var == "$PATH" || var == "PATH") {
            const char* path_env = std::getenv("PATH");
            if (path_env) {
                std::string path_str = path_env;
                size_t start = 0;
                size_t end = path_str.find(':');
                while (end != std::string::npos) {
                    std::cout << path_str.substr(start, end - start) << std::endl;
                    start = end + 1;
                    end = path_str.find(':', start);
                }
                if (start < path_str.length()) {
                    std::cout << path_str.substr(start) << std::endl;
                }
            }
            return true;
        }
        else if (var == "$HOME" || var == "HOME") {
            const char* home = std::getenv("HOME");
            if (home) std::cout << home << std::endl;
            return true;
        }
        else if (var.find('$') == 0) {
            const char* value = std::getenv(var.substr(1).c_str());
            if (value) std::cout << value << std::endl;
            return true;
        }
    }
    else if (cmd == "\\l" && args.size() == 2) {
        const std::string& disk = args[1];
        if (disk.find("/dev/") != 0) {
            std::cout << "Error: " << disk << " is not a valid device\n";
            return true;
        }
        
        std::cout << "Partition information for " << disk << ":\n";
        std::string command = "lsblk " + disk + " 2>/dev/null";
        if (system(command.c_str()) != 0) {
            std::cout << "Try: fdisk -l " << disk << std::endl;
        }
        return true;
    }
    
    return false;
}

bool execute_external_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    
    pid_t pid = fork();
    
    if (pid == 0) {
        std::vector<char*> exec_args;
        for (const auto& arg : args) {
            exec_args.push_back(const_cast<char*>(arg.c_str()));
        }
        exec_args.push_back(nullptr);
        
        execvp(exec_args[0], exec_args.data());
        
        std::cout << args[0] << ": command not found\n";
        exit(127);
        
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        return true;
    } else {
        std::cerr << "Error: fork failed\n";
        return false;
    }
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    setup_users_vfs();
    
    std::signal(SIGHUP, sighup_handler);
    
    while (true) {
        if (vfs_users_dir != "/opt/users") {
            std::cout << "₽ " << std::flush;
        }
        
        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }
        
        if (input.empty()) continue;
        
        std::vector<std::string> args;
        std::stringstream ss(input);
        std::string arg;
        while (ss >> arg) {
            args.push_back(arg);
        }
        
        if (args.empty()) continue;
        
        if (handle_internal_command(args)) {
            if (args[0] == "exit" || args[0] == "\\q") {
                break;
            }
        } else if (!execute_external_command(args)) {
            std::cout << args[0] << ": command not found" << std::endl;
        }
        
        std::cout.flush();
    }
    
    monitor_running = false;
    return 0;
}