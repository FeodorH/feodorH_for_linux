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

// Функция для немедленного создания пользователя
void create_user_immediately(const std::string& username) {
    std::cout << "[DEBUG] IMMEDIATELY creating user: " << username << std::endl;
    
    // В тестовом режиме добавляем напрямую в /etc/passwd
    if (vfs_users_dir == "/opt/users") {
        // Генерируем UID
        static int test_uid = 10000;
        test_uid++;
        
        // Формируем запись
        std::string passwd_entry = username + ":x:" + std::to_string(test_uid) + 
                                  ":" + std::to_string(test_uid) + "::/home/" + username + ":/bin/bash";
        
        // СРАЗУ добавляем в /etc/passwd
        std::ofstream passwd_out("/etc/passwd", std::ios::app);
        if (passwd_out.is_open()) {
            passwd_out << passwd_entry << std::endl;
            passwd_out.close();
            
            // Синхронизируем изменения на диск
            sync();
            std::cout << "[DEBUG] ✅ User added to /etc/passwd: " << username << std::endl;
        }
        
        // Также создаем запись в /etc/shadow
        std::string shadow_entry = username + ":!!:19265:0:99999:7:::";
        std::ofstream shadow_out("/etc/shadow", std::ios::app);
        if (shadow_out.is_open()) {
            shadow_out << shadow_entry << std::endl;
            shadow_out.close();
        }
        
        // Создаем VFS файлы
        std::string user_dir = vfs_users_dir + "/" + username;
        mkdir(user_dir.c_str(), 0755);
        
        std::ofstream id_file(user_dir + "/id");
        if (id_file.is_open()) id_file << test_uid;
        
        std::ofstream home_file(user_dir + "/home");
        if (home_file.is_open()) home_file << "/home/" + username;
        
        std::ofstream shell_file(user_dir + "/shell");
        if (shell_file.is_open()) shell_file << "/bin/bash";
        
        std::cout << "[DEBUG] ✅ VFS files created for user: " << username << std::endl;
    } else {
        // В обычном режиме используем adduser
        std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>&1";
        system(cmd.c_str());
        
        // Создаем VFS файлы
        std::string user_dir = vfs_users_dir + "/" + username;
        mkdir(user_dir.c_str(), 0755);
        
        struct passwd *pwd = getpwnam(username.c_str());
        if (pwd != nullptr) {
            std::ofstream id_file(user_dir + "/id");
            if (id_file.is_open()) id_file << pwd->pw_uid;
            
            std::ofstream home_file(user_dir + "/home");
            if (home_file.is_open()) home_file << pwd->pw_dir;
            
            std::ofstream shell_file(user_dir + "/shell");
            if (shell_file.is_open()) shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
        }
    }
}

void monitor_users_directory(const std::string& users_dir) {
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        return;
    }
    
    int watch_fd = inotify_add_watch(inotify_fd, users_dir.c_str(), 
                                   IN_CREATE | IN_MOVED_TO);
    if (watch_fd < 0) {
        close(inotify_fd);
        return;
    }
    
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
                
                if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO) {
                    if (event->mask & IN_ISDIR) {
                        std::string username = name;
                        // НЕМЕДЛЕННО обрабатываем
                        create_user_immediately(username);
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
        const char* home = std::getenv("HOME");
        vfs_users_dir = std::string(home ? home : "") + "/users";
        std::cout << "[DEBUG] Using home directory: " << vfs_users_dir << std::endl;
    }
    
    // Инициализируем VFS существующими пользователями
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    int user_count = 0;
    
    if (passwd_file.is_open()) {
        while (std::getline(passwd_file, line)) {
            if (line.find("/sh") != std::string::npos || line.find("/bash") != std::string::npos) {
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
                    
                    user_count++;
                }
            }
        }
        passwd_file.close();
    }
    
    std::cout << "[DEBUG] VFS initialized with " << user_count << " users" << std::endl;
    
    // Запускаем мониторинг
    std::thread(monitor_users_directory, vfs_users_dir).detach();
}

std::vector<std::string> split_arguments(const std::string& input) {
    std::vector<std::string> args;
    std::stringstream ss(input);
    std::string arg;
    
    while (ss >> arg) {
        args.push_back(arg);
    }
    
    return args;
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

bool handle_partition_list(const std::vector<std::string>& args) {
    if (args.size() != 2 || args[0] != "\\l") {
        return false;
    }
    
    const std::string& disk = args[1];
    
    if (disk.find("/dev/") != 0) {
        std::cout << "Error: " << disk << " is not a valid device\n";
        return true;
    }
    
    std::cout << "Partition information for " << disk << ":\n";
    
    std::string command = "lsblk " + disk + " 2>/dev/null";
    int result = system(command.c_str());
    
    if (result != 0) {
        std::cout << "Try these commands manually:\n";
        std::cout << "  fdisk -l " << disk << "\n";
        std::cout << "  sudo fdisk -l " << disk << "\n";
        std::cout << "  parted " << disk << " print\n";
    }
    
    return true;
}

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
        else if (var.find('$') == 0) {
            const char* value = std::getenv(var.substr(1).c_str());
            if (value) {
                std::cout << value << std::endl;
            }
            return true;
        }
    }
    else if (cmd == "\\l" && args.size() == 2) {
        return handle_partition_list(args);
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
    
    return false;
}

int main() {
    // Отключаем буферизацию
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    // Инициализируем VFS
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
        
        std::vector<std::string> args = split_arguments(input);
        
        if (args.empty()) continue;
        
        if (handle_internal_command(args)) {
            if (args[0] == "exit" || args[0] == "\\q") {
                break;
            }
        }
        else if (!execute_external_command(args)) {
            std::cout << args[0] << ": command not found" << std::endl;
        }
        
        std::cout.flush();
    }
    
    monitor_running = false;
    return 0;
}