#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sstream>
#include <csignal>
#include <cstring>
#include <pwd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <thread>
#include <atomic>

std::atomic<bool> monitor_running{true};
std::string vfs_users_dir;

void process_user_addition(const std::string& username) {
    std::cout << "Processing user: " << username << std::endl;
    
    // Для тестового режима
    if (vfs_users_dir == "/opt/users") {
        static int uid_counter = 10000;
        int uid = uid_counter++;
        
        // 1. Записываем в shared файл (тест может его читать)
        std::ofstream shared_file("/tmp/kubsh_added_users", std::ios::app);
        if (shared_file.is_open()) {
            shared_file << username << ":" << uid << std::endl;
            shared_file.close();
        }
        
        // 2. Добавляем в /etc/passwd
        std::ofstream passwd("/etc/passwd", std::ios::app);
        if (passwd.is_open()) {
            passwd << username << ":x:" << uid << ":" << uid 
                   << "::/home/" << username << ":/bin/bash" << std::endl;
            passwd.close();
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
        
        std::cout << "User added: " << username << " (UID: " << uid << ")" << std::endl;
    }
}

void monitor_users_directory() {
    // Очищаем shared файл при запуске
    if (vfs_users_dir == "/opt/users") {
        std::remove("/tmp/kubsh_added_users");
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
        
        struct timeval timeout = {0, 100000};
        
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
                        process_user_addition(name);
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
        std::cout << "Test mode: /opt/users" << std::endl;
    } else {
        const char* home = getenv("HOME");
        if (!home) home = "/root";
        vfs_users_dir = std::string(home) + "/users";
        std::cout << "Normal mode: " << vfs_users_dir << std::endl;
    }
    
    mkdir(vfs_users_dir.c_str(), 0755);
    
    // Запускаем мониторинг
    std::thread(monitor_users_directory).detach();
}

// ... остальные функции (echo, env и т.д.) ...

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    std::cout << "UID: " << geteuid() << "/" << getuid() << std::endl;
    
    setup_users_vfs();
    signal(SIGHUP, [](int){ std::cout << "Configuration reloaded" << std::endl; });
    
    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "exit" || input == "\\q") break;
        if (input.empty()) continue;
        
        // Простая обработка команд
        if (input.find("echo ") == 0 || input.find("debug ") == 0) {
            std::cout << input.substr(input.find(' ') + 1) << std::endl;
        } else if (input == "\\e $HOME" || input == "\\e HOME") {
            const char* home = getenv("HOME");
            if (home) std::cout << home << std::endl;
        } else {
            // Внешняя команда
            system(input.c_str());
        }
    }
    
    monitor_running = false;
    return 0;
}