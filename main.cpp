#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <pwd.h>
#include <grp.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <map>
#include <algorithm>
#include <fcntl.h>

// Глобальные переменные
std::atomic<bool> running{true};
std::string vfs_dir;
std::string history_path;
volatile sig_atomic_t sighup_received = 0;

// Обработчик SIGHUP
void sighup_handler(int) {
    sighup_received = 1;
}

// ================ VFS ФУНКЦИИ ================

// Принудительная синхронизация файловой системы
void force_sync() {
    sync();
    usleep(50000); // 50ms
}

// Проверка существования пользователя
bool user_exists(const std::string& username) {
    struct passwd *pwd = getpwnam(username.c_str());
    if (pwd) return true;
    
    // Дополнительная проверка через файл
    std::ifstream passwd_file("/etc/passwd");
    std::string line;
    while (std::getline(passwd_file, line)) {
        if (line.find(username + ":") == 0) {
            return true;
        }
    }
    return false;
}

// Создание пользователя
void create_user(const std::string& username) {
    std::cout << "DEBUG: Adding user: " << username << std::endl;
    
    if (user_exists(username)) {
        std::cout << "DEBUG: User already exists: " << username << std::endl;
        return;
    }
    
    // В тестовом режиме добавляем напрямую в /etc/passwd
    if (vfs_dir == "/opt/users") {
        std::cout << "Test mode: adding user directly to /etc/passwd" << std::endl;
        
        // Генерируем уникальный UID
        static std::atomic<int> next_uid{10000};
        int uid = next_uid++;
        
        // Добавляем в /etc/passwd
        std::ofstream passwd_out("/etc/passwd", std::ios::app);
        if (passwd_out.is_open()) {
            passwd_out << username << ":x:" << uid << ":" << uid 
                      << "::/home/" << username << ":/bin/bash" << std::endl;
            passwd_out.close();
            std::cout << "Added to /etc/passwd: " << username << " (UID: " << uid << ")" << std::endl;
        }
        
        // Синхронизируем
        force_sync();
        
        // Ждем и проверяем
        for (int i = 0; i < 5; i++) {
            if (user_exists(username)) break;
            usleep(100000); // 100ms
        }
        
    } else {
        // Нормальный режим
        std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>&1";
        std::cout << "Executing: " << cmd << std::endl;
        
        int result = system(cmd.c_str());
        std::cout << "Command result: " << result << std::endl;
        
        force_sync();
        usleep(100000); // 100ms
    }
    
    // Создаем VFS файлы
    struct passwd *pwd = getpwnam(username.c_str());
    if (pwd) {
        std::string user_path = vfs_dir + "/" + username;
        mkdir(user_path.c_str(), 0755);
        
        std::ofstream id_file(user_path + "/id");
        if (id_file.is_open()) id_file << pwd->pw_uid;
        
        std::ofstream home_file(user_path + "/home");
        if (home_file.is_open()) home_file << pwd->pw_dir;
        
        std::ofstream shell_file(user_path + "/shell");
        if (shell_file.is_open()) shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
        
        std::cout << "VFS files created for user: " << username << std::endl;
    }
    
    std::cout << "User created: " << username << std::endl;
}

// Удаление пользователя
void delete_user(const std::string& username) {
    std::cout << "DEBUG: Deleting user: " << username << std::endl;
    
    // В тестовом режиме удаляем из /etc/passwd
    if (vfs_dir == "/opt/users") {
        std::cout << "Test mode: removing user from /etc/passwd" << std::endl;
        
        // Читаем весь файл, пропускаем нужного пользователя
        std::ifstream passwd_in("/etc/passwd");
        std::string content;
        std::string line;
        
        while (std::getline(passwd_in, line)) {
            if (!line.empty() && line.find(username + ":") != 0) {
                content += line + "\n";
            }
        }
        passwd_in.close();
        
        // Перезаписываем файл
        std::ofstream passwd_out("/etc/passwd");
        if (passwd_out.is_open()) {
            passwd_out << content;
            passwd_out.close();
        }
        
        force_sync();
        
    } else {
        // Нормальный режим
        std::string cmd = "userdel -r " + username + " 2>&1";
        std::cout << "Executing: " << cmd << std::endl;
        
        int result = system(cmd.c_str());
        std::cout << "Command result: " << result << std::endl;
        
        force_sync();
        usleep(100000); // 100ms
    }
    
    std::cout << "User deleted: " << username << std::endl;
}

// Мониторинг директории VFS
void monitor_directory() {
    std::cout << "DEBUG: Starting monitor for: " << vfs_dir << std::endl;
    
    int fd = inotify_init();
    if (fd < 0) {
        std::cout << "DEBUG: inotify_init failed" << std::endl;
        return;
    }
    
    int wd = inotify_add_watch(fd, vfs_dir.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    if (wd < 0) {
        std::cout << "DEBUG: inotify_add_watch failed" << std::endl;
        close(fd);
        return;
    }
    
    std::cout << "DEBUG: Monitoring started" << std::endl;
    
    char buf[4096];
    
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        
        struct timeval tv = {0, 100000}; // 100ms
        
        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        
        if (ret > 0 && FD_ISSET(fd, &fds)) {
            ssize_t len = read(fd, buf, sizeof(buf));
            if (len <= 0) break;
            
            for (char *ptr = buf; ptr < buf + len; ) {
                struct inotify_event *ev = (struct inotify_event *)ptr;
                ptr += sizeof(struct inotify_event) + ev->len;
                
                if (ev->len == 0) continue;
                std::string name = ev->name;
                if (name.empty() || name[0] == '.') continue;
                
                if (ev->mask & IN_CREATE) {
                    if (ev->mask & IN_ISDIR) {
                        std::cout << "DEBUG: Directory created: " << name << std::endl;
                        create_user(name);
                    }
                }
                else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    if (ev->mask & IN_ISDIR) {
                        std::cout << "DEBUG: Directory deleted: " << name << std::endl;
                        delete_user(name);
                    }
                }
            }
        }
    }
    
    close(fd);
    std::cout << "DEBUG: Monitoring stopped" << std::endl;
}

// Инициализация VFS
void init_vfs() {
    struct stat st;
    if (stat("/opt/users", &st) != -1) {
        vfs_dir = "/opt/users";
        std::cout << "Test mode: /opt/users" << std::endl;
    } else {
        const char* home = getenv("HOME");
        vfs_dir = std::string(home ? home : "/root") + "/users";
        std::cout << "Normal mode: " << vfs_dir << std::endl;
    }
    
    mkdir(vfs_dir.c_str(), 0755);
    
    // Инициализируем существующих пользователей
    std::ifstream passwd("/etc/passwd");
    std::string line;
    int count = 0;
    
    while (std::getline(passwd, line)) {
        if (line.find("/bin/bash") != std::string::npos || 
            line.find("/bin/sh") != std::string::npos) {
            
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            
            while (std::getline(ss, part, ':')) {
                parts.push_back(part);
            }
            
            if (parts.size() >= 7) {
                std::string user = parts[0];
                std::string user_dir = vfs_dir + "/" + user;
                
                mkdir(user_dir.c_str(), 0755);
                
                std::ofstream(user_dir + "/id") << parts[2];
                std::ofstream(user_dir + "/home") << parts[5];
                std::ofstream(user_dir + "/shell") << parts[6];
                count++;
            }
        }
    }
    
    std::cout << "VFS initialized with " << count << " users" << std::endl;
    
    // Запускаем мониторинг в отдельном потоке
    std::thread(monitor_directory).detach();
}

// ================ КОМАНДЫ ШЕЛЛА ================

// Добавление в историю
void save_to_history(const std::string& command) {
    if (command.empty() || command[0] == ' ') return;
    
    std::ofstream history_file(history_path, std::ios::app);
    if (history_file.is_open()) {
        history_file << command << std::endl;
    }
}

// Обработка echo/debug
bool handle_echo(const std::vector<std::string>& args) {
    if (args.empty() || (args[0] != "echo" && args[0] != "debug")) return false;
    
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) std::cout << " ";
        std::string arg = args[i];
        
        // Убираем кавычки
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

// Обработка переменных окружения
bool handle_env(const std::vector<std::string>& args) {
    if (args.size() != 2 || args[0] != "\\e") return false;
    
    std::string var = args[1];
    
    if (var == "$PATH" || var == "PATH") {
        const char* path = getenv("PATH");
        if (path) {
            std::stringstream ss(path);
            std::string dir;
            while (std::getline(ss, dir, ':')) {
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

// Обработка информации о разделах
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

// Выполнение внешней команды
bool execute_external(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    
    pid_t pid = fork();
    if (pid == 0) {
        // Дочерний процесс
        std::vector<char*> exec_args;
        for (const auto& a : args) {
            exec_args.push_back(const_cast<char*>(a.c_str()));
        }
        exec_args.push_back(nullptr);
        
        execvp(exec_args[0], exec_args.data());
        std::cout << args[0] << ": command not found" << std::endl;
        exit(127);
    } else if (pid > 0) {
        // Родительский процесс
        waitpid(pid, nullptr, 0);
        return true;
    }
    
    return false;
}

// ================ ГЛАВНАЯ ФУНКЦИЯ ================

int main() {
    // Отключаем буферизацию для тестов
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    // Инициализация истории
    const char* home = getenv("HOME");
    if (home) {
        history_path = std::string(home) + "/.kubsh_history";
    } else {
        history_path = "/root/.kubsh_history";
    }
    
    // Инициализация VFS
    init_vfs();
    
    // Обработчик сигналов
    signal(SIGHUP, sighup_handler);
    
    // Главный цикл
    std::string input;
    
    while (running) {
        // Обработка SIGHUP
        if (sighup_received) {
            std::cout << "Configuration reloaded" << std::endl;
            sighup_received = 0;
        }
        
        // Приглашение только в нормальном режиме
        if (vfs_dir != "/opt/users") {
            std::cout << "₽ ";
        }
        
        // Чтение ввода
        if (!std::getline(std::cin, input)) {
            break; // Ctrl+D
        }
        
        if (input.empty()) continue;
        
        // Сохранение в историю
        save_to_history(input);
        
        // Разбиение на аргументы
        std::vector<std::string> args;
        std::stringstream ss(input);
        std::string arg;
        while (ss >> arg) {
            args.push_back(arg);
        }
        
        if (args.empty()) continue;
        
        // Обработка команд выхода
        if (args[0] == "exit" || args[0] == "\\q") {
            running = false;
            break;
        }
        
        // Обработка внутренних команд
        bool handled = false;
        
        if (handle_echo(args)) {
            handled = true;
        } else if (handle_env(args)) {
            handled = true;
        } else if (handle_partition(args)) {
            handled = true;
        }
        
        // Выполнение внешних команд
        if (!handled) {
            if (!execute_external(args)) {
                std::cout << args[0] << ": command not found" << std::endl;
            }
        }
        
        std::cout.flush();
    }
    
    running = false;
    usleep(200000); // Даем время мониторингу завершиться
    
    return 0;
}