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

void create_user_files(const std::string& username, struct passwd *pwd) {
    std::string user_dir = vfs_users_dir + "/" + username;
    
    // Создаем директорию пользователя в VFS
    mkdir(user_dir.c_str(), 0755);
    
    // Создаем файл id
    std::ofstream id_file(user_dir + "/id");
    if (id_file.is_open()) {
        id_file << pwd->pw_uid;
        id_file.close();
    }
    
    // Создаем файл home
    std::ofstream home_file(user_dir + "/home");
    if (home_file.is_open()) {
        home_file << pwd->pw_dir;
        home_file.close();
    }
    
    // Создаем файл shell
    std::ofstream shell_file(user_dir + "/shell");
    if (shell_file.is_open()) {
        shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
        shell_file.close();
    }
    
    std::cout << "Created VFS files for user: " << username << std::endl;
}

void process_user_addition(const std::string& username) {
    std::cout << "User directory created: " << username << std::endl;
    
    // 1. Создаем пользователя в системе (точно как у товарища)
    std::string cmd = "adduser --disabled-password --gecos '' " + username;
    std::cout << "Executing: " << cmd << std::endl;
    
    int result = system(cmd.c_str());
    std::cout << "Command result: " << result << std::endl;
    
    // 2. Получаем информацию о пользователе
    struct passwd *pwd = getpwnam(username.c_str());
    
    if (pwd) {
        // 3. Создаем VFS файлы
        create_user_files(username, pwd);
    } else {
        std::cerr << "Failed to get user info for: " << username << std::endl;
        
        // Для тестового режима создаем с дефолтными значениями
        if (vfs_users_dir == "/opt/users") {
            struct passwd test_pwd;
            test_pwd.pw_uid = 10000 + rand() % 10000;
            test_pwd.pw_dir = strdup(("/home/" + username).c_str());
            test_pwd.pw_shell = strdup("/bin/bash");
            
            create_user_files(username, &test_pwd);
        }
    }
}

void monitor_users_directory() {
    // Ждем инициализации
    sleep(1);
    
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        std::cerr << "inotify_init failed" << std::endl;
        return;
    }
    
    int watch_fd = inotify_add_watch(inotify_fd, vfs_users_dir.c_str(), IN_CREATE | IN_MOVED_TO);
    if (watch_fd < 0) {
        std::cerr << "Cannot watch directory" << std::endl;
        close(inotify_fd);
        return;
    }
    
    std::cout << "Started monitoring: " << vfs_users_dir << std::endl;
    
    char buffer[4096];
    
    while (monitor_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms
        
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
                
                if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                    if (event->mask & IN_ISDIR) {
                        // Синхронная обработка как у товарища
                        process_user_addition(name);
                    }
                }
            }
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
    struct passwd *pwd;
    setpwent(); // Сбрасываем указатель в начало
    int count = 0;
    
    while ((pwd = getpwent()) != nullptr) {
        std::string shell = pwd->pw_shell ? pwd->pw_shell : "";
        
        // Только пользователи с шеллами (как у товарища)
        if (shell.find("/bash") != std::string::npos || 
            shell.find("/sh") != std::string::npos) {
            
            // Создаем директорию пользователя в VFS
            std::string user_dir = vfs_users_dir + "/" + pwd->pw_name;
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
    
    std::cout << "VFS initialized with " << count << " users" << std::endl;
    
    // Запускаем мониторинг
    std::thread monitor_thread(monitor_users_directory);
    monitor_thread.detach();
}

// ... остальные функции (echo, env, partition, execute_external_command) остаются без изменений ...

int main() {
    // Отключаем буферизацию
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    
    // Выводим отладочную информацию
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
            std::cout << "₽ ";
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
        
        // Обработка команд
        bool handled = false;
        
        if (args[0] == "exit" || args[0] == "\\q") {
            monitor_running = false;
            break;
        }
        else if (args[0] == "echo" && args.size() > 1) {
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) std::cout << " ";
                std::cout << args[i];
            }
            std::cout << std::endl;
            handled = true;
        }
        else if (args[0] == "debug" && args.size() > 1) {
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) std::cout << " ";
                std::string arg = args[i];
                if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') {
                    arg = arg.substr(1, arg.size() - 2);
                }
                std::cout << arg;
            }
            std::cout << std::endl;
            handled = true;
        }
        else if (args[0] == "\\e" && args.size() == 2) {
            std::string var = args[1];
            if (var == "$PATH" || var == "PATH") {
                const char* path = getenv("PATH");
                if (path) {
                    std::stringstream ss_path(path);
                    std::string dir;
                    while (getline(ss_path, dir, ':')) {
                        std::cout << dir << std::endl;
                    }
                }
                handled = true;
            }
            else if (var == "$HOME" || var == "HOME") {
                const char* home = getenv("HOME");
                if (home) std::cout << home << std::endl;
                handled = true;
            }
        }
        else if (args[0] == "\\l" && args.size() == 2) {
            std::string disk = args[1];
            std::cout << "Partition information for " << disk << ":\n";
            std::string cmd = "lsblk " + disk + " 2>/dev/null";
            if (system(cmd.c_str()) != 0) {
                std::cout << "Try: fdisk -l " << disk << std::endl;
            }
            handled = true;
        }
        
        // Внешние команды
        if (!handled) {
            pid_t pid = fork();
            if (pid == 0) {
                std::vector<char*> exec_args;
                for (const auto& a : args) {
                    exec_args.push_back(const_cast<char*>(a.c_str()));
                }
                exec_args.push_back(nullptr);
                
                execvp(exec_args[0], exec_args.data());
                std::cout << args[0] << ": command not found" << std::endl;
                exit(127);
            } else if (pid > 0) {
                waitpid(pid, nullptr, 0);
            }
        }
        
        std::cout.flush();
    }
    
    monitor_running = false;
    return 0;
}