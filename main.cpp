#include <iostream>
#include <string>
#include <fstream>

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
        }

    }

    history_file.close();
    return 0;
}