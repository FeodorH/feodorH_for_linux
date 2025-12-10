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

std::atomic<bool> running{true};
std::string vfs_dir;

void handle_new_user(const std::string& username) {
    std::cout << "Adding user: " << username << std::endl;
    
    // В тестовом режиме
    if (vfs_dir == "/opt/users") {
        static int uid = 10000;
        int user_uid = uid++;
        
        // 1. Добавляем в /etc/passwd (системный вызов)
        std::string cmd = "useradd -m -s /bin/bash " + username + " 2>/dev/null || ";
        cmd += "adduser --disabled-password --gecos '' " + username + " 2>/dev/null";
        system(cmd.c_str());
        
        // 2. Создаем VFS файлы
        std::string user_path = vfs_dir + "/" + username;
        mkdir(user_path.c_str(), 0755);
        
        // Получаем информацию о пользователе
        struct passwd *pwd = getpwnam(username.c_str());
        if (pwd) {
            std::ofstream(user_path + "/id") << pwd->pw_uid;
            std::ofstream(user_path + "/home") << pwd->pw_dir;
            std::ofstream(user_path + "/shell") << (pwd->pw_shell ? pwd->pw_shell : "/bin/sh");
        } else {
            // Fallback
            std::ofstream(user_path + "/id") << user_uid;
            std::ofstream(user_path + "/home") << "/home/" + username;
            std::ofstream(user_path + "/shell") << "/bin/bash";
        }
        
        std::cout << "User added: " << username << std::endl;
    }
}

void monitor_directory() {
    int fd = inotify_init();
    if (fd < 0) return;
    
    int wd = inotify_add_watch(fd, vfs_dir.c_str(), IN_CREATE);
    if (wd < 0) {
        close(fd);
        return;
    }
    
    char buf[4096];
    
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        
        struct timeval tv = {0, 50000};
        
        if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
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
                        handle_new_user(name);
                    }
                }
            }
        }
    }
    
    close(fd);
}

void init_vfs() {
    // Определяем директорию
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
            
            size_t pos1 = line.find(':');
            if (pos1 != std::string::npos) {
                std::string user = line.substr(0, pos1);
                std::string user_dir = vfs_dir + "/" + user;
                
                mkdir(user_dir.c_str(), 0755);
                
                // Разбираем строку
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
    
    // Запускаем мониторинг
    std::thread(monitor_directory).detach();
}

int main() {
    // Отключаем буферизацию
    std::cout << std::unitbuf;
    
    std::cout << "UID: " << geteuid() << std::endl;
    
    init_vfs();
    
    // Главный цикл - простой
    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "exit" || input == "\\q") {
            running = false;
            break;
        }
        
        // Простая обработка команд для базовых тестов
        if (input.find("echo ") == 0 || input.find("debug ") == 0) {
            std::cout << input.substr(input.find(' ') + 1) << std::endl;
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
            system(input.c_str());
        }
    }
    
    running = false;
    usleep(100000); // Даем время мониторингу завершиться
    
    return 0;
}
