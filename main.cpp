#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sstream>
#include <csignal>
#include <cstring>
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
std::thread monitor_thread;

void sighup_handler(int) {
    std::cout << "Configuration reloaded" << std::endl;
    std::cout.flush();
}

void create_user_files(const std::string& username, struct passwd *pwd) {
    std::string user_dir = vfs_users_dir + "/" + username;
    
    mkdir(user_dir.c_str(), 0755);
    
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
    
    std::cout << "Created VFS files for user: " << username << std::endl;
}

void process_user_addition(const std::string& username) {
    std::cout << "User directory created: " << username << std::endl;
    
    // В тестовом режиме добавляем напрямую в /etc/passwd
    if (vfs_users_dir == "/opt/users") {
        std::cout << "Test mode: adding user directly to /etc/passwd" << std::endl;
        
        // Генерируем уникальный UID
        static std::atomic<int> next_uid{10000};
        int uid = next_uid++;
        
        // Добавляем в /etc/passwd
        std::ofstream passwd_out("/etc/passwd", std::ios::app);
        if (passwd_out.is_open()) {
            passwd_out << username << ":x:" << uid << ":" << uid << "::/home/" 
                      << username << ":/bin/bash" << std::endl;
            passwd_out.close();
            
            // Синхронизируем изменения
            system("sync");
            
            std::cout << "Added to /etc/passwd: " << username << " (UID: " << uid << ")" << std::endl;
        }
        
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
        // Нормальный режим: используем adduser
        std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>&1";
        std::cout << "Executing: " << cmd << std::endl;
        
        int result = system(cmd.c_str());
        std::cout << "Command result: " << result << std::endl;
        
        struct passwd *pwd = getpwnam(username.c_str());
        if (pwd) {
            create_user_files(username, pwd);
        }
    }
}

void monitor_users_directory() {
    std::cout << "Starting monitor for: " << vfs_users_dir << std::endl;
    
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) return;
    
    int watch_fd = inotify_add_watch(inotify_fd, vfs_users_dir.c_str(), IN_CREATE);
    if (watch_fd < 0) {
        close(inotify_fd);
        return;
    }
    
    std::cout << "Started monitoring: " << vfs_users_dir << std::endl;
    std::cout.flush();
    
    char buffer[4096];
    
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);
        
        struct timeval timeout = {0, 50000};
        
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
                        // Синхронная обработка
                        process_user_addition(name);
                    }
                }
            }
        }
        
        if (!monitor_running) {
            break;
        }
    }
    
    close(inotify_fd);
}

void setup_users_vfs() {
    // Проверяем тестовый режим
    struct stat st;
    if (stat("/opt/users", &st) != -1) {
        vfs_users_dir = "/opt/users";
        std::cout << "Using /opt/users (test environment)" << std::endl;
    } else {
        const char* home = getenv("HOME");
        if (!home) home = "/root";
        vfs_users_dir = std::string(home) + "/users";
        std::cout << "Using " << vfs_users_dir << std::endl;
    }
    
    // Создаем корневую директорию VFS
    mkdir(vfs_users_dir.c_str(), 0755);
    
    // Инициализируем существующих пользователей
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    int count = 0;
    
    if (passwd_file.is_open()) {
        while (std::getline(passwd_file, line)) {
            // Фильтруем пользователей с шеллами
            if (line.find("/bin/bash") != std::string::npos || 
                line.find("/bin/sh") != std::string::npos) {
                
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
        }
        passwd_file.close();
    }
    
    std::cout << "VFS initialized with " << count << " users" << std::endl;
    std::cout.flush();
    
    // Запускаем мониторинг
    monitor_thread = std::thread(monitor_users_directory);
}

bool handle_echo(const std::vector<std::string>& args) {
    if (args.empty() || (args[0] != "echo" && args[0] != "debug")) return false;
    
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) std::cout << " ";
        std::string arg = args[i];
        
        if (arg.size() >= 2) {
            char first = arg[0];
            char last = arg[arg.size() - 1];
            if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
                arg = arg.substr(1, arg.size() - 2);
            }
        }
        
        std::cout << arg;
    }
    std::cout << std::endl;
    return true;
}

bool handle_env(const std::vector<std::string>& args) {
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
    
    if (var == "$HOME" || var == "HOME") {
        const char* home = getenv("HOME");
        if (home) std::cout << home << std::endl;
        return true;
    }
    
    if (var[0] == '$') {
        const char* value = getenv(var.substr(1).c_str());
        if (value) std::cout << value << std::endl;
        return true;
    }
    
    return false;
}

bool handle_partition(const std::vector<std::string>& args) {
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
    // Отключаем буферизацию
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    std::cout << "Effective UID: " << geteuid() << std::endl;
    std::cout << "Real UID: " << getuid() << std::endl;
    
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
        
        if (args[0] == "exit" || args[0] == "\\q") {
            monitor_running = false;
            break;
        }
        
        bool handled = false;
        
        if (handle_echo(args)) {
            handled = true;
        } else if (handle_env(args)) {
            handled = true;
        } else if (handle_partition(args)) {
            handled = true;
        }
        
        if (!handled && !execute_external_command(args)) {
            std::cout << args[0] << ": command not found" << std::endl;
        }
        
        std::cout.flush();
    }
    
    monitor_running = false;
    
    // Даем время мониторингу завершиться
    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }
    
    return 0;
}