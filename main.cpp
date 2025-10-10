#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib> 

int main() {
  /// Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::ofstream history_file("/home/main_user/kubsh_history.txt", std::ios::app);

    if (!history_file.is_open()) {
        std::cerr << "Error: Cannot open history file!" << std::endl;
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
                std::cout << "$PATH not found!" << std::endl;
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
            std::cout<<"command not found!\n";
        }

    }

    history_file.close();
    return 0;
}


