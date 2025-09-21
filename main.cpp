#include <iostream>
#include <string>

int main() {
  /// Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";
        std::string input;
        
        if (!std::getline(std::cin, input)) {//exit 1
            // Ctrl+D
            std::cout << "\nExit" << std::endl;
            break;
        }
        
        if (input == "\\q") {//exit 2
            break;
        }
        
        std::cout<<input<<"\n";
        //std::cout << input << ": command not found" << std::endl;
    }
    
    return 0;
}
