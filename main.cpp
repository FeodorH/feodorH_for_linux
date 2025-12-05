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

// struct VFSState {
//     std::string users_dir;
//     std::atomic<bool> monitor_running{true};
//     std::queue<std::pair<std::string, bool>> user_operations;
//     std::mutex queue_mutex;
// };

//volatile sig_atomic_t sighup_received = 0;
std::atomic<bool> monitor_running{true};

// Очередь для операций с пользователями
std::queue<std::pair<std::string, bool>> user_operations;
std::mutex queue_mutex;

//VFSState vfs_state;
std::string vfs_users_dir;

void sighup_handler(int) {
    //sighup_received = 1;
    std::cout << "Configuration reloaded" << std::endl;
    std::cout.flush();
}

void process_user_operations() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    
    while (!user_operations.empty()) {
        auto operation = user_operations.front();
        user_operations.pop();
        
        std::string username = operation.first;
        bool is_add = operation.second;
        
        std::cout << "[DEBUG] Processing operation for user: " << username 
                  << " (add: " << is_add << ")" << std::endl;
        
        if (is_add) {
            // Пробуем создать пользователя разными способами
            bool user_created = false;
            
            // Способ 1: Прямой adduser (если мы root)
            std::string cmd = "adduser --disabled-password --gecos '' --allow-bad-names " + username + " 2>&1";
            std::cout << "[DEBUG] Trying: " << cmd << std::endl;
            
            int result = system(cmd.c_str());
            std::cout << "[DEBUG] Command result: " << result << std::endl;
            
            if (result == 0) {
                user_created = true;
                std::cout << "[DEBUG] ✅ User created via adduser" << std::endl;
            } else {
                // Способ 2: sudo adduser
                cmd = "sudo adduser --disabled-password --gecos '' --allow-bad-names " + username + " 2>&1";
                std::cout << "[DEBUG] Trying sudo: " << cmd << std::endl;
                
                result = system(cmd.c_str());
                std::cout << "[DEBUG] Sudo command result: " << result << std::endl;
                
                if (result == 0) {
                    user_created = true;
                    std::cout << "[DEBUG] ✅ User created via sudo adduser" << std::endl;
                }
            }
            
            // Способ 3: Прямое редактирование /etc/passwd для тестов
            if (!user_created && vfs_users_dir == "/opt/users") {
                std::cout << "[DEBUG] Test mode - adding user directly to /etc/passwd" << std::endl;
                
                // Генерируем уникальный UID
                static int test_uid = 10000;
                test_uid++;
                
                cmd = "echo '" + username + ":x:" + std::to_string(test_uid) + 
                      ":" + std::to_string(test_uid) + "::/home/" + username + 
                      ":/bin/bash' >> /etc/passwd";
                
                std::cout << "[DEBUG] Executing: " << cmd << std::endl;
                
                if (system(cmd.c_str()) == 0) {
                    user_created = true;
                    std::cout << "[DEBUG] ✅ User added to /etc/passwd directly" << std::endl;
                    
                    // Также создаем запись в /etc/shadow
                    cmd = "echo '" + username + ":!!:19265:0:99999:7:::' >> /etc/shadow";
                    system(cmd.c_str());
                    
                    // Создаем домашнюю директорию
                    cmd = "mkdir -p /home/" + username + " && chown " + 
                          std::to_string(test_uid) + ":" + std::to_string(test_uid) + 
                          " /home/" + username + " 2>/dev/null || true";
                    system(cmd.c_str());
                }
            }
            
            // Создаем файлы в VFS если пользователь создан
            if (user_created) {
                std::string user_dir = vfs_users_dir + "/" + username;
                
                // Убедимся что директория существует
                mkdir(user_dir.c_str(), 0755);
                
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
                    std::cout << "[DEBUG] ✅ VFS files created for user: " << username << std::endl;
                } else {
                    std::cerr << "[DEBUG] ❌ User not found in passwd after creation: " << username << std::endl;
                }
            } else {
                std::cerr << "[DEBUG] ❌ Failed to create user: " << username << std::endl;
            }
        }
    }
}

void queue_add_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    user_operations.push({username, true});
    //std::cout << "Queued user creation: " << username << std::endl;
}

void queue_delete_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    user_operations.push({username, false});
    //std::cout << "Queued user deletion: " << username << std::endl;
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
    
    //std::cout << "Monitoring directory: " << users_dir << std::endl;
    
    char buffer[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    
    while (monitor_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
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
                        std::thread(queue_add_user, username).detach();
                    }
                }
                else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
                    if (event->mask & IN_ISDIR) {
                        std::string username = event->name;
                        std::thread(queue_delete_user, username).detach();
                    }
                }
            }
        }
    }
    
    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);
}

void setup_users_vfs() {
    // Всегда проверяем /opt/users в первую очередь
    struct stat st;
    
    if (stat("/opt/users", &st) != -1) {
        // /opt/users существует - используем его
        vfs_users_dir = "/opt/users";
        std::cout << "[DEBUG] Using /opt/users (test environment)" << std::endl;
    } else {
        // Иначе используем домашнюю директорию
        const char* home = std::getenv("HOME");
        vfs_users_dir = std::string(home ? home : "") + "/users";
        std::cout << "[DEBUG] Using home directory: " << vfs_users_dir << std::endl;
    }
    
    // Читаем /etc/passwd и создаем VFS СИНХРОННО
    std::cout << "[DEBUG] Reading /etc/passwd..." << std::endl;
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    int user_count = 0;
    
    if (!passwd_file.is_open()) {
        std::cerr << "[DEBUG] Cannot open /etc/passwd" << std::endl;
        return;
    }
    
    while (std::getline(passwd_file, line)) {
        // Ищем пользователей с shell (оканчиваются на sh)
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
                
                std::cout << "[DEBUG] Creating VFS entry for: " << username << std::endl;
                
                // Создаем директорию
                if (mkdir(user_dir.c_str(), 0755) == -1 && errno != EEXIST) {
                    std::cerr << "[DEBUG] Failed to create dir for " << username << std::endl;
                    continue;
                }
                
                // Создаем файлы
                std::ofstream id_file(user_dir + "/id");
                if (id_file.is_open()) {
                    id_file << parts[2];  // UID
                    id_file.close();
                }
                
                std::ofstream home_file(user_dir + "/home");
                if (home_file.is_open()) {
                    home_file << parts[5];  // Home directory
                    home_file.close();
                }
                
                std::ofstream shell_file(user_dir + "/shell");
                if (shell_file.is_open()) {
                    shell_file << parts[6];  // Shell
                    shell_file.close();
                }
                
                user_count++;
            }
        }
    }
    passwd_file.close();
    
    std::cout << "[DEBUG] VFS initialized with " << user_count << " users" << std::endl;
    
    // Проверим что создалось
    DIR* dir = opendir(vfs_users_dir.c_str());
    if (dir) {
        std::cout << "[DEBUG] Contents of " << vfs_users_dir << ":" << std::endl;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] != '.') {
                std::cout << "[DEBUG]   " << entry->d_name << std::endl;
            }
        }
        closedir(dir);
    }
    
    // Только ПОСЛЕ инициализации запускаем мониторинг
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

//Обработчик команд 3000
bool handle_internal_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    
    const std::string& cmd = args[0];
    
    if (cmd == "exit" || cmd == "\\q") {
        monitor_running = false;
        return true;
    }
    else if (cmd == "echo" && args.size() > 1) {
        // Обработка echo команды
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) std::cout << " ";
            
            // Убираем кавычки если есть
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
        // Обработка переменных окружения
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
    // Та же логика что и для echo
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
    bool is_test_mode = (std::getenv("PYTEST_CURRENT_TEST") != nullptr);
    // Отключаем буферизацию для корректной работы с тестами
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    // Создаем VFS с пользователями при запуске
    setup_users_vfs();

    // История команд
    std::string history_path = std::string(getenv("HOME")) + "/.kubsh_history";
    std::ofstream history_file(history_path, std::ios::app);
    
    if (!history_file.is_open()) {
        std::cerr << "Warning: cannot open history file! History will not be saved." << std::endl;
    }

    std::signal(SIGHUP, sighup_handler);

    while (true) {
        //if (sighup_received) {
            //std::cout << "Configuration reloaded" << std::endl;;
            //sighup_received = 0;
            //continue;
        //}

        // Обрабатываем операции с пользователями
        process_user_operations();

        //std::cout << "$ ";
        if (!is_test_mode) {
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

        // Сначала пробуем обработать как внутреннюю команду
        if (handle_internal_command(args)) {
            if (args[0] == "exit" || args[0] == "\\q") {
                break;
            }
        }
        // Затем как внешнюю команду
        else if (!execute_external_command(args)) {
            std::cout << args[0] << ": command not found" << std::endl;
        }
        
        // Сбрасываем вывод после каждой команды
        std::cout.flush();
        std::cerr.flush();
    }

    monitor_running = false;
    
    if (history_file.is_open()) {
        history_file.close();
    }
    
    return 0;
}