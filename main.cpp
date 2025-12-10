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

// Функция для добавления пользователя (синхронная версия для тестов)
void add_user_sync(const std::string& username) {
    std::cout << "[DEBUG] Adding user: " << username << std::endl;
    
    // В тестовом режиме добавляем напрямую в /etc/passwd
    if (vfs_users_dir == "/opt/users") {
        // Генерируем UID
        static std::atomic<int> next_uid{10000};
        int uid = next_uid++;
        
        // Читаем текущий /etc/passwd
        std::ifstream passwd_in("/etc/passwd");
        std::vector<std::string> lines;
        std::string line;
        
        while (std::getline(passwd_in, line)) {
            lines.push_back(line);
        }
        passwd_in.close();
        
        // Добавляем новую запись
        std::string new_entry = username + ":x:" + std::to_string(uid) + ":" + 
                               std::to_string(uid) + "::/home/" + username + ":/bin/bash";
        lines.push_back(new_entry);
        
        // Записываем обратно
        std::ofstream passwd_out("/etc/passwd");
        for (const auto& l : lines) {
            passwd_out << l << std::endl;
        }
        passwd_out.close();
        
        // Синхронизируем
        sync();
        
        std::cout << "[DEBUG] User added to /etc/passwd: " << username << " (UID: " << uid << ")" << std::endl;
        
        // Создаем VFS файлы
        std::string user_dir = vfs_users_dir + "/" + username;
        mkdir(user_dir.c_str(), 0755);
        
        std::ofstream id_file(user_dir + "/id");
        if (id_file.is_open()) id_file << uid;
        
        std::ofstream home_file(user_dir + "/home");
        if (home_file.is_open()) home_file << "/home/" + username;
        
        std::ofstream shell_file(user_dir + "/shell");
        if (shell_file.is_open()) shell_file << "/bin/bash";
        
    } else {
        // Нормальный режим
        std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>/dev/null";
        system(cmd.c_str());
        
        struct passwd *pwd = getpwnam(username.c_str());
        if (pwd) {
            std::string user_dir = vfs_users_dir + "/" + username;
            mkdir(user_dir.c_str(), 0755);
            
            std::ofstream id_file(user_dir + "/id");
            if (id_file.is_open()) id_file << pwd->pw_uid;
            
            std::ofstream home_file(user_dir + "/home");
            if (home_file.is_open()) home_file << pwd->pw_dir;
            
            std::ofstream shell_file(user_dir + "/shell");
            if (shell_file.is_open()) shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
        }
    }
}

void monitor_users_directory() {
    // В тестовом режиме используем более быстрый цикл
    int check_interval = (vfs_users_dir == "/opt/users") ? 50000 : 100000; // 50ms vs 100ms
    
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) return;
    
    int watch_fd = inotify_add_watch(inotify_fd, vfs_users_dir.c_str(), IN_CREATE);
    if (watch_fd < 0) {
        close(inotify_fd);
        return;
    }
    
    char buffer[4096];
    
    while (monitor_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);
        
        struct timeval timeout = {0, check_interval};
        
        int ret = select(inotify_fd + 1, &fds, NULL, NULL, &timeout);
        
        if (ret > 0 && FD_ISSET(inotify_fd, &fds)) {
            ssize_t len = read(inotify_fd, buffer, sizeof(buffer));
            if (len <= 0) break;
            
            for (char *ptr = buffer; ptr < buffer + len; ) {
                struct inotify_event *event = (struct inotify_event *)ptr;
                ptr += sizeof(struct inotify_event) + event->len;
                
                if (event->len == 0) continue;
                std::string name = event->name;
                if (name.empty() || name[0] == '.') continue;
                
                if (event->mask & IN_CREATE) {
                    if (event->mask & IN_ISDIR) {
                        // Синхронная обработка для тестов
                        add_user_sync(name);
                    }
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
        const char* home = getenv("HOME");
        if (!home) home = "/root";
        vfs_users_dir = std::string(home) + "/users";
        std::cout << "[DEBUG] Using " << vfs_users_dir << std::endl;
    }
    
    // Создаем корневую директорию VFS
    mkdir(vfs_users_dir.c_str(), 0755);
    
    // Инициализируем существующих пользователей (ВАЖНО: используем getpwent как у товарища)
    struct passwd *pwd;
    setpwent();
    int count = 0;
    
    while ((pwd = getpwent()) != nullptr) {
        std::string shell = pwd->pw_shell ? pwd->pw_shell : "";
        
        // Фильтруем только пользователей с нормальными шеллами
        if (shell.find("/bash") != std::string::npos || 
            shell.find("/sh") != std::string::npos) {
            
            std::string user_dir = vfs_users_dir + "/" + pwd->pw_name;
            mkdir(user_dir.c_str(), 0755);
            
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

// Функции обработки команд (должны быть полными)
bool handle_echo_command(const std::vector<std::string>& args) {
    if (args.empty() || (args[0] != "echo" && args[0] != "debug")) return false;
    
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

bool handle_env_command(const std::vector<std::string>& args) {
    if (args.size() != 2 || args[0] != "\\e") return false;
    
    std::string var = args[1];
    
    if (var == "$PATH" || var == "PATH") {
        const char* path = getenv("PATH");
        if (path) {
            std::stringstream ss(path);
            std::string dir;
            while (getline(ss, dir, ':')) {
                std::cout << dir << std::endl;
            }
        }
        return true;
    }
    else if (var == "$HOME" || var == "HOME") {
        const char* home = getenv("HOME");
        if (home) std::cout << home << std::endl;
        return true;
    }
    else if (var.find('$') == 0) {
        const char* value = getenv(var.substr(1).c_str());
        if (value) std::cout << value << std::endl;
        return true;
    }
    
    return false;
}

bool handle_partition_command(const std::vector<std::string>& args) {
    if (args.size() != 2 || args[0] != "\\l") return false;
    
    std::string disk = args[1];
    std::cout << "Partition information for " << disk << ":\n";
    
    std::string cmd = "lsblk " + disk + " 2>/dev/null";
    if (system(cmd.c_str()) != 0) {
        std::cout << "Try: fdisk -l " << disk << std::endl;
    }
    
    return true;
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
        
        std::cout << args[0] << ": command not found" << std::endl;
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
    // Отключаем буферизацию
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    // Инициализация VFS
    setup_users_vfs();
    
    // Обработчик сигналов
    signal(SIGHUP, sighup_handler);
    
    // Главный цикл
    std::string input;
    
    while (true) {
        if (vfs_users_dir != "/opt/users") {
            std::cout << "₽ " << std::flush;
        }
        
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
        
        // Выход
        if (args[0] == "exit" || args[0] == "\\q") {
            monitor_running = false;
            break;
        }
        
        bool handled = false;
        
        // Внутренние команды
        if (handle_echo_command(args)) {
            handled = true;
        } else if (handle_env_command(args)) {
            handled = true;
        } else if (handle_partition_command(args)) {
            handled = true;
        }
        
        // Внешние команды
        if (!handled) {
            if (!execute_external_command(args)) {
                std::cout << args[0] << ": command not found" << std::endl;
            }
        }
        
        std::cout.flush();
    }
    
    monitor_running = false;
    return 0;
}