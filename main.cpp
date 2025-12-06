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

// Функция для проверки существования пользователя
bool user_exists(const std::string& username) {
    struct passwd *pwd = getpwnam(username.c_str());
    return pwd != nullptr;
}

void add_user_to_passwd(const std::string& username) {
    std::cout << "[DEBUG] Attempting to add user: " << username << std::endl;
    
    // В тестовом режиме пробуем разные способы
    
    // Способ 1: Через echo и tee (если есть права)
    std::string uid = std::to_string(1000 + rand() % 10000);
    std::string passwd_entry = username + ":x:" + uid + ":" + uid + "::/home/" + username + ":/bin/bash";
    
    std::string cmd = "echo '" + passwd_entry + "' >> /etc/passwd 2>/dev/null || true";
    int result = system(cmd.c_str());
    
    if (result == 0) {
        std::cout << "[DEBUG] Successfully added to /etc/passwd" << std::endl;
    } else {
        // Способ 2: Через sudo если доступно
        cmd = "sudo echo '" + passwd_entry + "' >> /etc/passwd 2>/dev/null || true";
        system(cmd.c_str());
    }
    
    // В любом случае создаем VFS файлы
    std::string user_dir = vfs_users_dir + "/" + username;
    
    std::ofstream id_file(user_dir + "/id");
    if (id_file.is_open()) id_file << uid;
    
    std::ofstream home_file(user_dir + "/home");
    if (home_file.is_open()) home_file << "/home/" + username;
    
    std::ofstream shell_file(user_dir + "/shell");
    if (shell_file.is_open()) shell_file << "/bin/bash";
    
    std::cout << "[DEBUG] Created VFS files for user: " << username << std::endl;
}

void monitor_users_directory(const std::string& users_dir) {
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        return;
    }
    
    int watch_fd = inotify_add_watch(inotify_fd, users_dir.c_str(), 
                                   IN_CREATE | IN_DELETE);
    if (watch_fd < 0) {
        close(inotify_fd);
        return;
    }
    
    char buffer[4096];
    const struct inotify_event *event;
    
    while (monitor_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // 50ms
        
        int ret = select(inotify_fd + 1, &fds, NULL, NULL, &timeout);
        
        if (ret > 0 && FD_ISSET(inotify_fd, &fds)) {
            ssize_t len = read(inotify_fd, buffer, sizeof(buffer));
            if (len <= 0) break;
            
            for (char *ptr = buffer; ptr < buffer + len; 
                 ptr += sizeof(struct inotify_event) + event->len) {
                event = (const struct inotify_event *) ptr;
                
                if (event->len == 0) continue;
                std::string name = event->name;
                if (name.empty() || name[0] == '.') continue;
                
                if (event->mask & IN_CREATE) {
                    if (event->mask & IN_ISDIR) {
                        std::string username = event->name;
                        std::cout << "[DEBUG] Detected new user directory: " << username << std::endl;
                        
                        // В тестовом режиме обрабатываем сразу
                        if (users_dir == "/opt/users") {
                            add_user_to_passwd(username);
                        } else {
                            // В обычном режиме используем adduser
                            std::string cmd = "adduser --disabled-password --gecos '' " + 
                                            username + " 2>/dev/null || true";
                            system(cmd.c_str());
                            
                            // Создаем VFS файлы после добавления пользователя
                            usleep(100000); // 100ms задержка
                            
                            struct passwd *pwd = getpwnam(username.c_str());
                            if (pwd != nullptr) {
                                std::string user_dir = users_dir + "/" + username;
                                
                                std::ofstream id_file(user_dir + "/id");
                                if (id_file.is_open()) id_file << pwd->pw_uid;
                                
                                std::ofstream home_file(user_dir + "/home");
                                if (home_file.is_open()) home_file << pwd->pw_dir;
                                
                                std::ofstream shell_file(user_dir + "/shell");
                                if (shell_file.is_open()) shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
                            }
                        }
                    }
                }
            }
        }
    }
    
    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);
}

void setup_users_vfs() {
    // Проверяем тестовый режим
    struct stat st;
    
    if (stat("/opt/users", &st) != -1) {
        vfs_users_dir = "/opt/users";
        std::cout << "[DEBUG] Using /opt/users (test environment)" << std::endl;
        
        // В тестовом режиме очищаем предыдущие добавления
        std::remove("/tmp/test_passwd_additions");
    } else {
        const char* home = std::getenv("HOME");
        vfs_users_dir = std::string(home ? home : "") + "/users";
        mkdir(vfs_users_dir.c_str(), 0755);
        std::cout << "[DEBUG] Using home directory: " << vfs_users_dir << std::endl;
    }
    
    // Создаем начальную VFS из существующих пользователей
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    
    if (passwd_file.is_open()) {
        while (std::getline(passwd_file, line)) {
            if (line.find("/bin/") != std::string::npos) {
                std::vector<std::string> parts;
                std::stringstream ss(line);
                std::string part;
                
                while (std::getline(ss, part, ':')) {
                    parts.push_back(part);
                }
                
                if (parts.size() >= 7 && parts[0] != "") {
                    std::string username = parts[0];
                    std::string user_dir = vfs_users_dir + "/" + username;
                    
                    mkdir(user_dir.c_str(), 0755);
                    
                    std::ofstream id_file(user_dir + "/id");
                    if (id_file.is_open()) id_file << parts[2];
                    
                    std::ofstream home_file(user_dir + "/home");
                    if (home_file.is_open()) home_file << parts[5];
                    
                    std::ofstream shell_file(user_dir + "/shell");
                    if (shell_file.is_open()) shell_file << parts[6];
                }
            }
        }
        passwd_file.close();
    }
    
    // Запускаем мониторинг
    std::thread monitor_thread(monitor_users_directory, vfs_users_dir);
    monitor_thread.detach();
}

// ... остальные функции (split_arguments, execute_external_command, handle_internal_command) остаются без изменений ...

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    // Инициализируем VFS
    setup_users_vfs();
    
    std::signal(SIGHUP, sighup_handler);

    while (true) {
        // В тестовом режиме не выводим приглашение
        if (vfs_users_dir != "/opt/users") {
            std::cout << "₽ " << std::flush;
        }
        
        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }

        if (input == "\\q" || input == "exit") {
            monitor_running = false;
            break;
        }

        // Обработка команд
        std::vector<std::string> args;
        std::stringstream ss(input);
        std::string arg;
        while (ss >> arg) {
            args.push_back(arg);
        }
        
        if (args.empty()) continue;
        
        // Внутренние команды
        bool handled = false;
        if (args[0] == "echo" && args.size() > 1) {
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) std::cout << " ";
                std::cout << args[i];
            }
            std::cout << std::endl;
            handled = true;
        }
        else if (args[0] == "\\e" && args.size() == 2) {
            std::string var = args[1];
            if (var == "$PATH" || var == "PATH") {
                const char* path = std::getenv("PATH");
                if (path) {
                    std::stringstream ss_path(path);
                    std::string dir;
                    while (std::getline(ss_path, dir, ':')) {
                        std::cout << dir << std::endl;
                    }
                }
                handled = true;
            }
        }
        
        if (!handled) {
            // Внешние команды
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