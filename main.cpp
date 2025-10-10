#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sstream>

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
        std::vector<char*> exec_args;
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

int main() {
  /// Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::ofstream history_file("/home/main_user/kubsh_history.txt", std::ios::app);

    if (!history_file.is_open()) {
        std::cerr << "Error: cannot open history file!" << std::endl;
        return 1;
    }

    history_file << std::unitbuf;//for rebuffing

    while (true) {
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
                bool executed = execute_external_command(args);
                if (!executed) {
                    std::cout << "Error: command not found!\n";
                }
            }
        }

    }

    history_file.close();
    return 0;
}
