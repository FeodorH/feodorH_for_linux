#include <iostream>
#include <string>
#include <fstream>

int main() {
  /// Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    
    std::string input;

    while (true) {
        std::cout << "$ ";
        
        if (!std::getline(std::cin, input)) {
            // Ctrl+D
            std::cout << "\nExit" << std::endl;
            break;
        }
        
        std::cout<<input<<" :)\n";

    }

    std::cout<<input<<" :)\n";

    return 0;
}
