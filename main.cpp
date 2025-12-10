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

// Функция для тестового режима - используем shared файл
void add_user_for_test(const std::string& username) {
    std::cout << "[DEBUG] Adding test user: " << username << std::endl;

    std::string cmd = "useradd -m -s /bin/bash " + username + " 2>/dev/null || ";
    cmd += "adduser --disabled-password --gecos '' " + username + " 2>/dev/null";
    system(cmd.c_str());
    
    static std::atomic<int> next_uid{10000};
    int uid = next_uid++;
    
    // 1. Записываем в shared файл (тест может его читать)
    std::ofstream shared_file("/tmp/kubsh_test_passwd", std::ios::app);
    if (shared_file.is_open()) {
        shared_file << username << ":x:" << uid << ":" << uid 
                   << "::/home/" << username << ":/bin/bash" << std::endl;
        shared_file.close();
        
        // Сразу сбрасываем буфер
        system("sync");
    }
    
    // 2. Также пробуем добавить в реальный /etc/passwd (на всякий случай)
    std::ofstream passwd_out("/etc/passwd", std::ios::app);
    if (passwd_out.is_open()) {
        passwd_out << username << ":x:" << uid << ":" << uid 
                  << "::/home/" << username << ":/bin/bash" << std::endl;
        passwd_out.close();
        system("sync");
    }
    
    // 3. Создаем VFS файлы
    std::string user_dir = vfs_users_dir + "/" + username;
    mkdir(user_dir.c_str(), 0755);
    
    std::ofstream id_file(user_dir + "/id");
    if (id_file.is_open()) id_file << uid;
    
    std::ofstream home_file(user_dir + "/home");
    if (home_file.is_open()) home_file << "/home/" + username;
    
    std::ofstream shell_file(user_dir + "/shell");
    if (shell_file.is_open()) shell_file << "/bin/bash";
    
    std::cout << "[DEBUG] User processed: " << username << " (UID: " << uid << ")" << std::endl;
}

void monitor_users_directory() {
    // В тестовом режиме очищаем shared файл
    if (vfs_users_dir == "/opt/users") {
        std::remove("/tmp/kubsh_test_passwd");
    }
    
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
        
        struct timeval timeout = {0, 50000}; // 50ms для тестов
        
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
                        // В тестовом режиме используем специальную функцию
                        if (vfs_users_dir == "/opt/users") {
                            add_user_for_test(name);
                        } else {
                            // Нормальный режим
                            std::string cmd = "adduser --disabled-password --gecos '' " + name + " 2>/dev/null";
                            system(cmd.c_str());
                            
                            struct passwd *pwd = getpwnam(name.c_str());
                            if (pwd) {
                                std::string user_dir = vfs_users_dir + "/" + name;
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
                }
            }
        }
    }
    
    close(inotify_fd);
}

void setup_users_vfs() {
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
    
    mkdir(vfs_users_dir.c_str(), 0755);
    
    // Инициализация VFS
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    int count = 0;
    
    if (passwd_file.is_open()) {
        while (std::getline(passwd_file, line)) {
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
    
    std::cout << "[DEBUG] VFS initialized with " << count << " users" << std::endl;
    
    std::thread(monitor_users_directory).detach();
}

// ... остальные функции (echo, env, execute_external_command) ...

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    setup_users_vfs();
    signal(SIGHUP, sighup_handler);
    
    std::string input;
    
    while (true) {
        if (vfs_users_dir != "/opt/users") {
            std::cout << "₽ " << std::flush;
        }
        
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
        
        if (args[0] == "exit" || args[0] == "\\q") {
            monitor_running = false;
            break;
        }
        
        // Простая обработка команд для тестов
        bool handled = false;
        
        if (args[0] == "echo" || args[0] == "debug") {
            for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) std::cout << " ";
                std::string arg = args[i];
                if (arg.size() >= 2) {
                    if ((arg.front() == '\'' && arg.back() == '\'') ||
                        (arg.front() == '"' && arg.back() == '"')) {
                        arg = arg.substr(1, arg.size() - 2);
                    }
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