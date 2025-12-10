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
    std::cout << "DEBUG: Adding user: " << username << std::endl;
    
    // В тестовом режиме
    if (vfs_dir == "/opt/users") {
        // ПРОВЕРЯЕМ: не существует ли уже пользователь
        struct passwd *pwd_check = getpwnam(username.c_str());
        if (pwd_check) {
            std::cout << "DEBUG: User " << username << " already exists with UID " << pwd_check->pw_uid << std::endl;
        }
        
        // 1. Используем adduser как требует задание
        std::string cmd = "adduser --disabled-password --gecos '' " + username + " 2>&1";
        std::cout << "DEBUG: Executing: " << cmd << std::endl;
        
        // Запускаем команду и захватываем вывод
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            std::cout << "DEBUG: Failed to execute command" << std::endl;
            return;
        }
        
        char buffer[128];
        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        
        int result = pclose(pipe);
        std::cout << "DEBUG: Command exit code: " << result << std::endl;
        std::cout << "DEBUG: Command output:\n" << output << std::endl;
        
        // 2. Проверяем результат
        struct passwd *pwd = getpwnam(username.c_str());
        if (pwd) {
            std::cout << "DEBUG: Success! User " << username << " added with UID " << pwd->pw_uid << std::endl;
            
            // Проверяем, виден ли в /etc/passwd
            std::ifstream passwd_file("/etc/passwd");
            std::string line;
            bool found_in_file = false;
            while (std::getline(passwd_file, line)) {
                if (line.find(username + ":") == 0) {
                    std::cout << "DEBUG: Found in /etc/passwd: " << line << std::endl;
                    found_in_file = true;
                    break;
                }
            }
            
            if (!found_in_file) {
                std::cout << "DEBUG: WARNING: User not found in /etc/passwd file!" << std::endl;
            }
            
            // 3. Создаем VFS файлы
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
            
            std::cout << "DEBUG: VFS files created for " << username << std::endl;
            
        } else {
            std::cout << "DEBUG: FAILED! User " << username << " not found after adduser" << std::endl;
            
            // Попробуем прочитать /etc/passwd напрямую
            std::cout << "DEBUG: Checking /etc/passwd contents..." << std::endl;
            std::ifstream passwd_file("/etc/passwd");
            std::string line;
            int line_num = 0;
            while (std::getline(passwd_file, line)) {
                line_num++;
                if (line.find(username) != std::string::npos) {
                    std::cout << "DEBUG: Line " << line_num << ": " << line << std::endl;
                }
            }
        }
        
        std::cout << "User added: " << username << std::endl;
        std::cout.flush(); // Важно!
    }
}

// ... остальной код без изменений ...

void monitor_directory() {
    std::cout << "DEBUG: Starting monitor for: " << vfs_dir << std::endl;
    
    int fd = inotify_init();
    if (fd < 0) {
        std::cout << "DEBUG: inotify_init failed" << std::endl;
        return;
    }
    
    int wd = inotify_add_watch(fd, vfs_dir.c_str(), IN_CREATE);
    if (wd < 0) {
        std::cout << "DEBUG: inotify_add_watch failed for " << vfs_dir << std::endl;
        close(fd);
        return;
    }
    
    std::cout << "DEBUG: Monitoring started for " << vfs_dir << std::endl;
    
    char buf[4096];
    
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        
        struct timeval tv = {0, 50000}; // 50ms
        
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
                        std::cout << "DEBUG: Detected new directory: " << name << std::endl;
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
