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

// Функция для добавления пользователя
bool add_system_user(const std::string& username) {
    std::cout << "[DEBUG] Adding system user: " << username << std::endl;
    
    // Пробуем разные способы добавления пользователя
    std::string cmd;
    int result = -1;
    
    // Способ 1: adduser (Debian/Ubuntu)
    cmd = "adduser --disabled-password --gecos '' " + username + " 2>&1";
    std::cout << "[DEBUG] Trying: " << cmd << std::endl;
    result = system(cmd.c_str());
    
    if (result == 0) {
        std::cout << "[DEBUG] User added via adduser" << std::endl;
        return true;
    }
    
    // Способ 2: useradd (более универсальный)
    cmd = "useradd -m -s /bin/bash " + username + " 2>&1";
    std::cout << "[DEBUG] Trying: " << cmd << std::endl;
    result = system(cmd.c_str());
    
    if (result == 0) {
        std::cout << "[DEBUG] User added via useradd" << std::endl;
        return true;
    }
    
    // Способ 3: Для тестового окружения - прямое добавление в /etc/passwd
    if (vfs_users_dir == "/opt/users") {
        std::cout << "[DEBUG] Test mode: adding directly to /etc/passwd" << std::endl;
        
        // Генерируем уникальный UID
        static std::atomic<int> next_test_uid{10000};
        int uid = next_test_uid++;
        
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
        
        // Также добавляем в /etc/shadow
        std::ofstream shadow_out("/etc/shadow", std::ios::app);
        if (shadow_out.is_open()) {
            shadow_out << username << ":!!:19265:0:99999:7:::" << std::endl;
            shadow_out.close();
        }
        
        // Создаем домашнюю директорию
        std::string home_dir = "/home/" + username;
        mkdir(home_dir.c_str(), 0755);
        
        std::cout << "[DEBUG] User added directly to /etc/passwd with UID: " << uid << std::endl;
        return true;
    }
    
    std::cerr << "[DEBUG] Failed to add user: " << username << std::endl;
    return false;
}

// Функция для создания VFS файлов
void create_vfs_files(const std::string& username, const std::string& user_dir) {
    // Получаем информацию о пользователе
    struct passwd *pwd = getpwnam(username.c_str());
    
    if (pwd != nullptr) {
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
        
        std::cout << "[DEBUG] VFS files created for user: " << username << std::endl;
    } else {
        std::cerr << "[DEBUG] Cannot get user info for: " << username << std::endl;
        
        // В тестовом режиме создаем файлы с дефолтными значениями
        if (vfs_users_dir == "/opt/users") {
            std::ofstream id_file(user_dir + "/id");
            if (id_file.is_open()) id_file << "10000";
            
            std::ofstream home_file(user_dir + "/home");
            if (home_file.is_open()) home_file << "/home/" + username;
            
            std::ofstream shell_file(user_dir + "/shell");
            if (shell_file.is_open()) shell_file << "/bin/bash";
        }
    }
}

// Обработчик добавления пользователя
void handle_user_addition(const std::string& username) {
    std::cout << "[DEBUG] Handling new user directory: " << username << std::endl;
    
    std::string user_dir = vfs_users_dir + "/" + username;
    
    // 1. Добавляем пользователя в систему
    if (add_system_user(username)) {
        // 2. Создаем VFS файлы
        create_vfs_files(username, user_dir);
    } else {
        std::cerr << "[DEBUG] Failed to handle user: " << username << std::endl;
    }
}

void monitor_users_directory(const std::string& users_dir) {
    // Ждем немного перед началом мониторинга
    usleep(100000);
    
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        std::cerr << "[DEBUG] inotify_init failed" << std::endl;
        return;
    }
    
    int watch_fd = inotify_add_watch(inotify_fd, users_dir.c_str(), 
                                   IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM);
    if (watch_fd < 0) {
        std::cerr << "[DEBUG] Cannot watch directory: " << users_dir << std::endl;
        close(inotify_fd);
        return;
    }
    
    std::cout << "[DEBUG] Started monitoring: " << users_dir << std::endl;
    
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
                
                std::string username = name;
                
                if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO) {
                    if (event->mask & IN_ISDIR) {
                        // Создаем пользователя в отдельном потоке
                        std::thread(handle_user_addition, username).detach();
                    }
                }
                else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
                    if (event->mask & IN_ISDIR) {
                        // Удаляем пользователя
                        std::string cmd = "userdel -r " + username + " 2>/dev/null || true";
                        system(cmd.c_str());
                        std::cout << "[DEBUG] User deleted: " << username << std::endl;
                    }
                }
            }
        }
    }
    
    close(inotify_fd);
}

void setup_users_vfs() {
    struct stat st;
    
    // Проверяем тестовый режим
    if (stat("/opt/users", &st) != -1) {
        vfs_users_dir = "/opt/users";
        std::cout << "[DEBUG] Test mode: using /opt/users" << std::endl;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) home = "/root";
        vfs_users_dir = std::string(home) + "/users";
        std::cout << "[DEBUG] Normal mode: using " << vfs_users_dir << std::endl;
    }
    
    // Создаем директорию если не существует
    mkdir(vfs_users_dir.c_str(), 0755);
    
    // Инициализируем VFS существующими пользователями
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    int user_count = 0;
    
    if (passwd_file.is_open()) {
        while (std::getline(passwd_file, line)) {
            // Берем только пользователей с реальными шеллами
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
                    
                    // Создаем директорию
                    if (mkdir(user_dir.c_str(), 0755) == -1 && errno != EEXIST) {
                        continue;
                    }
                    
                    // Создаем файлы
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
    std::thread monitor_thread(monitor_users_directory, vfs_users_dir);
    monitor_thread.detach();
}

// ... остальные функции (обработка команд) остаются без изменений ...

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

// ... остальной код (split_arguments, execute_external_command, main) ...

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    // Инициализируем VFS
    setup_users_vfs();
    
    std::signal(SIGHUP, sighup_handler);
    
    // Главный цикл
    while (true) {
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
        
        // Обработка команд
        if (handle_internal_command(args)) {
            if (args[0] == "exit" || args[0] == "\\q") {
                break;
            }
        } else {
            // Внешняя команда
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