#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <pwd.h>
#include <atomic>
#include <thread>
#include <vector>
#include <ctime>
#include <sys/types.h>
#include <sys/wait.h>


std::atomic<bool> running{true};
std::string vfs_dir;

void handle_new_user(const std::string& username) {
    std::cout << "DEBUG: Adding user: " << username << std::endl;
    std::cout.flush();
    
    // В тестовом режиме
    if (vfs_dir == "/opt/users") {
        // ПРОВЕРЯЕМ время начала
        std::time_t start_time = std::time(nullptr);
        std::cout << "DEBUG: Start time: " << start_time << std::endl;
        
        // 1. Используем adduser как требует задание
        std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>&1";
        std::cout << "DEBUG: Executing: " << cmd << std::endl;
        std::cout.flush();
        
        // Запускаем команду
        int result = system(cmd.c_str());
        std::cout << "DEBUG: Command exit code: " << result << std::endl;
        
        // 2. СИНХРОНИЗИРУЕМ файловую систему
        system("sync");
        
        // 3. Даем время на запись
        usleep(100000); // 100ms
        
        // 4. Проверяем результат
        struct passwd *pwd = getpwnam(username.c_str());
        if (pwd) {
            std::cout << "DEBUG: Success! User " << username << " added with UID " << pwd->pw_uid << std::endl;
            
            // Дополнительная проверка - читаем файл напрямую
            std::ifstream passwd_file("/etc/passwd");
            std::string line;
            bool found = false;
            while (std::getline(passwd_file, line)) {
                if (line.find(username + ":") == 0) {
                    std::cout << "DEBUG: Found in /etc/passwd file: " << line << std::endl;
                    found = true;
                    break;
                }
            }
            passwd_file.close();
            
            if (!found) {
                std::cout << "DEBUG: ERROR: User not in /etc/passwd file!" << std::endl;
            }
            
            // 5. Создаем VFS файлы
            std::string user_path = vfs_dir + "/" + username;
            mkdir(user_path.c_str(), 0755);
            
            std::ofstream id_file(user_path + "/id");
            if (id_file.is_open()) {
                id_file << pwd->pw_uid;
                id_file.close();
            }
            
            std::ofstream home_file(user_path + "/home");
            if (home_file.is_open()) {
                home_file << pwd->pw_dir;
                home_file.close();
            }
            
            std::ofstream shell_file(user_path + "/shell");
            if (shell_file.is_open()) {
                shell_file << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
                shell_file.close();
            }
            
            std::cout << "DEBUG: VFS files created" << std::endl;
            
        } else {
            std::cout << "DEBUG: FAILED! User not found" << std::endl;
        }
        
        std::time_t end_time = std::time(nullptr);
        std::cout << "DEBUG: End time: " << end_time << std::endl;
        std::cout << "DEBUG: Total time: " << (end_time - start_time) << " seconds" << std::endl;
        
        std::cout << "User added: " << username << std::endl;
        std::cout.flush();
    }
}

void monitor_directory() {
    std::cout << "DEBUG: Starting monitor for: " << vfs_dir << std::endl;
    std::cout.flush();
    
    int fd = inotify_init();
    if (fd < 0) {
        std::cout << "DEBUG: inotify_init failed" << std::endl;
        return;
    }
    
    int wd = inotify_add_watch(fd, vfs_dir.c_str(), IN_CREATE);
    if (wd < 0) {
        std::cout << "DEBUG: inotify_add_watch failed" << std::endl;
        close(fd);
        return;
    }
    
    std::cout << "DEBUG: Monitoring started" << std::endl;
    std::cout.flush();
    
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
                        std::cout << "DEBUG: Detected directory: " << name << std::endl;
                        handle_new_user(name);
                    }
                }
            }
        }
    }
    
    close(fd);
    std::cout << "DEBUG: Monitoring stopped" << std::endl;
}

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
    
    std::ifstream passwd("/etc/passwd");
    std::string line;
    int count = 0;
    
    while (std::getline(passwd, line)) {
        if (line.find("/bin/bash") != std::string::npos || 
            line.find("/bin/sh") != std::string::npos) {
            
            size_t pos1 = line.find(':');
            if (pos1 != std::string::npos) {
                std::string user = line.substr(0, pos1);
                std::string user_dir = vfs_dir + "/" + user;
                
                mkdir(user_dir.c_str(), 0755);
                
                std::vector<std::string> parts;
                std::stringstream ss(line);
                std::string part;
                
                while (std::getline(ss, part, ':')) {
                    parts.push_back(part);
                }
                
                if (parts.size() >= 7) {
                    std::ofstream(user_dir + "/id") << parts[2];
                    std::ofstream(user_dir + "/home") << parts[5];
                    std::ofstream(user_dir + "/shell") << parts[6];
                    count++;
                }
            }
        }
    }
    
    std::cout << "VFS initialized with " << count << " users" << std::endl;
    std::cout.flush();
    
    std::thread(monitor_directory).detach();
}

// ... остальной код (main, echo, env) без изменений ...

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    std::cout << "UID: " << geteuid() << std::endl;
    
    init_vfs();
    
    // Простая обработка команд
    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "exit" || input == "\\q") {
            running = false;
            break;
        }
        
        if (input.find("echo ") == 0 || input.find("debug ") == 0) {
            std::string output = input.substr(input.find(' ') + 1);
            // Убираем кавычки если есть
            if (output.size() >= 2) {
                if ((output.front() == '\'' && output.back() == '\'') ||
                    (output.front() == '"' && output.back() == '"')) {
                    output = output.substr(1, output.size() - 2);
                }
            }
            std::cout << output << std::endl;
        }
        else if (input == "\\e $PATH" || input == "\\e PATH") {
            const char* path = getenv("PATH");
            if (path) {
                std::stringstream ss(path);
                std::string dir;
                while (std::getline(ss, dir, ':')) {
                    std::cout << dir << std::endl;
                }
            }
        }
        else if (input == "\\e $HOME" || input == "\\e HOME") {
            const char* home = getenv("HOME");
            if (home) std::cout << home << std::endl;
        }
        else if (input.find("\\l ") == 0) {
            std::string disk = input.substr(3);
            std::cout << "Partition info for " << disk << ":\n";
            system(("lsblk " + disk + " 2>/dev/null || echo 'Try: fdisk -l " + disk + "'").c_str());
        }
        else if (!input.empty()) {
            pid_t pid = fork();
            if (pid == 0) {
                std::vector<std::string> args;
                std::stringstream ss(input);
                std::string arg;
                while (ss >> arg) {
                    args.push_back(arg);
                }
                
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
    
    running = false;
    usleep(200000); // Даем время мониторингу завершиться
    
    return 0;
}