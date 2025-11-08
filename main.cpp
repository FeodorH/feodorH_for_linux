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

volatile sig_atomic_t sighup_received = 0;
std::atomic<bool> monitor_running{true};

// Очередь для операций с пользователями
std::queue<std::pair<std::string, bool>> user_operations; // <username, is_add_operation>
std::mutex queue_mutex;

void sighup_handler(int) {
    sighup_received = 1;
}

// Функция для выполнения операций с пользователями в основном потоке
void process_user_operations() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    
    while (!user_operations.empty()) {
        auto operation = user_operations.front();
        user_operations.pop();
        
        std::string username = operation.first;
        bool is_add = operation.second;
        
        if (is_add) {
            std::cout << "Processing user creation: " << username << std::endl;
            
            // Создаем пользователя с sudo, разрешая "плохие" имена
            std::string command = "sudo adduser --disabled-password --gecos '' --allow-bad-names " + username;
            std::cout << "Executing: " << command << std::endl;
            
            int result = system(command.c_str());
            
            if (result == 0) {
                std::cout << "User " << username << " created successfully\n";
                
                // Обновляем информацию в каталоге пользователя
                std::string users_dir = std::string(getenv("HOME")) + "/users";
                std::string user_dir = users_dir + "/" + username;
                
                struct passwd *pwd = getpwnam(username.c_str());
                if (pwd != nullptr) {
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
                }
            } else {
                std::cerr << "Error: failed to create user " << username << "\n";
            }
        } else {
            std::cout << "Processing user deletion: " << username << std::endl;
            
            // Удаляем пользователя с sudo
            std::string command = "sudo userdel -r " + username;
            std::cout << "Executing: " << command << std::endl;
            
            int result = system(command.c_str());
            
            if (result == 0) {
                std::cout << "User " << username << " deleted successfully\n";
            } else {
                std::cerr << "Error: failed to delete user " << username << "\n";
            }
        }
    }
}

// Упрощенные функции для добавления операций в очередь
void queue_add_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    user_operations.push({username, true});
    std::cout << "Queued user creation: " << username << std::endl;
}

void queue_delete_user(const std::string& username) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    user_operations.push({username, false});
    std::cout << "Queued user deletion: " << username << std::endl;
}

// Функция для мониторинга изменений в каталоге users
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
    
    std::cout << "Monitoring directory: " << users_dir << std::endl;
    
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
                        std::cout << "=== Directory created: " << username << " ===" << std::endl;
                        
                        // Добавляем в очередь вместо непосредственного выполнения
                        std::thread(queue_add_user, username).detach();
                    }
                }
                else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
                    if (event->mask & IN_ISDIR) {
                        std::string username = event->name;
                        std::cout << "=== Directory deleted: " << username << " ===" << std::endl;
                        
                        // Добавляем в очередь вместо непосредственного выполнения
                        std::thread(queue_delete_user, username).detach();
                    }
                }
            }
        }
    }
    
    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);
}

// Функция для создания VFS с пользователями
void setup_users_vfs() {
    std::string home_dir = getenv("HOME");
    std::string users_dir = home_dir + "/users";
    
    // Проверяем существование каталога
    struct stat st;
    if (stat(users_dir.c_str(), &st) == -1) {
        // Каталог не существует, создаем
        if (mkdir(users_dir.c_str(), 0755) == -1) {
            std::cerr << "Error: cannot create users directory " << users_dir << std::endl;
            return;
        }
        std::cout << "Created users directory: " << users_dir << std::endl;
    }
    
    // Получаем всех пользователей системы
    setpwent();
    struct passwd *pwd;
    
    while ((pwd = getpwent()) != nullptr) {
        // Пропускаем системных пользователей и пользователей без логина
        if (pwd->pw_uid >= 1000 && pwd->pw_name[0] != '\0') {
            std::string username = pwd->pw_name;
            std::string user_dir = users_dir + "/" + username;
            
            // Создаем каталог пользователя
            if (mkdir(user_dir.c_str(), 0755) == -1 && errno != EEXIST) {
                std::cerr << "Error: cannot create user directory " << user_dir << std::endl;
                continue;
            }
            
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
        }
    }
    endpwent();
    
    // Запускаем мониторинг в отдельном потоке
    std::thread monitor_thread(monitor_users_directory, users_dir);
    monitor_thread.detach();
}

// Остальные функции остаются без изменений
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
        
        std::cerr << "Error: command '" << args[0] << "' not found\n";
        exit(1);
        
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        return true;
    } else {
        std::cerr << "Error: fork create failed!\n";
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

int main() {
    // Создаем VFS с пользователями при запуске
    setup_users_vfs();

    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::ofstream history_file("/home/main_user/kubsh_history.txt", std::ios::app);
    history_file << std::unitbuf;

    std::signal(SIGHUP, sighup_handler);

    if (!history_file.is_open()) {
        std::cerr << "Error: cannot open history file!" << std::endl;
        return 1;
    }

    std::cout << "PID: " << getpid() << std::endl;

    while (true) {
        if (sighup_received) {
            std::cout << "\nConfiguration reloaded\n";
            sighup_received = 0;
            continue;
        }

        // Обрабатываем операции с пользователями в основном потоке
        process_user_operations();

        std::cout << "$ ";
        std::string input;
        
        if (!std::getline(std::cin, input)||(input == "\\q")) {
            std::cout << "\nExit" << std::endl;
            monitor_running = false;
            break;
        }

        history_file << input << "\n";

        if (input.substr(0, 6) == "echo \"" && input[input.size() - 1]=='\"') {
            std::cout << input.substr(6, input.size()-7) << "\n";
        } else if(input == "\\e $PATH") {
            const char* path_env = std::getenv("PATH");
            if (path_env == nullptr) {
                std::cout << "Error: $PATH not found!" << std::endl;
            } else {
                std::string t = std::string(path_env);
                int i = 0;
                for(int j = 0; j < t.size(); j++) {
                    if(t.at(j) == ':') {
                        std::cout << t.substr(i, j-i) << "\n";
                        i = j + 1;
                    }
                }
                if (i < t.size()) {
                    std::cout << t.substr(i) << std::endl;
                }
            }
        } else {
            std::vector<std::string> args = split_arguments(input);
            
            if (!args.empty()) {
                if (args[0] == "\\l" && args.size() == 2) {
                    bool handled = handle_partition_list(args);
                    if (!handled) {
                        std::cout << "Error: invalid \\l usage. Use: \\l /dev/sda\n";
                    }
                } else {
                    bool executed = execute_external_command(args);
                    if (!executed) {
                        std::cout << "Error: command not found!\n";
                    }
                }
            }
        }
    }

    history_file.close();
    return 0;
}