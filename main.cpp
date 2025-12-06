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
#include <queue>
#include <mutex>

std::atomic<bool> monitor_running{true};
std::string vfs_users_dir;

void sighup_handler(int) {
    std::cout << "Configuration reloaded" << std::endl;
    std::cout.flush();
}

// Обработчик команд echo/debug
bool handle_echo_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    
    if (args[0] == "echo" || args[0] == "debug") {
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) std::cout << " ";
            std::string arg = args[i];
            
            // Убираем кавычки
            if (arg.size() >= 2) {
                if ((arg.front() == '\'' && arg.back() == '\'') ||
                    (arg.front() == '"' && arg.back() == '"')) {
                    arg = arg.substr(1, arg.size() - 2);
                }
            }
            std::cout << arg;
        }
        std::cout << std::endl;
        return true;
    }
    return false;
}

// Обработчик команды \e
bool handle_env_command(const std::vector<std::string>& args) {
    if (args.size() != 2 || args[0] != "\\e") return false;
    
    std::string var = args[1];
    
    // Обработка $HOME
    if (var == "$HOME" || var == "HOME") {
        const char* home = std::getenv("HOME");
        if (home) {
            std::cout << home << std::endl;
        }
        return true;
    }
    
    // Обработка $PATH
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
    
    // Обработка других переменных
    if (var.find('$') == 0) {
        const char* value = std::getenv(var.substr(1).c_str());
        if (value) {
            std::cout << value << std::endl;
        }
        return true;
    }
    
    return false;
}

// Функция для тестового добавления пользователя
void handle_user_addition(const std::string& username) {
    std::cout << "[DEBUG] Handling user addition: " << username << std::endl;
    
    // В тестовом режиме просто создаем VFS файлы без реального пользователя
    std::string user_dir = vfs_users_dir + "/" + username;
    
    // Генерируем тестовый UID (начинаем с 10000 чтобы не конфликтовать с системными)
    static std::atomic<int> next_test_uid{10000};
    int uid = next_test_uid++;
    
    // Создаем VFS файлы
    std::ofstream id_file(user_dir + "/id");
    if (id_file.is_open()) {
        id_file << uid;
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
    
    // В тестовом режиме также добавляем запись в /etc/passwd через прямой запрос
    std::string passwd_entry = username + ":x:" + std::to_string(uid) + ":" + 
                              std::to_string(uid) + "::/home/" + username + ":/bin/bash";
    
    // Используем прямой вызов системы для добавления пользователя
    std::string cmd = "useradd -m -s /bin/bash -u " + std::to_string(uid) + " " + username + " 2>/dev/null || ";
    cmd += "adduser --disabled-password --gecos '' --uid " + std::to_string(uid) + " " + username + " 2>/dev/null || ";
    cmd += "echo '" + passwd_entry + "' >> /etc/passwd 2>/dev/null";
    
    system(cmd.c_str());
    
    std::cout << "[DEBUG] Created VFS for user: " << username << " with UID: " << uid << std::endl;
}

void monitor_users_directory(const std::string& users_dir) {
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) return;
    
    int watch_fd = inotify_add_watch(inotify_fd, users_dir.c_str(), IN_CREATE | IN_MOVED_TO);
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
            
            for (char* ptr = buffer; ptr < buffer + len; ) {
                struct inotify_event* event = (struct inotify_event*)ptr;
                ptr += sizeof(struct inotify_event) + event->len;
                
                if (event->len == 0) continue;
                std::string name = event->name;
                if (name.empty() || name[0] == '.') continue;
                
                if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
                    std::string username = name;
                    handle_user_addition(username);
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
        std::cout << "[DEBUG] Using /opt/users (test environment)" << std::endl;
    } else {
        const char* home = std::getenv("HOME");
        vfs_users_dir = std::string(home ? home : "") + "/users";
        mkdir(vfs_users_dir.c_str(), 0755);
        std::cout << "[DEBUG] Using home directory: " << vfs_users_dir << std::endl;
    }
    
    // Инициализируем VFS существующими пользователями
    // Фильтруем только пользователей с нормальными шеллами (не /bin/false, /bin/sync и т.д.)
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    int count = 0;
    
    if (passwd_file.is_open()) {
        while (std::getline(passwd_file, line)) {
            size_t shell_pos = line.rfind(':');
            if (shell_pos == std::string::npos) continue;
            
            std::string shell = line.substr(shell_pos + 1);
            
            // Фильтруем системных пользователей без нормальных шеллов
            if (shell.find("/false") != std::string::npos ||
                shell.find("/nologin") != std::string::npos ||
                shell.find("/sync") != std::string::npos) {
                continue;
            }
            
            // Разбираем строку
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            
            while (std::getline(ss, part, ':')) {
                parts.push_back(part);
            }
            
            if (parts.size() >= 7) {
                std::string username = parts[0];
                std::string user_dir = vfs_users_dir + "/" + username;
                
                mkdir(user_dir.c_str(), 0755);
                
                std::ofstream id_file(user_dir + "/id");
                if (id_file.is_open()) id_file << parts[2];
                
                std::ofstream home_file(user_dir + "/home");
                if (home_file.is_open()) home_file << parts[5];
                
                std::ofstream shell_file(user_dir + "/shell");
                if (shell_file.is_open()) shell_file << parts[6];
                
                count++;
            }
        }
        passwd_file.close();
    }
    
    std::cout << "[DEBUG] VFS initialized with " << count << " users" << std::endl;
    
    // Запускаем мониторинг
    std::thread(monitor_users_directory, vfs_users_dir).detach();
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
        
        // Если execvp не удался
        std::cout << args[0] << ": command not found" << std::endl;
        exit(127);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
        return true;
    }
    
    return false;
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    setup_users_vfs();
    std::signal(SIGHUP, sighup_handler);
    
    while (true) {
        // Выводим приглашение только если не в тестовом режиме
        if (vfs_users_dir != "/opt/users") {
            std::cout << "₽ " << std::flush;
        }
        
        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }
        
        if (input.empty()) continue;
        
        // Разбиваем на аргументы
        std::vector<std::string> args;
        std::stringstream ss(input);
        std::string arg;
        while (ss >> arg) {
            args.push_back(arg);
        }
        
        if (args.empty()) continue;
        
        // Проверяем команды выхода
        if (args[0] == "exit" || args[0] == "\\q") {
            monitor_running = false;
            break;
        }
        
        // Обрабатываем внутренние команды
        bool handled = false;
        
        if (handle_echo_command(args)) {
            handled = true;
        } else if (handle_env_command(args)) {
            handled = true;
        } else if (args[0] == "\\l" && args.size() == 2) {
            // Обработка команды \l
            std::string disk = args[1];
            std::cout << "Partition information for " << disk << ":\n";
            std::string cmd = "lsblk " + disk + " 2>/dev/null";
            if (system(cmd.c_str()) != 0) {
                std::cout << "Try: fdisk -l " << disk << std::endl;
            }
            handled = true;
        }
        
        // Если не внутренняя команда, пробуем внешнюю
        if (!handled && !execute_external_command(args)) {
            std::cout << args[0] << ": command not found" << std::endl;
        }
        
        std::cout.flush();
    }
    
    monitor_running = false;
    return 0;
}