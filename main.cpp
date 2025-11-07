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

volatile sig_atomic_t sighup_received = 0;//флаг для работы SIGHUP

void sighup_handler(int) {
    sighup_received = 1;
}

std::vector<std::string> split_arguments(const std::string& input) {//для разбивки входящего потока в вектор
    std::vector<std::string> args;
    std::stringstream ss(input);
    std::string arg;
    
    while (ss >> arg) {
        args.push_back(arg);
    }
    
    return args;
}

bool execute_external_command(const std::vector<std::string>& args) {//для запуска бинарника
    if (args.empty()) return false;
    
    pid_t pid = fork();//клонируем процессы
    
    if (pid == 0) {//у дочернего pid == 0
        std::vector<char*> exec_args;//перевод в строки в стиле C
        for (const auto& arg : args) {
            exec_args.push_back(const_cast<char*>(arg.c_str()));
        }
        exec_args.push_back(nullptr);
        
        // пытаемся выполнить команду (автоматически ищет в PATH)
        execvp(exec_args[0], exec_args.data());
        
        // если дошли сюда - ошибка
        std::cerr << "Error: command '" << args[0] << "' not found\n";
        exit(1);
        
    } else if (pid > 0) {//для родительского процесса
        int status;
        waitpid(pid, &status, 0);
        return true;
    } else {
        // Ошибка fork
        std::cerr << "Error: fork create failed!\n";
        return false;
    }
}

//number 10{
// Функция для выполнения команды и получения вывода
std::string execute_command(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    
    FILE* pipe = popen(cmd, "r");//создаёт дочерний процесс для запуска команды и через pipe передаёт данные после выполнения
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    
    try {
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {//построчно добавляет результат
            result += buffer.data();
        }
    } catch (...) {
        pclose(pipe);
        throw;
    }
    
    pclose(pipe);
    return result;
}

// Функция для обработки \l команды
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
    
    // Пробуем разные команды, начиная с тех, что не требуют sudo
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
//}

int main() {
  /// Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::ofstream history_file("/home/main_user/kubsh_history.txt", std::ios::app);
    history_file << std::unitbuf;//for rebuffing

    std::signal(SIGHUP, sighup_handler);

    if (!history_file.is_open()) {
        std::cerr << "Error: cannot open history file!" << std::endl;
        return 1;
    }

    std::cout << "PID: " << getpid() << std::endl;

    /*struct sigaction sa;
    sa.sa_handler = sighup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, NULL);*/

    while (true) {

        if (sighup_received) {
            std::cout << "\nConfiguration reloaded\n";
            //std::cout << "$ " << std::flush;
            sighup_received = 0;
            continue;
        }

        std::cout << "$ ";
        std::string input;
        
        if (!std::getline(std::cin, input)||(input == "\\q")) {//exit 1
            // Ctrl+D
            std::cout << "\nExit" << std::endl;
            break;
        }

        history_file<<input<<"\n";

        if (input.substr(0, 6) == "echo \""&&input[input.size() - 1]=='\"') {
            std::cout << input.substr(6,input.size()-7) << "\n";
        }else if(input=="\\e $PATH"){
            const char* path_env = std::getenv("PATH");
            if (path_env == nullptr) {
                std::cout << "Error: $PATH not found!" << std::endl;
            }
            else{
            std::string t = std::string(path_env);
                int i = 0;
                for(int j = 0;j< t.size(); j++){
                    if(t.at(j)==':'){
                        std::cout<<t.substr(i,j-i)<<"\n";
                        i = j + 1;
                    }
                }
                if (i < t.size()) {
                    std::cout << t.substr(i) << std::endl;
                }
            }
        }
        else{
            std::vector<std::string> args = split_arguments(input);
            
            if (!args.empty()) {
                // Сначала проверяем специальную команду \l
                if (args[0] == "\\l" && args.size() == 2) {
                    bool handled = handle_partition_list(args);
                    if (!handled) {
                        std::cout << "Error: invalid \\l usage. Use: \\l /dev/sda\n";
                    }
                } else {
                    // Выполняем обычную команду
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