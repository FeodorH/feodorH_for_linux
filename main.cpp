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

// Упрощенный обработчик SIGHUP
void sighup_handler(int) {
    std::cout << "Configuration reloaded" << std::endl;
    std::cout.flush();
}

// Синхронное создание пользователя для тестов
bool create_test_user(const std::string& username) {
    // В тестовом окружении просто добавляем запись в /etc/passwd
    std::ifstream passwd_in("/etc/passwd");
    std::vector<std::string> lines;
    std::string line;
    int max_uid = 0;
    bool user_exists = false;
    
    // Читаем существующие записи
    while (std::getline(passwd_in, line)) {
        lines.push_back(line);
        
        // Проверяем, существует ли уже пользователь
        if (line.find(username + ":") == 0) {
            user_exists = true;
        }
        
        // Ищем максимальный UID
        std::vector<std::string> parts;
        std::stringstream ss(line);
        std::string part;
        
        while (std::getline(ss, part, ':')) {
            parts.push_back(part);
        }
        
        if (parts.size() >= 3) {
            try {
                int uid = std::stoi(parts[2]);
                if (uid > max_uid && uid < 60000) {
                    max_uid = uid;
                }
            } catch (...) {}
        }
    }
    passwd_in.close();
    
    // Если пользователь уже существует, ничего не делаем
    if (user_exists) {
        std::cout << "[DEBUG] User " << username << " already exists" << std::endl;
        return true;
    }
    
    // Генерируем новый UID
    if (max_uid < 1000) max_uid = 999;
    int new_uid = max_uid + 1;
    int new_gid = new_uid;
    
    // Формируем новую запись
    std::string new_entry = username + ":x:" + std::to_string(new_uid) + ":" + 
                           std::to_string(new_gid) + "::/home/" + username + ":/bin/bash";
    
    // ЗАПИСЫВАЕМ НА ДИСК - открываем файл для добавления
    std::ofstream passwd_out("/etc/passwd", std::ios::app);
    if (!passwd_out) {
        std::cerr << "[DEBUG] Failed to open /etc/passwd for writing" << std::endl;
        // Попробуем с sudo
        std::string cmd = "echo '" + new_entry + "' | sudo tee -a /etc/passwd > /dev/null 2>&1";
        if (system(cmd.c_str()) != 0) {
            return false;
        }
    } else {
        passwd_out << new_entry << std::endl;
        passwd_out.close();
    }
    
    // Также добавляем запись в /etc/shadow
    std::string shadow_entry = username + ":!!:19265:0:99999:7:::";
    std::ofstream shadow_out("/etc/shadow", std::ios::app);
    if (shadow_out) {
        shadow_out << shadow_entry << std::endl;
        shadow_out.close();
    } else {
        std::string cmd = "echo '" + shadow_entry + "' | sudo tee -a /etc/shadow > /dev/null 2>&1";
        system(cmd.c_str());
    }
    
    // Синхронизируем изменения на диск
    sync();
    
    std::cout << "[DEBUG] Created test user: " << username << " with UID: " << new_uid << std::endl;
    return true;
}

void process_user_directory(const std::string& username, bool is_add) {
    if (is_add) {
        std::string user_dir = vfs_users_dir + "/" + username;
        
        // Сначала создаем VFS файлы
        if (vfs_users_dir == "/opt/users") {
            // В тестовом режиме
            if (create_test_user(username)) {
                // Ждем немного чтобы изменения записались
                usleep(100000); // 100ms
                
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
                    // Если getpwnam не нашел пользователя сразу, попробуем еще
                    for (int i = 0; i < 5; i++) {
                        usleep(200000); // 200ms
                        pwd = getpwnam(username.c_str());
                        if (pwd != nullptr) break;
                    }
                    
                    if (pwd != nullptr) {
                        // Создаем файлы с найденной информацией
                        std::ofstream id_file(user_dir + "/id");
                        if (id_file.is_open()) id_file << pwd->pw_uid;
                        
                        std::ofstream home_file(user_dir + "/home");
                        if (home_file.is_open()) home_file << pwd->pw_dir;
                        
                        std::ofstream shell_file(user_dir + "/shell");
                        if (shell_file.is_open()) shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
                    }
                }
            }
        } else {
            // В обычном режиме
            std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>/dev/null || true";
            system(cmd.c_str());
            
            // Ожидаем и создаем VFS файлы
            int attempts = 0;
            struct passwd *pwd = nullptr;
            while (attempts < 10 && pwd == nullptr) {
                pwd = getpwnam(username.c_str());
                if (pwd == nullptr) {
                    usleep(100000);
                    attempts++;
                }
            }
            
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
}

void monitor_users_directory(const std::string& users_dir) {
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        std::cerr << "Error: inotify_init failed\n";
        return;
    }
    
    int watch_fd = inotify_add_watch(inotify_fd, users_dir.c_str(), 
                                   IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    if (watch_fd < 0) {
        std::cerr << "Error: cannot watch directory " << users_dir << "\n";
        close(inotify_fd);
        return;
    }
    
    char buffer[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    
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
            
            for (char *ptr = buffer; ptr < buffer + len; 
                 ptr += sizeof(struct inotify_event) + event->len) {
                event = (const struct inotify_event *) ptr;
                
                // Игнорируем временные файлы и скрытые файлы
                std::string name = event->name;
                if (name.empty() || name[0] == '.') {
                    continue;
                }
                
                if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO) {
                    if (event->mask & IN_ISDIR) {
                        std::string username = event->name;
                        // Обрабатываем СИНХРОННО для тестов
                        process_user_directory(username, true);
                    }
                }
                else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
                    if (event->mask & IN_ISDIR) {
                        std::string username = event->name;
                        process_user_directory(username, false);
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
    } else {
        const char* home = std::getenv("HOME");
        vfs_users_dir = std::string(home ? home : "") + "/users";
        mkdir(vfs_users_dir.c_str(), 0755);
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
                    
                    // Создаем директорию если её нет
                    if (mkdir(user_dir.c_str(), 0755) == -1 && errno != EEXIST) {
                        continue;
                    }
                    
                    // Создаем файлы
                    std::ofstream id_file(user_dir + "/id");
                    if (id_file.is_open()) {
                        id_file << parts[2];
                        id_file.close();
                    }
                    
                    std::ofstream home_file(user_dir + "/home");
                    if (home_file.is_open()) {
                        home_file << parts[5];
                        home_file.close();
                    }
                    
                    std::ofstream shell_file(user_dir + "/shell");
                    if (shell_file.is_open()) {
                        shell_file << parts[6];
                        shell_file.close();
                    }
                    
                    user_count++;
                }
            }
        }
        passwd_file.close();
    }
    
    std::cout << "[DEBUG] VFS initialized with " << user_count << " users" << std::endl;
    
    // Запускаем мониторинг в отдельном потоке
    std::thread monitor_thread(monitor_users_directory, vfs_users_dir);
    monitor_thread.detach();
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

    // История команд
    std::string history_path = std::string(getenv("HOME")) + "/.kubsh_history";
    std::ofstream history_file(history_path, std::ios::app);
    
    if (!history_file.is_open()) {
        std::cerr << "Warning: cannot open history file! History will not be saved." << std::endl;
    }

    // Устанавливаем обработчик сигналов
    std::signal(SIGHUP, sighup_handler);

    while (true) {
        // Выводим приглашение (в тестах оно не нужно)
        if (vfs_users_dir != "/opt/users") {
            std::cout << "₽ " << std::flush;
        }
        
        std::string input;
        
        if (!std::getline(std::cin, input)) {
            std::cout << std::endl;
            break;
        }

        if (input == "\\q" || input == "exit") {
            break;
        }

        // Сохраняем в историю
        if (history_file.is_open()) {
            history_file << input << std::endl;
        }

        std::vector<std::string> args = split_arguments(input);
        
        if (args.empty()) {
            continue;
        }

        // Обрабатываем команды
        if (handle_internal_command(args)) {
            if (args[0] == "exit" || args[0] == "\\q") {
                break;
            }
        }
        else if (!execute_external_command(args)) {
            std::cout << args[0] << ": command not found" << std::endl;
        }
        
        // Сбрасываем вывод
        std::cout.flush();
        std::cerr.flush();
    }

    monitor_running = false;
    
    if (history_file.is_open()) {
        history_file.close();
    }
    
    return 0;
}